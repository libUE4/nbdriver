/* SPDX-License-Identifier: GPL-2.0-only */
/* nbdriver 用户态单头接口：复制本文件即可使用，支持 C/C++。
 * 协议、命令号和调用实现全部在本文件中；不需要额外的 .c/.cpp。
 * 用法：open -> get_abi -> read/write_process_memory -> close。
 */
#ifndef NBDRIVER_SINGLE_HEADER_H
#define NBDRIVER_SINGLE_HEADER_H

#ifdef __KERNEL__
#error "nbdriver.h is a user-space SDK; use nbdriver_abi.h in kernel code"
#endif

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

/* 与内核 include/nbdriver_abi.h 保持一致。 */
#ifndef NBDRIVER_ABI_H
#define NBDRIVER_ABI_H

typedef uint32_t nbdriver_u32;
typedef int32_t nbdriver_s32;
typedef uint64_t nbdriver_u64;
typedef int64_t nbdriver_s64;

#define NBDRIVER_ABI_VERSION 5U
#define NBDRIVER_INSTALL_MAGIC1 0x4E424452U
#define NBDRIVER_INSTALL_MAGIC2 0x5244424EU

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

#define NBDRIVER_DEVPTS_FILTER_ABI_VERSION 1U
#define NBDRIVER_DEVPTS_FILTER_MAX_INDEX 31U
#define NBDRIVER_DEVPTS_FILTER_DISABLE 0U
#define NBDRIVER_DEVPTS_FILTER_ENABLE 1U
#define NBDRIVER_DEVPTS_FILTER_QUERY 2U

/* The filter only affects access/stat probes for /dev/pts slave nodes whose
 * minor is <= max_index in the configured target process. */
struct nbdriver_devpts_filter_request {
	nbdriver_u32 abi_version;
	nbdriver_s32 pid;
	nbdriver_u32 max_index;
	nbdriver_u32 enabled;
	nbdriver_u64 permission_hits;
	nbdriver_u64 getattr_hits;
};

#define NBDRIVER_IOC_DEVPTS_FILTER \
	_IOWR(NBDRIVER_IOC_MAGIC, 0x1c, struct nbdriver_devpts_filter_request)

#endif /* NBDRIVER_ABI_H */

