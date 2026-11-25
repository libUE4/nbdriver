/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_CONFIG_H
#define NBDRIVER_CONFIG_H
int nbdriver_config_init(void);
void nbdriver_config_exit(void);
long nbdriver_config_ioctl(unsigned long argument);
#endif
