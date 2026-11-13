// SPDX-License-Identifier: GPL-2.0-only
#include <linux/anon_inodes.h>
#include <linux/errno.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/uaccess.h>

#include "nbdriver_abi.h"
#include "nbdriver_comm.h"
#include "nbdriver_log.h"
#include "nbdriver_read.h"

/*
 * 这一层只依赖全系 EXPORT_SYMBOL(_GPL) 的接口：
 * anon_inode_getfile / get_unused_fd_flags / put_unused_fd / fd_install。
 * DDK 六个 KMI 的 Module.symvers 已确认全部导出，可直接静态链接。
 */

static long nbdriver_ioctl(struct file *file, unsigned int command,
			   unsigned long argument)
{
	struct nbdriver_abi_info info = {
		.version = NBDRIVER_ABI_VERSION,
		.size = sizeof(info),
		.features = NBDRIVER_FEATURE_READ_PROCESS |
			NBDRIVER_FEATURE_WRITE_PROCESS,
		.reserved = 0,
	};

	(void)file;

	switch (command) {
	case NBDRIVER_IOC_READ_PROCESS:
		return nbdriver_read_ioctl(argument);
	case NBDRIVER_IOC_WRITE_PROCESS:
		return nbdriver_write_ioctl(argument);
	case NBDRIVER_IOC_GET_ABI:
		if (!argument) {
			nbd_warn("stage=ioctl command=get_abi reason=null_argument error=%d\n",
				 -EFAULT);
			return -EFAULT;
		}
		if (copy_to_user((void __user *)argument, &info, sizeof(info))) {
			nbd_warn("stage=ioctl command=get_abi reason=copy_to_user_failed error=%d\n",
				 -EFAULT);
			return -EFAULT;
		}
		return 0;
	default:
		nbd_warn("stage=ioctl command=0x%08x reason=unknown_command error=%d\n",
			 command, -ENOTTY);
		return -ENOTTY;
	}
}

static const struct file_operations nbdriver_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = nbdriver_ioctl,
	.compat_ioctl = nbdriver_ioctl,
};

int nbdriver_comm_install_fd(int __user *output_fd)
{
	struct file *file;
	int fd;
	int written;

	if (!output_fd) {
		nbd_warn("stage=install_fd reason=null_output error=%d\n", -EFAULT);
		return -EFAULT;
	}

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		nbd_err("stage=install_fd operation=get_unused_fd_flags error=%d\n", fd);
		return fd;
	}

	file = anon_inode_getfile("[nbdriver]", &nbdriver_fops, NULL,
				  O_RDWR | O_CLOEXEC);
	if (IS_ERR(file)) {
		int error = PTR_ERR(file);

		nbd_err("stage=install_fd operation=anon_inode_getfile error=%d\n",
			error);
		put_unused_fd(fd);
		return error;
	}

	/*
	 * 先装 fd 再回报，避免用户态拿到一个尚未 install 的号；
	 * 这里先 copy 再 install 是为了在 copy 失败时能干净回滚。
	 */
	if (copy_to_user(output_fd, &fd, sizeof(fd))) {
		nbd_err("stage=install_fd operation=copy_to_user error=%d\n", -EFAULT);
		fput(file);
		put_unused_fd(fd);
		return -EFAULT;
	}

	fd_install(fd, file);

	written = fd;
	nbd_info("stage=install_fd status=ok fd=%d\n", written);
	return 0;
}

int nbdriver_comm_init(void)
{
	return 0;
}

void nbdriver_comm_exit(void)
{
}
