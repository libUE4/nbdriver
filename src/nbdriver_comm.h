/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_COMM_H
#define NBDRIVER_COMM_H

#include <linux/types.h>

int nbdriver_comm_init(void);
void nbdriver_comm_exit(void);
int nbdriver_comm_install_fd_to_user(int __user *output_fd);

#endif /* NBDRIVER_COMM_H */
