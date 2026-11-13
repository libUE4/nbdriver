/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_READ_H
#define NBDRIVER_READ_H

#include <linux/types.h>

struct task_struct;
struct mm_struct;
struct nbdriver_read_request;

/* Returns an mm reference or ERR_PTR; preserves the existing ptrace/LSM checks. */
struct mm_struct *nbdriver_access_mm(struct task_struct *task);
/* Caller owns an mm_users reference; false never faults missing pages in. */
int nbdriver_read_mm(struct mm_struct *mm, struct nbdriver_read_request *request,
		     u8 *bytes, bool fault_in);
/* For legacy userspace VMA names: caller holds mmap_lock, never faults pages. */
int nbdriver_read_mm_locked(struct mm_struct *mm,
		struct nbdriver_read_request *request, u8 *bytes);

int nbdriver_read_init(void);
void nbdriver_read_exit(void);
long nbdriver_read_ioctl(unsigned long argument);
long nbdriver_write_ioctl(unsigned long argument);
long nbdriver_readv_ioctl(unsigned long argument);
bool nbdriver_read_fault_in_ready(void);

#endif /* NBDRIVER_READ_H */
