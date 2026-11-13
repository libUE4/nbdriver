/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_QUERY_H
#define NBDRIVER_QUERY_H
#include <linux/types.h>
struct task_struct;

/* Holds an exec_update_lock read lock and task reference after ptrace checks. */
struct task_struct *nbdriver_query_get_process(pid_t pid, u64 expected_start_ns);
void nbdriver_query_put_process(struct task_struct *task);

int nbdriver_query_init(void);
void nbdriver_query_exit(void);
u32 nbdriver_query_features(void);
long nbdriver_query_ioctl(unsigned int command, unsigned long argument);
#endif
