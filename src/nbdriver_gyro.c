// SPDX-License-Identifier: GPL-2.0-only
/* Gyro overlay adapted from NBG-driver's sensor event layout and Q32.32 math.
 * Unlike that sendto source-buffer patch, replace only a validated target's
 * outgoing AF_UNIX packet with a private kernel iterator. SensorService and
 * game memory are never written; Z, bias, timestamp and other sensors survive.
 * All allocation/channel discovery is in ioctl/init, never in a probe.
 */
#include <linux/atomic.h>
#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/jiffies.h>
#include <linux/kprobes.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/net.h>
#include <linux/rcupdate.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/uio.h>
#include <linux/version.h>
#include <linux/wait.h>
#include <net/af_unix.h>
#include <net/sock.h>
#include "nbdriver_abi.h"
#include "nbdriver_gyro.h"
#include "nbdriver_gyro_math.h"
#include "nbdriver_ksym.h"
#include "nbdriver_log.h"
#include "nbdriver_query.h"
#include "nbdriver_runtime.h"

_Static_assert(sizeof(struct nbdriver_gyro_request) == 80, "gyro ABI size");
_Static_assert(offsetof(struct nbdriver_gyro_request, expected_start_ns) == 24, "gyro identity offset");
_Static_assert(offsetof(struct nbdriver_gyro_request, modified_events) == 48, "gyro counter offset");

#define GYRO_CHANNELS 64U
#define GYRO_FDS 16384U
#define GYRO_BATCH 1000U
#define GYRO_ACTIVE 16U
#define GYRO_BYTES (GYRO_BATCH * NBDRIVER_GYRO_EVENT_SIZE)

struct gyro_channel { struct file *file; struct sock *rx; unsigned int fd; };
struct gyro_session {
    struct file *owner; /* borrowed until final release disables and drains */
    struct task_struct *target;
    struct mm_struct *mm; /* mm_count, not mm_users */
    unsigned long scanned;
    unsigned int count;
    struct gyro_channel channels[GYRO_CHANNELS];
};
struct gyro_rates { s64 x, y; u32 mask; unsigned long expires; bool enabled; };
struct gyro_buffer { struct kvec vector; unsigned char bytes[GYRO_BYTES]; };
struct gyro_call {
    struct msghdr *message;
    struct iov_iter original;
    struct gyro_buffer *buffer;
    unsigned int slot, changed;
    size_t length;
};
static DEFINE_MUTEX(gyro_control);
static DEFINE_SPINLOCK(gyro_lock);
static DECLARE_WAIT_QUEUE_HEAD(gyro_wait);
static struct gyro_session __rcu *gyro_session;
static struct gyro_rates gyro_rates;
static struct gyro_buffer *gyro_buffers;
static unsigned long gyro_busy;
static atomic_t gyro_inflight = ATOMIC_INIT(0);
static atomic64_t gyro_modified = ATOMIC64_INIT(0);
static atomic64_t gyro_copy_failures = ATOMIC64_INIT(0);
static struct kretprobe gyro_probe;
static bool gyro_available;
static const struct file_operations *gyro_socket_file_ops;

static struct socket *gyro_socket(struct file *file)
{
    /* Same ownership/type check as sock_from_file(), without importing an
     * API absent from some vendor export tables. Never treat arbitrary
     * private_data as a socket. The caller holds the file-table lock. */
    if (!file || !gyro_socket_file_ops || file->f_op != gyro_socket_file_ops)
        return NULL;
    return file->private_data;
}

/* Caller holds RCU; target lifetime and cached file objects are referenced.
 * Checking the current fd-table entry prevents a closed/replaced channel or
 * exec from being treated as the previous receiver. No PID/UID-only matching.
 */
static bool gyro_target_live(const struct gyro_session *session)
{
    return pid_alive(session->target) &&
        !(READ_ONCE(session->target->flags) & PF_EXITING) &&
        READ_ONCE(session->target->mm) == session->mm;
}

