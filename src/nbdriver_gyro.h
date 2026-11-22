/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_GYRO_H
#define NBDRIVER_GYRO_H
struct file;
void nbdriver_gyro_init(void);
void nbdriver_gyro_exit(void);
void nbdriver_gyro_quiesce(void);
void nbdriver_gyro_release(struct file *file);
long nbdriver_gyro_ioctl(struct file *file, unsigned long argument);
#endif
