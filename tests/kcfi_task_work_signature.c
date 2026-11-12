/* SPDX-License-Identifier: GPL-2.0-only */
/* Userspace-only reproducer: an extra argument selects the known-bad call. */
#include "../src/nbdriver_cfi.h"

struct task_struct;
struct callback_head;
enum task_work_notify_mode {
	TWA_NONE,
	TWA_RESUME,
	TWA_SIGNAL,
};

int task_work_add(struct task_struct *task, struct callback_head *work,
		  enum task_work_notify_mode mode)
{
	(void)task;
	(void)work;
	return mode == TWA_RESUME ? 0 : 1;
}

typedef int (*old_work_fn)(struct task_struct *, struct callback_head *, int);
typedef __typeof__(&task_work_add) exact_work_fn;

__attribute__((noinline))
static int call_old(old_work_fn fn)
{
	return fn((void *)0, (void *)0, TWA_RESUME);
}

__attribute__((noinline))
static int call_exact(exact_work_fn fn)
{
	return fn((void *)0, (void *)0, TWA_RESUME);
}

/* The ported lsdriver call-site exemption, with callback metadata retained. */
__attribute__((noinline)) static int NBDRIVER_NO_CFI call_local(old_work_fn fn)
{
	return fn((void *)0, (void *)0, TWA_RESUME);
}

int main(int argc, char **argv)
{
	(void)argv;
	if (argc > 2)
		return call_local((old_work_fn)task_work_add);
	if (argc > 1)
		return call_old((old_work_fn)task_work_add);
	return call_exact(task_work_add);
}
