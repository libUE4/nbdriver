/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_HWBP_H
#define NBDRIVER_HWBP_H
#include <linux/types.h>
struct file;
void nbdriver_hwbp_init(void);
void nbdriver_hwbp_exit(void);
void nbdriver_hwbp_quiesce(void);
void nbdriver_hwbp_release(struct file *file);
long nbdriver_hwbp_ioctl(struct file *file, unsigned long argument);
struct pt_regs;
void nbdriver_context_init(void);
bool nbdriver_context_ready(void);
bool nbdriver_context_queue(struct file *, unsigned long long, struct pt_regs *);
void nbdriver_context_cancel(struct file *, unsigned long long);
long nbdriver_context_ioctl(struct file *, unsigned long);
void nbdriver_hwbp_context_rearm(struct file *, unsigned long long);
#endif
