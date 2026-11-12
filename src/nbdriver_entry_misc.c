// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include <linux/miscdevice.h>
#include <linux/module.h>

#include "nbdriver_comm.h"
#include "nbdriver_log.h"

/* 只依赖 Module.symvers 中导出的 misc 注册 API，不依赖私有 syscall。 */
static struct miscdevice nbdriver_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "nbdriver",
	.mode = 0600,
};

int nbdriver_misc_entry_init(void)
{
	int ret;

	nbdriver_misc.fops = nbdriver_comm_fops();
	ret = misc_register(&nbdriver_misc);
	if (ret)
		nbd_warn("stage=misc_init operation=misc_register error=%d\n", ret);
	else
		nbd_info("stage=misc_init path=/dev/nbdriver status=ready\n");
	return ret;
}

void nbdriver_misc_entry_exit(void)
{
	misc_deregister(&nbdriver_misc);
}
