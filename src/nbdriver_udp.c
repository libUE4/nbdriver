// SPDX-License-Identifier: GPL-2.0-only
/* Downlink capture adapted from workspace enkernel/udp_capture.c. Retains
 * UID filtering, per-CPU scratch ownership exchange, and bounded batches.
 * Adds nbdriver identity checks, FD ownership, retryable reads and poll. */
#include <linux/capability.h>
#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/kprobes.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/percpu.h>
#include <linux/pid.h>
#include <linux/poll.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/udp.h>
#include <linux/vmalloc.h>
#include <net/sock.h>

#include "nbdriver_abi.h"
#include "nbdriver_ksym.h"
#include "nbdriver_log.h"
#include "nbdriver_read.h"
#include "nbdriver_runtime.h"
#include "nbdriver_udp.h"

struct nbdriver_udp_capture {
    struct file *owner; /* borrowed: final file release stops capture */
    struct task_struct *target;
    struct mm_struct *mm; /* mm_count reference, never holds a process alive */
    kuid_t uid;
    int pid;
    u64 start_ns, generation;
    u32 min_payload, max_payload;
    bool enabled;
    spinlock_t ring_lock;
    struct nbdriver_udp_packet *pool, *stage;
    struct nbdriver_udp_packet * __percpu *scratch;
    struct nbdriver_udp_packet *ring[NBDRIVER_UDP_RING_SIZE];
    struct nbdriver_udp_packet *read_slots[NBDRIVER_UDP_READ_MAX];
    u32 head, tail, count, pending;
    u64 captured, overflow, truncated, malformed;
};

static DEFINE_MUTEX(udp_control_lock);
static DECLARE_WAIT_QUEUE_HEAD(udp_wait);
static struct nbdriver_udp_capture *udp_active;
static struct kprobe udp_probe;
static bool udp_available;
static u64 udp_generation;

static bool udp_target_live(const struct nbdriver_udp_capture *capture)
{
    return pid_alive(capture->target) &&
        !(READ_ONCE(capture->target->flags) & PF_EXITING) &&
        READ_ONCE(capture->target->mm) == capture->mm;
}

/* The core UDP receive path owns valid linear IP/UDP headers here. Payloads
 * may be non-linear, so never memcpy directly from skb->data. */
static int udp_packet_copy(struct nbdriver_udp_capture *capture,
                          struct sk_buff *skb, struct nbdriver_udp_packet *out)
{
    const struct iphdr *ip;
    const struct udphdr *udp;
    int offset, available, length, copy;
    bool gro;

    if (!skb || skb->protocol != htons(ETH_P_IP))
        return -EINVAL;
    ip = ip_hdr(skb);
    udp = udp_hdr(skb);
    if (!ip || !udp || ip->version != 4 || ip->protocol != IPPROTO_UDP)
        return -EINVAL;
    offset = skb_transport_offset(skb) + sizeof(*udp);
    if (offset < 0 || (unsigned int)offset > skb->len)
        return -EINVAL;
    available = skb->len - offset;
    length = ntohs(udp->len);
    if (length < (int)sizeof(*udp))
        return -EINVAL;
    length -= sizeof(*udp);
    gro = skb_is_gso(skb);
    if (gro) {
        if (!skb_shinfo(skb)->gso_size)
            return -EINVAL;
        length = available;
    }
    if (length > available)
        return -EINVAL;
    if ((u32)length < capture->min_payload)
        return -ENOMSG;
    copy = min_t(int, length, capture->max_payload);
    if (skb_copy_bits(skb, offset, out->payload, copy))
        return -EFAULT;
    out->src_ip = ip->saddr;
    out->dst_ip = ip->daddr;
    out->src_port = ntohs(udp->source);
    out->dst_port = ntohs(udp->dest);
    out->socket_uid = __kuid_val(capture->uid);
    out->original_len = length;
    out->payload_len = copy;
    out->flags = (copy < length ? NBDRIVER_UDP_F_TRUNCATED : 0) |
        (gro ? NBDRIVER_UDP_F_GRO : 0);
    out->gso_size = gro ? skb_shinfo(skb)->gso_size : 0;
    out->reserved = 0;
    out->reserved2 = 0;
    return 0;
}

