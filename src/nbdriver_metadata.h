/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_METADATA_H
#define NBDRIVER_METADATA_H
#include <linux/types.h>
int nbdriver_metadata_init(void);
void nbdriver_metadata_exit(void);
u32 nbdriver_metadata_features(void);
long nbdriver_metadata_ioctl(unsigned int command, unsigned long argument);
void nbdriver_kgsl_init(void);
void nbdriver_kgsl_exit(void);
u32 nbdriver_kgsl_features(void);
long nbdriver_kgsl_info_ioctl(unsigned long argument);
long nbdriver_kgsl_sync_ioctl(unsigned long argument);
void nbdriver_kgsl_visibility_init(void);
void nbdriver_kgsl_visibility_exit(void);
u32 nbdriver_kgsl_visibility_features(void);
long nbdriver_kgsl_visibility_ioctl(unsigned long argument);
#endif
