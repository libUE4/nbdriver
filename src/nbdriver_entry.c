// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include <linux/module.h>

#include "nbdriver_entry.h"

int nbdriver_entry_init(void)
{
	int ret;

	ret = nbdriver_reboot_entry_init();
	if (ret)
		pr_err("nbdriver: stage=entry dependency=reboot_kprobe status=failed error=%d\n", ret);
	return ret;
}

void nbdriver_entry_exit(void)
{
	nbdriver_reboot_entry_exit();
}
