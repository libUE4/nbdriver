// SPDX-License-Identifier: GPL-2.0-only
/* Generic GP/FPSIMD rendezvous, independent of any target application.
 * Exception context only queues work. Waiting and user copies happen after
 * return to task context. No offsets, instruction emulation or angle math. */
#include <linux/capability.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/refcount.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/task_work.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <asm/fpsimd.h>
#include <asm/ptrace.h>
#include "nbdriver_abi.h"
#include "nbdriver_cfi.h"
#include "nbdriver_hwbp.h"
#include "nbdriver_ksym.h"
#include "nbdriver_runtime.h"
#include "nbdriver_log.h"
#include "nbdriver_sf_groups.h"
#include "nbdriver_task_layout.h"

struct context_event {
    struct callback_head work;
    struct list_head list;
    struct file *owner; /* identity only, never dereferenced */
    refcount_t refs;
    bool ready, delivered, replied, cancelled;
    struct nbdriver_context_request capture, reply;
};
static LIST_HEAD(context_events);
static DEFINE_SPINLOCK(context_lock);
static DECLARE_WAIT_QUEUE_HEAD(context_wait);
static atomic64_t context_sequence = ATOMIC64_INIT(0);
static unsigned int context_count;
struct context_outcome { struct file *owner; u64 handle, event; int error; };
static struct context_outcome context_outcomes[64];
static unsigned int context_outcome_next;
static bool context_has_ready(struct file *owner)
{
    struct context_event *e; unsigned long flags; bool ready = false;
    spin_lock_irqsave(&context_lock, flags);
    list_for_each_entry(e, &context_events, list)
        if (e->owner == owner && e->ready && !e->delivered && !e->cancelled) { ready = true; break; }
    spin_unlock_irqrestore(&context_lock, flags);
    return ready;
}
static int (*context_task_work)(struct task_struct *, struct callback_head *, enum task_work_notify_mode);
static void (*context_flush_fp)(void);
static noinline int NBDRIVER_NO_CFI context_queue_work(struct callback_head *work)
{ return context_task_work(current, work, TWA_RESUME); }
static noinline void NBDRIVER_NO_CFI flush_fp(void)
{ context_flush_fp(); }
static void event_put(struct context_event *event)
{ if (refcount_dec_and_test(&event->refs)) kfree(event); }

static bool fp_usable(void)
{
#ifdef CONFIG_ARM64_SVE
    if (test_thread_flag(TIF_SVE)) return false;
#endif
#ifdef CONFIG_ARM64_SME
    if (test_thread_flag(TIF_SME)) return false;
#endif
    return context_flush_fp != NULL;
}

/* The target task owns these memory ranges at commit time. Register changes
 * occur only after all copies commit; restore earlier ranges on copy failure.
 * This is bounded small user memory, not a general remote-write engine. */
