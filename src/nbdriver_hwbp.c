// SPDX-License-Identifier: GPL-2.0-only
/* Raw ARM64 BP/WP adapted from the old NBG-driver's hwbp_core/watch:
 * BVR/BCR2..5, WVR/WCR0..3, debug_fault_info dispatch, finish_task_switch
 * return probe and CPU PM/hotplug restore. No perf API or event allocation.
 *
 * The public capture interface is one-shot. Disarm before ERET and keep PC
 * unchanged; blindly copying the old business callback's PC+=4 would discard
 * the original instruction. Explicit REARM requests the next observation.
 * No delayed rearm window is presented as an every-hit instruction trace.
 * Slots are exclusive raw resources; detect a conflicting physical owner
 * rather than overwrite its enabled configuration. No debug visibility hooks.
 */
#include <linux/atomic.h>
#include <linux/capability.h>
#include <linux/cpu_pm.h>
#include <linux/cpuhotplug.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/kprobes.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/percpu.h>
#include <linux/pid.h>
#include <linux/rcupdate.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <asm/esr.h>
#include <asm/compat.h>
#include <asm/ptrace.h>
#include "nbdriver_hwbp.h"
#include "nbdriver_hwbp_regs.h"
#include "nbdriver_hwbp_rules.h"
#include "nbdriver_ksym.h"
#include "nbdriver_log.h"
#include "nbdriver_query.h"
#include "nbdriver_runtime.h"

static_assert(sizeof(struct nbdriver_hwbp_request) == 128);
static_assert(sizeof(struct nbdriver_hwbp_record) == 320);

struct hwb_fault_info {
    int (*fn)(unsigned long, unsigned long, struct pt_regs *);
    int sig, code;
    const char *name;
};
struct hwb_point {
    struct file *owner;
    struct task_struct *task;
    struct mm_struct *mm; /* mm_count only; never prevent process teardown */
    u64 handle, address, base, control, sequence;
    u64 start_ns, thread_start_ns;
    s32 pid, tid;
    u32 type, length;
    bool group, armed, retiring, context;
    int error;
    struct nbdriver_hwbp_record records[NBDRIVER_HWBP_RECORDS];
};
struct hwb_cpu {
    u32 mask;
    u64 address[8], control[8];
};
static DEFINE_PER_CPU(struct hwb_cpu, hwb_cpu);
static DEFINE_MUTEX(hwb_control);
static DEFINE_RAW_SPINLOCK(hwb_lock);
static struct hwb_point *hwb_points[8];
static atomic_t hwb_armed = ATOMIC_INIT(0);
static atomic64_t hwb_next_handle = ATOMIC64_INIT(0);
static unsigned int hwb_brps, hwb_wrps, hwb_hw_wrps;
static struct hwb_fault_info *hwb_faults;
static int hwb_bp_index = -1, hwb_wp_index = -1;
static int (*hwb_original_bp)(unsigned long, unsigned long, struct pt_regs *);
static int (*hwb_original_wp)(unsigned long, unsigned long, struct pt_regs *);
static bool hwb_available, hwb_pm_registered;
static int hwb_init_error = -EOPNOTSUPP;
static int hwb_cpuhp = CPUHP_INVALID;
static struct kretprobe hwb_switch_probe;

static bool hwb_live(const struct hwb_point *point)
{
    return pid_alive(point->task) &&
        !(READ_ONCE(point->task->flags) & PF_EXITING) &&
        READ_ONCE(point->task->mm) == point->mm;
}

static bool hwb_matches(const struct hwb_point *point)
{
    return READ_ONCE(current->mm) == point->mm && hwb_live(point) &&
        (point->group ? current->group_leader == point->task : current == point->task);
}

static unsigned int hwb_physical(unsigned int slot)
{
    return slot < 4 ? NBD_HWB_BP_BASE + slot : slot - 4;
}

static void hwb_write(unsigned int slot, u64 address, u64 control)
{
    if (slot < 4) nbd_hwb_write_bp(hwb_physical(slot), address, control);
    else nbd_hwb_write_wp(hwb_physical(slot), address, control);
}

static bool hwb_owned(unsigned int slot, u64 address, u64 control)
{
    struct hwb_cpu *cache = this_cpu_ptr(&hwb_cpu);
    return (cache->mask & (1U << slot)) && address == cache->address[slot] &&
        control == cache->control[slot];
}

