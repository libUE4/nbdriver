// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/module.h>

#include "nbdriver_abi.h"
#include "nbdriver_comm.h"
#include "nbdriver_entry.h"

static int __init nbdriver_init(void)
{
	int ret;

	ret = nbdriver_comm_init();
	if (ret) {
		pr_err("nbdriver: stage=module_init dependency=comm status=failed error=%d\n", ret);
		return ret;
	}
	ret = nbdriver_entry_init();
	if (ret) {
		pr_err("nbdriver: stage=module_init dependency=entry status=failed error=%d\n", ret);
		nbdriver_comm_exit();
		return ret;
	}

	pr_info("nbdriver: 已加载, abi=%u\n", NBDRIVER_ABI_VERSION);
	return 0;
}

static void __exit nbdriver_exit(void)
{
	nbdriver_entry_exit();
	nbdriver_comm_exit();
	pr_info("nbdriver: 已卸载\n");
}

module_init(nbdriver_init);
module_exit(nbdriver_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("nbdriver ioctl communication");
MODULE_VERSION("0.1.0");