static int commit_context(struct context_event *event, struct pt_regs *regs)
{
    const struct nbdriver_context_request *r = &event->reply;
    u8 original[2][16];
    unsigned int i, done = 0;
    if (!r->deadline_ns || ktime_get_ns() >= min(r->deadline_ns, event->capture.deadline_ns) ||
        regs->pc != event->capture.regs.pc || regs->sp != event->capture.regs.sp)
        return -ESTALE;
    if (r->gp_mask & ~((1ULL << 33) - 1) || r->memory_count > 2) return -EINVAL;
    if ((r->gp_mask & NBDRIVER_CONTEXT_PC) &&
        ((r->regs.pc & 3) || !access_ok(u64_to_user_ptr(r->regs.pc), 4))) return -EINVAL;
    if ((r->gp_mask & NBDRIVER_CONTEXT_SP) &&
        ((r->regs.sp & 15) || !access_ok(u64_to_user_ptr(r->regs.sp), 16))) return -EINVAL;
    if (r->fp_mask && (!(event->capture.flags & NBDRIVER_CONTEXT_FP) || !fp_usable()))
        return -EOPNOTSUPP;
    for (i = 0; i < r->memory_count; ++i) {
        const struct nbdriver_context_memory *m = &r->memory[i];
        unsigned int j;
        if (!m->size || m->size > 16 || m->reserved ||
            !access_ok(u64_to_user_ptr(m->address), m->size)) return -EINVAL;
        for (j = 0; j < i; ++j)
            if (m->address < r->memory[j].address + r->memory[j].size &&
                r->memory[j].address < m->address + m->size) return -EINVAL;
        if (copy_from_user(original[i], u64_to_user_ptr(m->address), m->size)) return -EFAULT;
    }
    for (i = 0; i < r->memory_count; ++i) {
        done = i + 1;
        if (copy_to_user(u64_to_user_ptr(r->memory[i].address), r->memory[i].bytes,
                         r->memory[i].size)) goto restore;
    }
    if (ktime_get_ns() >= min(r->deadline_ns, event->capture.deadline_ns)) goto restore;
    /* flush after any sleeping user copy: a context switch may have restored
     * live registers since the capture. Preserve all unselected lanes. */
    if (r->fp_mask) {
        preempt_disable();
        if (!fp_usable()) { preempt_enable(); goto restore; }
        flush_fp();
        for (i = 0; i < 32; ++i) if (r->fp_mask & (1U << i))
            memcpy(&nbdriver_task_thread(current)->uw.fpsimd_state.vregs[i], r->regs.v[i], 16);
        set_thread_flag(TIF_FOREIGN_FPSTATE);
        preempt_enable();
    }
    for (i = 0; i < 31; ++i) if (r->gp_mask & (1ULL << i)) regs->regs[i] = r->regs.x[i];
    if (r->gp_mask & NBDRIVER_CONTEXT_SP) regs->sp = r->regs.sp;
    if (r->gp_mask & NBDRIVER_CONTEXT_PC) regs->pc = r->regs.pc;
    return 0;
restore:
    while (done) {
        unsigned long left;
        --done;
        left = copy_to_user(u64_to_user_ptr(r->memory[done].address), original[done],
                            r->memory[done].size);
        if (left) nbd_err("stage=context reason=memory_restore_fault error=%d\n", -EFAULT);
    }
    return -EFAULT;
}

/* Named assembly entry; strict kernel builds require its C prototype. */
struct module *nbdriver_context_work_run(struct callback_head *head);