static int udp_receive_pre(struct kprobe *probe, struct pt_regs *regs)
{
    struct nbdriver_udp_capture *capture = READ_ONCE(udp_active);
    struct sock *sk = (struct sock *)regs->regs[0];
    struct sk_buff *skb = (struct sk_buff *)regs->regs[1];
    struct nbdriver_udp_packet **scratch, *packet;
    unsigned long flags;
    bool notify;
    int ret;

    (void)probe;
    if (!capture || !smp_load_acquire(&capture->enabled) || !sk ||
        !uid_eq(sk->sk_uid, capture->uid) || !udp_target_live(capture))
        return 0;
    /* Kprobe handlers cannot migrate; nested probes are suppressed. The
     * current scratch slot is not visible to consumers until exchanged. */
    scratch = this_cpu_ptr(capture->scratch);
    packet = *scratch;
    ret = udp_packet_copy(capture, skb, packet);
    if (ret) {
        if (ret != -ENOMSG) {
            spin_lock_irqsave(&capture->ring_lock, flags);
            ++capture->malformed;
            spin_unlock_irqrestore(&capture->ring_lock, flags);
        }
        return 0;
    }
    packet->timestamp_ns = ktime_get_ns();
    spin_lock_irqsave(&capture->ring_lock, flags);
    if (!READ_ONCE(capture->enabled)) {
        spin_unlock_irqrestore(&capture->ring_lock, flags);
        return 0;
    }
    notify = !capture->count;
    if (capture->count == NBDRIVER_UDP_RING_SIZE) {
        ++capture->overflow;
        capture->tail = (capture->tail + 1) % NBDRIVER_UDP_RING_SIZE;
        --capture->count;
    }
    packet->sequence = ++capture->captured;
    if (packet->flags & NBDRIVER_UDP_F_TRUNCATED)
        ++capture->truncated;
    *scratch = capture->ring[capture->head];
    capture->ring[capture->head] = packet;
    capture->head = (capture->head + 1) % NBDRIVER_UDP_RING_SIZE;
    ++capture->count;
    spin_unlock_irqrestore(&capture->ring_lock, flags);
    if (notify)
        wake_up_interruptible_poll(&udp_wait, EPOLLIN | EPOLLRDNORM);
    return 0;
}
NOKPROBE_SYMBOL(udp_packet_copy);
NOKPROBE_SYMBOL(udp_receive_pre);

static void udp_capture_free(struct nbdriver_udp_capture *capture)
{
    if (!capture)
        return;
    free_percpu(capture->scratch);
    kvfree(capture->stage);
    kvfree(capture->pool);
    if (capture->mm)
        mmdrop(capture->mm);
    if (capture->target)
        put_task_struct(capture->target);
    kfree(capture);
}

/* Caller holds control lock; unregister drains every in-flight callback
 * before the active pointer or any scratch/ring allocation is released. */
static void udp_stop_locked(void)
{
    struct nbdriver_udp_capture *capture = udp_active;

    if (!capture)
        return;
    smp_store_release(&capture->enabled, false);
    unregister_kprobe(&udp_probe);
    WRITE_ONCE(udp_active, NULL);
    nbd_info("stage=udp_stop pid=%d generation=%llu captured=%llu overflow=%llu truncated=%llu missed=%lu\n",
        capture->pid, capture->generation, capture->captured,
        capture->overflow, capture->truncated, udp_probe.nmissed);
    udp_capture_free(capture);
    wake_up_interruptible_poll(&udp_wait, EPOLLHUP);
}

