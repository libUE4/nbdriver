// SPDX-License-Identifier: GPL-2.0-only
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <unistd.h>

#include "nbdriver/client.h"

/* 编译型 SDK 的连接语义与单头一致：-2 为候选 miss，-1 为确定错误。
 * GET_ABI 通过才返回 FD（包括 0），READY 和可选 feature 由调用方另查。
 * 本文件仅实现 client.h 中声明的接口；其余单头便利函数不自动成为导出函数。 */
static inline int nbdriver_bootstrap_try(unsigned int number, int *observed_error)
{
    struct nbdriver_abi_info info;
    int fd = NBDRIVER_INSTALL_PENDING;
    int error;

    errno = 0;
    /* Set all six arguments: scanned ordinary syscalls must never consume
     * uninitialized register arguments. These numbers are Android ARM64 ABI. */
    (void)syscall(number, NBDRIVER_INSTALL_MAGIC1, NBDRIVER_INSTALL_MAGIC2,
                  0, &fd, 0UL, 0UL);
    error = errno;
    if (fd == NBDRIVER_INSTALL_PENDING) {
        /* EBADF/EFAULT/EINVAL from real, unmodified syscall slots are
         * expected misses, not evidence that the driver's FD is broken. */
        if ((error == EPERM || error == EACCES || error == ENOMEM ||
             error == EMFILE || error == ENFILE || error == EAGAIN) &&
            (!*observed_error || error == EPERM || error == EACCES))
            *observed_error = error;
        return -2;
    }
    if (fd < 0) {
        errno = fd >= -4095 ? -fd : EPROTO;
        return -1;
    }
    memset(&info, 0, sizeof(info));
    if (ioctl(fd, NBDRIVER_IOC_GET_ABI, &info) < 0) {
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

int nbdriver_open(void)
{
    unsigned int index;
    int fd, observed_error = 0;

    for (index = 0; index < NBDRIVER_INSTALL_CANDIDATE_COUNT; ++index) {
        fd = nbdriver_bootstrap_try(nbdriver_install_candidate(index), &observed_error);
        if (fd != -2)
            return fd;
    }
    fd = nbdriver_bootstrap_try(NBDRIVER_INSTALL_REBOOT_SYSCALL, &observed_error);
    if (fd != -2)
        return fd;
    errno = observed_error ? observed_error : ENODEV;
    return -1;
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

/* 与单头及 NBG-Draw 同名入口保持一致，不隐式重置调用方请求。 */
int nbdriver_acquire_surfaceflinger_groups(
	int fd, struct nbdriver_sf_groups_request *request)
{
	if (fd < 0 || !request) {
		errno = EINVAL;
		return -1;
	}
	return ioctl(fd, NBDRIVER_IOC_SF_GROUPS_ACQUIRE, request);
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

/* UDP capture is raw downlink data, not coordinate decryption. One driver FD
 * owns one active capture; use poll(fd, POLLIN, timeout) to avoid busy waits.
 * STOP/final close discards its queue. A failed read may be retried: sequence
 * numbers let consumers discard partially copied duplicate records. */
int nbdriver_udp_capture_start(int fd, int pid,
    nbdriver_u32 min_payload, nbdriver_u32 max_payload,
    nbdriver_u64 expected_start_ns, struct nbdriver_udp_start_request *out)
{
    nbdriver_u32 minimum = min_payload ? min_payload : 16;
    nbdriver_u32 maximum = max_payload ? max_payload : NBDRIVER_UDP_MAX_PAYLOAD;
    if (!out) { errno = EINVAL; return -1; }
    memset(out, 0, sizeof(*out));
    if (fd < 0 || pid <= 0 || maximum > NBDRIVER_UDP_MAX_PAYLOAD || minimum > maximum) {
        errno = EINVAL; return -1;
    }
    out->pid = pid;
    out->min_payload = minimum;
    out->max_payload = maximum;
    out->expected_start_ns = expected_start_ns;
    if (ioctl(fd, NBDRIVER_IOC_UDP_START, out) < 0) return -1;
    if (!out->generation || !out->expected_start_ns || out->pid != pid) {
        errno = EPROTO; return -1;
    }
    return 0;
}

/* Returns number of records, zero for an empty queue, or -1/errno. */
int nbdriver_udp_capture_read(int fd, struct nbdriver_udp_packet *packets,
    nbdriver_u32 capacity, nbdriver_u64 generation, struct nbdriver_udp_status *status)
{
    struct nbdriver_udp_read_request request;
    int result;
    if (status) memset(status, 0, sizeof(*status));
    if (fd < 0 || !packets || !generation || !capacity || capacity > NBDRIVER_UDP_READ_MAX) {
        errno = EINVAL; return -1;
    }
    memset(&request, 0, sizeof(request));
    request.packets = (nbdriver_u64)(uintptr_t)packets;
    request.generation = generation;
    request.capacity = capacity;
    result = ioctl(fd, NBDRIVER_IOC_UDP_READ, &request);
    if (status) *status = request.status;
    if (result < 0) return -1;
    if (request.count > capacity || request.status.size != sizeof(request.status) ||
        request.status.generation != generation || request.status.error) {
        errno = EPROTO; return -1;
    }
    return (int)request.count;
}

int nbdriver_udp_capture_status(int fd, struct nbdriver_udp_status *out)
{
    if (!out) { errno = EINVAL; return -1; }
    memset(out, 0, sizeof(*out));
    if (fd < 0) { errno = EINVAL; return -1; }
    if (ioctl(fd, NBDRIVER_IOC_UDP_STATUS, out) < 0) return -1;
    if (out->size != sizeof(*out)) { errno = EPROTO; return -1; }
    return 0;
}

int nbdriver_udp_capture_stop(int fd)
{
    if (fd < 0) { errno = EINVAL; return -1; }
    return ioctl(fd, NBDRIVER_IOC_UDP_STOP, 0);
}

/* Optional raw-register capture. Start from a zeroed request; INFO is
 * supported discovery, ENOTTY means an older driver. READ is nondestructive:
 * preserve after_sequence only after a successful call. Close clears ownership.
 */
int nbdriver_hwbp(int fd, struct nbdriver_hwbp_request *request)
{
    unsigned int operation, capacity;
    int result;
    if (fd < 0 || !request) { errno = EINVAL; return -1; }
    operation = request->operation;
    capacity = request->capacity;
    request->version = NBDRIVER_HWBP_ABI_VERSION;
    request->size = sizeof(*request);
    result = ioctl(fd, NBDRIVER_IOC_HWBP, request);
    if (result < 0) return -1;
    if (request->version != NBDRIVER_HWBP_ABI_VERSION ||
        request->size != sizeof(*request) || request->operation != operation ||
        request->error ||
        (operation == NBDRIVER_HWBP_READ && request->count > capacity) ||
        (operation == NBDRIVER_HWBP_ADD && !request->handle)) {
        errno = EPROTO; return -1;
    }
    return 0;
}

