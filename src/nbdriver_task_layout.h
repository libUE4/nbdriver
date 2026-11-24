/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_TASK_LAYOUT_H
#define NBDRIVER_TASK_LAYOUT_H

#include <linux/sched.h>
#include <linux/compat.h>
#include <asm/processor.h>

int nbdriver_task_layout_init(void);
extern unsigned long nbdriver_task_thread_offset;

static inline struct thread_struct *nbdriver_task_thread(struct task_struct *task)
{
	return (struct thread_struct *)((char *)task + nbdriver_task_thread_offset);
}

static inline unsigned long *nbdriver_task_user_tls(struct task_struct *task)
{
	/* Keep the architecture's native/compat TLS selection. Only the enclosing
	 * task's thread member may move; its inner layout is verified at startup. */
	return (unsigned long *)((char *)task_user_tls(task) +
		((long)nbdriver_task_thread_offset - (long)offsetof(struct task_struct, thread)));
}

#endif
