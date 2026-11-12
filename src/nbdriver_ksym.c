// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include <linux/kprobes.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/types.h>

#include "nbdriver_ksym.h"
#include "nbdriver_log.h"

#ifndef KSYM_NAME_LEN
#define KSYM_NAME_LEN 128
#endif

#ifndef __nocfi
#define __nocfi
#endif

typedef unsigned long (*nbdriver_lookup_fn_t)(const char *name);

static nbdriver_lookup_fn_t nbdriver_lookup_fn;

int nbdriver_ksym_init(void)
{
	struct kprobe probe;
	int ret;

	if (nbdriver_lookup_fn)
		return 0;

	memset(&probe, 0, sizeof(probe));
	probe.symbol_name = "kallsyms_lookup_name";

	ret = register_kprobe(&probe);
	if (ret) {
		nbd_err("stage=ksym_init symbol=kallsyms_lookup_name operation=register_kprobe error=%d\n",
			ret);
		return ret;
	}

	nbdriver_lookup_fn = (nbdriver_lookup_fn_t)probe.addr;
	unregister_kprobe(&probe);

	if (!nbdriver_lookup_fn) {
		nbd_err("stage=ksym_init symbol=kallsyms_lookup_name reason=null_address error=%d\n",
			-ENOENT);
		return -ENOENT;
	}

	nbd_info("stage=ksym_init status=ready\n");
	return 0;
}

void nbdriver_ksym_exit(void)
{
	nbdriver_lookup_fn = NULL;
}

unsigned long __nocfi nbdriver_ksym_lookup(const char *name)
{
	unsigned long address;

	if (!name || !name[0]) {
		nbd_err("stage=ksym_lookup reason=invalid_name error=%d\n", -EINVAL);
		return 0;
	}
	if (!nbdriver_lookup_fn) {
		nbd_err("stage=ksym_lookup symbol=%s reason=resolver_unavailable error=%d\n",
			name, -ENODEV);
		return 0;
	}

	address = nbdriver_lookup_fn(name);
	if (!address)
		nbd_warn("stage=ksym_lookup symbol=%s reason=not_found error=%d\n",
			 name, -ENOENT);

	return address;
}
