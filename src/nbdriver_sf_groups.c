/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * 仅处理当前调用线程的固定 input/inet 补充组请求，保留已有组。
 * 校验初始用户命名空间、四个 system UID、组长 comm 和真实 SF 可执行路径。
 * 这里的真实 SF 校验不同于 nbdriver_sf_read_caller() 的 euid 读取门禁。
 * 不修改整个宿主 UID/SELinux，也不替已有线程批量更新凭据。
 * group_info 的分配引用与安装后的凭据引用分别持有、分别释放。
 */
#include <linux/cred.h>
#include <linux/atomic.h>
#include <linux/build_bug.h>
#include <linux/dcache.h>
#include <linux/errno.h>
#include <linux/file.h>
#include <linux/limits.h>
#include <linux/mm.h>
#include <linux/refcount.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/uidgid.h>
#include <linux/user_namespace.h>

#include "nbdriver_abi.h"
#include "nbdriver_cfi.h"
#include "nbdriver_log.h"
#include "nbdriver_ksym.h"
#include "nbdriver_sf_groups.h"

#define NBDRIVER_AID_SYSTEM 1000U
#define NBDRIVER_AID_INPUT  1004U
#define NBDRIVER_AID_INET   3003U

#define NBDRIVER_SF_EXE_PATH "/system/bin/surfaceflinger"
#define NBDRIVER_SF_COMM     "surfaceflinger"

typedef __typeof__(&get_task_exe_file) nbdriver_get_task_exe_file_fn;
typedef __typeof__(&groups_alloc) nbdriver_groups_alloc_fn;
typedef __typeof__(&groups_free) nbdriver_groups_free_fn;
typedef __typeof__(&groups_search) nbdriver_groups_search_fn;
typedef __typeof__(&groups_sort) nbdriver_groups_sort_fn;
typedef __typeof__(&set_current_groups) nbdriver_set_current_groups_fn;

static nbdriver_get_task_exe_file_fn nbdriver_get_task_exe_file_ptr;
static nbdriver_groups_alloc_fn nbdriver_groups_alloc_ptr;
static nbdriver_groups_free_fn nbdriver_groups_free_ptr;
static nbdriver_groups_search_fn nbdriver_groups_search_ptr;
static nbdriver_groups_sort_fn nbdriver_groups_sort_ptr;
static nbdriver_set_current_groups_fn nbdriver_set_current_groups_ptr;

static int nbdriver_sf_identity_resolve(void)
{
	nbdriver_get_task_exe_file_fn fn = READ_ONCE(nbdriver_get_task_exe_file_ptr);

	if (fn)
		return 0;
	fn = (nbdriver_get_task_exe_file_fn)
		nbdriver_ksym_lookup_optional("get_task_exe_file");
	if (!fn)
		return -ENOSYS;
	WRITE_ONCE(nbdriver_get_task_exe_file_ptr, fn);
	return 0;
}

static int nbdriver_sf_groups_resolve_symbols(void)
{
	int ret;

	if (nbdriver_get_task_exe_file_ptr && nbdriver_groups_alloc_ptr &&
		nbdriver_groups_free_ptr && nbdriver_groups_search_ptr &&
		nbdriver_groups_sort_ptr && nbdriver_set_current_groups_ptr)
		return 0;

	ret = nbdriver_sf_identity_resolve();
	if (ret)
		return ret;
	nbdriver_groups_alloc_ptr =
		(nbdriver_groups_alloc_fn)nbdriver_ksym_lookup_optional("groups_alloc");
	nbdriver_groups_free_ptr =
		(nbdriver_groups_free_fn)nbdriver_ksym_lookup_optional("groups_free");
	nbdriver_groups_search_ptr =
		(nbdriver_groups_search_fn)nbdriver_ksym_lookup_optional("groups_search");
	nbdriver_groups_sort_ptr =
		(nbdriver_groups_sort_fn)nbdriver_ksym_lookup_optional("groups_sort");
	nbdriver_set_current_groups_ptr =
		(nbdriver_set_current_groups_fn)nbdriver_ksym_lookup_optional("set_current_groups");

	return nbdriver_get_task_exe_file_ptr && nbdriver_groups_alloc_ptr &&
		nbdriver_groups_free_ptr && nbdriver_groups_search_ptr &&
		nbdriver_groups_sort_ptr && nbdriver_set_current_groups_ptr ? 0 : -ENOSYS;
}

