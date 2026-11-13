// SPDX-License-Identifier: GPL-2.0-only
#include <stddef.h>
#include "nbdriver_abi.h"

_Static_assert(NBDRIVER_ABI_VERSION == 5U, "abi version");
_Static_assert(sizeof(struct nbdriver_abi_info) == 16U, "abi info size");
_Static_assert(_IOC_SIZE(NBDRIVER_IOC_GET_ABI) == 16U, "ioctl ABI size");
_Static_assert(sizeof(struct nbdriver_read_request) == 48U, "read request size");
_Static_assert(sizeof(struct nbdriver_write_request) == 48U, "write request size");
_Static_assert(_IOC_SIZE(NBDRIVER_IOC_WRITE_PROCESS) == 48U, "write ioctl size");
_Static_assert(NBDRIVER_FEATURE_READ_FAULT_IN == (1U << 2),
	"fault-in feature");
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

_Static_assert(sizeof(struct nbdriver_pid_request) == 272, "PID request size");
_Static_assert(sizeof(struct nbdriver_module_request) == 280, "module request size");
_Static_assert(sizeof(struct nbdriver_process_identity) == 16, "identity size");
_Static_assert(sizeof(struct nbdriver_thread_request) == 40, "thread request size");
_Static_assert(sizeof(struct nbdriver_thread_environment) == 56, "thread env size");
_Static_assert(sizeof(struct nbdriver_thread_fpstate) == 56, "thread FP size");
_Static_assert(offsetof(struct nbdriver_thread_fpstate, fpsr) == 48, "FP offset");
_Static_assert(sizeof(struct nbdriver_readv_entry) == 40, "readv entry size");
_Static_assert(sizeof(struct nbdriver_readv_request) == 32, "readv request size");
_Static_assert(offsetof(struct nbdriver_readv_entry, failed_address) == 32, "readv error address");
_Static_assert(_IOC_SIZE(NBDRIVER_IOC_READV) == 32, "readv ioctl size");
_Static_assert(_IOC_NR(NBDRIVER_IOC_READV) == 0x16, "readv ioctl number");

_Static_assert(sizeof(struct nbdriver_maps_request) == 40, "maps size");
_Static_assert(sizeof(struct nbdriver_mapping_request) == 64, "mapping size");
_Static_assert(sizeof(struct nbdriver_fd_size_request) == 32, "FD size");
_Static_assert(sizeof(struct nbdriver_gpu_object_info) == 48, "GPU payload size");
_Static_assert(sizeof(struct nbdriver_gpu_info_request) == 88, "GPU request size");
_Static_assert(sizeof(struct nbdriver_random_request) == 24, "random size");
_Static_assert(offsetof(struct nbdriver_gpu_info_request, info) == 40, "GPU payload offset");
_Static_assert(offsetof(struct nbdriver_mapping_request, flags) == 56, "mapping flags offset");
_Static_assert(_IOC_NR(NBDRIVER_IOC_MAPS) == 0x17, "maps command");
_Static_assert(_IOC_SIZE(NBDRIVER_IOC_GPU_INFO) == 88, "GPU ioctl size");

_Static_assert(sizeof(struct nbdriver_devpts_filter_request) == 32, "devpts filter size");
_Static_assert(offsetof(struct nbdriver_devpts_filter_request, permission_hits) == 16, "devpts hits offset");
_Static_assert(_IOC_SIZE(NBDRIVER_IOC_DEVPTS_FILTER) == 32, "devpts ioctl size");
_Static_assert(_IOC_NR(NBDRIVER_IOC_DEVPTS_FILTER) == 0x1c, "devpts ioctl number");
