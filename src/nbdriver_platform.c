/* ===== BEGIN src/platform/nbdriver_entry.c ===== */
// SPDX-License-Identifier: GPL-2.0-only
#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/kprobes.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/stop_machine.h>
#include <linux/vmalloc.h>
#include <asm/unistd.h>
#include <linux/ptrace.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/task_work.h>
#include <linux/types.h>
#include <linux/uaccess.h>

#include "nbdriver_abi.h"
#include "nbdriver_comm.h"
#include "nbdriver_entry.h"
#include "nbdriver_log.h"
#include "nbdriver_ksym.h"
#include "nbdriver_cfi.h"

/* Both connection entries only schedule work;
 * anon-fd creation and copy_to_user happen in task context. */
typedef __typeof__(&task_work_add) nbdriver_task_work_add_fn_t;
static nbdriver_task_work_add_fn_t nbdriver_task_work_add_fn;

struct nbdriver_fd_work {
	struct callback_head callback;
	int __user *output_fd;
};


static noinline int NBDRIVER_NO_CFI
nbdriver_queue_task_work(struct task_struct *task, struct callback_head *work)
{
	return nbdriver_task_work_add_fn(task, work, TWA_RESUME);
}

__visible __used noinline struct module *
nbdriver_fd_work_run(struct callback_head *head)
{
	struct nbdriver_fd_work *work =
		container_of(head, struct nbdriver_fd_work, callback);
	int ret = -EPERM;

	/* DFM reference behavior: root and Android system (uid 1000) may obtain
	 * the anonymous fd.  Read-only ioctl gates below apply the same identity. */
	if (!uid_eq(current_euid(), GLOBAL_ROOT_UID) &&
	    !uid_eq(current_euid(), KUIDT_INIT(1000))) {
		nbd_warn("stage=fd_work reason=uid_not_allowed error=%d\n", -EPERM);
		goto out;
	}

	ret = nbdriver_comm_install_fd(work->output_fd);
	if (ret)
		nbd_err("stage=fd_work operation=install_fd error=%d\n", ret);

out:
	if (ret && work->output_fd &&
	    put_user(ret, work->output_fd))
		nbd_warn("stage=fd_work reason=error_reply_fault error=%d\n", -EFAULT);
	kfree(work);
	return THIS_MODULE;
}

static __naked void nbdriver_fd_work_fn(struct callback_head *head)
{
	asm volatile(
		".inst 0xd503245f\n"
		".inst 0xd503233f\n"
		"stp x29, x30, [sp, #-16]!\n"
		"mov x29, sp\n"
		"bl nbdriver_fd_work_run\n"
		"ldp x29, x30, [sp], #16\n"
		".inst 0xd50323bf\n"
		"b module_put\n"
	);
}

static int nbdriver_schedule_fd(int __user *output_fd)
{
	struct nbdriver_fd_work *work;
	int ret;

	work = kzalloc(sizeof(*work), GFP_ATOMIC);
	if (!work)
		return -ENOMEM;
	if (!try_module_get(THIS_MODULE)) {
		kfree(work);
		return -ENODEV;
	}
	work->output_fd = output_fd;
	init_task_work(&work->callback, nbdriver_fd_work_fn);
	ret = nbdriver_queue_task_work(current, &work->callback);
	if (ret) {
		module_put(THIS_MODULE);
		kfree(work);
		return ret;
	}
	return 0;
}

/* The table entry is optional. All internals are resolved at runtime; no
 * unexported symbol becomes a module import. Keep a writable alias only for
 * the selected, aligned pointer, and patch while other CPUs are stopped. */
typedef asmlinkage long (*nbdriver_syscall_fn)(const struct pt_regs *);
typedef int (*nbdriver_stop_machine_fn)(int (*)(void *), void *, const struct cpumask *);
typedef long (*nbdriver_kernel_copy_fn)(void *, const void *, size_t);
typedef void (*nbdriver_tasks_sync_fn)(void);

