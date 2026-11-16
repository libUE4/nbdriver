/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_RUNTIME_H
#define NBDRIVER_RUNTIME_H

#include <linux/types.h>

/* STATUS 不替代 GET_ABI 能力检查；CONTROL 使用本次加载的 handle/token。
 * QUIESCE 不关闭全部 FD，也不承诺清除所有过滤资源；完整回收在模块退出阶段。
 * RESUME 只恢复同模块运行，不自动恢复 UDP 队列。 */
int nbdriver_runtime_init(void);
void nbdriver_runtime_exit(void);
long nbdriver_runtime_ioctl(unsigned long argument);
long nbdriver_runtime_control_ioctl(unsigned long argument);
bool nbdriver_runtime_quiesced(void);
u32 nbdriver_runtime_features(void);

#endif /* NBDRIVER_RUNTIME_H */
