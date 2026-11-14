/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_RUNTIME_H
#define NBDRIVER_RUNTIME_H

#include <linux/types.h>

int nbdriver_runtime_init(void);
void nbdriver_runtime_exit(void);
long nbdriver_runtime_ioctl(unsigned long argument);
long nbdriver_runtime_control_ioctl(unsigned long argument);
bool nbdriver_runtime_quiesced(void);
u32 nbdriver_runtime_features(void);

#endif /* NBDRIVER_RUNTIME_H */
