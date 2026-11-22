/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_HWBP_H
#define NBDRIVER_HWBP_H
struct file;
void nbdriver_hwbp_init(void);
void nbdriver_hwbp_exit(void);
void nbdriver_hwbp_quiesce(void);
void nbdriver_hwbp_release(struct file *file);
long nbdriver_hwbp_ioctl(struct file *file, unsigned long argument);
#endif
