// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include <linux/module.h>

#include "nbdriver_entry.h"
#include "nbdriver_log.h"
#include "nbdriver_ksym.h"

/*
 * 统一入口分发。目前只有 reboot 探针一条路径：
 * sys_call_table 槽位改写需要能写内核文本段，而 __set_fixmap /
 * aarch64_insn_patch_text 在 DDK 六个 KMI 的 Module.symvers 里都未导出，
 * System.map 里也查不到 aarch64_insn_patch_text，因此不做该路径。
 */
int nbdriver_entry_init(void)
{
	int ret;

	ret = nbdriver_reboot_entry_init();
	if (ret) {
		nbd_err("stage=entry_init path=reboot status=failed error=%d\n",
			ret);
		return ret;
	}

	nbd_info("stage=entry_init path=reboot status=ready target=%s\n",
		 nbdriver_reboot_entry_target());
	return 0;
}

void nbdriver_entry_exit(void)
{
	nbdriver_reboot_entry_exit();
}

const char *nbdriver_entry_target(void)
{
	return nbdriver_reboot_entry_target();
}
