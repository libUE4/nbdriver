// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <unistd.h>

#include "nbdriver/client.h"

int nbdriver_open(void)
{
	struct nbdriver_abi_info info;
	int fd = NBDRIVER_INSTALL_PENDING;
	int error;

	errno = 0;
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
	error = errno;
	if (fd < 0) {
		if (fd != NBDRIVER_INSTALL_PENDING)
			errno = fd >= -4095 ? -fd : EPROTO;
		else
			errno = error && error != EINVAL ? error : ENODEV;
		return -1;
	}
	memset(&info, 0, sizeof(info));
	if (nbdriver_get_abi(fd, &info) < 0) {
		error = errno;
		close(fd);
		errno = error;
		return -1;
	}
	if (info.version != NBDRIVER_ABI_VERSION || info.size != sizeof(info)) {
		close(fd);
		errno = EPROTO;
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

int nbdriver_get_runtime_status(int fd, struct nbdriver_runtime_status *status)
{
	if (fd < 0 || !status) {
		errno = EINVAL;
		return -1;
	}
	memset(status, 0, sizeof(*status));
	return ioctl(fd, NBDRIVER_IOC_RUNTIME_STATUS, status);
}

int nbdriver_runtime_control(int fd, struct nbdriver_runtime_control *control)
{
	if (fd < 0 || !control) {
		errno = EINVAL;
		return -1;
	}
	return ioctl(fd, NBDRIVER_IOC_RUNTIME_CONTROL, control);
}

int nbdriver_load_module_path(const char *path, const char *parameters)
{
	int module_fd;
	long ret;

	if (!path) {
		errno = EINVAL;
		return -1;
	}
	module_fd = open(path, O_RDONLY | O_CLOEXEC);
	if (module_fd < 0)
		return -1;
#if defined(SYS_finit_module)
	ret = syscall(SYS_finit_module, module_fd,
		      parameters ? parameters : "", 0UL);
#elif defined(__NR_finit_module)
	ret = syscall(__NR_finit_module, module_fd,
		      parameters ? parameters : "", 0UL);
#else
	ret = -1;
	errno = ENOSYS;
#endif
	{
		int saved = errno;
		close(module_fd);
		errno = saved;
	}
	return (int)ret;
}

int nbdriver_read_process(int fd, struct nbdriver_read_request *request)
{
	if (fd < 0 || !request) {
		errno = EINVAL;
		return -1;
	}

	request->copied = 0;
	request->error = 0;
	request->failed_address = 0;
	return ioctl(fd, NBDRIVER_IOC_READ_PROCESS, request);
}

int nbdriver_write_process(int fd, struct nbdriver_write_request *request)
{
	if (fd < 0 || !request) {
		errno = EINVAL;
		return -1;
	}

	request->copied = 0;
	request->error = 0;
	request->failed_address = 0;
	return ioctl(fd, NBDRIVER_IOC_WRITE_PROCESS, request);
}