static bool nbdriver_sf_system_credentials(const struct cred *cred)
{
	kuid_t system_uid = KUIDT_INIT(NBDRIVER_AID_SYSTEM);

	/* The initial user namespace is the only level-zero namespace and has
	 * no parent. Preserve that restriction without importing init_user_ns,
	 * which some vendor kernels do not export to modules. */
	return cred->user_ns && !cred->user_ns->parent &&
		cred->user_ns->level == 0 &&
		uid_eq(cred->uid, system_uid) &&
		uid_eq(cred->euid, system_uid) &&
		uid_eq(cred->suid, system_uid) &&
		uid_eq(cred->fsuid, system_uid);
}

static noinline bool NBDRIVER_NO_CFI nbdriver_is_surfaceflinger(void)
{
	struct file *exe_file;
	char leader_comm[TASK_COMM_LEN];
	char *path_buffer;
	char *path;
	bool matched = false;

	get_task_comm(leader_comm, current->group_leader);
	if (strcmp(leader_comm, NBDRIVER_SF_COMM) != 0)
		return false;

	exe_file = READ_ONCE(nbdriver_get_task_exe_file_ptr)(current->group_leader);
	if (!exe_file)
		return false;

	path_buffer = (char *)__get_free_page(GFP_KERNEL);
	if (!path_buffer)
		goto out_file;

	path = d_path(&exe_file->f_path, path_buffer, PAGE_SIZE);
	if (!IS_ERR(path) && strcmp(path, NBDRIVER_SF_EXE_PATH) == 0)
		matched = true;

	free_page((unsigned long)path_buffer);
out_file:
	fput(exe_file);
	return matched;
}

bool nbdriver_sf_control_caller(void)
{
	if (!nbdriver_sf_system_credentials(current_cred()) ||
	    nbdriver_sf_identity_resolve())
		return false;
	return nbdriver_is_surfaceflinger();
}

static noinline bool NBDRIVER_NO_CFI
nbdriver_has_group(const struct cred *cred, kgid_t gid)
{
	if (gid_eq(cred->fsgid, gid))
		return true;
	if (!cred->group_info)
		return false;
	return nbdriver_groups_search_ptr(cred->group_info, gid) != 0;
}

static noinline u32 NBDRIVER_NO_CFI nbdriver_present_groups(const struct cred *cred)
{
	u32 present = 0;

	if (nbdriver_has_group(cred, KGIDT_INIT(NBDRIVER_AID_INPUT)))
		present |= NBDRIVER_SF_GROUP_INPUT;
	if (nbdriver_has_group(cred, KGIDT_INIT(NBDRIVER_AID_INET)))
		present |= NBDRIVER_SF_GROUP_INET;
	return present;
}

static void nbdriver_log_group_state(const char *stage, u32 mask)
{
	const struct group_info *groups = current_cred()->group_info;
	int index;

	nbd_warn("stage=sf_groups state=%s mask=0x%x fsgid=%u ngroups=%u\n",
		stage, mask, __kgid_val(current_cred()->fsgid),
		groups ? groups->ngroups : 0U);
	if (!groups)
		return;
	for (index = 0; index < groups->ngroups && index < 16; ++index)
		nbd_warn("stage=sf_groups state=%s gid[%d]=%u\n", stage, index,
			__kgid_val(groups->gid[index]));
}

/* Keep the complete credential operation out of CFI/KCFI instrumentation.
 * These targets are found at runtime and are not module-linkable symbols. */
