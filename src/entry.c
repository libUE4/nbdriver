// SPDX-License-Identifier: GPL-2.0-only
#include <linux/init.h>
#include <linux/module.h>

static int __init nbdriver_init(void)
{
	pr_info("nbdriver: scaffold loaded\n");
	return 0;
}

static void __exit nbdriver_exit(void)
{
	pr_info("nbdriver: scaffold unloaded\n");
}

module_init(nbdriver_init);
module_exit(nbdriver_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("nbdriver rewrite scaffold");
MODULE_VERSION("0.1.0");
