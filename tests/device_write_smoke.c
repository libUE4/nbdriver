// SPDX-License-Identifier: GPL-2.0-only
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "nbdriver/client.h"

int main(void)
{
	int fd;
	struct nbdriver_abi_info info = { 0 };

	fd = nbdriver_open();
	if (fd < 0) {
		perror("nbdriver_open");
		return 1;
	}
	if (nbdriver_get_abi(fd, &info) < 0) {
		perror("nbdriver_get_abi");
		return 1;
	}
	printf("abi=%u size=%u features=0x%x\n",
	       info.version, info.size, info.features);
	if (info.version != NBDRIVER_ABI_VERSION ||
	    !(info.features & NBDRIVER_FEATURE_READ_PROCESS) ||
	    !(info.features & NBDRIVER_FEATURE_WRITE_PROCESS))
		return 2;
	return 0;
}