/* IRQs disabled and hwb_lock held. Clear only the register value we own. */
static void hwb_clear_local(unsigned int slot)
{
    struct hwb_cpu *cache = this_cpu_ptr(&hwb_cpu);
    u64 address, control;
    if (!(cache->mask & (1U << slot))) return;
    nbd_hwb_read(slot < 4, hwb_physical(slot), &address, &control);
    if (address == cache->address[slot] && control == cache->control[slot])
        hwb_write(slot, 0, 0);
    cache->mask &= ~(1U << slot);
    cache->address[slot] = cache->control[slot] = 0;
}

static void hwb_apply_cpu(void *unused)
{
    struct hwb_cpu *cache = this_cpu_ptr(&hwb_cpu);
    unsigned long irqflags;
    unsigned int slot, local_brps, local_wrps;
    (void)unused;
    if (!atomic_read(&hwb_armed) && !cache->mask) return;
    nbd_hwb_counts(&local_brps, &local_wrps);
    raw_spin_lock_irqsave(&hwb_lock, irqflags);
    for (slot = 0; slot < ARRAY_SIZE(hwb_points); ++slot) {
        struct hwb_point *point = hwb_points[slot];
        u64 address, control;
        if (!point || point->retiring || !point->armed || !hwb_matches(point)) {
            hwb_clear_local(slot);
            continue;
        }
        /* Heterogeneous CPUs need not implement the boot CPU's slot count. */
        if (hwb_physical(slot) >= (slot < 4 ? local_brps : local_wrps)) {
            point->error = -ENOSPC;
            continue;
        }
        nbd_hwb_read(slot < 4, hwb_physical(slot), &address, &control);
        if ((control & 1) && !hwb_owned(slot, address, control)) {
            point->error = -EBUSY;
            cache->mask &= ~(1U << slot);
            continue;
        }
        nbd_hwb_enable();
        /* Always rewrite on a relevant switch: software cache cannot prove
         * that the hardware still contains the last value we wrote. */
        hwb_write(slot, point->base, point->control);
        cache->address[slot] = point->base;
        cache->control[slot] = point->control;
        cache->mask |= 1U << slot;
    }
    raw_spin_unlock_irqrestore(&hwb_lock, irqflags);
}

static void hwb_disarm(struct hwb_point *point, unsigned int slot,
    unsigned long fault, struct pt_regs *regs, bool imprecise)
{
    struct nbdriver_hwbp_record *record;
    hwb_clear_local(slot);
    if (point->retiring || !point->armed) return;
    point->armed = false;
    atomic_dec(&hwb_armed);
    record = &point->records[point->sequence % NBDRIVER_HWBP_RECORDS];
    memset(record, 0, sizeof(*record));
    record->sequence = ++point->sequence;
    record->timestamp_ns = ktime_get_ns();
    record->handle = point->handle;
    record->address = fault;
    record->pid = task_tgid_vnr(current);
    record->tid = task_pid_vnr(current);
    record->type = point->type;
    record->flags = (imprecise ? NBDRIVER_HWBP_RECORD_IMPRECISE : 0) |
        (!user_mode(regs) ? NBDRIVER_HWBP_RECORD_KERNEL_ACCESS : 0);
    record->pc = regs->pc;
    record->sp = regs->sp;
    record->pstate = regs->pstate;
    memcpy(record->x, regs->regs, sizeof(record->x));
}

/* Keep retiring entries until all CPU slots and in-flight handlers drain. */
static int hwb_breakpoint(unsigned long address, unsigned long esr, struct pt_regs *regs)
{
    unsigned long irqflags;
    unsigned int slot;
    bool handled = false;
    int result;
    rcu_read_lock();
    if (!user_mode(regs)) goto original;
    raw_spin_lock_irqsave(&hwb_lock, irqflags);
    for (slot = 0; slot < hwb_brps; ++slot) {
        struct hwb_point *point = hwb_points[slot];
        u64 installed, control;
        if (!point || point->address != regs->pc) continue;
        if (!(this_cpu_ptr(&hwb_cpu)->mask & (1U << slot))) continue;
        nbd_hwb_read(true, hwb_physical(slot), &installed, &control);
        if (installed != point->base || control != point->control) continue;
        if (!hwb_owned(slot, installed, control)) continue;
        if (hwb_matches(point)) {
            const bool capture = point->armed;
            hwb_disarm(point, slot, regs->pc, regs, false);
            if (capture && point->context && !point->retiring &&
                !nbdriver_context_queue(point->owner, point->handle, regs)) point->error = -EAGAIN;
        }
        else hwb_clear_local(slot); /* stale register after exec: do not loop */
        handled = true;
    }
    raw_spin_unlock_irqrestore(&hwb_lock, irqflags);
    if (handled) { rcu_read_unlock(); return 0; } /* PC unchanged */
original:
    result = hwb_original_bp(address, esr, regs);
    rcu_read_unlock();
    return result;
}

