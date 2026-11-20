/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_TOUCH_H
#define NBDRIVER_TOUCH_H

#include <linux/types.h>

struct file;

int nbdriver_touch_init(void);
void nbdriver_touch_exit(void);
long nbdriver_touch_ioctl(struct file *file, unsigned int command,
                          unsigned long argument);
void nbdriver_touch_release(struct file *file);
u32 nbdriver_touch_features(void);

#endif /* NBDRIVER_TOUCH_H */
