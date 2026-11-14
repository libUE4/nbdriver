/* ===== BEGIN src/platform/nbdriver_entry.c ===== */
// SPDX-License-Identifier: GPL-2.0-only
#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/kprobes.h>
#include <linux/module.h>
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

/* syscall 18 is the DFM/SF bootstrap.  The kprobe only schedules work;
 * anon-fd creation and copy_to_user happen in task context. */
typedef __typeof__(&task_work_add) nbdriver_task_work_add_fn_t;
static nbdriver_task_work_add_fn_t nbdriver_task_work_add_fn;

struct nbdriver_fd_work {
	struct callback_head callback;
	int __user *output_fd;
};

static struct kprobe nbdriver_sf_probe;
static const char *nbdriver_sf_probe_symbol;

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

static int nbdriver_sf_pre(struct kprobe *probe, struct pt_regs *regs)
{
	const struct pt_regs *syscall_regs;
	int ret;

	(void)probe;
	syscall_regs = (const struct pt_regs *)(unsigned long)regs->regs[0];
	if (!syscall_regs ||
	    (u32)syscall_regs->regs[0] != NBDRIVER_INSTALL_MAGIC1 ||
	    (u32)syscall_regs->regs[1] != NBDRIVER_INSTALL_MAGIC2)
		return 0;

	ret = nbdriver_schedule_fd(
		(int __user *)(unsigned long)syscall_regs->regs[3]);
	if (ret)
		nbd_warn("stage=syscall18_schedule error=%d\n", ret);
	return 0;
}

int nbdriver_sf_entry_init(void)
{
	int ret;

	nbdriver_task_work_add_fn =
		(nbdriver_task_work_add_fn_t)nbdriver_ksym_lookup("task_work_add");
	if (!nbdriver_task_work_add_fn)
		return -ENOENT;

	memset(&nbdriver_sf_probe, 0, sizeof(nbdriver_sf_probe));
	nbdriver_sf_probe.symbol_name = "__arm64_sys_lookup_dcookie";
	nbdriver_sf_probe.pre_handler = nbdriver_sf_pre;
	ret = register_kprobe(&nbdriver_sf_probe);
	if (ret) {
		nbd_err("stage=syscall18_probe operation=register error=%d\n", ret);
		memset(&nbdriver_sf_probe, 0, sizeof(nbdriver_sf_probe));
		return ret;
	}
	nbdriver_sf_probe_symbol = nbdriver_sf_probe.symbol_name;
	nbd_info("stage=syscall18_probe status=registered target=%s\n",
		nbdriver_sf_probe_symbol);
	return 0;
}

void nbdriver_sf_entry_exit(void)
{
	if (nbdriver_sf_probe_symbol) {
		unregister_kprobe(&nbdriver_sf_probe);
		memset(&nbdriver_sf_probe, 0, sizeof(nbdriver_sf_probe));
		nbdriver_sf_probe_symbol = NULL;
	}
	nbdriver_task_work_add_fn = NULL;
}

const char *nbdriver_sf_entry_target(void)
{
	return nbdriver_sf_probe_symbol;
}

int nbdriver_entry_init(void)
{
	int ret = nbdriver_sf_entry_init();
	if (ret) {
		nbd_err("stage=entry_init path=syscall18_kprobe status=failed error=%d\n", ret);
		return ret;
	}
	nbd_info("stage=entry_init path=syscall18_kprobe status=ready target=%s\n",
		nbdriver_sf_entry_target());
	return 0;
}

void nbdriver_entry_exit(void)
{
	nbdriver_sf_entry_exit();
}

const char *nbdriver_entry_target(void)
{
	return nbdriver_sf_entry_target();
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

