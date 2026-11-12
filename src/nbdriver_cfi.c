/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * CFI compatibility port from workspace lsdriver/export_fun.h and Makefile.
 * Legacy slowpath handling uses the synchronous text patch API; KCFI uses
 * local call-site exemptions and retains module callback type metadata.
 */
#include <linux/bug.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/types.h>

#include "nbdriver_cfi.h"
#include "nbdriver_ksym.h"
#include "nbdriver_log.h"

#ifdef NBDRIVER_LEGACY_CFI
#define NBDRIVER_BTI_C 0xd503245fU
#define NBDRIVER_RET_X30 0xd65f03c0U
#define NBDRIVER_CFI_MAX_PATCHES 6

typedef int (*nbdriver_patch_text_fn_t)(void *addresses[], u32 instructions[],
				       int count);

static nbdriver_patch_text_fn_t nbdriver_patch_text;
static void *nbdriver_cfi_addresses[NBDRIVER_CFI_MAX_PATCHES];
static u32 nbdriver_cfi_original[NBDRIVER_CFI_MAX_PATCHES];
static int nbdriver_cfi_count;

/* Same fail-closed handler as lsdriver for legacy vendor KMI omissions. */
void __noreturn NBDRIVER_NO_CFI
__ubsan_handle_cfi_check_fail_abort(void *data, void *ptr, void *vtable);

void __noreturn NBDRIVER_NO_CFI
__ubsan_handle_cfi_check_fail_abort(void *data, void *ptr, void *vtable)
{
	(void)data;
	(void)ptr;
	(void)vtable;
	BUG();
}

/* Keep the exemption at this actual indirect call, even under inlining/LTO. */
static noinline int NBDRIVER_NO_CFI
nbdriver_cfi_patch(void *addresses[], u32 instructions[], int count)
{
	return nbdriver_patch_text(addresses, instructions, count);
}
#endif

int nbdriver_cfi_init(void)
{
#ifdef NBDRIVER_LEGACY_CFI
	static const char *const symbols[] = {
		"__cfi_slowpath",
		"__cfi_slowpath_diag",
		"_cfi_slowpath",
	};
	u32 replacement[NBDRIVER_CFI_MAX_PATCHES];
	unsigned int i;
	int count = 0;
	int ret;

	nbdriver_patch_text = (nbdriver_patch_text_fn_t)
		nbdriver_ksym_lookup("aarch64_insn_patch_text");
	if (!nbdriver_patch_text) {
		nbd_warn("stage=cfi_init mode=legacy reason=patch_api_missing local_calls=enabled\n");
		return 0;
	}

	for (i = 0; i < ARRAY_SIZE(symbols); ++i) {
		unsigned long address = nbdriver_ksym_lookup(symbols[i]);
		int previous;
		bool duplicate = false;

		if (!address)
			continue;
		if (!IS_ALIGNED(address, sizeof(u32))) {
			nbd_err("stage=cfi_init symbol=%s reason=unaligned error=%d\n",
				symbols[i], -EINVAL);
			return -EINVAL;
		}
		for (previous = 0; previous < count; previous += 2)
			if (nbdriver_cfi_addresses[previous] == (void *)address)
				duplicate = true;
		if (duplicate)
			continue;

		nbdriver_cfi_addresses[count] = (void *)address;
		nbdriver_cfi_original[count] = READ_ONCE(*(u32 *)address);
		replacement[count++] = NBDRIVER_BTI_C;
		nbdriver_cfi_addresses[count] = (void *)(address + sizeof(u32));
		nbdriver_cfi_original[count] =
			READ_ONCE(*(u32 *)(address + sizeof(u32)));
		replacement[count++] = NBDRIVER_RET_X30;
	}
	if (!count) {
		nbd_info("stage=cfi_init mode=legacy slowpaths=0 local_calls=enabled\n");
		return 0;
	}

	ret = nbdriver_cfi_patch(nbdriver_cfi_addresses, replacement, count);
	if (ret) {
		int restore = nbdriver_cfi_patch(nbdriver_cfi_addresses,
						 nbdriver_cfi_original, count);

		nbd_err("stage=cfi_init operation=patch error=%d rollback=%d\n",
			ret, restore);
		return ret;
	}
	nbdriver_cfi_count = count;
	nbd_info("stage=cfi_init mode=legacy slowpaths=%d status=patched\n",
		 count / 2);
#else
	nbd_info("stage=cfi_init mode=local_calls callback_cfi=preserved\n");
#endif
	return 0;
}

void nbdriver_cfi_exit(void)
{
#ifdef NBDRIVER_LEGACY_CFI
	if (nbdriver_cfi_count) {
		int ret = nbdriver_cfi_patch(nbdriver_cfi_addresses,
					    nbdriver_cfi_original, nbdriver_cfi_count);
		if (ret)
			nbd_err("stage=cfi_exit operation=restore error=%d\n", ret);
		else
			nbd_info("stage=cfi_exit status=restored\n");
	}
	nbdriver_cfi_count = 0;
	nbdriver_patch_text = NULL;
#endif
}