__visible __used noinline struct module *nbdriver_context_work_run(struct callback_head *head)
{
    struct context_event *event = container_of(head, struct context_event, work);
    struct pt_regs *regs = task_pt_regs(current);
    unsigned long flags;
    int error = -ECANCELED;
    long waited;
    bool rearm = false;
    if ((current->flags & PF_EXITING) || READ_ONCE(event->cancelled) ||
        !user_mode(regs) || regs->pc != event->capture.regs.pc ||
        regs->sp != event->capture.regs.sp) goto finish;
    preempt_disable();
    if (fp_usable()) {
        flush_fp();
        memcpy(event->capture.regs.v, nbdriver_task_thread(current)->uw.fpsimd_state.vregs, 512);
        event->capture.flags |= NBDRIVER_CONTEXT_FP;
    }
    preempt_enable();
    event->capture.deadline_ns = ktime_get_ns() + 20000000ULL;
    spin_lock_irqsave(&context_lock, flags);
    event->ready = true;
    spin_unlock_irqrestore(&context_lock, flags);
    wake_up_all(&context_wait);
    waited = wait_event_interruptible_timeout(context_wait,
        READ_ONCE(event->replied) || READ_ONCE(event->cancelled), msecs_to_jiffies(20));
    spin_lock_irqsave(&context_lock, flags);
    if (!event->cancelled && !event->replied) error = waited < 0 ? (int)waited : -ETIMEDOUT;
    if (event->replied && !event->cancelled &&
        (event->reply.flags & NBDRIVER_CONTEXT_APPLY)) error = 0;
    spin_unlock_irqrestore(&context_lock, flags);
    if (!error && !nbdriver_runtime_quiesced()) {
        error = commit_context(event, regs);
        rearm = !error && (event->reply.flags & NBDRIVER_CONTEXT_REARM) &&
            regs->pc != event->capture.regs.pc;
    }
finish:
    spin_lock_irqsave(&context_lock, flags);
    list_del(&event->list); --context_count;
    spin_unlock_irqrestore(&context_lock, flags);
    if (rearm && !READ_ONCE(event->cancelled))
        nbdriver_hwbp_context_rearm(event->owner, event->capture.handle);
    spin_lock_irqsave(&context_lock, flags);
    context_outcomes[context_outcome_next++ % 64] = (struct context_outcome) {
        event->owner, event->capture.handle, event->capture.event, error };
    spin_unlock_irqrestore(&context_lock, flags);
    wake_up_all(&context_wait);
    if (error != -ECANCELED && error)
        nbd_warn("stage=context reason=commit_failed error=%d\n", error);
    event_put(event);
    return THIS_MODULE;
}
/* Tail-call module_put: no instruction or return into this module afterward. */
static __naked void context_work_callback(struct callback_head *head)
{
    asm volatile(".inst 0xd503245f\n.inst 0xd503233f\n"
        "stp x29,x30,[sp,#-16]!\nmov x29,sp\nbl nbdriver_context_work_run\n"
        "ldp x29,x30,[sp],#16\n.inst 0xd50323bf\nb module_put\n");
}