static bool gyro_receiver(struct gyro_session *session, struct sock *sender)
{
    struct files_struct *files;
    struct fdtable *table;
    struct sock *peer = READ_ONCE(unix_sk(sender)->peer);
    unsigned int i;
    bool found = false;
    if (!peer || !gyro_target_live(session)) return false;
    task_lock(session->target);
    files = READ_ONCE(session->target->mm) == session->mm ? session->target->files : NULL;
    if (files) {
        table = files_fdtable(files);
        for (i = 0; i < session->count; ++i) {
            const struct gyro_channel *channel = &session->channels[i];
            if (channel->rx == peer && channel->fd < table->max_fds &&
                rcu_dereference_raw(table->fd[channel->fd]) == channel->file) {
                found = true;
                break;
            }
        }
    }
    task_unlock(session->target);
    return found;
}

static int gyro_enter(struct kretprobe_instance *instance, struct pt_regs *regs)
{
    struct socket *socket = (void *)(unsigned long)regs->regs[0];
    struct msghdr *message = (void *)(unsigned long)regs->regs[1];
    size_t length = regs->regs[2], copied;
    struct gyro_call *call = (void *)instance->data;
    struct gyro_session *session;
    struct gyro_rates rates;
    struct iov_iter input;
    unsigned long flags;
    unsigned int slot, changed;
    if (!READ_ONCE(gyro_rates.enabled) ||
        !time_before(jiffies, READ_ONCE(gyro_rates.expires)) ||
        !socket || !socket->sk || !message ||
        !uid_eq(current_euid(), KUIDT_INIT(1000)) ||
        strncmp(current->group_leader->comm, "system_server", TASK_COMM_LEN) ||
        socket->type != SOCK_SEQPACKET || socket->sk->sk_family != AF_UNIX ||
        message->msg_namelen || message->msg_controllen ||
        !(message->msg_flags & MSG_DONTWAIT) || !length || length > GYRO_BYTES ||
        length % NBDRIVER_GYRO_EVENT_SIZE ||
        iov_iter_count(&message->msg_iter) != length) return 1;
    /* Only user-origin payloads; never reinterpret arbitrary kernel traffic. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
    if (!iter_is_iovec(&message->msg_iter) && !iter_is_ubuf(&message->msg_iter)) return 1;
#else
    if (!iter_is_iovec(&message->msg_iter)) return 1;
#endif
    rcu_read_lock();
    session = rcu_dereference(gyro_session);
    if (!session || !gyro_receiver(session, socket->sk)) goto skip;
    spin_lock_irqsave(&gyro_lock, flags);
    rates = gyro_rates;
    if (!rates.enabled || !time_before(jiffies, rates.expires)) {
        spin_unlock_irqrestore(&gyro_lock, flags);
        goto skip;
    }
    slot = find_first_zero_bit(&gyro_busy, GYRO_ACTIVE);
    if (slot == GYRO_ACTIVE) {
        spin_unlock_irqrestore(&gyro_lock, flags);
        atomic64_inc(&gyro_copy_failures);
        goto skip;
    }
    __set_bit(slot, &gyro_busy);
    spin_unlock_irqrestore(&gyro_lock, flags);
    input = message->msg_iter;
    pagefault_disable();
    copied = copy_from_iter(gyro_buffers[slot].bytes, length, &input);
    pagefault_enable();
    changed = copied == length ? nbdriver_gyro_overlay(gyro_buffers[slot].bytes,
        length, rates.mask, rates.x, rates.y) : 0;
    if (!changed) {
        if (copied != length) atomic64_inc(&gyro_copy_failures);
        spin_lock_irqsave(&gyro_lock, flags);
        __clear_bit(slot, &gyro_busy);
        spin_unlock_irqrestore(&gyro_lock, flags);
        goto skip;
    }
    call->message = message;
    call->original = message->msg_iter;
    call->buffer = &gyro_buffers[slot];
    call->slot = slot;
    call->length = length;
    call->changed = changed;
    call->buffer->vector.iov_base = call->buffer->bytes;
    call->buffer->vector.iov_len = length;
    /* Publish in-flight before leaving RCU, so final FD release can drain it. */
    atomic_inc(&gyro_inflight);
    iov_iter_kvec(&message->msg_iter, WRITE, &call->buffer->vector, 1, length);
    rcu_read_unlock();
    return 0;
skip:
    rcu_read_unlock();
    return 1;
}

