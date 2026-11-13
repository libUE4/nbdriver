// SPDX-License-Identifier: GPL-2.0-only
#include <stddef.h>
#include "nbdriver_abi.h"

_Static_assert(NBDRIVER_ABI_VERSION == 4U, "abi version");
_Static_assert(sizeof(struct nbdriver_abi_info) == 16U, "abi info size");
_Static_assert(_IOC_SIZE(NBDRIVER_IOC_GET_ABI) == 16U, "ioctl ABI size");
_Static_assert(sizeof(struct nbdriver_read_request) == 48U, "read request size");
_Static_assert(sizeof(struct nbdriver_write_request) == 48U, "write request size");
_Static_assert(_IOC_SIZE(NBDRIVER_IOC_WRITE_PROCESS) == 48U, "write ioctl size");
_Static_assert(offsetof(struct nbdriver_read_request, address) == 8U, "read address offset");
_Static_assert(offsetof(struct nbdriver_read_request, expected_start_ns) == 24U,
	"read identity offset");
_Static_assert(offsetof(struct nbdriver_read_request, copied) == 40U, "read result offset");
_Static_assert(offsetof(struct nbdriver_read_request, error) == 44U, "read error offset");
_Static_assert(_IOC_SIZE(NBDRIVER_IOC_READ_PROCESS) == 48U, "read ioctl size");
_Static_assert(_IOC_TYPE(NBDRIVER_IOC_READ_PROCESS) == NBDRIVER_IOC_MAGIC,
	"read ioctl type");
_Static_assert(_IOC_NR(NBDRIVER_IOC_READ_PROCESS) == 1U, "read ioctl number");

int main(void)
{
	struct nbdriver_abi_info info = { 0 };

	return (int)info.size;
}
