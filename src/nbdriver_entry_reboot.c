// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include <linux/cred.h>
#include <linux/kprobes.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/task_work.h>
#include <linux/uaccess.h>

#include <asm/ptrace.h>

#include "nbdriver_abi.h"
#include "nbdriver_comm.h"
#include "nbdriver_entry.h"

struct nbdriver_fd_task_work {
	struct callback_head callback;
	int __user *output_fd;
};

static struct kprobe nbdriver_reboot_probe;
static bool nbdriver_reboot_probe_registered;

static void nbdriver_install_fd_work(struct callback_head *head)
{
	struct nbdriver_fd_task_work *work =
		container_of(head, struct nbdriver_fd_task_work, callback);
	int ret;

	ret = nbdriver_comm_install_fd_to_user(work->output_fd);
	if (ret)
		pr_warn("nbdriver: stage=fd_install status=failed error=%d\n", ret);
	kfree(work);
	module_put(THIS_MODULE);
}

static int nbdriver_schedule_fd_install(const struct pt_regs *syscall_regs)
{
	struct nbdriver_fd_task_work *work;
	int ret;

	work = kzalloc(sizeof(*work), GFP_ATOMIC);
	if (!work)
		return -ENOMEM;
	if (!try_module_get(THIS_MODULE)) {
		kfree(work);
		return -ENODEV;
	}

	work->output_fd = (int __user *)(unsigned long)syscall_regs->regs[3];
	init_task_work(&work->callback, nbdriver_install_fd_work);
	ret = task_work_add(current, &work->callback, TWA_RESUME);
	if (ret) {
		module_put(THIS_MODULE);
		kfree(work);
		return ret;
	}

	return 0;
}

/* __arm64_sys_* receives the saved syscall registers in x0. */
static int nbdriver_reboot_wrapper_pre(struct kprobe *probe,
				       struct pt_regs *regs)
{
	const struct pt_regs *syscall_regs;
	int ret;

	(void)probe;
	if (!uid_eq(current_euid(), GLOBAL_ROOT_UID))
		return 0;
	syscall_regs = (const struct pt_regs *)(unsigned long)regs->regs[0];
	if (!syscall_regs ||
	    (u32)syscall_regs->regs[0] != NBDRIVER_INSTALL_MAGIC1 ||
	    (u32)syscall_regs->regs[1] != NBDRIVER_INSTALL_MAGIC2)
		return 0;

	ret = nbdriver_schedule_fd_install(syscall_regs);
	if (ret)
		pr_warn("nbdriver: stage=reboot_probe schedule=task_work_add error=%d\n", ret);
	return 0;
}

/* __se_sys_* receives the syscall's scalar arguments in x0..x4. */
static int nbdriver_reboot_se_pre(struct kprobe *probe, struct pt_regs *regs)
{
	int ret;

	(void)probe;
	if (!uid_eq(current_euid(), GLOBAL_ROOT_UID))
		return 0;
	if ((u32)regs->regs[0] != NBDRIVER_INSTALL_MAGIC1 ||
	    (u32)regs->regs[1] != NBDRIVER_INSTALL_MAGIC2)
		return 0;

	ret = nbdriver_schedule_fd_install(regs);
	if (ret)
		pr_warn("nbdriver: stage=reboot_probe schedule=task_work_add error=%d\n", ret);
	return 0;
}

static int nbdriver_register_reboot_probe(const char *symbol,
					 int (*handler)(struct kprobe *,
							struct pt_regs *))
{
	int ret;

	memset(&nbdriver_reboot_probe, 0, sizeof(nbdriver_reboot_probe));
	nbdriver_reboot_probe.symbol_name = symbol;
	nbdriver_reboot_probe.pre_handler = handler;
	ret = register_kprobe(&nbdriver_reboot_probe);
	if (ret)
		return ret;

	nbdriver_reboot_probe_registered = true;
	pr_info("nbdriver: stage=reboot_probe symbol=%s status=registered\n", symbol);
	return 0;
}

int nbdriver_reboot_entry_init(void)
{
	int ret;

	ret = nbdriver_register_reboot_probe("__arm64_sys_reboot",
					    nbdriver_reboot_wrapper_pre);
	if (!ret)
		return 0;

	pr_warn("nbdriver: stage=reboot_probe symbol=__arm64_sys_reboot error=%d fallback=__se_sys_reboot\n", ret);
	return nbdriver_register_reboot_probe("__se_sys_reboot",
					      nbdriver_reboot_se_pre);
}

void nbdriver_reboot_entry_exit(void)
{
	if (!nbdriver_reboot_probe_registered)
		return;

	unregister_kprobe(&nbdriver_reboot_probe);
	nbdriver_reboot_probe_registered = false;
	memset(&nbdriver_reboot_probe, 0, sizeof(nbdriver_reboot_probe));
}
