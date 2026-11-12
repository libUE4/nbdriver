/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_ABI_H
#define NBDRIVER_ABI_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/ioctl.h>
typedef __u32 nbdriver_u32;
typedef __s32 nbdriver_s32;
#else
#include <stdint.h>
#include <sys/ioctl.h>
typedef uint32_t nbdriver_u32;
typedef int32_t nbdriver_s32;
#endif

#define NBDRIVER_ABI_VERSION 2U
#define NBDRIVER_INSTALL_MAGIC1 0x4E424452U
#define NBDRIVER_INSTALL_MAGIC2 0x5244424EU

struct nbdriver_abi_info {
	nbdriver_u32 version;
	nbdriver_u32 size;
	nbdriver_u32 features;
	nbdriver_u32 reserved;
};

#define NBDRIVER_IOC_MAGIC 'N'

#define NBDRIVER_IOC_GET_ABI \
	_IOR(NBDRIVER_IOC_MAGIC, 0x00, struct nbdriver_abi_info)

#endif /* NBDRIVER_ABI_H */