static struct nbdriver_udp_capture *udp_capture_alloc(
    const struct nbdriver_udp_start_request *request)
{
    struct nbdriver_udp_capture *capture;
    struct mm_struct *mm;
    struct pid *pid;
    const struct cred *cred;
    unsigned int i, cpu, used;
    long ret = -ENOMEM;

    capture = kzalloc(sizeof(*capture), GFP_KERNEL);
    if (!capture)
        return ERR_PTR(-ENOMEM);
    spin_lock_init(&capture->ring_lock);
    pid = find_get_pid(request->pid);
    if (!pid) { ret = -ESRCH; goto fail; }
    capture->target = get_pid_task(pid, PIDTYPE_TGID);
    put_pid(pid);
    if (!capture->target) { ret = -ESRCH; goto fail; }
    capture->pid = request->pid;
    capture->start_ns = READ_ONCE(capture->target->start_boottime);
    if (request->expected_start_ns && request->expected_start_ns != capture->start_ns) {
        ret = -ESTALE; goto fail;
    }
    mm = nbdriver_access_mm(capture->target);
    if (IS_ERR_OR_NULL(mm)) {
        ret = mm ? PTR_ERR(mm) : -ESRCH; goto fail;
    }
    mmgrab(mm);
    capture->mm = mm;
    mmput(mm);
    cred = get_task_cred(capture->target);
    capture->uid = cred->uid;
    put_cred(cred);
    capture->min_payload = request->min_payload ? request->min_payload : 16;
    capture->max_payload = request->max_payload ? request->max_payload : NBDRIVER_UDP_MAX_PAYLOAD;
    capture->pool = kvcalloc(NBDRIVER_UDP_RING_SIZE + NBDRIVER_UDP_READ_MAX +
                             num_possible_cpus(), sizeof(*capture->pool), GFP_KERNEL);
    capture->stage = kvcalloc(NBDRIVER_UDP_READ_MAX, sizeof(*capture->stage), GFP_KERNEL);
    capture->scratch = alloc_percpu(struct nbdriver_udp_packet *);
    ret = -ENOMEM;
    if (!capture->pool || !capture->stage || !capture->scratch)
        goto fail;
    used = 0;
    for (i = 0; i < NBDRIVER_UDP_RING_SIZE; ++i)
        capture->ring[i] = &capture->pool[used++];
    for (i = 0; i < NBDRIVER_UDP_READ_MAX; ++i)
        capture->read_slots[i] = &capture->pool[used++];
    for_each_possible_cpu(cpu)
        *per_cpu_ptr(capture->scratch, cpu) = &capture->pool[used++];
    if (!udp_target_live(capture)) { ret = -ESRCH; goto fail; }
    return capture;
fail:
    udp_capture_free(capture);
    return ERR_PTR(ret);
}

static long udp_start(struct file *owner, unsigned long argument)
{
    struct nbdriver_udp_start_request request;
    struct nbdriver_udp_capture *capture;
    u32 minimum, maximum;
    long ret;

    if (!argument || copy_from_user(&request, (void __user *)argument, sizeof(request)))
        return -EFAULT;
    minimum = request.min_payload ? request.min_payload : 16;
    maximum = request.max_payload ? request.max_payload : NBDRIVER_UDP_MAX_PAYLOAD;
    if (request.pid <= 0 || request.reserved || request.generation ||
        maximum > NBDRIVER_UDP_MAX_PAYLOAD || minimum > maximum)
        return -EINVAL;
    mutex_lock(&udp_control_lock);
    if (!udp_available) { ret = -EOPNOTSUPP; goto unlock; }
    if (nbdriver_runtime_quiesced()) { ret = -ESHUTDOWN; goto unlock; }
    if (udp_active) { ret = -EBUSY; goto unlock; }
    capture = udp_capture_alloc(&request);
    if (IS_ERR(capture)) { ret = PTR_ERR(capture); goto unlock; }
    capture->owner = owner;
    if (!++udp_generation)
        ++udp_generation;
    capture->generation = udp_generation;
    memset(&udp_probe, 0, sizeof(udp_probe));
    udp_probe.symbol_name = "udp_queue_rcv_one_skb";
    udp_probe.pre_handler = udp_receive_pre;
    WRITE_ONCE(udp_active, capture);
    ret = register_kprobe(&udp_probe);
    if (ret) {
        WRITE_ONCE(udp_active, NULL);
        udp_capture_free(capture);
        nbd_warn("stage=udp_start reason=register_probe error=%ld\n", ret);
        goto unlock;
    }
    smp_store_release(&capture->enabled, true);
    request.expected_start_ns = capture->start_ns;
    request.generation = capture->generation;
    request.min_payload = minimum;
    request.max_payload = maximum;
    if (copy_to_user((void __user *)argument, &request, sizeof(request))) {
        udp_stop_locked(); ret = -EFAULT; goto unlock;
    }
    nbd_info("stage=udp_start pid=%d uid=%u generation=%llu min=%u max=%u ring=%u\n",
        capture->pid, __kuid_val(capture->uid), capture->generation,
        minimum, maximum, NBDRIVER_UDP_RING_SIZE);
unlock:
    mutex_unlock(&udp_control_lock);
    return ret;
}

