/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_ENTRY_H
#define NBDRIVER_ENTRY_H

struct module;
struct callback_head;
/* Called by the assembly task_work trampoline; it must remain visible. */
struct module *nbdriver_fd_work_run(struct callback_head *head);

/*
 * 首次取 FD 的入口。reboot 探针是唯一跨 5.10-6.18 可用的形态：
 *   - sys_call_table 槽位改写要写内核文本段，缺可用的导出符号
 *   - kprobe 只需 register_kprobe，全系 EXPORT_SYMBOL_GPL
 */
int nbdriver_entry_init(void);
void nbdriver_entry_exit(void);

int nbdriver_sf_entry_init(void);
void nbdriver_sf_entry_exit(void);
const char *nbdriver_sf_entry_target(void);

/* 已注册的探针目标符号名，未注册返回 NULL。 */
const char *nbdriver_entry_target(void);


#endif /* NBDRIVER_ENTRY_H */
