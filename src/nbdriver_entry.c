// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include <linux/module.h>

#include "nbdriver_entry.h"
#include "nbdriver_log.h"
#include "nbdriver_ksym.h"
#include "nbdriver_cfi.h"

enum nbdriver_entry_mode { NBDRIVER_ENTRY_NONE, NBDRIVER_ENTRY_MISC,
			    NBDRIVER_ENTRY_REBOOT };
static enum nbdriver_entry_mode nbdriver_mode;

/* 先用导出 API；只有 misc 初始化失败才尝试私有符号路径。 */
int nbdriver_entry_init(void)
{
	int ret, misc_error;

#ifdef NBDRIVER_HAS_MISC
	misc_error = nbdriver_misc_entry_init();
	if (!misc_error) {
		nbdriver_mode = NBDRIVER_ENTRY_MISC;
		return 0;
	}
#else
	misc_error = -EOPNOTSUPP;
	nbd_warn("stage=misc_init reason=symbols_not_exported error=%d\n",
		 misc_error);
#endif

	ret = nbdriver_ksym_init();
	if (ret)
		goto fallback_failed;
	ret = nbdriver_cfi_init();
	if (ret)
		goto err_ksym;

	ret = nbdriver_reboot_entry_init();
	if (ret)
		goto err_cfi;

	nbdriver_mode = NBDRIVER_ENTRY_REBOOT;
	nbd_info("stage=entry_init path=reboot_fallback status=ready target=%s\n",
		 nbdriver_reboot_entry_target());
	return 0;

err_cfi:
	nbdriver_cfi_exit();
err_ksym:
	nbdriver_ksym_exit();
fallback_failed:
	nbd_err("stage=entry_init paths=misc,reboot status=failed misc_error=%d fallback_error=%d\n",
		misc_error, ret);
	return ret;
}

void nbdriver_entry_exit(void)
{
	if (nbdriver_mode == NBDRIVER_ENTRY_MISC) {
#ifdef NBDRIVER_HAS_MISC
		nbdriver_misc_entry_exit();
#endif
	} else if (nbdriver_mode == NBDRIVER_ENTRY_REBOOT) {
		nbdriver_reboot_entry_exit();
		nbdriver_cfi_exit();
		nbdriver_ksym_exit();
	}
	nbdriver_mode = NBDRIVER_ENTRY_NONE;
}

const char *nbdriver_entry_target(void)
{
	return nbdriver_mode == NBDRIVER_ENTRY_MISC ? "/dev/nbdriver" :
		nbdriver_reboot_entry_target();
}
