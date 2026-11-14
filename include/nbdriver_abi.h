/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_ABI_H
#define NBDRIVER_ABI_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/ioctl.h>
typedef __u32 nbdriver_u32;
typedef __s32 nbdriver_s32;
typedef __u64 nbdriver_u64;
typedef __s64 nbdriver_s64;
#else
#include <stdint.h>
#include <sys/ioctl.h>
typedef uint32_t nbdriver_u32;
typedef int32_t nbdriver_s32;
typedef uint64_t nbdriver_u64;
typedef int64_t nbdriver_s64;
#endif

#define NBDRIVER_ABI_VERSION 5U
#define NBDRIVER_INSTALL_MAGIC1 0x4E424452U
#define NBDRIVER_INSTALL_MAGIC2 0x5244424EU
/* Untouched bootstrap output; all other negative values are -errno. */
#define NBDRIVER_INSTALL_PENDING (-2147483647 - 1)

#define NBDRIVER_FEATURE_READ_PROCESS  (1U << 0)
#define NBDRIVER_FEATURE_WRITE_PROCESS (1U << 1)
#define NBDRIVER_FEATURE_READ_FAULT_IN (1U << 2)
#define NBDRIVER_FEATURE_FIND_PROCESS (1U << 3)
#define NBDRIVER_FEATURE_MODULE_BASE  (1U << 4)
#define NBDRIVER_FEATURE_PROCESS_ID   (1U << 5)
#define NBDRIVER_FEATURE_THREAD_ID    (1U << 6)
#define NBDRIVER_FEATURE_THREAD_ENV   (1U << 7)
#define NBDRIVER_FEATURE_THREAD_FP    (1U << 8)
#define NBDRIVER_FEATURE_READV        (1U << 9)
#define NBDRIVER_FEATURE_MAPS         (1U << 10)
#define NBDRIVER_FEATURE_MAPPING      (1U << 11)
#define NBDRIVER_FEATURE_FD_SIZE      (1U << 12)
#define NBDRIVER_FEATURE_GPU_INFO     (1U << 13)
#define NBDRIVER_FEATURE_RANDOM       (1U << 14)
#define NBDRIVER_FEATURE_DEVPTS       (1U << 15)
#define NBDRIVER_FEATURE_THREAD_SIGNAL (1U << 16)
#define NBDRIVER_FEATURE_VMA           (1U << 17)
#define NBDRIVER_FEATURE_GPU_SYNC       (1U << 18)
#define NBDRIVER_FEATURE_GPU_VISIBILITY (1U << 19)
#define NBDRIVER_FEATURE_PROC_VISIBILITY (1U << 20)
#define NBDRIVER_FEATURE_RUNTIME_STATUS (1U << 21)
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

/* Loader/runtime handshake.  This is deliberately additive and read-only:
 * the kernel owns the handle/token and the client only verifies readiness. */
#define NBDRIVER_RUNTIME_READY    (1U << 0)
#define NBDRIVER_RUNTIME_RESIDENT (1U << 1)
#define NBDRIVER_RUNTIME_PROBES  (1U << 2)
#define NBDRIVER_RUNTIME_QUIESCED (1U << 3)
#define NBDRIVER_RUNTIME_CONTROL_QUIESCE 1U
#define NBDRIVER_RUNTIME_CONTROL_RESUME  2U

struct nbdriver_runtime_status {
	nbdriver_u32 version;
	nbdriver_u32 size;
	nbdriver_u32 flags;
	nbdriver_s32 error;
	nbdriver_u64 handle;
	nbdriver_u64 token_lo;
	nbdriver_u64 token_hi;
	nbdriver_u64 generation;
	nbdriver_u32 probe_mask;
	nbdriver_u32 reserved;
};

struct nbdriver_runtime_control {
	nbdriver_u32 version;
	nbdriver_u32 size;
	nbdriver_u32 command;
	nbdriver_u32 reserved;
	nbdriver_u64 handle;
	nbdriver_u64 token_lo;
	nbdriver_u64 token_hi;
	nbdriver_s32 error;
	nbdriver_u32 reserved2;
};

#define NBDRIVER_IOC_MAGIC 'N'

#define NBDRIVER_IOC_GET_ABI \
	_IOR(NBDRIVER_IOC_MAGIC, 0x00, struct nbdriver_abi_info)