static int hwb_watchpoint(unsigned long fault, unsigned long esr, struct pt_regs *regs)
{
    unsigned long irqflags;
    unsigned int slot, closest = 8, local_brps, local_wrps;
    u64 distance = U64_MAX, foreign = U64_MAX;
    u32 access = (esr & ESR_ELx_WNR) ? NBDRIVER_HWBP_W : NBDRIVER_HWBP_R;
    bool handled = false;
    int result;
    rcu_read_lock();
    fault = untagged_addr(fault);
    nbd_hwb_counts(&local_brps, &local_wrps);
    raw_spin_lock_irqsave(&hwb_lock, irqflags);
    for (slot = 0; slot < min(hwb_hw_wrps, local_wrps); ++slot) {
        struct hwb_point *point = slot < hwb_wrps ? hwb_points[slot + 4] : NULL;
        u64 installed, control, dist;
        u32 bytes;
        nbd_hwb_read(false, slot, &installed, &control);
        if (!(control & 1) || !(((control >> 3) & 3) & access)) continue;
        bytes = (control >> 5) & 0xff;
        if (!bytes) continue;
        if (point && hwb_owned(slot + 4, installed, control) &&
            installed == point->base && control == point->control && !hwb_matches(point)) {
            hwb_clear_local(slot + 4);
            handled = true;
            continue;
        }
        if (!point || !hwb_owned(slot + 4, installed, control) ||
            installed != point->base || control != point->control) {
            dist = nbd_hwb_distance(fault, installed + __ffs(bytes),
                __fls(bytes) - __ffs(bytes) + 1);
            foreign = min(foreign, dist);
            continue;
        }
        dist = nbd_hwb_distance(fault, point->address, point->length);
        if (dist < distance) { closest = slot + 4; distance = dist; }
        if (!dist) {
            hwb_disarm(point, slot + 4, fault, regs, false);
            handled = true;
        }
    }
    /* ARM64 FAR can be near rather than inside watched bytes. Do not steal
     * an imprecise exception when a foreign physical slot is equally close. */
    if (!handled && closest < 8 && distance < foreign) {
        hwb_disarm(hwb_points[closest], closest, fault, regs, true);
        handled = true;
    }
    raw_spin_unlock_irqrestore(&hwb_lock, irqflags);
    if (handled) { rcu_read_unlock(); return 0; } /* preserve kernel uaccess */
    result = hwb_original_wp(fault, esr, regs);
    rcu_read_unlock();
    return result;
}

static int hwb_switch_return(struct kretprobe_instance *instance, struct pt_regs *regs)
{
    (void)instance; (void)regs;
    hwb_apply_cpu(NULL);
    return 0;
}

static int hwb_cpu_pm(struct notifier_block *block, unsigned long event, void *data)
{
    (void)block; (void)data;
    if (event == CPU_PM_EXIT || event == CPU_CLUSTER_PM_EXIT) hwb_apply_cpu(NULL);
    return NOTIFY_OK;
}
static struct notifier_block hwb_pm = { .notifier_call = hwb_cpu_pm };
static int hwb_cpu_online(unsigned int cpu)
{
    (void)cpu; hwb_apply_cpu(NULL); return 0;
}
static int hwb_cpu_offline(unsigned int cpu)
{
    unsigned long flags;
    unsigned int slot;
    (void)cpu;
    raw_spin_lock_irqsave(&hwb_lock, flags);
    for (slot = 0; slot < 8; ++slot) hwb_clear_local(slot);
    raw_spin_unlock_irqrestore(&hwb_lock, flags);
    return 0;
}

static void hwb_free_point(struct hwb_point *point)
{
    mmdrop(point->mm);
    put_task_struct(point->task);
    kvfree(point);
}