static nbdriver_syscall_fn *nbdriver_syscall_table;
static nbdriver_syscall_fn nbdriver_syscall_original;
static nbdriver_syscall_fn *nbdriver_syscall_alias;
static void *nbdriver_syscall_mapping;
static nbdriver_stop_machine_fn nbdriver_stop_machine_ptr;
static nbdriver_kernel_copy_fn nbdriver_kernel_read_ptr;
static nbdriver_kernel_copy_fn nbdriver_kernel_write_ptr;
static nbdriver_tasks_sync_fn nbdriver_tasks_sync_ptr;
static int nbdriver_syscall_number = -1;
static bool nbdriver_table_pinned;
static bool nbdriver_table_supported;
static bool nbdriver_reboot_active;
static DEFINE_MUTEX(nbdriver_entry_lock);

static bool nbdriver_install_magic(const struct pt_regs *regs)
{
	return regs && (u32)regs->regs[0] == NBDRIVER_INSTALL_MAGIC1 &&
		(u32)regs->regs[1] == NBDRIVER_INSTALL_MAGIC2;
}

static asmlinkage long nbdriver_syscall_install(const struct pt_regs *regs)
{
	if (!nbdriver_install_magic(regs) ||
	    regs->syscallno != READ_ONCE(nbdriver_syscall_number))
		return -ENOSYS;
	return nbdriver_schedule_fd((int __user *)(unsigned long)regs->regs[3]);
}

struct nbdriver_table_patch {
	nbdriver_syscall_fn expected;
	nbdriver_syscall_fn replacement;
};

static noinline int NBDRIVER_NO_CFI nbdriver_table_patch_cpu(void *argument)
{
	struct nbdriver_table_patch *patch = argument;
	nbdriver_syscall_fn current_fn;
	int ret;

	ret = nbdriver_kernel_read_ptr(&current_fn,
		&nbdriver_syscall_table[nbdriver_syscall_number], sizeof(current_fn));
	if (ret)
		return ret;
	/* Never overwrite another owner's entry, including during restoration. */
	if (current_fn != patch->expected)
		return -ESTALE;
	ret = nbdriver_kernel_write_ptr(nbdriver_syscall_alias,
		&patch->replacement, sizeof(patch->replacement));
	if (ret)
		return ret;
	smp_wmb();
	return 0;
}

static noinline int NBDRIVER_NO_CFI
nbdriver_table_replace(nbdriver_syscall_fn expected, nbdriver_syscall_fn replacement)
{
	struct nbdriver_table_patch patch = { expected, replacement };

	return nbdriver_stop_machine_ptr(nbdriver_table_patch_cpu, &patch, NULL);
}

static noinline void NBDRIVER_NO_CFI nbdriver_table_wait(void)
{
	/* Wait until tasks which fetched the old entry have left module text. */
	nbdriver_tasks_sync_ptr();
}

