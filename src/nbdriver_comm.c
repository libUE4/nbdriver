// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/uaccess.h>

#include "nbdriver_abi.h"
#include "nbdriver_comm.h"

static long nbdriver_ioctl(struct file *file, unsigned int command,
			   unsigned long argument)
{
	struct nbdriver_abi_info info = {
		.version = NBDRIVER_ABI_VERSION,
		.size = sizeof(info),
		.features = 0,
		.reserved = 0,
	};

	(void)file;

	switch (command) {
	case NBDRIVER_IOC_GET_ABI:
		if (!argument)
			return -EFAULT;
		if (copy_to_user((void __user *)argument, &info, sizeof(info)))
			return -EFAULT;
		return 0;
	default:
		return -ENOTTY;
	}
}

static const struct file_operations nbdriver_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = nbdriver_ioctl,
	.compat_ioctl = nbdriver_ioctl,
};

static struct miscdevice nbdriver_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "nbdriver",
	.fops = &nbdriver_fops,
	.mode = 0600,
};

int nbdriver_comm_init(void)
{
	return misc_register(&nbdriver_device);
}

void nbdriver_comm_exit(void)
{
	misc_deregister(&nbdriver_device);
}
