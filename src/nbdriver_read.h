/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_READ_H
#define NBDRIVER_READ_H

int nbdriver_read_init(void);
void nbdriver_read_exit(void);
long nbdriver_read_ioctl(unsigned long argument);

#endif /* NBDRIVER_READ_H */