static void udp_status_locked(struct nbdriver_udp_status *status)
{
    struct nbdriver_udp_capture *capture = udp_active;
    unsigned long flags;

    memset(status, 0, sizeof(*status));
    status->size = sizeof(*status);
    status->sample_ns = ktime_get_ns();
    if (!capture)
        return;
    status->pid = capture->pid;
    status->uid = __kuid_val(capture->uid);
    status->start_ns = capture->start_ns;
    status->generation = capture->generation;
    status->min_payload = capture->min_payload;
    status->max_payload = capture->max_payload;
    status->active = capture->enabled && udp_target_live(capture);
    status->error = status->active ? 0 : -ESRCH;
    spin_lock_irqsave(&capture->ring_lock, flags);
    status->captured = capture->captured;
    status->overflow = capture->overflow;
    status->truncated = capture->truncated;
    status->malformed = capture->malformed;
    status->queued = capture->count + capture->pending;
    spin_unlock_irqrestore(&capture->ring_lock, flags);
    status->probe_missed = READ_ONCE(udp_probe.nmissed);
}

static long udp_read(struct file *owner, unsigned long argument)
{
    struct nbdriver_udp_read_request request;
    struct nbdriver_udp_capture *capture;
    unsigned long flags;
    u32 i, count = 0;
    long ret = 0;

    if (!argument || copy_from_user(&request, (void __user *)argument, sizeof(request)))
        return -EFAULT;
    if (!request.packets || !request.generation || !request.capacity ||
        request.capacity > NBDRIVER_UDP_READ_MAX)
        return -EINVAL;
    request.count = 0;
    mutex_lock(&udp_control_lock);
    capture = udp_active;
    if (!capture) { ret = -ENODATA; goto reply; }
    if (capture->owner != owner) { ret = -EPERM; goto unlock; }
    if (request.generation != capture->generation) { ret = -ESTALE; goto reply; }
    if (!udp_target_live(capture)) { ret = -ESRCH; goto reply; }
    if (!capture->pending) {
        spin_lock_irqsave(&capture->ring_lock, flags);
        while (count < request.capacity && capture->count) {
            struct nbdriver_udp_packet *packet = capture->ring[capture->tail];
            capture->ring[capture->tail] = capture->read_slots[count];
            capture->read_slots[count++] = packet;
            capture->tail = (capture->tail + 1) % NBDRIVER_UDP_RING_SIZE;
            --capture->count;
        }
        spin_unlock_irqrestore(&capture->ring_lock, flags);
        /* Usercopy and clearing unused payload bytes happen outside spinlock. */
        for (i = 0; i < count; ++i) {
            size_t used = offsetof(struct nbdriver_udp_packet, payload) +
                capture->read_slots[i]->payload_len;
            memcpy(&capture->stage[i], capture->read_slots[i], used);
            memset((char *)&capture->stage[i] + used, 0, sizeof(*capture->stage) - used);
        }
        capture->pending = count;
    }
    count = min_t(u32, capture->pending, request.capacity);
    if (count && copy_to_user((void __user *)(unsigned long)request.packets,
                              capture->stage, count * sizeof(*capture->stage))) {
        ret = -EFAULT; goto reply;
    }
    request.count = count;
reply:
    udp_status_locked(&request.status);
    request.status.error = ret;
    if (!ret)
        request.status.queued -= count;
    if (copy_to_user((void __user *)argument, &request, sizeof(request))) {
        ret = -EFAULT; goto unlock;
    }
    /* Keep detached bytes for retry after either data or reply copy failure.
     * A partially copied failed read may be seen again: sequence deduplicates. */
    if (!ret && capture && count) {
        capture->pending -= count;
        memmove(capture->stage, capture->stage + count,
                capture->pending * sizeof(*capture->stage));
    }
unlock:
    mutex_unlock(&udp_control_lock);
    return ret;
}

