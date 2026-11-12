/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_LOG_H
#define NBDRIVER_LOG_H

#include <linux/kernel.h>

/*
 * 加载失败时要能从 dmesg 直接读出「哪一步、哪个符号、什么错误」。
 * 统一前缀便于 grep：nbdriver: stage=... 。
 */
#define nbd_err(fmt, ...) \
	pr_err("nbdriver: " fmt, ##__VA_ARGS__)
#define nbd_warn(fmt, ...) \
	pr_warn("nbdriver: " fmt, ##__VA_ARGS__)
#define nbd_info(fmt, ...) \
	pr_info("nbdriver: " fmt, ##__VA_ARGS__)

/* 解析失败只报一次，避免同一符号刷屏。 */
void nbdriver_log_missing_once(const char *stage, const char *symbol, int error);

#endif /* NBDRIVER_LOG_H */