static noinline void NBDRIVER_NO_CFI
nbdriver_put_group_info(struct group_info *groups)
{
	bool last;

	/* Select by the actual header type, including vendor backports.  Only
	 * the selected expression is emitted, preserving refcount saturation. */
	BUILD_BUG_ON(!__builtin_types_compatible_p(__typeof__(groups->usage), atomic_t) &&
		     !__builtin_types_compatible_p(__typeof__(groups->usage), refcount_t));
	last = __builtin_choose_expr(
		__builtin_types_compatible_p(__typeof__(groups->usage), refcount_t),
		refcount_dec_and_test((refcount_t *)&groups->usage),
		atomic_dec_and_test((atomic_t *)&groups->usage));
	if (last)
		nbdriver_groups_free_ptr(groups);
}

static noinline int NBDRIVER_NO_CFI
nbdriver_append_groups(u32 missing)
{
	const struct cred *cred = current_cred();
	const struct group_info *old_groups = cred->group_info;
	struct group_info *new_groups;
	int add_count = 0;
	int index;
	int ret;

	if (missing & NBDRIVER_SF_GROUP_INPUT)
		++add_count;
	if (missing & NBDRIVER_SF_GROUP_INET)
		++add_count;
	if (add_count == 0)
		return 0;
	if (old_groups->ngroups > NGROUPS_MAX - add_count)
		return -E2BIG;

	new_groups = nbdriver_groups_alloc_ptr(old_groups->ngroups + add_count);
	if (!new_groups)
		return -ENOMEM;

	for (index = 0; index < old_groups->ngroups; ++index)
		new_groups->gid[index] = old_groups->gid[index];
	if (missing & NBDRIVER_SF_GROUP_INPUT)
		new_groups->gid[index++] = KGIDT_INIT(NBDRIVER_AID_INPUT);
	if (missing & NBDRIVER_SF_GROUP_INET)
		new_groups->gid[index++] = KGIDT_INIT(NBDRIVER_AID_INET);

	nbdriver_groups_sort_ptr(new_groups);
	ret = nbdriver_set_current_groups_ptr(new_groups);
	/* Match the in-kernel setgroups path exactly. set_groups() takes an
	 * additional reference on success; the caller still drops its original
	 * allocation reference on both success and failure. Calling groups_free()
	 * directly here would leave current->cred->group_info dangling. */
	nbdriver_put_group_info(new_groups);
	return ret;
}

long nbdriver_sf_groups_ioctl(unsigned long argument)
{
	struct nbdriver_sf_groups_request request;
	u32 before;
	u32 missing;
	int ret;

	if (!argument)
		return -EFAULT;
	if (copy_from_user(&request, (void __user *)argument,
			   sizeof(request)))
		return -EFAULT;
	if (request.abi_version != NBDRIVER_SF_GROUPS_ABI_VERSION ||
		request.reserved || request.out_present || request.out_added ||
		request.out_group_count || request.out_reserved[0] ||
		request.out_reserved[1] || request.out_reserved[2])
		return -EINVAL;

	ret = nbdriver_sf_groups_resolve_symbols();
	if (ret)
		return ret;
	if (!nbdriver_sf_control_caller())
		return -EACCES;

	before = nbdriver_present_groups(current_cred());
	nbdriver_log_group_state("before", before);
	missing = NBDRIVER_SF_GROUP_REQUIRED & ~before;
	ret = nbdriver_append_groups(missing);
	if (ret) {
		nbd_warn("stage=sf_groups operation=set_current_groups error=%d\n", ret);
		return ret;
	}

	request.out_present = nbdriver_present_groups(current_cred());
	nbdriver_log_group_state("after", request.out_present);
	request.out_added = request.out_present & ~before;
	request.out_group_count = current_cred()->group_info->ngroups;
	if (request.out_present != NBDRIVER_SF_GROUP_REQUIRED)
		return -EIO;
	if (copy_to_user((void __user *)argument, &request,
			 sizeof(request)))
		return -EFAULT;

	nbd_info("stage=sf_groups pid=%d tid=%d present=0x%x added=0x%x groups=%u\n",
		task_tgid_nr(current), task_pid_nr(current), request.out_present,
		request.out_added, request.out_group_count);
	return 0;
}