/* Control mutex held; no usercopy or sleeping while holding hwb_lock. */
static void hwb_remove_points(struct file *owner, u64 handle)
{
    struct hwb_point *removed[8] = {};
    unsigned long flags;
    unsigned int slot;
    bool any = false;
    raw_spin_lock_irqsave(&hwb_lock, flags);
    for (slot = 0; slot < 8; ++slot) {
        struct hwb_point *point = hwb_points[slot];
        if (!point || (owner && point->owner != owner) || (handle && point->handle != handle)) continue;
        point->retiring = true;
        if (point->armed) { point->armed = false; atomic_dec(&hwb_armed); }
        removed[slot] = point;
        any = true;
    }
    raw_spin_unlock_irqrestore(&hwb_lock, flags);
    if (!any) return;
    nbdriver_context_cancel(owner, handle);
    /* Synchronous IPIs run after each CPU exits its debug exception. */
    on_each_cpu(hwb_apply_cpu, NULL, 1);
    synchronize_rcu();
    raw_spin_lock_irqsave(&hwb_lock, flags);
    for (slot = 0; slot < 8; ++slot) if (removed[slot]) hwb_points[slot] = NULL;
    raw_spin_unlock_irqrestore(&hwb_lock, flags);
    for (slot = 0; slot < 8; ++slot) if (removed[slot]) hwb_free_point(removed[slot]);
}

static int hwb_add(struct file *owner, struct nbdriver_hwbp_request *request)
{
    struct task_struct *leader, *task = NULL;
    struct mm_struct *mm;
    struct hwb_point *point;
    struct pid *id;
    unsigned long flags;
    u64 base, control;
    unsigned int first, limit, slot, i;
    int error = -EINVAL;
    if ((request->flags & ~NBDRIVER_HWBP_CONTEXT) || request->pid <= 0 || request->tid < 0 || !request->expected_start_ns ||
        !nbd_hwb_shape(request->address, request->type, request->length, &base, &control)) return -EINVAL;
    if ((request->flags & NBDRIVER_HWBP_CONTEXT) &&
        (request->type != NBDRIVER_HWBP_X || !nbdriver_context_ready())) return -EOPNOTSUPP;
    leader = nbdriver_query_get_process(request->pid, request->expected_start_ns);
    if (IS_ERR(leader)) return PTR_ERR(leader);
    if (request->tid && request->tid != request->pid) {
        if (!request->expected_thread_start_ns) goto put_leader;
        id = find_get_pid(request->tid);
        if (!id) { error = -ESRCH; goto put_leader; }
        task = get_pid_task(id, PIDTYPE_PID);
        put_pid(id);
        if (!task || task->group_leader != leader) { error = -ESRCH; goto put_task; }
        if (READ_ONCE(task->start_boottime) != request->expected_thread_start_ns) {
            error = -ESTALE; goto put_task;
        }
    } else { task = leader; get_task_struct(task); }
    mm = get_task_mm(task);
    if (!mm) { error = -ESRCH; goto put_task; }
    if (READ_ONCE(task->flags) & PF_EXITING) { error = -ESRCH; goto put_mm; }
    if (is_compat_thread(task_thread_info(task)) || request->address >= mm->task_size ||
        request->length > mm->task_size - request->address) goto put_mm;
    point = kvzalloc(sizeof(*point), GFP_KERNEL);
    if (!point) { error = -ENOMEM; goto put_mm; }
    point->owner = owner; point->task = task; point->mm = mm;
    point->pid = request->pid;
    point->tid = request->tid ? task_pid_vnr(task) : 0;
    point->start_ns = READ_ONCE(leader->start_boottime);
    point->thread_start_ns = READ_ONCE(task->start_boottime);
    point->address = request->address; point->base = base; point->control = control;
    point->type = request->type; point->length = request->length;
    point->group = !request->tid;
    point->context = !!(request->flags & NBDRIVER_HWBP_CONTEXT);
    point->handle = atomic64_inc_return(&hwb_next_handle);
    point->armed = true;
    first = request->type == NBDRIVER_HWBP_X ? 0 : 4;
    limit = first + (first ? hwb_wrps : hwb_brps);
    for (i = first; i < limit; ++i) {
        struct hwb_point *other = hwb_points[i];
        if (other && other->mm == mm && other->address == point->address &&
            (other->group || point->group || other->task == task)) {
            error = -EEXIST; goto free_point;
        }
    }
    for (slot = first; slot < limit; ++slot) if (!hwb_points[slot]) break;
    if (slot == limit) { error = -ENOSPC; goto free_point; }
    mmgrab(mm);
    raw_spin_lock_irqsave(&hwb_lock, flags);
    hwb_points[slot] = point;
    atomic_inc(&hwb_armed);
    raw_spin_unlock_irqrestore(&hwb_lock, flags);
    request->handle = point->handle;
    request->expected_thread_start_ns = READ_ONCE(task->start_boottime);
    /* Release exec_update_lock before IPIs; keep independent task/mm refs. */
    nbdriver_query_put_process(leader);
    mmput(mm);
    on_each_cpu(hwb_apply_cpu, NULL, 1);
    error = READ_ONCE(point->error);
    if (error) { hwb_remove_points(owner, point->handle); request->handle = 0; }
    return error;
free_point:
    kvfree(point);
put_mm:
    mmput(mm);
put_task:
    if (task) put_task_struct(task);
put_leader:
    nbdriver_query_put_process(leader);
    return error;
}