#define NBDRIVER_IOC_READ_PROCESS \
	_IOWR(NBDRIVER_IOC_MAGIC, 0x01, struct nbdriver_read_request)
#define NBDRIVER_IOC_WRITE_PROCESS \
	_IOWR(NBDRIVER_IOC_MAGIC, 0x02, struct nbdriver_write_request)
#define NBDRIVER_IOC_RUNTIME_STATUS \
	_IOR(NBDRIVER_IOC_MAGIC, 0x22, struct nbdriver_runtime_status)
#define NBDRIVER_IOC_RUNTIME_CONTROL \
	_IOWR(NBDRIVER_IOC_MAGIC, 0x23, struct nbdriver_runtime_control)


/* Additive ABI 5 extensions; existing request layouts and ioctl numbers stay fixed. */
#define NBDRIVER_QUERY_NAME_MAX 256U
#define NBDRIVER_THREAD_NAME_MAX 16U
#define NBDRIVER_READV_MAX 1024U
#define NBDRIVER_READV_ITEM_MAX (512U * 1024U)
#define NBDRIVER_READV_TOTAL_MAX (16U * 1024U * 1024U)
#define NBDRIVER_THREAD_TLS_VALID (1U << 0)
#define NBDRIVER_THREAD_APGA_VALID (1U << 1)
#define NBDRIVER_THREAD_FP_VALID 1U

struct nbdriver_pid_request {
	char name[NBDRIVER_QUERY_NAME_MAX];
	nbdriver_s32 pid;
	nbdriver_u32 reserved;
	nbdriver_u64 start_ns;
};

struct nbdriver_module_request {
	nbdriver_s32 pid;
	nbdriver_u32 reserved;
	nbdriver_u64 expected_start_ns;
	nbdriver_u64 base;
	char name[NBDRIVER_QUERY_NAME_MAX];
};

struct nbdriver_process_identity {
	nbdriver_s32 pid;
	nbdriver_u32 reserved;
	nbdriver_u64 start_ns;
};

struct nbdriver_thread_request {
	nbdriver_s32 pid;
	nbdriver_s32 tid;
	nbdriver_u64 expected_start_ns;
	nbdriver_u64 thread_start_ns;
	char comm[NBDRIVER_THREAD_NAME_MAX];
};

struct nbdriver_thread_environment {
	nbdriver_s32 pid;
	nbdriver_s32 tid;
	nbdriver_u32 valid;
	nbdriver_u32 reserved;
	nbdriver_u64 expected_start_ns;
	nbdriver_u64 expected_thread_start_ns;
	nbdriver_u64 tls;
	nbdriver_u64 apga_lo;
	nbdriver_u64 apga_hi;
};

/* Read-only, coherent action + blocked mask snapshot under sighand->siglock.
 * ARM64 native signal ABI: 64 signals / one u64 sigset. No target mutation. */
struct nbdriver_thread_signal {
	nbdriver_s32 pid;
	nbdriver_s32 tid;
	nbdriver_u32 signo;
	nbdriver_u32 reserved;
	nbdriver_u64 expected_start_ns;
	nbdriver_u64 expected_thread_start_ns;
	nbdriver_u64 handler;
	nbdriver_u64 flags;
	nbdriver_u64 restorer;
	nbdriver_u64 action_mask;
	nbdriver_u64 blocked_mask;
	nbdriver_u64 sample_ns;
};

struct nbdriver_thread_fpstate {
	nbdriver_s32 pid;
	nbdriver_s32 tid;
	nbdriver_u32 valid;
	nbdriver_u32 reserved;
	nbdriver_u64 expected_start_ns;
	nbdriver_u64 expected_thread_start_ns;
	nbdriver_u64 tls;
	nbdriver_u64 sample_ns;
	nbdriver_u32 fpsr;
	nbdriver_u32 fpcr;
};

struct nbdriver_readv_entry {
	nbdriver_u64 address;
	nbdriver_u64 buffer;
	nbdriver_u32 size;
	nbdriver_u32 copied;
	nbdriver_s32 error;
	nbdriver_u32 reserved;
	nbdriver_u64 failed_address;
};

struct nbdriver_readv_request {
	nbdriver_s32 pid;
	nbdriver_u32 count;
	nbdriver_u64 entries;
	nbdriver_u64 expected_start_ns;
	nbdriver_u32 completed;
	nbdriver_s32 error;
};