static int gyro_return(struct kretprobe_instance *instance, struct pt_regs *regs)
{
    struct gyro_call *call = (void *)instance->data;
    const size_t consumed = call->length - iov_iter_count(&call->message->msg_iter);
    unsigned long flags;
    call->message->msg_iter = call->original;
    iov_iter_advance(&call->message->msg_iter, consumed);
    if ((long)regs_return_value(regs) == (long)call->length)
        atomic64_add(call->changed, &gyro_modified);
    spin_lock_irqsave(&gyro_lock, flags);
    __clear_bit(call->slot, &gyro_busy);
    spin_unlock_irqrestore(&gyro_lock, flags);
    if (atomic_dec_and_test(&gyro_inflight)) wake_up_all(&gyro_wait);
    return 0;
}

static void gyro_free_session(struct gyro_session *session)
{
    unsigned int i;
    if (!session) return;
    for (i = 0; i < session->count; ++i) fput(session->channels[i].file);
    if (session->mm) mmdrop(session->mm);
    if (session->target) put_task_struct(session->target);
    kfree(session);
}

static struct gyro_session *gyro_collect(struct file *owner,
    const struct nbdriver_gyro_request *request)
{
    struct gyro_session *session;
    struct task_struct *task;
    struct files_struct *files;
    struct fdtable *table;
    unsigned int fd;
    int error = 0;
    task = nbdriver_query_get_process(request->pid, request->expected_start_ns);
    if (IS_ERR(task)) return ERR_CAST(task);
    session = kzalloc(sizeof(*session), GFP_KERNEL);
    if (!session) { nbdriver_query_put_process(task); return ERR_PTR(-ENOMEM); }
    get_task_struct(task);
    session->target = task;
    session->owner = owner;
    task_lock(task);
    session->mm = task->mm;
    if (session->mm) mmgrab(session->mm);
    files = task->files;
    if (!files || !session->mm) error = -ESRCH;
    else {
        spin_lock(&files->file_lock);
        table = files_fdtable(files);
        if (table->max_fds > GYRO_FDS) error = -E2BIG;
        else for_each_set_bit(fd, table->open_fds, table->max_fds) {
            struct file *file = rcu_dereference_raw(table->fd[fd]);
            struct socket *socket = file ? gyro_socket(file) : NULL;
            if (!socket || !socket->sk || socket->type != SOCK_SEQPACKET ||
                socket->sk->sk_family != AF_UNIX) continue;
            if (session->count == GYRO_CHANNELS) { error = -E2BIG; break; }
            get_file(file);
            session->channels[session->count++] =
                (struct gyro_channel){file, socket->sk, fd};
        }
        spin_unlock(&files->file_lock);
    }
    task_unlock(task);
    nbdriver_query_put_process(task);
    if (!error && !session->count) error = -ENODATA;
    if (error) { gyro_free_session(session); return ERR_PTR(error); }
    session->scanned = jiffies;
    return session;
}

/* gyro_control held. Detach before synchronize_rcu so no new entry can borrow
 * the old session. Private send buffers live independently until module exit.
 */