static struct hwb_point *hwb_find(struct file *owner, u64 handle)
{
    unsigned int i;
    for (i = 0; i < 8; ++i)
        if (hwb_points[i] && hwb_points[i]->owner == owner && hwb_points[i]->handle == handle)
            return hwb_points[i];
    return NULL;
}

static void hwb_status(struct nbdriver_hwbp_request *request, struct hwb_point *point)
{
    unsigned long flags;
    request->flags = hwb_available ? NBDRIVER_HWBP_AVAILABLE | NBDRIVER_HWBP_RAW_REGISTERS : 0;
    request->exec_slots = hwb_brps;
    request->watch_slots = hwb_wrps;
    request->switch_missed = READ_ONCE(hwb_switch_probe.nmissed);
    request->hits = request->dropped = 0;
    if (!point) return;
    raw_spin_lock_irqsave(&hwb_lock, flags);
    /* A retained dead thread need not retain its group_leader object. */
    request->pid = point->pid;
    request->tid = point->tid;
    request->expected_start_ns = point->start_ns;
    request->expected_thread_start_ns = point->thread_start_ns;
    request->address = point->address; request->type = point->type; request->length = point->length;
    request->hits = point->sequence;
    request->dropped = point->sequence > NBDRIVER_HWBP_RECORDS ? point->sequence - NBDRIVER_HWBP_RECORDS : 0;
    if (point->armed) request->flags |= NBDRIVER_HWBP_ARMED;
    if (point->context) request->flags |= NBDRIVER_HWBP_CONTEXT;
    if (!hwb_live(point)) request->flags |= NBDRIVER_HWBP_TARGET_GONE;
    if (point->error) request->error = point->error;
    raw_spin_unlock_irqrestore(&hwb_lock, flags);
}

void nbdriver_hwbp_context_rearm(struct file *owner, unsigned long long handle)
{
    struct hwb_point *point; unsigned long flags; bool apply = false;
    mutex_lock(&hwb_control);
    point = hwb_find(owner, handle);
    if (point && point->context && !nbdriver_runtime_quiesced()) {
        raw_spin_lock_irqsave(&hwb_lock, flags);
        if (!point->retiring && !point->armed && hwb_live(point)) {
            point->armed = true; point->error = 0; atomic_inc(&hwb_armed); apply = true;
        }
        raw_spin_unlock_irqrestore(&hwb_lock, flags);
    }
    if (apply) on_each_cpu(hwb_apply_cpu, NULL, 1);
    mutex_unlock(&hwb_control);
}

