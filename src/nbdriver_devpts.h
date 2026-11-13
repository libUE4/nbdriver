/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_DEVPTS_H
#define NBDRIVER_DEVPTS_H

#include <linux/types.h>

int nbdriver_devpts_init(void);
void nbdriver_devpts_exit(void);
u32 nbdriver_devpts_features(void);
long nbdriver_devpts_ioctl(unsigned long argument);

#endif /* NBDRIVER_DEVPTS_H */