static void gyro_stop_locked(void)
{
    struct gyro_session *old;
    unsigned long flags;
    spin_lock_irqsave(&gyro_lock, flags);
    memset(&gyro_rates, 0, sizeof(gyro_rates));
    spin_unlock_irqrestore(&gyro_lock, flags);
    old = rcu_dereference_protected(gyro_session, lockdep_is_held(&gyro_control));
    RCU_INIT_POINTER(gyro_session, NULL);
    if (old) { synchronize_rcu(); gyro_free_session(old); }
}

long nbdriver_gyro_ioctl(struct file *file, unsigned long argument)
{
    struct nbdriver_gyro_request request;
    struct gyro_session *session, *next;
    struct gyro_rates rates = {};
    unsigned long flags, now;
    long ret = 0;
    if (!argument || copy_from_user(&request, (void __user *)argument, sizeof(request))) return -EFAULT;
    if (request.version != NBDRIVER_GYRO_ABI_VERSION || request.size != sizeof(request)) return -EPROTO;
    if (request.reserved || request.operation > NBDRIVER_GYRO_STOP) return -EINVAL;
    if (request.operation == NBDRIVER_GYRO_SET) {
        if (request.pid <= 0 || !request.expected_start_ns || !request.type_mask ||
            request.type_mask & ~3U || !request.lease_ms ||
            request.lease_ms > NBDRIVER_GYRO_MAX_LEASE_MS) return -EINVAL;
        if (nbdriver_gyro_bits_to_q32(request.delta_x_bits, &rates.x) ||
            nbdriver_gyro_bits_to_q32(request.delta_y_bits, &rates.y) ||
            rates.x > (1LL<<32) || rates.x < -(1LL<<32) ||
            rates.y > (1LL<<32) || rates.y < -(1LL<<32)) return -ERANGE;
    }
    mutex_lock(&gyro_control);
    session = rcu_dereference_protected(gyro_session, lockdep_is_held(&gyro_control));
    if (request.operation != NBDRIVER_GYRO_STATUS && session && session->owner != file &&
        READ_ONCE(gyro_rates.enabled) && time_before(jiffies, READ_ONCE(gyro_rates.expires))) ret = -EBUSY;
    else if (request.operation == NBDRIVER_GYRO_STOP) gyro_stop_locked();
    else if (request.operation == NBDRIVER_GYRO_SET) {
        if (!gyro_available) ret = -EOPNOTSUPP;
        else if (nbdriver_runtime_quiesced()) ret = -ESHUTDOWN;
        else {
            const bool reuse = session && gyro_target_live(session) && session->owner == file &&
                task_tgid_nr(session->target) == request.pid &&
                READ_ONCE(session->target->start_boottime) == request.expected_start_ns &&
                READ_ONCE(session->target->mm) == session->mm &&
                time_before(jiffies, session->scanned + msecs_to_jiffies(500));
            if (!reuse) {
                unsigned int i;
                bool same;
                next = gyro_collect(file, &request);
                same = !IS_ERR(next) && session && next->owner == session->owner &&
                    next->target == session->target && next->mm == session->mm &&
                    next->count == session->count;
                if (same) for (i = 0; i < next->count; ++i) {
                    const struct gyro_channel *a = &next->channels[i];
                    const struct gyro_channel *b = &session->channels[i];
                    if (a->file != b->file || a->rx != b->rx || a->fd != b->fd) {
                        same = false;
                        break;
                    }
                }
                if (same) {
                    /* Freshly revalidated, identical channels need no RCU
                     * handoff or zero-rate gap. Compare consumed fields only. */
                    session->scanned = next->scanned;
                    gyro_free_session(next);
                } else {
                    gyro_stop_locked();
                    if (IS_ERR(next)) ret = PTR_ERR(next);
                    else rcu_assign_pointer(gyro_session, next);
                }
            }
            if (!ret) {
                rates.enabled = true;
                rates.mask = request.type_mask;
                rates.expires = jiffies + msecs_to_jiffies(request.lease_ms);
                spin_lock_irqsave(&gyro_lock, flags);
                gyro_rates = rates;
                spin_unlock_irqrestore(&gyro_lock, flags);
            }
        }
    }
    session = rcu_dereference_protected(gyro_session, lockdep_is_held(&gyro_control));
    now = jiffies;
    request.flags = gyro_available ? NBDRIVER_GYRO_AVAILABLE : 0;
    if (session && gyro_target_live(session) && READ_ONCE(gyro_rates.enabled) &&
        time_before(now, READ_ONCE(gyro_rates.expires))) request.flags |= NBDRIVER_GYRO_ACTIVE;
    spin_lock_irqsave(&gyro_lock, flags);
    rates = gyro_rates;
    spin_unlock_irqrestore(&gyro_lock, flags);
    request.pid = session ? task_tgid_nr(session->target) : 0;
    request.expected_start_ns = session ? READ_ONCE(session->target->start_boottime) : 0;
    request.type_mask = rates.mask;
    request.delta_x_bits = nbdriver_gyro_q32_to_bits(rates.x);
    request.delta_y_bits = nbdriver_gyro_q32_to_bits(rates.y);
    request.lease_ms = rates.enabled && time_before(now, rates.expires)
        ? jiffies_to_msecs(rates.expires - now) : 0;
    request.channels = session ? session->count : 0;
    request.modified_events = atomic64_read(&gyro_modified);
    request.copy_failures = atomic64_read(&gyro_copy_failures);
    request.missed = READ_ONCE(gyro_probe.nmissed);
    request.error = ret;
    mutex_unlock(&gyro_control);
    if (copy_to_user((void __user *)argument, &request, sizeof(request))) return -EFAULT;
    return ret;
}

