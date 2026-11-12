// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/module.h>

#include "nbdriver_abi.h"
#include "nbdriver_comm.h"
#include "nbdriver_entry.h"
#include "nbdriver_ksym.h"
#include "nbdriver_log.h"
#include "nbdriver_cfi.h"
#include "nbdriver_read.h"

static int __init nbdriver_init(void)
{
	int ret;

	ret = nbdriver_comm_init();
	if (ret) {
		nbd_err("stage=module_init dependency=comm error=%d\n", ret);
		return ret;
	}

	ret = nbdriver_ksym_init();
	if (ret) {
		nbd_err("stage=module_init dependency=ksym error=%d\n", ret);
		goto err_comm;
	}

	ret = nbdriver_cfi_init();
	if (ret) {
		nbd_err("stage=module_init dependency=cfi error=%d\n", ret);
		goto err_ksym;
	}

	ret = nbdriver_read_init();
	if (ret) {
		nbd_err("stage=module_init dependency=read error=%d\n", ret);
		goto err_cfi;
	}

	ret = nbdriver_entry_init();
	if (ret) {
		nbd_err("stage=module_init dependency=entry error=%d\n", ret);
		goto err_read;
	}

	nbd_info("stage=module_init status=loaded abi=%u entry=%s\n",
		 NBDRIVER_ABI_VERSION, nbdriver_entry_target());
	return 0;

err_read:
	nbdriver_read_exit();
err_cfi:
	nbdriver_cfi_exit();
err_ksym:
	nbdriver_ksym_exit();
err_comm:
	nbdriver_comm_exit();
	return ret;
}

static void __exit nbdriver_exit(void)
{
	nbdriver_entry_exit();
	nbdriver_read_exit();
	nbdriver_cfi_exit();
	nbdriver_ksym_exit();
	nbdriver_comm_exit();
	nbd_info("stage=module_exit status=unloaded\n");
}

module_init(nbdriver_init);
module_exit(nbdriver_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("nbdriver ioctl communication");
MODULE_VERSION("0.1.0");
