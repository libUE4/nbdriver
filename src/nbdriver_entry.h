/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_ENTRY_H
#define NBDRIVER_ENTRY_H

/* 首选 misc 导出接口；只在设备节点注册失败时尝试旧 reboot 探针。 */
int nbdriver_entry_init(void);
void nbdriver_entry_exit(void);

/* 已注册的探针目标符号名，未注册返回 NULL。 */
const char *nbdriver_entry_target(void);

int nbdriver_reboot_entry_init(void);
void nbdriver_reboot_entry_exit(void);
const char *nbdriver_reboot_entry_target(void);

int nbdriver_misc_entry_init(void);
void nbdriver_misc_entry_exit(void);

#endif /* NBDRIVER_ENTRY_H */
