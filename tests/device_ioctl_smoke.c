// SPDX-License-Identifier: GPL-2.0-only
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "nbdriver/client.h"

static int expect_ioctl_error(int fd, unsigned long command, void *argument,
			      int expected_errno, const char *name)
{
	int result;
	int saved_errno;

	errno = 0;
	result = ioctl(fd, command, argument);
	saved_errno = errno;
	printf("%s: result=%d errno=%d expected_errno=%d\n",
	       name, result, saved_errno, expected_errno);
	return result == -1 && saved_errno == expected_errno ? 0 : -1;
}

int main(int argc, char **argv)
{
	struct nbdriver_abi_info info = { 0 };
	int fd;
	int flags;
	int saved_errno;
	int i;

	if (argc == 2 && !strcmp(argv[1], "--expect-absent")) {
		errno = 0;
		fd = nbdriver_open();
		saved_errno = errno;
		if (fd >= 0) {
			close(fd);
			fprintf(stderr, "device still opens after unload\n");
			return 1;
		}
		printf("bootstrap_absent: errno=%d expected_errno=%d\n", saved_errno, ENODEV);
		return saved_errno == ENODEV ? 0 : 1;
	}
	if (argc != 1) {
		fprintf(stderr, "usage: %s [--expect-absent]\n", argv[0]);
		return 2;
	}

	fd = nbdriver_open();
	if (fd < 0) {
		perror("nbdriver_open");
		return 1;
	}
	flags = fcntl(fd, F_GETFD);
	if (flags < 0 || !(flags & FD_CLOEXEC)) {
		fprintf(stderr, "FD_CLOEXEC missing or F_GETFD failed\n");
		goto out;
	}
	if (nbdriver_get_abi(fd, &info)) {
		perror("nbdriver_get_abi");
		goto out;
	}
	printf("abi: version=%u size=%u features=%u reserved=%u\n",
	       info.version, info.size, info.features, info.reserved);
	if (info.version != NBDRIVER_ABI_VERSION ||
	    info.size != sizeof(info) || info.reserved != 0) {
		fprintf(stderr, "unexpected ABI response\n");
		goto out;
	}
	if (expect_ioctl_error(fd, NBDRIVER_IOC_GET_ABI, NULL, EFAULT,
			       "null_output"))
		goto out;
	if (expect_ioctl_error(fd, _IO(NBDRIVER_IOC_MAGIC, 0x7f), NULL,
			       ENOTTY, "unknown_command"))
		goto out;
	if (close(fd)) {
		perror("close");
		return 1;
	}
	if (expect_ioctl_error(fd, NBDRIVER_IOC_GET_ABI, &info, EBADF,
			       "closed_fd"))
		return 1;
	for (i = 0; i < 16; ++i) {
		fd = nbdriver_open();
		if (fd < 0) {
			perror("reopen");
			return 1;
		}
		if (nbdriver_get_abi(fd, &info)) {
			perror("reopened ABI query");
			close(fd);
			return 1;
		}
		if (close(fd)) {
			perror("close reopened FD");
			return 1;
		}
	}
	printf("reopen_query_close: iterations=%d\n", i);
	puts("device_ioctl_smoke: PASS");
	return 0;

out:
	close(fd);
	return 1;
}
