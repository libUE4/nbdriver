/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_READ_H
#define NBDRIVER_READ_H

#include <linux/types.h>

int nbdriver_read_init(void);
void nbdriver_read_exit(void);
long nbdriver_read_ioctl(unsigned long argument);
long nbdriver_write_ioctl(unsigned long argument);
bool nbdriver_read_fault_in_ready(void);

#endif /* NBDRIVER_READ_H */