#ifdef __cplusplus
extern "C" {
#endif

/* glibc hides syscall() in strict ISO C mode; do not change feature macros
 * after a caller may already have included system headers. */
#if defined(__GLIBC__) && !defined(__USE_MISC)
extern long int syscall(long int number, ...) __THROW;
#endif

#ifndef NBDRIVER_REBOOT_SYSCALL
#if defined(SYS_reboot)
#define NBDRIVER_REBOOT_SYSCALL SYS_reboot
#elif defined(__NR_reboot)
#define NBDRIVER_REBOOT_SYSCALL __NR_reboot
#else
#error "reboot syscall number is unavailable on this platform"
#endif
#endif

/* 驱动将匿名 FD 写入 fd，不能用 reboot 的返回值判断连接成功。 */
static inline int nbdriver_open(void)
{
	int fd = -1;

	(void)syscall(NBDRIVER_REBOOT_SYSCALL, NBDRIVER_INSTALL_MAGIC1,
		      NBDRIVER_INSTALL_MAGIC2, 0, &fd);
	if (fd < 0) {
		errno = ENODEV;
		return -1;
	}
	errno = 0;
	return fd;
}

static inline void nbdriver_close(int fd)
{
	if (fd >= 0)
		close(fd);
}

static inline int nbdriver_ioctl(int fd, unsigned long command, void *argument)
{
	return ioctl(fd, command, argument);
}

static inline int nbdriver_get_abi(int fd, struct nbdriver_abi_info *info)
{
	if (fd < 0 || !info) {
		errno = EINVAL;
		return -1;
	}

	return ioctl(fd, NBDRIVER_IOC_GET_ABI, info);
}

static inline int nbdriver_read_process(int fd, struct nbdriver_read_request *request)
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

static inline int nbdriver_write_process(int fd, struct nbdriver_write_request *request)
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

/* 简单读写接口：返回 0 表示成功，-1 表示失败并设置 errno。
 * 单次最多 64 KiB；失败时可能已经读写部分数据，不会自动重试或清零。
 * 需要 copied/error/failed_address 或 PID 身份校验时，使用上面的请求接口。
 */
static inline int nbdriver_read_process_memory(int fd, int pid,
					       nbdriver_u64 address,
					       void *buffer, size_t size)
{
	struct nbdriver_read_request request;

	if (fd < 0 || pid <= 0 || !buffer || !size || size > NBDRIVER_READ_MAX) {
		errno = EINVAL;
		return -1;
	}
	memset(&request, 0, sizeof(request));
	request.pid = pid;
	request.size = (nbdriver_u32)size;
	request.address = address;
	request.buffer = (nbdriver_u64)(uintptr_t)buffer;
	return nbdriver_read_process(fd, &request);
}

static inline int nbdriver_write_process_memory(int fd, int pid,
						nbdriver_u64 address,
						const void *buffer, size_t size)
{
	struct nbdriver_write_request request;

	if (fd < 0 || pid <= 0 || !buffer || !size || size > NBDRIVER_WRITE_MAX) {
		errno = EINVAL;
		return -1;
	}
	memset(&request, 0, sizeof(request));
	request.pid = pid;
	request.size = (nbdriver_u32)size;
	request.address = address;
	request.buffer = (nbdriver_u64)(uintptr_t)buffer;
	return nbdriver_write_process(fd, &request);
}

/* 查询接口：显式传 fd/pid，与现有单头调用方式一致。 */
static inline int nbdriver_copy_query_name(char *out, size_t capacity, const char *name)
{
	size_t length = 0;

	if (!name || !name[0]) {
		errno = EINVAL;
		return -1;
	}
	while (length < capacity && name[length])
		++length;
	if (length == capacity) {
		errno = ENAMETOOLONG;
		return -1;
	}
	memcpy(out, name, length + 1);
	return 0;
}

static inline int nbdriver_find_process(int fd, const char *name)
{
	struct nbdriver_pid_request request;

	memset(&request, 0, sizeof(request));
	if (fd < 0) {
		errno = EINVAL;
		return -1;
	}
	if (nbdriver_copy_query_name(request.name, sizeof(request.name), name) < 0)
		return -1;
	if (ioctl(fd, NBDRIVER_IOC_FIND_PROCESS, &request) < 0)
		return -1;
	if (request.pid <= 0) {
		errno = EPROTO;
		return -1;
	}
	return request.pid;
}

static inline int nbdriver_get_module_base(int fd, int pid, const char *name,
					   nbdriver_u64 *base)
{
	struct nbdriver_module_request request;

	if (base)
		*base = 0;
	if (fd < 0 || pid <= 0 || !base) {
		errno = EINVAL;
		return -1;
	}
	memset(&request, 0, sizeof(request));
	request.pid = pid;
	if (nbdriver_copy_query_name(request.name, sizeof(request.name), name) < 0)
		return -1;
	if (ioctl(fd, NBDRIVER_IOC_MODULE_BASE, &request) < 0)
		return -1;
	*base = request.base;
	return 0;
}

static inline int nbdriver_get_process_start_time(int fd, int pid, nbdriver_u64 *start_ns)
{
	struct nbdriver_process_identity request;

	if (start_ns)
		*start_ns = 0;
	if (fd < 0 || pid <= 0 || !start_ns) {
		errno = EINVAL;
		return -1;
	}
	memset(&request, 0, sizeof(request));
	request.pid = pid;
	if (ioctl(fd, NBDRIVER_IOC_PROCESS_ID, &request) < 0)
		return -1;
	*start_ns = request.start_ns;
	return 0;
}

static inline int nbdriver_get_thread_id(int fd, int pid, const char *comm)
{
	struct nbdriver_thread_request request;

	if (fd < 0 || pid <= 0) {
		errno = EINVAL;
		return -1;
	}
	memset(&request, 0, sizeof(request));
	request.pid = pid;
	if (nbdriver_copy_query_name(request.comm, sizeof(request.comm), comm) < 0)
		return -1;
	if (ioctl(fd, NBDRIVER_IOC_THREAD_ID, &request) < 0)
		return -1;
	if (request.tid <= 0) {
		errno = EPROTO;
		return -1;
	}
	return request.tid;
}

static inline int nbdriver_get_thread_environment(int fd, int pid, int tid,
				 struct nbdriver_thread_environment *out)
{
	if (!out) {
		errno = EINVAL;
		return -1;
	}
	memset(out, 0, sizeof(*out));
	if (fd < 0 || pid <= 0 || tid <= 0) {
		errno = EINVAL;
		return -1;
	}
	out->pid = pid;
	out->tid = tid;
	return ioctl(fd, NBDRIVER_IOC_THREAD_ENV, out);
}

static inline int nbdriver_get_thread_fpstate(int fd, int pid, int tid,
				     struct nbdriver_thread_fpstate *out)
{
	if (!out) {
		errno = EINVAL;
		return -1;
	}
	memset(out, 0, sizeof(*out));
	if (fd < 0 || pid <= 0 || tid <= 0) {
		errno = EINVAL;
		return -1;
	}
	out->pid = pid;
	out->tid = tid;
	return ioctl(fd, NBDRIVER_IOC_THREAD_FP, out);
}

/* ioctl 返回 0 代表批次已处理；逐项检查 entry.error/copied。
 * 每项最多 512 KiB，批次合计最多 16 MiB，最多 1024 项。
 * 精确身份校验和 completed/error 输出使用此原始请求接口。
 */
static inline int nbdriver_readv_process(int fd, struct nbdriver_readv_request *request)
{
	if (fd < 0 || !request) {
		errno = EINVAL;
		return -1;
	}
	request->completed = 0;
	request->error = 0;
	return ioctl(fd, NBDRIVER_IOC_READV, request);
}

static inline int nbdriver_read_process_vector(int fd, int pid,
	struct nbdriver_readv_entry *entries, nbdriver_u32 count, nbdriver_u64 expected_start_ns)
{
	struct nbdriver_readv_request request;

	if (fd < 0 || pid <= 0 || !entries || !count || count > NBDRIVER_READV_MAX) {
		errno = EINVAL;
		return -1;
	}
	memset(&request, 0, sizeof(request));
	request.pid = pid;
	request.count = count;
	request.entries = (nbdriver_u64)(uintptr_t)entries;
	request.expected_start_ns = expected_start_ns;
	return nbdriver_readv_process(fd, &request);
}

/* Raw maps request retains required length/identity on ENOSPC. */
static inline int nbdriver_process_maps(int fd, struct nbdriver_maps_request *request)
{
	if (fd < 0 || !request) {
		errno = EINVAL;
		return -1;
	}
	request->length = request->copied = 0;
	request->error = 0;
	return ioctl(fd, NBDRIVER_IOC_MAPS, request);
}

/* buffer==NULL and capacity==0 query the required length (excluding NUL). */
static inline int nbdriver_get_process_maps(int fd, int pid, char *buffer,
	nbdriver_u32 capacity, nbdriver_u32 *length, nbdriver_u64 expected_start_ns)
{
	struct nbdriver_maps_request request;
	int ret;

	if (length)
		*length = 0;
	if (fd < 0 || pid <= 0 || !length || capacity > NBDRIVER_MAPS_MAX ||
	    (!!buffer != !!capacity)) {
		errno = EINVAL;
		return -1;
	}
	memset(&request, 0, sizeof(request));
	request.pid = pid;
	request.buffer = (nbdriver_u64)(uintptr_t)buffer;
	request.capacity = capacity;
	request.expected_start_ns = expected_start_ns;
	ret = nbdriver_process_maps(fd, &request);
	*length = request.length;
	return ret;
}

static inline int nbdriver_query_mapping(int fd, int pid, nbdriver_u64 address,
	nbdriver_u64 expected_start_ns, struct nbdriver_mapping_request *out)
{
	if (!out) {
		errno = EINVAL;
		return -1;
	}
	memset(out, 0, sizeof(*out));
	if (fd < 0 || pid <= 0 || !address) {
		errno = EINVAL;
		return -1;
	}
	out->pid = pid;
	out->address = address;
	out->expected_start_ns = expected_start_ns;
	return ioctl(fd, NBDRIVER_IOC_MAPPING, out);
}

static inline int nbdriver_get_file_descriptor_size(int fd, int pid, int target_fd,
	nbdriver_u64 expected_start_ns, nbdriver_u64 *size)
{
	struct nbdriver_fd_size_request request;

	if (size)
		*size = 0;
	if (fd < 0 || pid <= 0 || target_fd < 0 || !size) {
		errno = EINVAL;
		return -1;
	}
	memset(&request, 0, sizeof(request));
	request.pid = pid;
	request.target_fd = target_fd;
	request.expected_start_ns = expected_start_ns;
	if (ioctl(fd, NBDRIVER_IOC_FD_SIZE, &request) < 0)
		return -1;
	*size = request.size;
	return 0;
}

/* Return 0 when transport/validation succeeded; inspect out->result for the
 * actual KGSL return. Missing/unverified vendor backends return -1/errno. */
static inline int nbdriver_read_gpu_object_info(int fd, int pid, int target_fd,
	nbdriver_u32 object_id, nbdriver_u64 expected_start_ns,
	struct nbdriver_gpu_info_request *out)
{
	if (!out) {
		errno = EINVAL;
		return -1;
	}
	memset(out, 0, sizeof(*out));
	if (fd < 0 || pid <= 0 || target_fd < 0 || !object_id) {
		errno = EINVAL;
		return -1;
	}
	out->pid = pid;
	out->target_fd = target_fd;
	out->object_id = object_id;
	out->expected_start_ns = expected_start_ns;
	return ioctl(fd, NBDRIVER_IOC_GPU_INFO, out);
}

/* Returns the number of bytes, or -1/errno. Maximum 64 bytes per request. */
/* Configure PID-scoped /dev/pts probe filtering. ENABLE installs both
 * security_inode_permission and security_inode_getattr probes; DISABLE removes
 * them. QUERY only reports state/counters and never changes filtering. */
static inline int nbdriver_configure_devpts_filter(int fd, int pid, int enabled,
    nbdriver_u32 max_index, struct nbdriver_devpts_filter_request *out)
{
    struct nbdriver_devpts_filter_request request;

    if (fd < 0 || !out || enabled > (int)NBDRIVER_DEVPTS_FILTER_QUERY || enabled < 0 ||
        (enabled == NBDRIVER_DEVPTS_FILTER_ENABLE &&
         (pid <= 0 || max_index > NBDRIVER_DEVPTS_FILTER_MAX_INDEX))) {
        errno = EINVAL;
        return -1;
    }
    memset(&request, 0, sizeof(request));
    request.abi_version = NBDRIVER_DEVPTS_FILTER_ABI_VERSION;
    request.pid = pid;
    request.max_index = max_index;
    request.enabled = (nbdriver_u32)enabled;
    if (ioctl(fd, NBDRIVER_IOC_DEVPTS_FILTER, &request) < 0)
        return -1;
    if (request.abi_version != NBDRIVER_DEVPTS_FILTER_ABI_VERSION) {
        errno = EPROTO;
        return -1;
    }
    *out = request;
    return 0;
}

static inline int nbdriver_query_devpts_filter(int fd,
    struct nbdriver_devpts_filter_request *out)
{
    return nbdriver_configure_devpts_filter(fd, 0,
        NBDRIVER_DEVPTS_FILTER_QUERY, 0, out);
}

static inline int nbdriver_disable_devpts_filter(int fd,
    struct nbdriver_devpts_filter_request *out)
{
    return nbdriver_configure_devpts_filter(fd, 0,
        NBDRIVER_DEVPTS_FILTER_DISABLE, 0, out);
}

static inline int nbdriver_read_random(int fd, void *buffer, size_t size)
{
	struct nbdriver_random_request request;

	if (fd < 0 || !buffer || !size || size > NBDRIVER_RANDOM_MAX) {
		errno = EINVAL;
		return -1;
	}
	memset(&request, 0, sizeof(request));
	request.buffer = (nbdriver_u64)(uintptr_t)buffer;
	request.size = (nbdriver_u32)size;
	if (ioctl(fd, NBDRIVER_IOC_RANDOM, &request) < 0)
		return -1;
	if (request.error || request.copied != size) {
		errno = request.error < 0 ? -request.error : EPROTO;
		return -1;
	}
	return (int)request.copied;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* NBDRIVER_SINGLE_HEADER_H */
