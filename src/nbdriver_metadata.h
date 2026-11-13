/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_METADATA_H
#define NBDRIVER_METADATA_H
#include <linux/types.h>
int nbdriver_metadata_init(void);
void nbdriver_metadata_exit(void);
u32 nbdriver_metadata_features(void);
long nbdriver_metadata_ioctl(unsigned int command, unsigned long argument);
#endif