static noinline int NBDRIVER_NO_CFI nbdriver_table_init(void)
{
	static const char *const ni_names[] = {
		"__arm64_sys_ni_syscall.cfi_jt", "__arm64_sys_ni_syscall",
		"sys_ni_syscall.cfi_jt", "sys_ni_syscall",
	};
	nbdriver_syscall_fn ni[ARRAY_SIZE(ni_names)], candidate;
	unsigned long image_offset_address, image_offset;
	unsigned long address, physical;
	struct page *page;
	unsigned int index, n;
	int ret;

	if (nbdriver_table_pinned)
		return 0;
	nbdriver_syscall_table = (nbdriver_syscall_fn *)
		nbdriver_ksym_lookup_optional("sys_call_table");
	nbdriver_stop_machine_ptr = (nbdriver_stop_machine_fn)
		nbdriver_ksym_lookup_optional("stop_machine");
	nbdriver_kernel_read_ptr = (nbdriver_kernel_copy_fn)
		nbdriver_ksym_lookup_optional("copy_from_kernel_nofault");
	nbdriver_kernel_write_ptr = (nbdriver_kernel_copy_fn)
		nbdriver_ksym_lookup_optional("copy_to_kernel_nofault");
	nbdriver_tasks_sync_ptr = (nbdriver_tasks_sync_fn)
		nbdriver_ksym_lookup_optional("synchronize_rcu_tasks");
	if (!nbdriver_tasks_sync_ptr)
		nbdriver_tasks_sync_ptr = (nbdriver_tasks_sync_fn)
			nbdriver_ksym_lookup_optional("synchronize_rcu_tasks_rude");
	image_offset_address = nbdriver_ksym_lookup_optional("kimage_voffset");
	if (!nbdriver_syscall_table || !nbdriver_stop_machine_ptr ||
	    !nbdriver_kernel_read_ptr || !nbdriver_kernel_write_ptr ||
	    !nbdriver_tasks_sync_ptr || !image_offset_address) {
		nbd_warn("stage=syscall_table reason=missing_dependency table=%u stop=%u read=%u write=%u sync=%u image=%u\n",
			!!nbdriver_syscall_table, !!nbdriver_stop_machine_ptr,
			!!nbdriver_kernel_read_ptr, !!nbdriver_kernel_write_ptr,
			!!nbdriver_tasks_sync_ptr, !!image_offset_address);
		return -ENOENT;
	}
	for (n = 0; n < ARRAY_SIZE(ni_names); ++n)
		ni[n] = (nbdriver_syscall_fn)nbdriver_ksym_lookup_optional(ni_names[n]);
	/* Only select slots in the SDK's search domain; a later unused slot
	 * outside that domain would create a loaded but unreachable driver. */
	for (index = 0; index < NBDRIVER_INSTALL_CANDIDATE_COUNT; ++index) {
		unsigned int number = nbdriver_install_candidate(index);
		if (number >= __NR_syscalls)
			continue;
		ret = nbdriver_kernel_read_ptr(&candidate,
			&nbdriver_syscall_table[number], sizeof(candidate));
		if (ret)
			return ret;
		for (n = 0; n < ARRAY_SIZE(ni_names); ++n)
			if (ni[n] && candidate == ni[n])
				break;
		if (n == ARRAY_SIZE(ni_names))
			continue;
		nbdriver_syscall_original = candidate;
		nbdriver_syscall_number = number;
		break;
	}
	if (index == NBDRIVER_INSTALL_CANDIDATE_COUNT)
		return -ENOSPC;
	ret = nbdriver_kernel_read_ptr(&image_offset,
		(const void *)image_offset_address, sizeof(image_offset));
	if (ret)
		goto clear;
	address = (unsigned long)&nbdriver_syscall_table[nbdriver_syscall_number];
	physical = address - image_offset;
	if (!IS_ALIGNED(address, sizeof(candidate)) || !pfn_valid(PHYS_PFN(physical))) {
		ret = -EFAULT;
		goto clear;
	}
	page = pfn_to_page(PHYS_PFN(physical));
	nbdriver_syscall_mapping = vmap(&page, 1, VM_MAP, PAGE_KERNEL);
	if (!nbdriver_syscall_mapping) {
		ret = -ENOMEM;
		goto clear;
	}
	nbdriver_syscall_alias = (nbdriver_syscall_fn *)
		((char *)nbdriver_syscall_mapping + offset_in_page(physical));
	/* An active table entry owns a module reference. Normal rmmod therefore
	 * fails EBUSY until authenticated QUIESCE restores the slot. A failed
	 * restore can return an error instead of freeing live module text. */
	if (!try_module_get(THIS_MODULE)) {
		ret = -ENODEV;
		goto unmap;
	}
	ret = nbdriver_table_replace(nbdriver_syscall_original, nbdriver_syscall_install);
	if (ret) {
		module_put(THIS_MODULE);
		goto unmap;
	}
	nbdriver_table_pinned = true;
	nbdriver_table_supported = true;
	nbd_info("stage=syscall_table status=registered nr=%d unload=quiesce_first\n",
		nbdriver_syscall_number);
	return 0;
unmap:
	vunmap(nbdriver_syscall_mapping);
	nbdriver_syscall_mapping = NULL;
	nbdriver_syscall_alias = NULL;
clear:
	nbdriver_syscall_number = -1;
	nbdriver_syscall_original = NULL;
	return ret;
}

