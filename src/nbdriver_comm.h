/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_COMM_H
#define NBDRIVER_COMM_H

#include <linux/types.h>

int nbdriver_comm_init(void);
void nbdriver_comm_exit(void);

/*
 * 在 current 的 fd 表中安装 CLOEXEC 匿名 FD，成功返回 0，失败返回负 errno。
 * 先回写后 fd_install；用户只可在引导调用返回后使用整数，不可提前跨线程发布。
 * 回写失败会撤销预留 FD；不可写的输出地址不保证能收到 worker 的错误回执。
 * fops.owner 在 FD 生命周期持有模块引用，关闭连接不等于卸载驱动。
 */
int nbdriver_comm_install_fd(int __user *output_fd);

#endif /* NBDRIVER_COMM_H */
