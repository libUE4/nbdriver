// SPDX-License-Identifier: GPL-2.0-only
/* Verify the task fields used by the module before publishing any interface.
 * Vendor task tails can grow without changing the inner ARM64 thread layout.
 * Read the running kernel's BTF; never select a tail by model name or release. */
#include <linux/types.h>
#include <uapi/linux/btf.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include "nbdriver_cfi.h"
#include "nbdriver_ksym.h"
#include "nbdriver_log.h"
#include "nbdriver_task_layout.h"

unsigned long nbdriver_task_thread_offset = offsetof(struct task_struct, thread);

struct btf;
static s32 (*layout_find)(const struct btf *, const char *, u8);
static const struct btf_type *(*layout_type)(const struct btf *, u32);
static const char *(*layout_name)(const struct btf *, u32);

static noinline s32 NBDRIVER_NO_CFI find_type(const struct btf *btf, const char *name)
{
	return layout_find(btf, name, BTF_KIND_STRUCT);
}

static noinline const struct btf_type *NBDRIVER_NO_CFI
type_by_id(const struct btf *btf, u32 id)
{
	return layout_type(btf, id);
}

static noinline const char *NBDRIVER_NO_CFI
name_by_offset(const struct btf *btf, u32 offset)
{
	return layout_name(btf, offset);
}

static int member_offset(const struct btf *btf, const struct btf_type *type,
			 const char *name, unsigned long *offset)
{
	const struct btf_member *members;
	u32 count, i;
	bool found = false;

	if (!type || BTF_INFO_KIND(type->info) != BTF_KIND_STRUCT)
		return -EINVAL;
	count = BTF_INFO_VLEN(type->info);
	if (count > 4096 || type->size > 65536)
		return -E2BIG;
	members = (const struct btf_member *)(type + 1);
	for (i = 0; i < count; ++i) {
		const char *member = name_by_offset(btf, members[i].name_off);
		u32 bits = members[i].offset;
		if (!member || strcmp(member, name))
			continue;
		if (found || (BTF_INFO_KFLAG(type->info) && (bits >> 24)))
			return -EINVAL;
		if (BTF_INFO_KFLAG(type->info))
			bits &= 0xffffff;
		if ((bits & 7) || bits / 8 >= type->size)
			return -EINVAL;
		*offset = bits / 8;
		found = true;
	}
	return found ? 0 : -ENOENT;
}

struct layout_field { const char *name; unsigned long offset; };

static int verify_fields(const struct btf *btf, const struct btf_type *type,
			 const struct layout_field *fields, size_t count)
{
	size_t i;
	for (i = 0; i < count; ++i) {
		unsigned long actual = 0;
		int ret = member_offset(btf, type, fields[i].name, &actual);
		if (ret || actual != fields[i].offset) {
			nbd_err("stage=task_layout field=%s expected=%lu observed=%lu error=%d\n",
				fields[i].name, fields[i].offset, actual, ret);
			return -ELIBBAD;
		}
	}
	return 0;
}

int nbdriver_task_layout_init(void)
{
	const struct btf **slot;
	const struct btf *btf;
	const struct btf_type *task_type, *thread_type;
	unsigned long offset = 0;
	s32 task_id, thread_id;
	int ret;
#define TASK_FIELD(name) { #name, offsetof(struct task_struct, name) }
	const struct layout_field task_fields[] = {
		TASK_FIELD(stack), TASK_FIELD(usage), TASK_FIELD(flags),
		TASK_FIELD(mm), TASK_FIELD(active_mm), TASK_FIELD(pid), TASK_FIELD(tgid),
		TASK_FIELD(group_leader), TASK_FIELD(start_time), TASK_FIELD(start_boottime),
		TASK_FIELD(real_cred), TASK_FIELD(cred), TASK_FIELD(comm),
		TASK_FIELD(files), TASK_FIELD(signal), TASK_FIELD(sighand),
		TASK_FIELD(blocked), TASK_FIELD(task_works), TASK_FIELD(alloc_lock),
	};
#undef TASK_FIELD
#define THREAD_FIELD(name) { #name, offsetof(struct thread_struct, name) }
	const struct layout_field thread_fields[] = {
		THREAD_FIELD(uw),
#ifdef CONFIG_ARM64_PTR_AUTH
		THREAD_FIELD(keys_user),
#endif
	};
#undef THREAD_FIELD

	slot = (const struct btf **)nbdriver_ksym_lookup_optional("btf_vmlinux");
	if (!slot) {
		nbd_info("stage=task_layout source=build_headers btf=unavailable\n");
		return 0;
	}
	btf = READ_ONCE(*slot);
	if (IS_ERR_OR_NULL(btf)) {
		nbd_info("stage=task_layout source=build_headers btf=disabled\n");
		return 0;
	}
	layout_find = (void *)nbdriver_ksym_lookup_optional("btf_find_by_name_kind");
	layout_type = (void *)nbdriver_ksym_lookup_optional("btf_type_by_id");
	layout_name = (void *)nbdriver_ksym_lookup_optional("btf_name_by_offset");
	if (!layout_find || !layout_type || !layout_name)
		return -EOPNOTSUPP;
	task_id = find_type(btf, "task_struct");
	thread_id = find_type(btf, "thread_struct");
	if (task_id <= 0 || thread_id <= 0)
		return -ELIBBAD;
	task_type = type_by_id(btf, task_id);
	thread_type = type_by_id(btf, thread_id);
	if (!task_type || !thread_type || thread_type->size != sizeof(struct thread_struct))
		return -ELIBBAD;
	ret = verify_fields(btf, task_type, task_fields, ARRAY_SIZE(task_fields));
	if (!ret)
		ret = verify_fields(btf, thread_type, thread_fields, ARRAY_SIZE(thread_fields));
	if (!ret)
		ret = member_offset(btf, task_type, "thread", &offset);
	if (ret)
		return ret;
	if ((offset & (__alignof__(struct thread_struct) - 1)) ||
	    offset > task_type->size || sizeof(struct thread_struct) > task_type->size - offset)
		return -ELIBBAD;
	nbdriver_task_thread_offset = offset;
	nbd_info("stage=task_layout source=kernel_btf status=verified task_size=%u thread_offset=%lu compiled_offset=%lu\n",
		task_type->size, offset, offsetof(struct task_struct, thread));
	return 0;
}