static int nbdriver_table_remove(void)
{
	int ret;

	if (!nbdriver_table_pinned)
		return 0;
	ret = nbdriver_table_replace(nbdriver_syscall_install, nbdriver_syscall_original);
	if (ret) {
		nbd_err("stage=syscall_table operation=restore nr=%d error=%d module=retained\n",
			nbdriver_syscall_number, ret);
		return ret;
	}
	nbdriver_table_wait();
	nbd_info("stage=syscall_table status=restored nr=%d\n", nbdriver_syscall_number);
	vunmap(nbdriver_syscall_mapping);
	nbdriver_syscall_mapping = NULL;
	nbdriver_syscall_alias = NULL;
	nbdriver_syscall_number = -1;
	nbdriver_syscall_original = NULL;
	nbdriver_table_pinned = false;
	module_put(THIS_MODULE);
	return 0;
}

static int nbdriver_reboot_pre(struct kprobe *probe, struct pt_regs *regs)
{
	const struct pt_regs *syscall_regs;
	int ret;

	(void)probe;
	syscall_regs = (const struct pt_regs *)(unsigned long)regs->regs[0];
	if (!nbdriver_install_magic(syscall_regs) ||
	    syscall_regs->syscallno != NBDRIVER_INSTALL_REBOOT_SYSCALL)
		return 0;
	ret = nbdriver_schedule_fd((int __user *)(unsigned long)syscall_regs->regs[3]);
	if (ret)
		nbd_warn("stage=reboot_schedule error=%d\n", ret);
	return 0;
}

static struct kprobe nbdriver_reboot_probe = {
	.symbol_name = "__arm64_sys_reboot",
	.pre_handler = nbdriver_reboot_pre,
};

int nbdriver_entry_init(void)
{
	int table_ret, reboot_ret;

	nbdriver_task_work_add_fn = (nbdriver_task_work_add_fn_t)
		nbdriver_ksym_lookup("task_work_add");
	if (!nbdriver_task_work_add_fn)
		return -ENOENT;
	table_ret = nbdriver_table_init();
	if (table_ret)
		nbd_warn("stage=entry_init path=syscall_table error=%d\n", table_ret);
	reboot_ret = register_kprobe(&nbdriver_reboot_probe);
	if (!reboot_ret)
		nbdriver_reboot_active = true;
	else
		nbd_warn("stage=entry_init path=reboot_probe error=%d\n", reboot_ret);
	if (table_ret && reboot_ret) {
		nbd_err("stage=entry_init reason=no_entry table_error=%d reboot_error=%d\n",
			table_ret, reboot_ret);
		return reboot_ret;
	}
	nbd_info("stage=entry_init status=ready syscall_nr=%d reboot=%u\n",
		nbdriver_syscall_number, nbdriver_reboot_active);
	return 0;
}

int nbdriver_entry_quiesce(void)
{
	int ret;

	mutex_lock(&nbdriver_entry_lock);
	ret = nbdriver_table_remove();
	mutex_unlock(&nbdriver_entry_lock);
	return ret;
}

int nbdriver_entry_resume(void)
{
	int ret = 0;

	mutex_lock(&nbdriver_entry_lock);
	if (nbdriver_table_supported)
		ret = nbdriver_table_init();
	if (ret && nbdriver_reboot_active) {
		nbd_warn("stage=entry_resume path=reboot_only table_error=%d\n", ret);
		ret = 0;
	}
	mutex_unlock(&nbdriver_entry_lock);
	return ret;
}

void nbdriver_entry_exit(void)
{
	/* An installed table entry pins the module; QUIESCE must detach it
	 * before normal module_exit can execute. Initialization installs it last. */
	WARN_ON_ONCE(nbdriver_table_pinned);
	if (nbdriver_reboot_active) {
		unregister_kprobe(&nbdriver_reboot_probe);
		nbdriver_reboot_active = false;
	}
	nbdriver_task_work_add_fn = NULL;
}