#define NBDRIVER_IOC_FIND_PROCESS \
	_IOWR(NBDRIVER_IOC_MAGIC, 0x10, struct nbdriver_pid_request)
#define NBDRIVER_IOC_MODULE_BASE \
	_IOWR(NBDRIVER_IOC_MAGIC, 0x11, struct nbdriver_module_request)
#define NBDRIVER_IOC_PROCESS_ID \
	_IOWR(NBDRIVER_IOC_MAGIC, 0x12, struct nbdriver_process_identity)
#define NBDRIVER_IOC_THREAD_ID \
	_IOWR(NBDRIVER_IOC_MAGIC, 0x13, struct nbdriver_thread_request)
#define NBDRIVER_IOC_THREAD_ENV \
	_IOWR(NBDRIVER_IOC_MAGIC, 0x14, struct nbdriver_thread_environment)
#define NBDRIVER_IOC_THREAD_SIGNAL \
	_IOWR(NBDRIVER_IOC_MAGIC, 0x1d, struct nbdriver_thread_signal)
/* Same layout as MAPPING; physical/page_size remain zero, no page-table walk. */
#define NBDRIVER_IOC_VMA \
	_IOWR(NBDRIVER_IOC_MAGIC, 0x1e, struct nbdriver_mapping_request)
#define NBDRIVER_IOC_THREAD_FP \
	_IOWR(NBDRIVER_IOC_MAGIC, 0x15, struct nbdriver_thread_fpstate)
#define NBDRIVER_IOC_READV \
	_IOWR(NBDRIVER_IOC_MAGIC, 0x16, struct nbdriver_readv_request)

#define NBDRIVER_MAPS_MAX (4U * 1024U * 1024U)
#define NBDRIVER_RANDOM_MAX 64U
#define NBDRIVER_MAPPING_READ   (1U << 0)
#define NBDRIVER_MAPPING_WRITE  (1U << 1)
#define NBDRIVER_MAPPING_EXEC   (1U << 2)
#define NBDRIVER_MAPPING_SHARED (1U << 3)

/* capacity includes the terminating NUL; length excludes it. A zero capacity
 * and zero buffer query the required length. ENOSPC copies no partial text. */
struct nbdriver_maps_request {
	nbdriver_s32 pid;
	nbdriver_u32 capacity;
	nbdriver_u64 buffer;
	nbdriver_u64 expected_start_ns;
	nbdriver_u32 length;
	nbdriver_u32 copied;
	nbdriver_s32 error;
	nbdriver_u32 reserved;
};

struct nbdriver_mapping_request {
	nbdriver_s32 pid;
	nbdriver_u32 reserved;
	nbdriver_u64 address;
	nbdriver_u64 expected_start_ns;
	nbdriver_u64 physical;
	nbdriver_u64 page_size;
	nbdriver_u64 vma_start;
	nbdriver_u64 vma_end;
	nbdriver_u32 flags;
	nbdriver_u32 reserved2;
};

struct nbdriver_fd_size_request {
	nbdriver_s32 pid;
	nbdriver_s32 target_fd;
	nbdriver_u64 expected_start_ns;
	nbdriver_u64 size;
	nbdriver_s32 error;
	nbdriver_u32 reserved;
};

/* Matches the 48-byte KGSL GPUOBJ_INFO payload; never expose kernel pointers. */
struct nbdriver_gpu_object_info {
	nbdriver_u64 gpu_address;
	nbdriver_u64 flags;
	nbdriver_u64 size;
	nbdriver_u64 virtual_length;
	nbdriver_u64 virtual_address;
	nbdriver_u32 id;
	nbdriver_u32 reserved;
};

struct nbdriver_gpu_info_request {
	nbdriver_s32 pid;
	nbdriver_s32 target_fd;
	nbdriver_u32 object_id;
	nbdriver_u32 reserved;
	nbdriver_u64 expected_start_ns;
	nbdriver_s64 result;
	nbdriver_u64 sample_ns;
	struct nbdriver_gpu_object_info info;
};

struct nbdriver_random_request {
	nbdriver_u64 buffer;
	nbdriver_u32 size;
	nbdriver_u32 copied;
	nbdriver_s32 error;
	nbdriver_u32 reserved;
};

