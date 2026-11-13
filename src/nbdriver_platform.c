/* ===== BEGIN src/platform/nbdriver_entry.c ===== */
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
/* ===== END src/platform/nbdriver_entry.c ===== */

/* ===== BEGIN src/platform/nbdriver_entry_reboot.c ===== */
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

/*
 * pre_handler 运行在探测上下文，禁止睡眠，不能在其中建 FD。
 * 因此只把工作排进 task_work，由进程上下文完成。
 */

/*
 * task_work_add 在 5.10/5.15/6.1/6.6 的 Module.symvers 中未导出，
 * 静态引用会在 .ko 里留下未定义符号，insmod 直接失败。
 * 因此只保留函数指针，通过 kallsyms 运行时解析。
 * 必须使用目标内核声明的完整类型：将枚举参数写为 int 虽然宽度相同，
 * 在 KCFI 下却有不同的类型哈希，会在间接调用前触发 BRK 并 panic。
 * typeof 只用于获取类型，不产生对未导出 task_work_add 的静态引用。
 */
typedef __typeof__(&task_work_add) nbdriver_task_work_add_fn_t;

struct nbdriver_fd_work {
	struct callback_head callback;
	int __user *output_fd;
};

static struct kprobe nbdriver_probe;
static const char *nbdriver_probe_symbol;
static nbdriver_task_work_add_fn_t nbdriver_task_work_add_fn;

/* lsdriver-style exemption only at the dynamically resolved API call. */
static noinline int NBDRIVER_NO_CFI
nbdriver_queue_task_work(struct task_struct *task, struct callback_head *work)
{
	return nbdriver_task_work_add_fn(task, work, TWA_RESUME);
}

/* A direct BL from the naked callback below needs a stable LTO-visible symbol. */
__visible __used noinline struct module *
nbdriver_fd_work_run(struct callback_head *head);

__visible __used noinline struct module *
nbdriver_fd_work_run(struct callback_head *head)
{
	struct nbdriver_fd_work *work =
		container_of(head, struct nbdriver_fd_work, callback);
	int ret = -EPERM;

	/* 这里已经是正常进程上下文，current 可信，可以做权限检查。 */
	if (!uid_eq(current_euid(), GLOBAL_ROOT_UID)) {
		nbd_warn("stage=fd_work reason=not_root error=%d\n", -EPERM);
		goto out;
	}

	ret = nbdriver_comm_install_fd(work->output_fd);
	if (ret)
		nbd_err("stage=fd_work operation=install_fd error=%d\n", ret);

out:
	/* Additive bootstrap error channel; old SDKs still treat any negative
	 * output as failure. copy_to_user runs only in this task context. */
	if (ret && work->output_fd &&
	    put_user(ret, work->output_fd))
		nbd_warn("stage=fd_work reason=error_reply_fault error=%d\n", -EFAULT);
	kfree(work);
	return THIS_MODULE;
}

/*
 * task_work_run invokes this with the ordinary task_work_func_t prototype,
 * preserving callback CFI metadata. Restore the caller's frame and LR before
 * branching directly into the exported core-kernel module_put(). Its RET
 * returns to task_work_run, so releasing the last reference cannot leave a
 * return address in module text. No compiler tail-call optimization is needed.
 * PAC/BTI use architectural HINT encodings on CPUs without those features.
 */
static __naked void nbdriver_fd_work_fn(struct callback_head *head)
{
	asm volatile(
		".inst 0xd503245f\n" /* bti c */
		".inst 0xd503233f\n" /* paciasp */
		"stp x29, x30, [sp, #-16]!\n"
		"mov x29, sp\n"
		"bl nbdriver_fd_work_run\n"
		"ldp x29, x30, [sp], #16\n"
		".inst 0xd50323bf\n" /* autiasp */
		"b module_put\n"
	);
}

