/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_COMM_H
#define NBDRIVER_COMM_H

#include <linux/types.h>

int nbdriver_comm_init(void);
void nbdriver_comm_exit(void);

/*
 * 在 current 的 fd 表里安装匿名 FD，并把 fd 号写到用户态 output_fd。
 * 失败返回负 errno，且保证不会在用户态留下无效的 fd 值。
 */
int nbdriver_comm_install_fd(int __user *output_fd);

#endif /* NBDRIVER_COMM_H */