long nbdriver_hwbp_ioctl(struct file *owner, unsigned long argument)
{
    struct nbdriver_hwbp_request request;
    struct nbdriver_hwbp_record *records = NULL;
    struct hwb_point *point = NULL;
    unsigned long flags;
    u64 added = 0, sequence, oldest;
    int error = 0;
    if (!capable(CAP_SYS_PTRACE)) return -EPERM;
    if (!argument || copy_from_user(&request, (void __user *)argument, sizeof(request))) return -EFAULT;
    if (request.version != NBDRIVER_HWBP_ABI_VERSION || request.size != sizeof(request)) return -EPROTO;
    if (request.reserved || request.operation > NBDRIVER_HWBP_REARM) return -EINVAL;
    if (request.operation == NBDRIVER_HWBP_READ) {
        if (!request.handle || !request.records || !request.capacity || request.capacity > NBDRIVER_HWBP_RECORDS) return -EINVAL;
        records = kvmalloc_array(request.capacity, sizeof(*records), GFP_KERNEL);
        if (!records) return -ENOMEM;
    }
    request.count = 0; request.error = 0;
    mutex_lock(&hwb_control);
    if (!hwb_available) { error = hwb_init_error; goto status; }
    if (request.operation == NBDRIVER_HWBP_ADD || request.operation == NBDRIVER_HWBP_REARM) {
        if (nbdriver_runtime_quiesced()) { error = -ESHUTDOWN; goto status; }
    }
    if (request.operation == NBDRIVER_HWBP_ADD) {
        request.handle = 0;
        error = hwb_add(owner, &request);
        if (!error) { added = request.handle; point = hwb_find(owner, added); }
    } else if (request.operation == NBDRIVER_HWBP_CLEAR) {
        hwb_remove_points(owner, 0);
        request.handle = 0;
    } else if (request.handle) {
        point = hwb_find(owner, request.handle);
        if (!point) { error = -ENOENT; goto status; }
        if (request.operation == NBDRIVER_HWBP_REMOVE) {
            hwb_remove_points(owner, request.handle); point = NULL;
        } else if (request.operation == NBDRIVER_HWBP_REARM) {
            raw_spin_lock_irqsave(&hwb_lock, flags);
            if (!hwb_live(point)) error = -ESTALE;
            else if (point->armed) error = -EALREADY;
            else { point->error = 0; point->armed = true; atomic_inc(&hwb_armed); }
            raw_spin_unlock_irqrestore(&hwb_lock, flags);
            if (!error) on_each_cpu(hwb_apply_cpu, NULL, 1);
        } else if (request.operation == NBDRIVER_HWBP_READ) {
            raw_spin_lock_irqsave(&hwb_lock, flags);
            if (request.after_sequence > point->sequence) error = -EINVAL;
            else {
                oldest = point->sequence >= NBDRIVER_HWBP_RECORDS ? point->sequence - NBDRIVER_HWBP_RECORDS + 1 : 1;
                sequence = max(request.after_sequence + 1, oldest);
                for (; sequence <= point->sequence && request.count < request.capacity; ++sequence)
                    records[request.count++] = point->records[(sequence - 1) % NBDRIVER_HWBP_RECORDS];
                if (request.count) request.after_sequence = records[request.count - 1].sequence;
            }
            raw_spin_unlock_irqrestore(&hwb_lock, flags);
            if (!error && copy_to_user(u64_to_user_ptr(request.records), records, request.count * sizeof(*records))) error = -EFAULT;
        }
    } else if (request.operation != NBDRIVER_HWBP_INFO) error = -EINVAL;
status:
    hwb_status(&request, point);
    if (!error && request.error) error = request.error;
    request.error = error;
    if (request.operation != NBDRIVER_HWBP_INFO && request.operation != NBDRIVER_HWBP_READ)
        nbd_info("stage=hwbp operation=%u status=%s error=%d type=%u hits=%llu\n",
            request.operation, error ? "failed" : "ok", error, request.type,
            (unsigned long long)request.hits);
    if (copy_to_user((void __user *)argument, &request, sizeof(request))) {
        /* A failed ADD reply must not leave an unreachable live breakpoint. */
        if (added) hwb_remove_points(owner, added);
        error = -EFAULT;
    }
    mutex_unlock(&hwb_control);
    kvfree(records);
    return error;
}

void nbdriver_hwbp_release(struct file *owner)
{
    mutex_lock(&hwb_control);
    hwb_remove_points(owner, 0);
    mutex_unlock(&hwb_control);
}
void nbdriver_hwbp_quiesce(void)
{
    mutex_lock(&hwb_control);
    hwb_remove_points(NULL, 0);
    mutex_unlock(&hwb_control);
}