#define NBDRIVER_IOC_MAPS \
	_IOWR(NBDRIVER_IOC_MAGIC, 0x17, struct nbdriver_maps_request)
#define NBDRIVER_IOC_MAPPING \
	_IOWR(NBDRIVER_IOC_MAGIC, 0x18, struct nbdriver_mapping_request)
#define NBDRIVER_IOC_FD_SIZE \
	_IOWR(NBDRIVER_IOC_MAGIC, 0x19, struct nbdriver_fd_size_request)
#define NBDRIVER_IOC_GPU_INFO \
	_IOWR(NBDRIVER_IOC_MAGIC, 0x1a, struct nbdriver_gpu_info_request)
#define NBDRIVER_IOC_RANDOM \
	_IOWR(NBDRIVER_IOC_MAGIC, 0x1b, struct nbdriver_random_request)

/* Automatic filtering is armed by module init; this ABI is read-only.
 * pids[0] = DFM, pids[1] = RMCN, in the caller PID namespace.
 * target_count includes all instances; a PID slot represents one instance.
 * Command 0x1c is retired and must not be reused. */
#define NBDRIVER_DEVPTS_PACKAGE_COUNT 2U
#define NBDRIVER_DEVPTS_MAX_INDEX 31U
struct nbdriver_devpts_status {
	nbdriver_u32 size;
	nbdriver_u32 active;
	nbdriver_u32 max_index;
	nbdriver_u32 target_count;
	nbdriver_s32 pids[NBDRIVER_DEVPTS_PACKAGE_COUNT];
	nbdriver_u64 permission_hits;
	nbdriver_u64 getattr_hits;
	nbdriver_u64 generation;
	nbdriver_s32 last_error;
	nbdriver_u32 reserved;
};
#define NBDRIVER_IOC_DEVPTS_STATUS \
	_IOR(NBDRIVER_IOC_MAGIC, 0x1f, struct nbdriver_devpts_status)


#define NBDRIVER_GPU_SYNC_MAX_ID 4096U
#define NBDRIVER_GPU_SYNC_WORDS (NBDRIVER_GPU_SYNC_MAX_ID / 64U)
#define NBDRIVER_GPU_BACKEND_IDR_LOCKED 1U
#define NBDRIVER_GPU_STAGE_NONE 0U
#define NBDRIVER_GPU_STAGE_IDENTITY 1U
#define NBDRIVER_GPU_STAGE_LAYOUT 2U
#define NBDRIVER_GPU_STAGE_PROCESS 3U
#define NBDRIVER_GPU_STAGE_IDR 4U
struct nbdriver_gpu_sync_request {
    nbdriver_s32 pid;
    nbdriver_s32 target_fd;
    nbdriver_u64 expected_start_ns;
    nbdriver_u32 max_id;
    nbdriver_u32 reserved;
    nbdriver_u32 first_free;
    nbdriver_u32 occupied_count;
    nbdriver_u32 backend;
    nbdriver_u32 fail_stage;
    nbdriver_s32 error;
    nbdriver_u32 reserved2;
    nbdriver_u64 sample_ns;
    nbdriver_u64 occupied[NBDRIVER_GPU_SYNC_WORDS];
};
#define NBDRIVER_GPU_VISIBILITY_QUERY 0U
#define NBDRIVER_GPU_VISIBILITY_HIDE 1U
#define NBDRIVER_GPU_VISIBILITY_SHOW 2U
#define NBDRIVER_GPU_VISIBILITY_MAX_TARGETS 64U
struct nbdriver_gpu_visibility_request {
    nbdriver_s32 pid;
    nbdriver_u32 operation;
    nbdriver_u64 expected_start_ns;
    nbdriver_u32 hidden;
    nbdriver_u32 active;
    nbdriver_u32 target_count;
    nbdriver_s32 error;
    nbdriver_u64 directory_hits;
    nbdriver_u64 access_hits;
    nbdriver_u64 missed;
};
#define NBDRIVER_IOC_GPU_SYNC \
    _IOWR(NBDRIVER_IOC_MAGIC, 0x20, struct nbdriver_gpu_sync_request)
#define NBDRIVER_IOC_GPU_VISIBILITY \
    _IOWR(NBDRIVER_IOC_MAGIC, 0x21, struct nbdriver_gpu_visibility_request)

#endif /* NBDRIVER_ABI_H */