long nbdriver_udp_ioctl(struct file *owner, unsigned int command,
                       unsigned long argument)
{
    struct nbdriver_udp_status status;
    long ret = 0;

    if (!nbdriver_sf_read_caller() && !capable(CAP_SYS_PTRACE))
        return -EPERM;
    if (command == NBDRIVER_IOC_UDP_START)
        return udp_start(owner, argument);
    if (command == NBDRIVER_IOC_UDP_READ)
        return udp_read(owner, argument);
    mutex_lock(&udp_control_lock);
    if (udp_active && udp_active->owner != owner) { ret = -EPERM; goto unlock; }
    switch (command) {
    case NBDRIVER_IOC_UDP_STOP:
        udp_stop_locked();
        break;
    case NBDRIVER_IOC_UDP_STATUS:
        udp_status_locked(&status);
        if (!argument || copy_to_user((void __user *)argument, &status, sizeof(status)))
            ret = -EFAULT;
        break;
    default:
        ret = -ENOTTY;
        break;
    }
unlock:
    mutex_unlock(&udp_control_lock);
    return ret;
}

__poll_t nbdriver_udp_poll(struct file *owner, poll_table *wait)
{
    struct nbdriver_udp_capture *capture;
    unsigned long flags;
    __poll_t result = 0;

    poll_wait(owner, &udp_wait, wait);
    mutex_lock(&udp_control_lock);
    capture = udp_active;
    if (!capture) result = EPOLLHUP;
    else if (capture->owner != owner) result = EPOLLERR;
    else if (!udp_target_live(capture)) result = EPOLLHUP;
    else {
        spin_lock_irqsave(&capture->ring_lock, flags);
        if (capture->count || capture->pending)
            result = EPOLLIN | EPOLLRDNORM;
        spin_unlock_irqrestore(&capture->ring_lock, flags);
    }
    mutex_unlock(&udp_control_lock);
    return result;
}

void nbdriver_udp_release(struct file *owner)
{
    mutex_lock(&udp_control_lock);
    if (udp_active && udp_active->owner == owner)
        udp_stop_locked();
    mutex_unlock(&udp_control_lock);
}

void nbdriver_udp_quiesce(void)
{
    mutex_lock(&udp_control_lock);
    udp_stop_locked();
    mutex_unlock(&udp_control_lock);
}

void nbdriver_udp_init(void)
{
    struct kprobe probe = {
        .symbol_name = "udp_queue_rcv_one_skb",
        .pre_handler = udp_receive_pre,
    };
    int ret;

    BUILD_BUG_ON(sizeof(struct nbdriver_udp_packet) != 2096);
    BUILD_BUG_ON(sizeof(struct nbdriver_udp_start_request) != 32);
    BUILD_BUG_ON(sizeof(struct nbdriver_udp_status) != 96);
    BUILD_BUG_ON(sizeof(struct nbdriver_udp_read_request) != 120);
    ret = register_kprobe(&probe);
    if (!ret) {
        unregister_kprobe(&probe);
        udp_available = true;
    }
    nbd_info("stage=udp_init supported=%u error=%d probe=udp_queue_rcv_one_skb\n",
        udp_available, ret);
}

void nbdriver_udp_exit(void)
{
    nbdriver_udp_quiesce();
    udp_available = false;
}

u32 nbdriver_udp_features(void)
{
    return udp_available ? NBDRIVER_FEATURE_UDP_CAPTURE : 0;
}