void nbdriver_gyro_release(struct file *file)
{
    struct gyro_session *session;
    mutex_lock(&gyro_control);
    session = rcu_dereference_protected(gyro_session, lockdep_is_held(&gyro_control));
    if (session && session->owner == file) {
        gyro_stop_locked();
        /* fops.owner still pins the module while we drain private iterators. */
        wait_event(gyro_wait, !atomic_read(&gyro_inflight));
    }
    mutex_unlock(&gyro_control);
}
void nbdriver_gyro_quiesce(void)
{
    mutex_lock(&gyro_control);
    gyro_stop_locked();
    wait_event(gyro_wait, !atomic_read(&gyro_inflight));
    mutex_unlock(&gyro_control);
}
void nbdriver_gyro_init(void)
{
    int ret;
    gyro_socket_file_ops = (const struct file_operations *)
        nbdriver_ksym_lookup_optional("socket_file_ops");
    if (!gyro_socket_file_ops) {
        nbd_warn("stage=gyro status=unavailable reason=socket_file_ops_missing\n");
        return;
    }
    gyro_buffers = kvcalloc(GYRO_ACTIVE, sizeof(*gyro_buffers), GFP_KERNEL);
    if (!gyro_buffers) { nbd_warn("stage=gyro status=unavailable error=%d\n", -ENOMEM); return; }
    gyro_probe = (struct kretprobe){
        .kp.symbol_name = "unix_dgram_sendmsg", .entry_handler = gyro_enter,
        .handler = gyro_return, .maxactive = GYRO_ACTIVE, .data_size = sizeof(struct gyro_call),
    };
    ret = register_kretprobe(&gyro_probe);
    if (ret) {
        kvfree(gyro_buffers); gyro_buffers = NULL;
        nbd_warn("stage=gyro status=unavailable error=%d\n", ret);
        return;
    }
    gyro_available = true;
    nbd_info("stage=gyro status=ready lease_max_ms=%u\n", NBDRIVER_GYRO_MAX_LEASE_MS);
}
void nbdriver_gyro_exit(void)
{
    nbdriver_gyro_quiesce();
    if (gyro_available) { unregister_kretprobe(&gyro_probe); gyro_available = false; }
    kvfree(gyro_buffers); gyro_buffers = NULL;
    gyro_socket_file_ops = NULL;
}
