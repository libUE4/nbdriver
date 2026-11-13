/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_ABI_H
#define NBDRIVER_ABI_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/ioctl.h>
typedef __u32 nbdriver_u32;
typedef __s32 nbdriver_s32;
typedef __u64 nbdriver_u64;
#else
#include <stdint.h>
#include <sys/ioctl.h>
typedef uint32_t nbdriver_u32;
typedef int32_t nbdriver_s32;
typedef uint64_t nbdriver_u64;
#endif

#define NBDRIVER_ABI_VERSION 5U
#define NBDRIVER_INSTALL_MAGIC1 0x4E424452U
#define NBDRIVER_INSTALL_MAGIC2 0x5244424EU

#define NBDRIVER_FEATURE_READ_PROCESS  (1U << 0)
#define NBDRIVER_FEATURE_WRITE_PROCESS (1U << 1)
#define NBDRIVER_FEATURE_READ_FAULT_IN (1U << 2)
#define NBDRIVER_READ_MAX (64U * 1024U)
#define NBDRIVER_WRITE_MAX (64U * 1024U)

struct nbdriver_read_request {
	nbdriver_s32 pid;
	nbdriver_u32 size;
	nbdriver_u64 address;
	nbdriver_u64 buffer;
	nbdriver_u64 expected_start_ns;
	nbdriver_u64 failed_address;
	nbdriver_u32 copied;
	nbdriver_s32 error;
};

struct nbdriver_write_request {
	nbdriver_s32 pid;
	nbdriver_u32 size;
	nbdriver_u64 address;
	nbdriver_u64 buffer;
	nbdriver_u64 expected_start_ns;
	nbdriver_u64 failed_address;
	nbdriver_u32 copied;
	nbdriver_s32 error;
};

struct nbdriver_abi_info {
	nbdriver_u32 version;
	nbdriver_u32 size;
	nbdriver_u32 features;
	nbdriver_u32 reserved;
};

#define NBDRIVER_IOC_MAGIC 'N'

#define NBDRIVER_IOC_GET_ABI \
	_IOR(NBDRIVER_IOC_MAGIC, 0x00, struct nbdriver_abi_info)
#define NBDRIVER_IOC_READ_PROCESS \
	_IOWR(NBDRIVER_IOC_MAGIC, 0x01, struct nbdriver_read_request)
#define NBDRIVER_IOC_WRITE_PROCESS \
	_IOWR(NBDRIVER_IOC_MAGIC, 0x02, struct nbdriver_write_request)

#endif /* NBDRIVER_ABI_H */
