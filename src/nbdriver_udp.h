/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_UDP_H
#define NBDRIVER_UDP_H

#include <linux/poll.h>
#include <linux/types.h>

struct file;
void nbdriver_udp_init(void);
void nbdriver_udp_exit(void);
void nbdriver_udp_quiesce(void);
u32 nbdriver_udp_features(void);
long nbdriver_udp_ioctl(struct file *owner, unsigned int command,
                       unsigned long argument);
void nbdriver_udp_release(struct file *owner);
__poll_t nbdriver_udp_poll(struct file *owner, poll_table *wait);

#endif
