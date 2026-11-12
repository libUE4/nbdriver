// SPDX-License-Identifier: GPL-2.0-only
#include <linux/kernel.h>
#include <linux/module.h>

#include "nbdriver_log.h"

/*
 * 刷入失败的现场通常只能拿到 dmesg。符号缺失是最常见的失败原因，
 * 但同一个符号可能在初始化里被查多次，这里只报首次。
 */
static const char *nbdriver_reported[16];
static int nbdriver_reported_count;

void nbdriver_log_missing_once(const char *stage, const char *symbol, int error)
{
	int i;

	for (i = 0; i < nbdriver_reported_count; i++) {
		if (nbdriver_reported[i] == symbol)
			return;
	}

	if (nbdriver_reported_count < ARRAY_SIZE(nbdriver_reported))
		nbdriver_reported[nbdriver_reported_count++] = symbol;

	pr_err("nbdriver: stage=%s symbol=%s reason=not_found error=%d\n",
	       stage, symbol ? symbol : "(null)", error);
}