static int nbdriver_schedule_fd(int __user *output_fd)
{
	struct nbdriver_fd_work *work;
	int ret;

	work = kzalloc(sizeof(*work), GFP_ATOMIC);
	if (!work) {
		nbd_err("stage=fd_schedule operation=kzalloc error=%d\n", -ENOMEM);
		return -ENOMEM;
	}

	if (!try_module_get(THIS_MODULE)) {
		nbd_err("stage=fd_schedule operation=try_module_get error=%d\n",
			-ENODEV);
		kfree(work);
		return -ENODEV;
	}

	work->output_fd = output_fd;
	init_task_work(&work->callback, nbdriver_fd_work_fn);

	ret = nbdriver_queue_task_work(current, &work->callback);
	if (ret) {
		nbd_err("stage=fd_schedule operation=task_work_add error=%d\n", ret);
		module_put(THIS_MODULE);
		kfree(work);
		return ret;
	}

	return 0;
}

/*
 * __arm64_sys_reboot 由 SYSCALL_DEFINE 生成，形参是 const struct pt_regs *，
 * 实参在 syscall_regs->regs[0..5]；见 arch/arm64/include/asm/syscall_wrapper.h
 * 的 SC_ARM64_REGS_TO_ARGS。
 *
 * 权限检查留在 task_work 回调，保持探针处理路径最小。
 * 本次 pstore 与原件反汇编对应的是 task_work_add 的 KCFI 类型检查陷阱，
 * 不能据此将崩溃归因于 current_euid()；task_struct 布局仍需匹配目标内核。
 */
static int nbdriver_reboot_pre(struct kprobe *probe, struct pt_regs *regs)
{
	const struct pt_regs *syscall_regs;
	int ret;

	(void)probe;

	syscall_regs = (const struct pt_regs *)(unsigned long)regs->regs[0];
	if (!syscall_regs)
		return 0;

	if ((u32)syscall_regs->regs[0] != NBDRIVER_INSTALL_MAGIC1 ||
	    (u32)syscall_regs->regs[1] != NBDRIVER_INSTALL_MAGIC2)
		return 0;

	ret = nbdriver_schedule_fd(
		(int __user *)(unsigned long)syscall_regs->regs[3]);
	if (ret)
		return 0;

	/*
	 * 不跳过 reboot 本体：让它带着非法 magic 自行返回 -EINVAL，
	 * 避免手工构造 syscall 返回路径。
	 */
	return 0;
}

static int nbdriver_register_probe(const char *symbol)
{
	int ret;

	memset(&nbdriver_probe, 0, sizeof(nbdriver_probe));
	nbdriver_probe.symbol_name = symbol;
	nbdriver_probe.pre_handler = nbdriver_reboot_pre;

	ret = register_kprobe(&nbdriver_probe);
	if (ret) {
		nbd_warn("stage=reboot_probe operation=register_kprobe symbol=%s error=%d\n",
			 symbol, ret);
		return ret;
	}

	nbdriver_probe_symbol = symbol;
	nbd_info("stage=reboot_probe symbol=%s status=registered\n", symbol);
	return 0;
}

int nbdriver_reboot_entry_init(void)
{
	int ret;
	unsigned long address;

	nbdriver_task_work_add_fn =
		(nbdriver_task_work_add_fn_t)nbdriver_ksym_lookup("task_work_add");
	if (!nbdriver_task_work_add_fn) {
		/*
		 * 该符号在 5.10/5.15/6.1/6.6 的 Module.symvers 里未导出，
		 * 6.12/6.18 才是 EXPORT_SYMBOL_GPL。不能静态链接，只能运行时解析。
		 */
		nbdriver_log_missing_once("entry_init", "task_work_add", -ENOENT);
		return -ENOENT;
	}

	address = nbdriver_ksym_lookup("__arm64_sys_reboot");
	if (!address) {
		nbdriver_log_missing_once("entry_init", "__arm64_sys_reboot",
					  -ENOENT);
		return -ENOENT;
	}

	ret = nbdriver_register_probe("__arm64_sys_reboot");
	if (ret)
		return ret;

	nbd_info("stage=entry_init path=reboot_probe status=ready\n");
	return 0;
}

void nbdriver_reboot_entry_exit(void)
{
	if (!nbdriver_probe_symbol)
		return;

	unregister_kprobe(&nbdriver_probe);
	memset(&nbdriver_probe, 0, sizeof(nbdriver_probe));
	nbdriver_probe_symbol = NULL;
	nbdriver_task_work_add_fn = NULL;
}

const char *nbdriver_reboot_entry_target(void)
{
	return nbdriver_probe_symbol;
}
/* ===== END src/platform/nbdriver_entry_reboot.c ===== */

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