u32 nbdriver_entry_probe_mask(void)
{
	return (READ_ONCE(nbdriver_table_pinned) ? 1U : 0U) |
		(READ_ONCE(nbdriver_reboot_active) ? 2U : 0U);
}

const char *nbdriver_entry_target(void)
{
	return READ_ONCE(nbdriver_table_pinned) ? "sys_call_table" :
		READ_ONCE(nbdriver_reboot_active) ? "__arm64_sys_reboot" : "none";
}
/* ===== END src/platform/nbdriver_entry.c ===== */

/* ===== BEGIN src/platform/nbdriver_ksym.c ===== */
// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include <linux/kprobes.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/types.h>

#include "nbdriver_ksym.h"
#include "nbdriver_log.h"
#include "nbdriver_cfi.h"

#ifndef KSYM_NAME_LEN
#define KSYM_NAME_LEN 128
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

noinline unsigned long NBDRIVER_NO_CFI nbdriver_ksym_lookup(const char *name)
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

/* Optional feature discovery can be frequent; absence is not an init error. */
noinline unsigned long NBDRIVER_NO_CFI nbdriver_ksym_lookup_optional(const char *name)
{
	if (!name || !name[0] || !nbdriver_lookup_fn)
		return 0;
	return nbdriver_lookup_fn(name);
}
/* ===== END src/platform/nbdriver_ksym.c ===== */

/* ===== BEGIN src/platform/nbdriver_cfi.c ===== */
/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * CFI compatibility port from workspace lsdriver/export_fun.h and Makefile.
 * Use only local call-site exemptions for dynamically resolved functions;
 * retain callback type metadata and never patch global kernel CFI slowpaths.
 */
#include <linux/bug.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/types.h>

#include "nbdriver_cfi.h"
#include "nbdriver_ksym.h"
#include "nbdriver_log.h"

#ifdef NBDRIVER_LEGACY_CFI
/* Preserve a fail-closed local runtime for legacy toolchains whose KMI does
 * not export the abort handler. No kernel CFI instructions are modified. */
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
#endif

int nbdriver_cfi_init(void)
{
	nbd_info("stage=cfi_init mode=local_calls callback_cfi=preserved kernel_text=unchanged\n");
	return 0;
}

void nbdriver_cfi_exit(void)
{
	/* All exemptions are module-local; there is no global state to restore. */
}
/* ===== END src/platform/nbdriver_cfi.c ===== */

/* ===== BEGIN src/platform/nbdriver_log.c ===== */
// SPDX-License-Identifier: GPL-2.0-only
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#include "nbdriver_log.h"

/*
 * 刷入失败的现场通常只能拿到 dmesg。符号缺失是最常见的失败原因，
 * 但同一个符号可能在初始化里被查多次，这里只报首次。
 */
static char nbdriver_reported[16][KSYM_NAME_LEN];
static unsigned int nbdriver_reported_count;
static DEFINE_SPINLOCK(nbdriver_reported_lock);

void nbdriver_log_missing_once(const char *stage, const char *symbol, int error)
{
	const char *name = symbol ? symbol : "(null)";
	unsigned long flags;
	unsigned int i;

	spin_lock_irqsave(&nbdriver_reported_lock, flags);
	for (i = 0; i < nbdriver_reported_count; i++) {
		if (!strcmp(nbdriver_reported[i], name)) {
			spin_unlock_irqrestore(&nbdriver_reported_lock, flags);
			return;
		}
	}
	if (nbdriver_reported_count < ARRAY_SIZE(nbdriver_reported)) {
		snprintf(nbdriver_reported[nbdriver_reported_count],
			 sizeof(nbdriver_reported[0]), "%s", name);
		++nbdriver_reported_count;
	}
	spin_unlock_irqrestore(&nbdriver_reported_lock, flags);
	nbd_err("stage=%s symbol=%s reason=not_found error=%d\n",
		stage ? stage : "(null)", name, error);
}
/* ===== END src/platform/nbdriver_log.c ===== */
