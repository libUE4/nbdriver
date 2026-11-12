/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_KSYM_H
#define NBDRIVER_KSYM_H

#include <linux/types.h>

/*
 * 5.5 起 kallsyms_lookup_name 不再 EXPORT_SYMBOL；DDK 六个 KMI 的
 * Module.symvers 均为 NOT_EXPORTED，但 System.map 里都有该符号。
 * 因此只能经一次 kprobe 取地址，再按函数指针调用。
 */
int nbdriver_ksym_init(void);
void nbdriver_ksym_exit(void);

/* 解析不到返回 0。调用方必须检查，不能直接解引用。 */
unsigned long nbdriver_ksym_lookup(const char *name);

#endif /* NBDRIVER_KSYM_H */
