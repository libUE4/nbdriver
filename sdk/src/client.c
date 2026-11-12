// SPDX-License-Identifier: GPL-2.0-only
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "nbdriver/client.h"

int nbdriver_open(void)
{
	int fd;
	struct nbdriver_abi_info info = { 0 };

	/* 设备名可能被占用，只有 ABI 匹配才接受该 FD。 */
	fd = open("/dev/nbdriver", O_RDWR | O_CLOEXEC);
	if (fd >= 0) {
		if (!ioctl(fd, NBDRIVER_IOC_GET_ABI, &info) &&
		    info.version == NBDRIVER_ABI_VERSION &&
		    info.size == sizeof(info) && !info.reserved)
			return fd;
		(void)close(fd);
	}
	fd = -1;

#if defined(SYS_reboot)
	(void)syscall(SYS_reboot, NBDRIVER_INSTALL_MAGIC1,
		      NBDRIVER_INSTALL_MAGIC2, 0, &fd);
#elif defined(__NR_reboot)
	(void)syscall(__NR_reboot, NBDRIVER_INSTALL_MAGIC1,
		      NBDRIVER_INSTALL_MAGIC2, 0, &fd);
#else
	errno = ENOSYS;
	return -1;
#endif

	if (fd < 0) {
		errno = ENODEV;
		return -1;
	}
	errno = 0;
	return fd;
}

int nbdriver_get_abi(int fd, struct nbdriver_abi_info *info)
{
	if (fd < 0 || !info) {
		errno = EINVAL;
		return -1;
	}

	return ioctl(fd, NBDRIVER_IOC_GET_ABI, info);
}
