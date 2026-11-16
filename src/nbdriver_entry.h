/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_ENTRY_H
#define NBDRIVER_ENTRY_H

#include <linux/types.h>

struct module;
struct callback_head;
/* Called by the assembly task_work trampoline; it must remain visible. */
struct module *nbdriver_fd_work_run(struct callback_head *head);

/* 动态 NI 空槽与独立 reboot 后备。表入口本身持有引用，与连接 FD 引用不同；
 * 匹配 handle/token 的 QUIESCE 恢复槽位并等待旧调用离开后才释放表引用。 */
int nbdriver_entry_init(void);
void nbdriver_entry_exit(void);
/* Control callers hold their connection FD (and therefore a module reference). */
int nbdriver_entry_quiesce(void);
int nbdriver_entry_resume(void);
u32 nbdriver_entry_probe_mask(void);
const char *nbdriver_entry_target(void);

#endif /* NBDRIVER_ENTRY_H */