static void hwb_empty_ipi(void *unused) { (void)unused; }
void nbdriver_hwbp_exit(void)
{
    nbdriver_hwbp_quiesce();
    if (hwb_faults) {
        if (hwb_bp_index >= 0 && READ_ONCE(hwb_faults[hwb_bp_index].fn) == hwb_breakpoint)
            WRITE_ONCE(hwb_faults[hwb_bp_index].fn, hwb_original_bp);
        if (hwb_wp_index >= 0 && READ_ONCE(hwb_faults[hwb_wp_index].fn) == hwb_watchpoint)
            WRITE_ONCE(hwb_faults[hwb_wp_index].fn, hwb_original_wp);
        on_each_cpu(hwb_empty_ipi, NULL, 1);
        synchronize_rcu();
    }
    if (hwb_cpuhp != CPUHP_INVALID) {
        cpuhp_remove_state_nocalls(hwb_cpuhp); hwb_cpuhp = CPUHP_INVALID;
    }
    if (hwb_pm_registered) { cpu_pm_unregister_notifier(&hwb_pm); hwb_pm_registered = false; }
    if (hwb_switch_probe.kp.addr) {
        unregister_kretprobe(&hwb_switch_probe); hwb_switch_probe.kp.addr = NULL;
    }
    hwb_available = false;
    hwb_faults = NULL;
}

void nbdriver_hwbp_init(void)
{
    static const char *const names[] = { "finish_task_switch", "finish_task_switch.isra.0",
        "finish_task_switch.constprop.0", "finish_task_switch.isra" };
    unsigned long table, bp, wp;
    unsigned int i;
    u64 dfr = read_sysreg(id_aa64dfr0_el1);
    int error = -ENOENT;
    hwb_brps = min_t(unsigned int, ((dfr >> 12) & 15) + 1, 6);
    hwb_brps = hwb_brps > NBD_HWB_BP_BASE ? hwb_brps - NBD_HWB_BP_BASE : 0;
    hwb_hw_wrps = ((dfr >> 20) & 15) + 1;
    hwb_wrps = min_t(unsigned int, hwb_hw_wrps, NBD_HWB_SLOTS);
    table = nbdriver_ksym_lookup_optional("debug_fault_info");
    if (!table) table = nbdriver_ksym_lookup_optional("debug_fault_info_table");
    bp = nbdriver_ksym_lookup_optional("breakpoint_handler");
    wp = nbdriver_ksym_lookup_optional("watchpoint_handler");
    if (!table || !bp || !wp || !hwb_brps || !hwb_wrps) goto unavailable;
    hwb_faults = (void *)table;
    /* ARM64 debug_fault_info has eight entries, not the old driver's scan of
     * sixteen. Resolve the handlers rather than guessing watchpoint index. */
    for (i = 0; i < 8; ++i) {
        if ((unsigned long)READ_ONCE(hwb_faults[i].fn) == bp) hwb_bp_index = i;
        if ((unsigned long)READ_ONCE(hwb_faults[i].fn) == wp) hwb_wp_index = i;
    }
    if (hwb_bp_index < 0 || hwb_wp_index < 0) goto unavailable;
    for (i = 0; i < ARRAY_SIZE(names); ++i) {
        hwb_switch_probe.kp.symbol_name = names[i];
        hwb_switch_probe.handler = hwb_switch_return;
        hwb_switch_probe.maxactive = max_t(int, 64, num_possible_cpus() * 4);
        error = register_kretprobe(&hwb_switch_probe);
        if (!error) break;
        hwb_switch_probe.kp.addr = NULL;
    }
    if (error) goto unavailable;
    error = cpu_pm_register_notifier(&hwb_pm);
    if (error) goto unavailable;
    hwb_pm_registered = true;
    hwb_cpuhp = cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN, "nbdriver:hwbp",
        hwb_cpu_online, hwb_cpu_offline);
    if (hwb_cpuhp < 0) { error = hwb_cpuhp; hwb_cpuhp = CPUHP_INVALID; goto unavailable; }
    hwb_original_bp = READ_ONCE(hwb_faults[hwb_bp_index].fn);
    hwb_original_wp = READ_ONCE(hwb_faults[hwb_wp_index].fn);
    WRITE_ONCE(hwb_faults[hwb_bp_index].fn, hwb_breakpoint);
    WRITE_ONCE(hwb_faults[hwb_wp_index].fn, hwb_watchpoint);
    hwb_init_error = 0;
    hwb_available = true;
    nbdriver_context_init();
    nbd_info("stage=hwbp status=ready backend=raw_register exec_slots=%u watch_slots=%u mode=capture_once\n", hwb_brps, hwb_wrps);
    return;
unavailable:
    hwb_init_error = error;
    nbdriver_hwbp_exit();
    nbd_warn("stage=hwbp status=unavailable backend=raw_register error=%d\n", error);
}