bool nbdriver_context_queue(struct file *owner, unsigned long long handle, struct pt_regs *regs)
{
    struct context_event *event;
    unsigned long flags;
    int error;
    if (!nbdriver_context_ready() || !try_module_get(THIS_MODULE)) return false;
    event = kzalloc(sizeof(*event), GFP_ATOMIC);
    if (!event) { module_put(THIS_MODULE); return false; }
    refcount_set(&event->refs, 1); event->owner = owner;
    init_task_work(&event->work, context_work_callback);
    event->capture.version = NBDRIVER_CONTEXT_VERSION;
    event->capture.size = sizeof(event->capture);
    event->capture.handle = handle;
    event->capture.event = atomic64_inc_return(&context_sequence);
    event->capture.pid = task_tgid_vnr(current); event->capture.tid = task_pid_vnr(current);
    event->capture.flags = NBDRIVER_CONTEXT_AVAILABLE;
    memcpy(event->capture.regs.x, regs->regs, sizeof(regs->regs));
    event->capture.regs.sp = regs->sp; event->capture.regs.pc = regs->pc;
    spin_lock_irqsave(&context_lock, flags);
    if (context_count >= 16) { spin_unlock_irqrestore(&context_lock, flags); goto fail; }
    list_add_tail(&event->list, &context_events); ++context_count;
    spin_unlock_irqrestore(&context_lock, flags);
    error = context_queue_work(&event->work);
    if (!error) return true;
    spin_lock_irqsave(&context_lock, flags);
    list_del(&event->list); --context_count;
    spin_unlock_irqrestore(&context_lock, flags);
fail:
    kfree(event); module_put(THIS_MODULE); return false;
}
void nbdriver_context_cancel(struct file *owner, unsigned long long handle)
{
    struct context_event *event; unsigned long flags;
    spin_lock_irqsave(&context_lock, flags);
    list_for_each_entry(event, &context_events, list)
        if ((!owner || owner == event->owner) && (!handle || handle == event->capture.handle))
            event->cancelled = true;
    spin_unlock_irqrestore(&context_lock, flags);
    wake_up_all(&context_wait);
}
long nbdriver_context_ioctl(struct file *owner, unsigned long argument)
{
    struct nbdriver_context_request *r;
    struct context_event *event, *found = NULL;
    unsigned long flags;
    int error = 0;
    if (!capable(CAP_SYS_PTRACE) && !nbdriver_sf_control_caller()) return -EPERM;
    r = memdup_user((void __user *)argument, sizeof(*r));
    if (IS_ERR(r)) return PTR_ERR(r);
    if (r->version != NBDRIVER_CONTEXT_VERSION || r->size != sizeof(*r) ||
        r->reserved || r->operation > NBDRIVER_CONTEXT_OUTCOME) { error = -EINVAL; goto out; }
    if (r->operation == NBDRIVER_CONTEXT_INFO) {
        r->flags = nbdriver_context_ready() ? NBDRIVER_CONTEXT_AVAILABLE | NBDRIVER_CONTEXT_FP : 0;
        goto reply;
    }
    if (r->operation == NBDRIVER_CONTEXT_OUTCOME) {
        unsigned int i; error = -EAGAIN;
        spin_lock_irqsave(&context_lock, flags);
        for (i = 0; i < ARRAY_SIZE(context_outcomes); ++i) {
            const struct context_outcome *o = &context_outcomes[i];
            if (o->owner == owner && o->event == r->event && o->handle == r->handle) {
                error = o->error; r->flags = error ? 0 : NBDRIVER_CONTEXT_COMMITTED; break;
            }
        }
        spin_unlock_irqrestore(&context_lock, flags);
        goto reply;
    }
    if (r->operation == NBDRIVER_CONTEXT_NEXT)
        wait_event_interruptible_timeout(context_wait, context_has_ready(owner), msecs_to_jiffies(2));
    spin_lock_irqsave(&context_lock, flags);
    list_for_each_entry(event, &context_events, list) {
        if (event->owner != owner || event->cancelled || !event->ready) continue;
        if (r->operation == NBDRIVER_CONTEXT_NEXT) {
            if (event->delivered || event->replied ||
                (r->handle && r->handle != event->capture.handle)) continue;
            event->delivered = true; *r = event->capture; r->operation = NBDRIVER_CONTEXT_NEXT;
            found = event; refcount_inc(&event->refs); break;
        }
        if (r->event != event->capture.event || r->handle != event->capture.handle) continue;
        if (event->replied || !event->delivered || ktime_get_ns() >= event->capture.deadline_ns)
            error = -ESTALE;
        else if (r->flags & ~(NBDRIVER_CONTEXT_APPLY | NBDRIVER_CONTEXT_REARM)) error = -EINVAL;
        else if (nbdriver_runtime_quiesced()) error = -ESHUTDOWN;
        else { event->reply = *r; event->replied = true; }
        found = event; refcount_inc(&event->refs); break;
    }
    spin_unlock_irqrestore(&context_lock, flags);
    if (!found) error = r->operation == NBDRIVER_CONTEXT_NEXT ? -EAGAIN : -ESTALE;
    if (r->operation == NBDRIVER_CONTEXT_REPLY) wake_up_all(&context_wait);
reply:
    r->error = error;
    if (copy_to_user((void __user *)argument, r, sizeof(*r))) {
        if (found && r->operation == NBDRIVER_CONTEXT_NEXT) {
            spin_lock_irqsave(&context_lock, flags); found->delivered = false;
            spin_unlock_irqrestore(&context_lock, flags);
        }
        error = -EFAULT;
    }
out:
    if (found) event_put(found);
    kfree(r); return error;
}
bool nbdriver_context_ready(void) { return context_task_work && context_flush_fp; }
void nbdriver_context_init(void)
{
    context_task_work = (void *)nbdriver_ksym_lookup_optional("task_work_add");
    context_flush_fp = (void *)nbdriver_ksym_lookup_optional("fpsimd_save_and_flush_cpu_state");
    nbd_info("stage=context available=%u backend=task_work timeout_ms=20\n", nbdriver_context_ready());
}
