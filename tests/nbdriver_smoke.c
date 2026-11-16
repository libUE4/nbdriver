/* 综合设备测试：构造自己的进程、线程与缓冲区，包含实际读写及失败回收检查。
 * 运行需要原生 Android ARM64、匹配驱动与对应权限；不是纯查询/只读探针。
 * 普通 Linux 主机只做构建检查，不直接执行真实 Android bootstrap。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "nbdriver/client.h"

_Static_assert(sizeof(struct nbdriver_read_request) == 48U,
	       "read request layout");
_Static_assert(sizeof(struct nbdriver_write_request) == 48U,
	       "write request layout");
_Static_assert(sizeof(struct nbdriver_abi_info) == 16U,
	       "abi info layout");
_Static_assert(offsetof(struct nbdriver_read_request, failed_address) == 32U,
	       "read failed_address offset");
_Static_assert(offsetof(struct nbdriver_read_request, copied) == 40U,
	       "read copied offset");
_Static_assert(offsetof(struct nbdriver_read_request, error) == 44U,
	       "read error offset");
_Static_assert(offsetof(struct nbdriver_write_request, failed_address) == 32U,
	       "write failed_address offset");
_Static_assert(offsetof(struct nbdriver_write_request, copied) == 40U,
	       "write copied offset");
_Static_assert(offsetof(struct nbdriver_write_request, error) == 44U,
	       "write error offset");

static int failures;
static int skips;
static int passes;
static const char *active_section;
static int section_passes;
static int section_failures;
static int section_skips;
static size_t page_size;
static volatile sig_atomic_t alarm_fired;

static void alarm_handler(int signal_number)
{
	(void)signal_number;
	alarm_fired = 1;
}

static void section_finish(void)
{
	if (!active_section)
		return;
	if (failures == section_failures && skips == section_skips) {
		if (passes == section_passes)
			printf("  完成\n");
		else
			printf("  通过（%d项）\n", passes - section_passes);
		return;
	}
	printf("  通过%d项，失败%d项，跳过%d项\n", passes - section_passes,
	       failures - section_failures, skips - section_skips);
}

static void section(const char *name)
{
	section_finish();
	active_section = name;
	section_passes = passes;
	section_failures = failures;
	section_skips = skips;
	printf("\n[%s]\n", name);
}

static void check_result(int ok, const char *name)
{
	if (ok) {
		++passes;
		return;
	}
	++failures;
	fprintf(stderr, "  失败：%s / %s\n", active_section ? active_section : "测试准备", name);
}

static void skip_result(const char *name)
{
	++skips;
	fprintf(stderr, "  跳过：%s\n", name);
}

static const char *error_text(int error_number)
{
	switch (error_number) {
	case 0: return "成功";
	case EACCES: return "权限被拒绝";
	case EAGAIN: return "请重试";
	case EFAULT: return "地址无效";
	case EINVAL: return "参数无效";
	case ENODATA: return "页面未驻留";
	case ENODEV: return "设备不存在";
	case ENOMEM: return "内存不足";
	case ENOSYS: return "功能未实现";
	case ENOTTY: return "不支持的命令";
	case EPERM: return "权限不足";
	case EPIPE: return "管道已关闭";
	case ESRCH: return "进程不存在";
	case ESTALE: return "进程身份已变化";
	case ETIMEDOUT: return "超时";
	default: return "未知错误";
	}
}

static void print_fault_read(const char *name, int rc, int saved_errno,
			     const struct nbdriver_read_request *request)
{
	(void)name;
	fprintf(stderr, "  详细：读取返回=%d，错误=%d(%s)，已读=%u，内部错误=%d，失败地址=0x%llx\n",
		rc, saved_errno, error_text(saved_errno), request->copied,
		request->error, (unsigned long long)request->failed_address);
}

static void print_fault_write(const char *name, int rc, int saved_errno,
			      const struct nbdriver_write_request *request)
{
	(void)name;
	fprintf(stderr, "  详细：写入返回=%d，错误=%d(%s)，已写=%u，内部错误=%d，失败地址=0x%llx\n",
		rc, saved_errno, error_text(saved_errno), request->copied,
		request->error, (unsigned long long)request->failed_address);
}

static struct nbdriver_read_request read_request(pid_t pid,
						 const void *address, void *buffer,
						 size_t size)
{
	struct nbdriver_read_request request = {
		.pid = pid,
		.size = (nbdriver_u32)size,
		.address = (nbdriver_u64)(uintptr_t)address,
		.buffer = (nbdriver_u64)(uintptr_t)buffer,
	};

	return request;
}

static struct nbdriver_write_request write_request(pid_t pid,
						  void *address, const void *buffer,
						  size_t size)
{
	struct nbdriver_write_request request = {
		.pid = pid,
		.size = (nbdriver_u32)size,
		.address = (nbdriver_u64)(uintptr_t)address,
		.buffer = (nbdriver_u64)(uintptr_t)buffer,
	};

	return request;
}

static int read_full(int fd, void *buffer, size_t size)
{
	size_t done = 0;
	unsigned char *bytes = buffer;

	while (done < size) {
		ssize_t count = read(fd, bytes + done, size - done);

		if (count > 0) {
			done += (size_t)count;
			continue;
		}
		if (count == 0) {
			errno = EPIPE;
			return -1;
		}
		if (errno == EINTR) {
			if (alarm_fired) {
				errno = ETIMEDOUT;
				return -1;
			}
			continue;
		}
		return -1;
	}
	return 0;
}

static int write_full(int fd, const void *buffer, size_t size)
{
	size_t done = 0;
	const unsigned char *bytes = buffer;

	while (done < size) {
		ssize_t count = write(fd, bytes + done, size - done);

		if (count > 0) {
			done += (size_t)count;
			continue;
		}
		if (count == 0) {
			errno = EPIPE;
			return -1;
		}
		if (errno == EINTR) {
			if (alarm_fired) {
				errno = ETIMEDOUT;
				return -1;
			}
			continue;
		}
		return -1;
	}
	return 0;
}

static int wait_child(pid_t pid, int *status)
{
	pid_t result;

	for (;;) {
		result = waitpid(pid, status, 0);
		if (result == pid)
			return 0;
		if (result < 0 && errno == EINTR) {
			if (alarm_fired) {
				errno = ETIMEDOUT;
				return -1;
			}
			continue;
		}
		return -1;
	}
}

static void kill_and_reap(pid_t pid)
{
	int status;

	if (pid <= 0)
		return;
	(void)kill(pid, SIGKILL);
	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR)
			break;
	}
}

static int mincore_page(const void *address, unsigned char *resident)
{
	unsigned char value = 0;

	if (mincore((void *)address, page_size, &value) != 0)
		return -1;
	*resident = value & 1U;
	return 0;
}

static void close_fd(int *fd)
{
	if (*fd >= 0) {
		(void)close(*fd);
		*fd = -1;
	}
}

static int test_abi_and_fd(int fd, const struct nbdriver_abi_info *info)
{
	struct nbdriver_abi_info request_info;
	struct nbdriver_read_request bad_read;
	struct nbdriver_write_request bad_write;
	int closed_fd = -1;
	int result;
	int saved_errno;
	int flags;
	int i;
	int ok;

	section("接口、句柄和参数");
	check_result(info->version == NBDRIVER_ABI_VERSION, "接口版本");
	check_result(info->size == sizeof(*info) && info->reserved == 0,
		     "接口布局");
	check_result((info->features & NBDRIVER_FEATURE_READ_PROCESS) != 0,
		     "读取功能");
	check_result((info->features & NBDRIVER_FEATURE_WRITE_PROCESS) != 0,
		     "写入功能");
	check_result((info->features & NBDRIVER_FEATURE_READ_FAULT_IN) != 0,
		     "自动补页功能");
	check_result(_IOC_SIZE(NBDRIVER_IOC_GET_ABI) == sizeof(*info) &&
		     _IOC_SIZE(NBDRIVER_IOC_READ_PROCESS) ==
			     sizeof(struct nbdriver_read_request) &&
		     _IOC_SIZE(NBDRIVER_IOC_WRITE_PROCESS) ==
			     sizeof(struct nbdriver_write_request),
		     "调用结构布局");

	flags = fcntl(fd, F_GETFD);
	check_result(flags >= 0 && (flags & FD_CLOEXEC) != 0,
		     "执行新程序时关闭句柄");
	for (i = 0; i < 3; ++i) {
		int repeated_fd = nbdriver_open();
		int repeated_flags = repeated_fd < 0 ? -1 :
			fcntl(repeated_fd, F_GETFD);
		int repeated_ok = repeated_fd >= 0 &&
			(repeated_flags & FD_CLOEXEC) != 0;

		if (repeated_fd >= 0)
			(void)close(repeated_fd);
		if (!repeated_ok)
			break;
	}
	check_result(i == 3, "重复打开和关闭");

	memset(&request_info, 0, sizeof(request_info));
	errno = 0;
	result = ioctl(fd, _IO(NBDRIVER_IOC_MAGIC, 0x7f), NULL);
	saved_errno = errno;
	check_result(result == -1 && saved_errno == ENOTTY,
		     "拒绝未知命令");

	errno = 0;
	result = ioctl(fd, NBDRIVER_IOC_GET_ABI, NULL);
	saved_errno = errno;
	check_result(result == -1 && saved_errno == EFAULT,
		     "拒绝空接口指针");

	errno = 0;
	result = ioctl(fd, NBDRIVER_IOC_GET_ABI, (void *)(uintptr_t)1);
	saved_errno = errno;
	check_result(result == -1 && saved_errno == EFAULT,
		     "拒绝无效接口指针");

	memset(&bad_read, 0, sizeof(bad_read));
	bad_read.pid = getpid();
	bad_read.address = (nbdriver_u64)(uintptr_t)&bad_read;
	bad_read.buffer = (nbdriver_u64)(uintptr_t)&request_info;
	result = nbdriver_read_process(fd, &bad_read);
	saved_errno = errno;
	check_result(result == -1 && saved_errno == EINVAL &&
		     bad_read.copied == 0 && bad_read.error == 0,
		     "拒绝无效读取大小");

	memset(&bad_write, 0, sizeof(bad_write));
	bad_write.pid = getpid();
	bad_write.size = NBDRIVER_WRITE_MAX + 1U;
	bad_write.address = (nbdriver_u64)(uintptr_t)&bad_write;
	bad_write.buffer = (nbdriver_u64)(uintptr_t)&request_info;
	result = nbdriver_write_process(fd, &bad_write);
	saved_errno = errno;
	check_result(result == -1 && saved_errno == EINVAL &&
		     bad_write.copied == 0 && bad_write.error == 0,
		     "拒绝无效写入大小");

	errno = 0;
	result = nbdriver_read_process(-1, NULL);
	saved_errno = errno;
	check_result(result == -1 && saved_errno == EINVAL,
		     "读取参数检查");
	errno = 0;
	result = nbdriver_write_process(-1, NULL);
	saved_errno = errno;
	check_result(result == -1 && saved_errno == EINVAL,
		     "写入参数检查");

	closed_fd = nbdriver_open();
	ok = closed_fd >= 0;
	if (ok) {
		(void)close(closed_fd);
		errno = 0;
		result = ioctl(closed_fd, NBDRIVER_IOC_GET_ABI, &request_info);
		saved_errno = errno;
		ok = result == -1 && saved_errno == EBADF;
	}
	check_result(ok, "拒绝已关闭句柄");
	return 0;
}

static int test_self_pages(int fd)
{
	unsigned char *mapping = MAP_FAILED;
	unsigned char *output = NULL;
	unsigned char write_pattern[64];
	unsigned char readback[64];
	struct nbdriver_read_request read_req;
	struct nbdriver_write_request write_req;
	uintptr_t boundary;
	int result;
	int saved_errno;
	int ok;
	size_t i;

	section("本进程读写和边界");
	mapping = mmap(NULL, page_size * 3, PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	output = malloc(128);
	if (mapping == MAP_FAILED || !output) {
		check_result(0, "分配本进程映射");
		if (mapping != MAP_FAILED)
			(void)munmap(mapping, page_size * 3);
		free(output);
		return -1;
	}
	memset(mapping, 0x11, page_size);
	memset(mapping + page_size, 0x22, page_size);
	boundary = (uintptr_t)(mapping + page_size - 19);
	memset(output, 0, 64);
	read_req = read_request(getpid(), (void *)boundary, output, 64);
	result = nbdriver_read_process(fd, &read_req);
	check_result(result == 0 && read_req.error == 0 && read_req.copied == 64 &&
		     output[0] == 0x11 && output[18] == 0x11 &&
		     output[19] == 0x22 && output[63] == 0x22,
		     "本进程跨页读取");

	for (i = 0; i < sizeof(write_pattern); ++i)
		write_pattern[i] = (unsigned char)(0xa0U + i);
	write_req = write_request(getpid(), (void *)(boundary + 3),
					 write_pattern, sizeof(write_pattern));
	result = nbdriver_write_process(fd, &write_req);
	saved_errno = errno;
	if (result != 0 || write_req.error != 0 ||
	    write_req.copied != sizeof(write_pattern))
		print_fault_write("本进程跨页写入", result, saved_errno,
				  &write_req);
	check_result(result == 0 && write_req.error == 0 &&
		     write_req.copied == sizeof(write_pattern),
		     "本进程跨页写入");
	memset(readback, 0, sizeof(readback));
	read_req = read_request(getpid(), (void *)(boundary + 3), readback,
				 sizeof(readback));
	result = nbdriver_read_process(fd, &read_req);
	check_result(result == 0 && read_req.error == 0 &&
		     read_req.copied == sizeof(readback) &&
		     memcmp(readback, write_pattern, sizeof(readback)) == 0,
		     "本进程写后回读");

	read_req = read_request(getpid(), mapping, output, 16);
	read_req.expected_start_ns = read_req.expected_start_ns + 1U;
	result = nbdriver_read_process(fd, &read_req);
	saved_errno = errno;
	check_result(result == -1 && saved_errno == ESTALE &&
		     read_req.error == -ESTALE && read_req.copied == 0 &&
		     read_req.failed_address == (nbdriver_u64)(uintptr_t)mapping,
		     "拒绝过期进程身份");

	if (mprotect(mapping + page_size, page_size, PROT_NONE) != 0) {
		check_result(0, "设置页面保护");
	} else {
		memset(output, 0xcc, 64);
		read_req = read_request(getpid(), mapping + page_size - 13,
					output, 40);
		result = nbdriver_read_process(fd, &read_req);
		saved_errno = errno;
		ok = result == -1 && saved_errno == EACCES &&
			read_req.error == -EACCES && read_req.copied == 13 &&
			read_req.failed_address == (nbdriver_u64)(uintptr_t)(mapping +
									page_size) &&
				output[13] == 0xcc;
		check_result(ok, "保护页部分读取");

		memset(output, 0xcc, 16);
		read_req = read_request(getpid(), mapping + page_size, output, 16);
		result = nbdriver_read_process(fd, &read_req);
		saved_errno = errno;
		check_result(result == -1 && saved_errno == EACCES &&
			     read_req.error == -EACCES && read_req.copied == 0 &&
			     read_req.failed_address == (nbdriver_u64)(uintptr_t)(mapping +
										page_size),
			     "拒绝无读取权限的页面");

		if (munmap(mapping + page_size, page_size) != 0) {
			check_result(0, "创建映射空洞");
		} else {
			memset(output, 0xcc, 64);
			read_req = read_request(getpid(), mapping + page_size - 13,
						output, 40);
			result = nbdriver_read_process(fd, &read_req);
			saved_errno = errno;
			check_result(result == -1 && saved_errno == EFAULT &&
				     read_req.error == -EFAULT && read_req.copied == 13 &&
				     read_req.failed_address ==
					 (nbdriver_u64)(uintptr_t)(mapping + page_size) &&
				     output[13] == 0xcc,
				     "空洞前部分读取");
		}
	}

	{
		void *readonly = mmap(NULL, page_size, PROT_READ,
				       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

		if (readonly == MAP_FAILED) {
			check_result(0, "分配只读映射");
		} else {
			write_req = write_request(getpid(), readonly, write_pattern,
						  sizeof(write_pattern));
			result = nbdriver_write_process(fd, &write_req);
			saved_errno = errno;
			check_result(result == -1 && saved_errno == EACCES &&
				     write_req.error == -EACCES && write_req.copied == 0 &&
				     write_req.failed_address ==
					 (nbdriver_u64)(uintptr_t)readonly,
				     "拒绝写入只读页面");
			(void)munmap(readonly, page_size);
		}
	}

	(void)munmap(mapping, page_size);
	(void)munmap(mapping + page_size * 2, page_size);
	free(output);
	return 0;
}

static int test_fault_in_and_unresident_write(int fd)
{
	unsigned char *mapping = MAP_FAILED;
	unsigned char *write_mapping = MAP_FAILED;
	unsigned char output[16];
	unsigned char write_data[16];
	struct nbdriver_read_request read_req;
	struct nbdriver_write_request write_req;
	unsigned char before;
	unsigned char after = 0;
	int result;
	int saved_errno;
	int ok;
	size_t i;

	section("自动补页和非驻留写入");
	mapping = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED) {
		check_result(0, "分配补页测试映射");
		return -1;
	}
	if (madvise(mapping, page_size, MADV_DONTNEED) != 0 ||
	    mincore_page(mapping, &before) != 0) {
		check_result(0, "检查补页前驻留状态");
		(void)munmap(mapping, page_size);
		return -1;
	}
	if (before) {
		skip_result("本进程页面已驻留，无法验证补页转换");
	} else {
		memset(output, 0xcc, sizeof(output));
		read_req = read_request(getpid(), mapping, output, sizeof(output));
		result = nbdriver_read_process(fd, &read_req);
		saved_errno = errno;
		if (result != 0 || read_req.error != 0 ||
		    read_req.copied != sizeof(output))
			print_fault_read("self_fault_in_read", result, saved_errno,
					 &read_req);
		if (mincore_page(mapping, &after) != 0)
			ok = 0;
		else
			ok = result == 0 && read_req.error == 0 &&
				read_req.copied == sizeof(output) && !before && after &&
				!memcmp(output, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0",
				       sizeof(output));
		check_result(ok, "本进程补页后读取零数据");
		if (after && madvise(mapping, page_size, MADV_DONTNEED) == 0 &&
			mincore_page(mapping, &before) == 0 && !before) {
			memset(output, 0xcc, sizeof(output));
			read_req = read_request(getpid(), mapping, output, sizeof(output));
			result = nbdriver_read_process(fd, &read_req);
			saved_errno = errno;
			if (result != 0 || read_req.error != 0 ||
			    read_req.copied != sizeof(output))
				print_fault_read("页面回收后再次补页", result,
						 saved_errno, &read_req);
			ok = mincore_page(mapping, &after) == 0 && after &&
				result == 0 && read_req.error == 0 &&
				read_req.copied == sizeof(output) &&
				!memcmp(output, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0",
				       sizeof(output));
			check_result(ok, "页面回收后再次补页");
		} else {
			skip_result("未能回收页面，无法验证再次补页");
		}
	}
	(void)munmap(mapping, page_size);

	write_mapping = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
				     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (write_mapping == MAP_FAILED) {
		check_result(0, "分配非驻留写入映射");
		return -1;
	}
	if (madvise(write_mapping, page_size, MADV_DONTNEED) != 0 ||
	    mincore_page(write_mapping, &before) != 0) {
		check_result(0, "检查非驻留写入前状态");
		(void)munmap(write_mapping, page_size);
		return -1;
	}
	if (before) {
		skip_result("页面已驻留，无法验证非驻留写入");
	} else {
		for (i = 0; i < sizeof(write_data); ++i)
			write_data[i] = (unsigned char)(0x70U + i);
		write_req = write_request(getpid(), write_mapping, write_data,
					 sizeof(write_data));
		result = nbdriver_write_process(fd, &write_req);
		saved_errno = errno;
		ok = result == -1 && saved_errno == ENODATA &&
			write_req.error == -ENODATA && write_req.copied == 0 &&
			write_req.failed_address ==
				(nbdriver_u64)(uintptr_t)write_mapping &&
			mincore_page(write_mapping, &after) == 0 && !after;
		check_result(ok, "非驻留写入不触发补页");
	}
	(void)munmap(write_mapping, page_size);
	return 0;
}

struct remote_page_report {
	uintptr_t address;
	int error;
	unsigned char resident;
};

static int test_remote_fault_in(int fd)
{
	int parent_to_child[2] = { -1, -1 };
	int child_to_parent[2] = { -1, -1 };
	pid_t child = -1;
	int status = 0;
	int reaped = 0;
	int result;
	int saved_errno;
	int ok;
	unsigned char command;
	unsigned char output[32];
	struct remote_page_report report;
	struct remote_page_report after_report;
	struct nbdriver_read_request request;

	section("远程进程自动补页");
	if (pipe(parent_to_child) != 0 || pipe(child_to_parent) != 0) {
		close_fd(&parent_to_child[0]);
		close_fd(&parent_to_child[1]);
		close_fd(&child_to_parent[0]);
		close_fd(&child_to_parent[1]);
		check_result(0, "创建远程补页管道");
		return -1;
	}
	child = fork();
	if (child < 0) {
		close_fd(&parent_to_child[0]);
		close_fd(&parent_to_child[1]);
		close_fd(&child_to_parent[0]);
		close_fd(&child_to_parent[1]);
		check_result(0, "创建远程补页进程");
		return -1;
	}
	if (child == 0) {
		void *mapping;
		struct remote_page_report child_report;
		unsigned char child_command;
		unsigned char resident;
		int child_status = 0;

		close_fd(&parent_to_child[1]);
		close_fd(&child_to_parent[0]);
		mapping = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
				      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		memset(&child_report, 0, sizeof(child_report));
		if (mapping == MAP_FAILED) {
			child_report.error = errno;
		} else if (madvise(mapping, page_size, MADV_DONTNEED) != 0 ||
			   mincore_page(mapping, &resident) != 0) {
			child_report.error = errno;
		} else {
			child_report.address = (uintptr_t)mapping;
			child_report.resident = resident;
		}
		if (write_full(child_to_parent[1], &child_report,
			       sizeof(child_report)) != 0)
			child_status = 2;
		if (!child_status && read_full(parent_to_child[0], &child_command,
					       sizeof(child_command)) != 0)
			child_status = 2;
		if (!child_status && child_command == 1U) {
			memset(&child_report, 0, sizeof(child_report));
			if (mincore_page(mapping, &resident) != 0)
				child_report.error = errno;
			else
				child_report.resident = resident;
			if (write_full(child_to_parent[1], &child_report,
				       sizeof(child_report)) != 0)
				child_status = 2;
		}
		if (mapping != MAP_FAILED)
			(void)munmap(mapping, page_size);
		close_fd(&parent_to_child[0]);
		close_fd(&child_to_parent[1]);
		_exit(child_status);
	}
	close_fd(&parent_to_child[0]);
	close_fd(&child_to_parent[1]);
	if (read_full(child_to_parent[0], &report, sizeof(report)) != 0) {
		kill_and_reap(child);
		child = -1;
		close_fd(&parent_to_child[1]);
		close_fd(&child_to_parent[0]);
		check_result(0, "获取远程映射信息");
		return -1;
	}
	if (report.error != 0 || report.address == 0) {
		command = 0;
		(void)write_full(parent_to_child[1], &command, sizeof(command));
		if (wait_child(child, &status) != 0)
			kill_and_reap(child);
		close_fd(&parent_to_child[1]);
		close_fd(&child_to_parent[0]);
		check_result(0, "检查远程补页前状态");
		return -1;
	}
	if (report.resident) {
		command = 0;
		(void)write_full(parent_to_child[1], &command, sizeof(command));
		if (wait_child(child, &status) != 0)
			kill_and_reap(child);
		close_fd(&parent_to_child[1]);
		close_fd(&child_to_parent[0]);
		skip_result("远程页面已驻留，无法验证补页转换");
		return 0;
	}

	memset(output, 0xcc, sizeof(output));
	request = read_request(child, (void *)report.address, output,
				       sizeof(output));
	result = nbdriver_read_process(fd, &request);
	saved_errno = errno;
	if (result != 0 || request.error != 0 || request.copied != sizeof(output))
		print_fault_read("remote_fault_in_read", result, saved_errno, &request);
	ok = result == 0 && request.error == 0 && request.copied == sizeof(output) &&
		!memcmp(output, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0",
		       sizeof(output));
	check_result(ok, "远程补页后读取零数据");

	command = 1U;
	if (write_full(parent_to_child[1], &command, sizeof(command)) != 0 ||
	    read_full(child_to_parent[0], &after_report, sizeof(after_report)) != 0) {
		kill_and_reap(child);
		child = -1;
		reaped = 1;
		close_fd(&parent_to_child[1]);
		close_fd(&child_to_parent[0]);
		check_result(0, "获取远程补页后状态");
		return -1;
	}
	check_result(after_report.error == 0 && after_report.resident != 0,
		     "确认远程页面已驻留");
	if (wait_child(child, &status) == 0)
		reaped = 1;
	else
		kill_and_reap(child);
	check_result(reaped && WIFEXITED(status) && WEXITSTATUS(status) == 0,
		     "远程补页进程清理");
	close_fd(&parent_to_child[1]);
	close_fd(&child_to_parent[0]);
	return 0;
}

static int test_remote_write(int fd)
{
	int parent_to_child[2] = { -1, -1 };
	int child_to_parent[2] = { -1, -1 };
	pid_t child = -1;
	int status = 0;
	int reaped = 0;
	int result;
	int saved_errno;
	int child_ok;
	unsigned char command;
	unsigned char expected[32];
	unsigned char output[32];
	uintptr_t address = 0;
	struct nbdriver_write_request write_req;
	struct nbdriver_read_request read_req;

	section("远程写入和回读");
	memset(expected, 0x6d, sizeof(expected));
	if (pipe(parent_to_child) != 0 || pipe(child_to_parent) != 0) {
		close_fd(&parent_to_child[0]);
		close_fd(&parent_to_child[1]);
		close_fd(&child_to_parent[0]);
		close_fd(&child_to_parent[1]);
		check_result(0, "创建远程写入管道");
		return -1;
	}
	child = fork();
	if (child < 0) {
		close_fd(&parent_to_child[0]);
		close_fd(&parent_to_child[1]);
		close_fd(&child_to_parent[0]);
		close_fd(&child_to_parent[1]);
		check_result(0, "创建远程写入进程");
		return -1;
	}
	if (child == 0) {
		void *mapping;
		unsigned char child_command;
		int child_status = 0;

		close_fd(&parent_to_child[1]);
		close_fd(&child_to_parent[0]);
		mapping = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
				      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (mapping == MAP_FAILED) {
			address = 0;
			if (write_full(child_to_parent[1], &address,
				       sizeof(address)) != 0)
				child_status = 2;
		} else {
			memset(mapping, 0x19, page_size);
			address = (uintptr_t)mapping;
			if (write_full(child_to_parent[1], &address, sizeof(address)) != 0 ||
			    read_full(parent_to_child[0], &child_command,
				      sizeof(child_command)) != 0)
				child_status = 2;
			if (!child_status && child_command == 1U) {
				child_ok = memcmp(mapping, expected, sizeof(expected)) == 0;
				if (write_full(child_to_parent[1], &child_ok,
					       sizeof(child_ok)) != 0)
					child_status = 2;
			}
		}
		if (mapping != MAP_FAILED)
			(void)munmap(mapping, page_size);
		close_fd(&parent_to_child[0]);
		close_fd(&child_to_parent[1]);
		_exit(child_status);
	}
	close_fd(&parent_to_child[0]);
	close_fd(&child_to_parent[1]);
	if (read_full(child_to_parent[0], &address, sizeof(address)) != 0 ||
	    address == 0) {
		kill_and_reap(child);
		child = -1;
		close_fd(&parent_to_child[1]);
		close_fd(&child_to_parent[0]);
		check_result(0, "获取远程写入地址");
		return -1;
	}
	write_req = write_request(child, (void *)address, expected,
					 sizeof(expected));
	result = nbdriver_write_process(fd, &write_req);
	saved_errno = errno;
	if (result != 0 || write_req.error != 0 ||
	    write_req.copied != sizeof(expected))
		print_fault_write("远程写入", result, saved_errno, &write_req);
	check_result(result == 0 && write_req.error == 0 &&
		     write_req.copied == sizeof(expected), "远程写入");

	memset(output, 0, sizeof(output));
	read_req = read_request(child, (void *)address, output, sizeof(output));
	result = nbdriver_read_process(fd, &read_req);
	saved_errno = errno;
	if (result != 0 || read_req.error != 0 || read_req.copied != sizeof(output))
		print_fault_read("远程写后回读", result, saved_errno, &read_req);
	check_result(result == 0 && read_req.error == 0 &&
		     read_req.copied == sizeof(output) &&
		     memcmp(output, expected, sizeof(output)) == 0,
		     "远程写后回读");

	command = 1U;
	if (write_full(parent_to_child[1], &command, sizeof(command)) != 0 ||
	    read_full(child_to_parent[0], &child_ok, sizeof(child_ok)) != 0) {
		kill_and_reap(child);
		child = -1;
		reaped = 1;
		close_fd(&parent_to_child[1]);
		close_fd(&child_to_parent[0]);
		check_result(0, "获取远程直接校验结果");
		return -1;
	}
	check_result(child_ok != 0, "远程进程直接校验");
	if (wait_child(child, &status) == 0)
		reaped = 1;
	else
		kill_and_reap(child);
	check_result(reaped && WIFEXITED(status) && WEXITSTATUS(status) == 0,
		     "远程写入进程清理");
	close_fd(&parent_to_child[1]);
	close_fd(&child_to_parent[0]);
	return 0;
}

static int test_exited_pid(int fd)
{
	pid_t child;
	int status;
	unsigned char output[16];
	struct nbdriver_read_request request;
	int result;
	int saved_errno;

	section("已退出进程");
	child = fork();
	if (child < 0) {
		check_result(0, "创建退出测试进程");
		return -1;
	}
	if (child == 0)
		_exit(0);
	if (wait_child(child, &status) != 0) {
		kill_and_reap(child);
		check_result(0, "回收已退出进程");
		return -1;
	}
	request = read_request(child, (void *)(uintptr_t)page_size, output,
				       sizeof(output));
	result = nbdriver_read_process(fd, &request);
	saved_errno = errno;
	check_result(result == -1 && saved_errno == ESRCH &&
		     request.error == -ESRCH && request.copied == 0 &&
		     request.failed_address == (nbdriver_u64)page_size,
		     "拒绝读取已退出进程");
	return 0;
}

static int test_cow_rejection(int fd)
{
	int parent_to_child[2] = { -1, -1 };
	int child_to_parent[2] = { -1, -1 };
	unsigned char *mapping = MAP_FAILED;
	unsigned char replacement[32];
	unsigned char output[32];
	unsigned char command;
	unsigned char child_ok;
	uintptr_t address;
	pid_t child = -1;
	int status = 0;
	int reaped = 0;
	int result;
	int saved_errno;
	struct nbdriver_write_request write_req;
	struct nbdriver_read_request read_req;

	section("写时复制保护");
	mapping = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED) {
		check_result(0, "分配写时复制映射");
		return -1;
	}
	memset(mapping, 0x41, page_size);
	memset(replacement, 0x92, sizeof(replacement));
	if (pipe(parent_to_child) != 0 || pipe(child_to_parent) != 0) {
		close_fd(&parent_to_child[0]);
		close_fd(&parent_to_child[1]);
		close_fd(&child_to_parent[0]);
		close_fd(&child_to_parent[1]);
		(void)munmap(mapping, page_size);
		check_result(0, "创建写时复制管道");
		return -1;
	}
	child = fork();
	if (child < 0) {
		close_fd(&parent_to_child[0]);
		close_fd(&parent_to_child[1]);
		close_fd(&child_to_parent[0]);
		close_fd(&child_to_parent[1]);
		(void)munmap(mapping, page_size);
		check_result(0, "创建写时复制进程");
		return -1;
	}
	address = (uintptr_t)mapping;
	if (child == 0) {
		unsigned char child_command;
		int child_status = 0;

		close_fd(&parent_to_child[1]);
		close_fd(&child_to_parent[0]);
		if (write_full(child_to_parent[1], &address, sizeof(address)) != 0 ||
		    read_full(parent_to_child[0], &child_command,
			      sizeof(child_command)) != 0)
			child_status = 2;
		if (!child_status && child_command == 1U) {
			child_ok = 1U;
			if (memcmp(mapping, "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
				   sizeof(replacement)) != 0)
				child_ok = 0U;
			if (write_full(child_to_parent[1], &child_ok,
				       sizeof(child_ok)) != 0)
				child_status = 2;
		}
		(void)munmap(mapping, page_size);
		close_fd(&parent_to_child[0]);
		close_fd(&child_to_parent[1]);
		_exit(child_status);
	}
	close_fd(&parent_to_child[0]);
	close_fd(&child_to_parent[1]);
	if (read_full(child_to_parent[0], &address, sizeof(address)) != 0) {
		kill_and_reap(child);
		child = -1;
		close_fd(&parent_to_child[1]);
		close_fd(&child_to_parent[0]);
		(void)munmap(mapping, page_size);
		check_result(0, "获取写时复制地址");
		return -1;
	}
	write_req = write_request(child, (void *)address, replacement,
					 sizeof(replacement));
	result = nbdriver_write_process(fd, &write_req);
	saved_errno = errno;
	check_result(result == -1 && saved_errno == EACCES &&
		     write_req.error == -EACCES && write_req.copied == 0 &&
		     write_req.failed_address == (nbdriver_u64)address,
		     "拒绝绕过写时复制");

	memset(output, 0, sizeof(output));
	read_req = read_request(getpid(), mapping, output, sizeof(output));
	result = nbdriver_read_process(fd, &read_req);
	check_result(result == 0 && read_req.error == 0 &&
		     read_req.copied == sizeof(output) &&
		     !memcmp(output, "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
			     sizeof(output)),
		     "父进程数据未改变");

	command = 1U;
	if (write_full(parent_to_child[1], &command, sizeof(command)) != 0 ||
	    read_full(child_to_parent[0], &child_ok, sizeof(child_ok)) != 0) {
		kill_and_reap(child);
		child = -1;
		reaped = 1;
		close_fd(&parent_to_child[1]);
		close_fd(&child_to_parent[0]);
		(void)munmap(mapping, page_size);
		check_result(0, "获取写时复制校验结果");
		return -1;
	}
	check_result(child_ok != 0, "子进程数据未改变");
	if (wait_child(child, &status) == 0)
		reaped = 1;
	else
		kill_and_reap(child);
	check_result(reaped && WIFEXITED(status) && WEXITSTATUS(status) == 0,
		     "写时复制进程清理");
	close_fd(&parent_to_child[1]);
	close_fd(&child_to_parent[0]);
	(void)munmap(mapping, page_size);
	return 0;
}

struct race_state {
	void *address;
	size_t size;
	atomic_int stop;
	atomic_int thread_error;
	atomic_int thread_errno;
	atomic_uint iterations;
};

static void *madvise_thread(void *argument)
{
	struct race_state *state = argument;

	while (!atomic_load_explicit(&state->stop, memory_order_relaxed)) {
		if (madvise(state->address, state->size, MADV_DONTNEED) != 0) {
			atomic_store(&state->thread_errno, errno);
			atomic_store(&state->thread_error, 1);
			break;
		}
		atomic_fetch_add(&state->iterations, 1U);
	}
	return NULL;
}

static int test_concurrent_madvise(int fd)
{
	struct race_state state;
	struct nbdriver_read_request request;
	unsigned char *mapping;
	unsigned char output[16];
	pthread_t thread;
	unsigned int completed = 0;
	unsigned int transient_errors = 0;
	int thread_created = 0;
	int result;
	int saved_errno;
	int unexpected = 0;

	section("并发回收页面和读取");
	mapping = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED) {
		check_result(0, "分配并发测试映射");
		return -1;
	}
	memset(mapping, 0x5a, page_size);
	state.address = mapping;
	state.size = page_size;
	atomic_init(&state.stop, 0);
	atomic_init(&state.thread_error, 0);
	atomic_init(&state.thread_errno, 0);
	atomic_init(&state.iterations, 0);
	if (pthread_create(&thread, NULL, madvise_thread, &state) != 0) {
		check_result(0, "创建页面回收线程");
		(void)munmap(mapping, page_size);
		return -1;
	}
	thread_created = 1;
	for (completed = 0; completed < 1000U; ++completed) {
		memset(output, 0xcc, sizeof(output));
		request = read_request(getpid(), mapping, output, sizeof(output));
		result = nbdriver_read_process(fd, &request);
		saved_errno = errno;
		if (result == 0 && request.error == 0 && request.copied ==
			    sizeof(output))
			continue;
		if (result == -1 && (saved_errno == EAGAIN ||
				     saved_errno == ENODATA || saved_errno == EFAULT)) {
			transient_errors++;
			continue;
		}
		print_fault_read("concurrent_read", result, saved_errno, &request);
		unexpected = 1;
		break;
	}
	atomic_store(&state.stop, 1);
	if (thread_created)
		(void)pthread_join(thread, NULL);
	if (atomic_load(&state.thread_error)) {
		fprintf(stderr, "  详细：回收线程错误=%d(%s)，循环=%u\n",
		       atomic_load(&state.thread_errno),
		       error_text(atomic_load(&state.thread_errno)),
		       atomic_load(&state.iterations));
	}
	printf("  并发读取=%u，允许的瞬时错误=%u，回收循环=%u\n",
	       completed, transient_errors, atomic_load(&state.iterations));
	check_result(completed == 1000U && !unexpected &&
		     !atomic_load(&state.thread_error),
		     "千次并发读取和清理");
	(void)munmap(mapping, page_size);
	return 0;
}

static int test_inherited_fd_downgrade(int fd)
{
	pid_t child;
	int status;

	section("降权后继承句柄");
	child = fork();
	if (child < 0) {
		check_result(0, "创建降权进程");
		return -1;
	}
	if (child == 0) {
		unsigned char output[16];
		struct nbdriver_read_request request;
		int result;
		int saved_errno;

		if (setgid(65534) != 0 || setuid(65534) != 0)
			_exit(2);
		request = read_request(getpid(), output, output, sizeof(output));
		result = nbdriver_read_process(fd, &request);
		saved_errno = errno;
		_exit(result == -1 && saved_errno == EPERM ? 0 : 1);
	}
	if (wait_child(child, &status) != 0) {
		kill_and_reap(child);
		check_result(0, "回收降权进程");
		return -1;
	}
	if (WIFEXITED(status) && WEXITSTATUS(status) == 2) {
		skip_result("系统不允许降权");
		return 0;
	}
	check_result(WIFEXITED(status) && WEXITSTATUS(status) == 0,
		     "降权后拒绝读取");
	return 0;
}

static int test_executable_mapping(int fd)
{
	section("可执行页面写入");
#if defined(__aarch64__)
	{
		uint32_t *code;
		uint32_t instructions[2];
		struct nbdriver_write_request request;
		int result;
		int saved_errno;
		int (*function)(void);
		const uint32_t old_value = 0x1a5U;
		const uint32_t value = 0x5a3U;

		code = mmap(NULL, page_size, PROT_READ | PROT_WRITE | PROT_EXEC,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (code == MAP_FAILED) {
			skip_result("系统不允许可执行匿名映射");
			return 0;
		}
		code[0] = 0x52800000U | (old_value << 5);
		code[1] = 0xd65f03c0U;
		(void)code[0];
		__builtin___clear_cache((char *)code,
					(char *)code + sizeof(instructions));
		function = (int (*)(void))(uintptr_t)code;
		check_result(function() == (int)old_value, "执行初始指令");
		instructions[0] = 0x52800000U | (value << 5);
		instructions[1] = 0xd65f03c0U;
		request = write_request(getpid(), code, instructions,
					 sizeof(instructions));
		result = nbdriver_write_process(fd, &request);
		saved_errno = errno;
		if (result != 0 || request.error != 0 || request.copied !=
			    sizeof(instructions))
			print_fault_write("rwx_code_write", result, saved_errno, &request);
		check_result(result == 0 && request.error == 0 &&
			     request.copied == sizeof(instructions) &&
			     function() == (int)value,
			     "驱动写入后直接执行");
		(void)munmap(code, page_size);
	}
#else
	(void)fd;
	skip_result("当前不是ARM64，跳过指令执行");
#endif
	return 0;
}

#define READ_SCATTER_COUNT 512U
#define READ_SPEED_BYTES (64U * 1024U * 1024U)

static size_t scatter_offset(size_t index)
{
	return ((index * 251U) & (READ_SCATTER_COUNT - 1U)) * page_size +
		(index * 37U) % (page_size - sizeof(uint64_t) + 1U);
}

static void fill_read_fixture(unsigned char *mapping, size_t size)
{
	size_t i;

	for (i = 0; i < size; ++i)
		mapping[i] = (unsigned char)(i * 13U + (i >> 8) * 17U +
					    (i >> 12) * 7U + 0x27U);
}

static int test_read_matrix(int fd, pid_t pid, uintptr_t address,
			    const unsigned char *expected, size_t span,
			    const char *scope)
{
	static const size_t sizes[] = {
		1, 2, 4, 8, 12, 16, 32, 64, 128, 256, 512, 1024,
		4096, 16384, NBDRIVER_READ_MAX,
	};
	const size_t offsets[] = { 0, 1, page_size - 3 };
	struct nbdriver_read_request request;
	unsigned char *output;
	uint64_t values[READ_SCATTER_COUNT];
	nbdriver_u64 identity = 0;
	char name[80];
	size_t i, j;
	int result, saved_errno, ok;

	section(!strcmp(scope, "self") ? "本进程多项读取" : "远程进程多项读取");
	output = malloc(NBDRIVER_READ_MAX + 2U);
	if (!output) {
		check_result(0, "分配多项读取缓冲");
		return -1;
	}
	for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
		ok = 1;
		for (j = 0; j < sizeof(offsets) / sizeof(offsets[0]); ++j) {
			memset(output, 0xcc, sizes[i] + 2U);
			request = read_request(pid, (void *)(address + offsets[j]),
					       output + 1, sizes[i]);
			request.expected_start_ns = identity;
			errno = 0;
			result = nbdriver_read_process(fd, &request);
			saved_errno = errno;
			if (result || request.error || request.copied != sizes[i] ||
			    request.failed_address || !request.expected_start_ns ||
			    memcmp(output + 1, expected + offsets[j], sizes[i]) ||
			    output[0] != 0xcc || output[sizes[i] + 1U] != 0xcc) {
				fprintf(stderr, "  详细：读取大小=%zu字节，偏移=%zu字节\n", sizes[i], offsets[j]);
				print_fault_read("read_matrix", result, saved_errno, &request);
				ok = 0;
				break;
			}
			identity = request.expected_start_ns;
		}
		snprintf(name, sizeof(name), "%zu字节的对齐、非对齐和页边界读取", sizes[i]);
		check_result(ok, name);
	}
	free(output);

	ok = 1;
	for (i = 0; i < READ_SCATTER_COUNT; ++i) {
		size_t offset = scatter_offset(i);

		request = read_request(pid, (void *)(address + offset), &values[i],
				       sizeof(values[i]));
		request.expected_start_ns = identity;
		errno = 0;
		result = nbdriver_read_process(fd, &request);
		saved_errno = errno;
		if (result || request.error || request.copied != sizeof(values[i]) ||
		    request.failed_address ||
		    memcmp(&values[i], expected + offset, sizeof(values[i]))) {
			fprintf(stderr, "  详细：离散读取第%zu项，偏移=%zu字节\n", i, offset);
			print_fault_read("scatter_read", result, saved_errno, &request);
			ok = 0;
			break;
		}
	}
	snprintf(name, sizeof(name), "512个离散地址读取");
	check_result(ok, name);


	{
		const struct {
			const char *name;
			uintptr_t address;
			size_t size;
			int error_number;
			int request_error;
			nbdriver_u32 copied;
			uintptr_t failed_address;
		} cases[] = {
			{ "错误前正常读取", address + 101, 8, 0, 0, 8, 0 },
			{ "零长度", address, 0, EINVAL, 0, 0, 0 },
			{ "无效地址", 1, 8, EFAULT, -EFAULT, 0, 1 },
			{ "超出单次上限", address, NBDRIVER_READ_MAX + 1U, EINVAL, 0, 0, 0 },
			{ "无权限页面", address + span, 8, EACCES, -EACCES, 0,
			  address + span },
			{ "保护页前部分读取", address + span - 4, 8, EACCES, -EACCES, 4,
			  address + span },
			{ "映射空洞", address + span + page_size, 8, EFAULT, -EFAULT, 0,
			  address + span + page_size },
			{ "错误后正常读取", address + 203, 8, 0, 0, 8, 0 },
		};
		unsigned char bytes[16];

		for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
			memset(bytes, 0xcc, sizeof(bytes));
			request = read_request(pid, (void *)cases[i].address, bytes,
					       cases[i].size);
			request.expected_start_ns = identity;
			errno = 0;
			result = nbdriver_read_process(fd, &request);
			saved_errno = errno;
			ok = result == (cases[i].error_number ? -1 : 0) &&
				saved_errno == cases[i].error_number &&
				request.error == cases[i].request_error &&
				request.copied == cases[i].copied &&
				request.failed_address == cases[i].failed_address;
			if (cases[i].copied)
				ok = ok && !memcmp(bytes, expected + (cases[i].address - address),
						cases[i].copied);
			for (j = cases[i].copied; j < sizeof(bytes); ++j)
				ok = ok && bytes[j] == 0xcc;
			if (!ok)
				fprintf(stderr, "  详细：混合读取第%zu项，返回=%d，错误=%d(%s)，已读=%u，内部错误=%d\n",
					i, result, saved_errno, error_text(saved_errno), request.copied,
					request.error);
			snprintf(name, sizeof(name), "混合读取：%s", cases[i].name);
			check_result(ok, name);
		}
	}
	return 0;
}

static int read_speed_operation(int fd, pid_t pid, uintptr_t address,
				size_t size, int scattered, unsigned char *output,
				nbdriver_u64 *identity, uint64_t *calls)
{
	struct nbdriver_read_request request;
	size_t done = 0;
	size_t count = scattered ? READ_SCATTER_COUNT :
		(size + NBDRIVER_READ_MAX - 1U) / NBDRIVER_READ_MAX;
	size_t i;
	int result, saved_errno;

	for (i = 0; i < count; ++i) {
		size_t offset = scattered ? scatter_offset(i) : done;
		size_t length = scattered ? sizeof(uint64_t) : size - done;

		if (length > NBDRIVER_READ_MAX)
			length = NBDRIVER_READ_MAX;
		request = read_request(pid, (void *)(address + offset), output + done,
				       length);
		request.expected_start_ns = *identity;
		result = nbdriver_read_process(fd, &request);
		saved_errno = errno;
		(*calls)++;
		if (result || request.error || request.copied != length ||
		    request.failed_address || !request.expected_start_ns) {
			print_fault_read("read_speed_operation", result, saved_errno, &request);
			return -1;
		}
		*identity = request.expected_start_ns;
		done += length;
	}
	return 0;
}

static int read_speed_integrity(const unsigned char *output,
				const unsigned char *expected, size_t size, int scattered)
{
	size_t i;

	if (!scattered)
		return !memcmp(output, expected, size);
	for (i = 0; i < READ_SCATTER_COUNT; ++i) {
		if (memcmp(output + i * sizeof(uint64_t), expected + scatter_offset(i),
			   sizeof(uint64_t)))
			return 0;
	}
	return 1;
}

static int test_read_speed(int fd, pid_t pid, uintptr_t address,
			   const unsigned char *expected, const char *scope)
{
	static const struct {
		const char *label;
		size_t size;
		unsigned int loops;
		int scattered;
	} plans[] = {
		{ "8B", 8, 10000, 0 },
		{ "64B", 64, 10000, 0 },
		{ "4KiB", 4096, 5000, 0 },
		{ "64KiB", 65536, 1000, 0 },
		{ "512KiB", 512U * 1024U, 200, 0 },
		{ "8MiB", 8U * 1024U * 1024U, 16, 0 },
		{ "64MiB", READ_SPEED_BYTES, 8, 0 },
		{ "512x8B", READ_SCATTER_COUNT * sizeof(uint64_t), 100, 1 },
	};
	unsigned char *output;
	size_t plan;
	char name[80];
	uint64_t total_bytes = 0;
	uint64_t total_calls = 0;

	section(!strcmp(scope, "self") ? "本进程读取速度" : "远程进程读取速度");
	printf("  驻留页面预热后测3轮，取每轮平均耗时的中值；整块校验不计时\n");
	printf("  超过64KiB分块读取，512项离散读取逐项调用\n");
	output = malloc(READ_SPEED_BYTES + 2U);
	if (!output) {
		check_result(0, "分配测速缓冲");
		return -1;
	}
	for (plan = 0; plan < sizeof(plans) / sizeof(plans[0]); ++plan) {
		double per_op[3] = { 0 };
		uint64_t calls = 0;
		nbdriver_u64 identity = 0;
		unsigned int round, loop;
		int ok = 1;
		size_t calls_per_op = plans[plan].scattered ? READ_SCATTER_COUNT :
			(plans[plan].size + NBDRIVER_READ_MAX - 1U) / NBDRIVER_READ_MAX;

		memset(output, 0xcc, plans[plan].size + 2U);
		for (loop = 0; loop < (plans[plan].size > 512U * 1024U ? 2U : 32U); ++loop) {
			if (read_speed_operation(fd, pid, address, plans[plan].size,
						 plans[plan].scattered, output + 1, &identity,
						 &calls)) {
				ok = 0;
				break;
			}
		}
		ok = ok && read_speed_integrity(output + 1, expected, plans[plan].size,
						plans[plan].scattered);
		for (round = 0; ok && round < 3U; ++round) {
			struct timespec start, end;
			int64_t elapsed;
			uint64_t expected_calls = (uint64_t)plans[plan].loops * calls_per_op;

			calls = 0;
			memset(output, 0xcc, plans[plan].size + 2U);
			if (clock_gettime(CLOCK_MONOTONIC, &start)) {
				ok = 0;
				break;
			}
			for (loop = 0; loop < plans[plan].loops; ++loop) {
				if (alarm_fired || read_speed_operation(fd, pid, address,
						plans[plan].size, plans[plan].scattered,
						output + 1, &identity, &calls)) {
					ok = 0;
					break;
				}
			}
			if (clock_gettime(CLOCK_MONOTONIC, &end)) {
				ok = 0;
				break;
			}
			elapsed = (int64_t)(end.tv_sec - start.tv_sec) * INT64_C(1000000000) +
				end.tv_nsec - start.tv_nsec;
			ok = ok && elapsed > 0 && calls == expected_calls &&
				read_speed_integrity(output + 1, expected, plans[plan].size,
						     plans[plan].scattered) &&
				output[0] == 0xcc && output[plans[plan].size + 1U] == 0xcc;
			if (!ok) {
				fprintf(stderr, "  详细：%s第%u轮，完成=%u/%u，调用=%llu/%llu，数据校验失败\n",
					plans[plan].label, round + 1U, loop, plans[plan].loops,
					(unsigned long long)calls, (unsigned long long)expected_calls);
				break;
			}
			per_op[round] = (double)elapsed / plans[plan].loops / 1000.0;
		}
		if (ok) {
			double temporary;
			unsigned int i, j;

			for (i = 0; i < 3U; ++i) {
				for (j = i + 1U; j < 3U; ++j) {
					if (per_op[i] > per_op[j]) {
						temporary = per_op[i];
						per_op[i] = per_op[j];
						per_op[j] = temporary;
					}
				}
			}
			if (plans[plan].scattered)
				printf("  512项离散：%.2f微秒/组，%.3f微秒/项\n",
				       per_op[1], per_op[1] / READ_SCATTER_COUNT);
			else
				printf("  %-7s %9.3f微秒/次，%8.2f MiB/s\n",
				       plans[plan].label, per_op[1],
				       plans[plan].size / (1024.0 * 1024.0) * 1000000.0 / per_op[1]);
			total_bytes += (uint64_t)plans[plan].size * plans[plan].loops * 3U;
			total_calls += calls_per_op * (uint64_t)plans[plan].loops * 3U;
		}
		snprintf(name, sizeof(name), "%s读取测速", plans[plan].label);
		check_result(ok, name);
		if (alarm_fired)
			break;
	}
	printf("  完成：累计%.2f GiB，共%llu次读取调用\n",
	       total_bytes / (1024.0 * 1024.0 * 1024.0), (unsigned long long)total_calls);
	free(output);
	return 0;
}

static int test_multi_reads_and_speed(int fd)
{
	struct {
		uintptr_t address;
		int error;
	} report = { 0 };
	size_t span = page_size * READ_SCATTER_COUNT;
	size_t total;
	unsigned char *mapping;
	int parent_to_child[2] = { -1, -1 };
	int child_to_parent[2] = { -1, -1 };
	pid_t child = -1;
	unsigned char command = 1;
	int status = 0;

	section("大量读取准备");
	if (span < READ_SPEED_BYTES + page_size)
		span = (READ_SPEED_BYTES + page_size * 2U - 1U) / page_size * page_size;
	total = span + page_size * 3U;
	mapping = mmap(NULL, total, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED) {
		check_result(0, "分配大量读取映射");
		return -1;
	}
	fill_read_fixture(mapping, span);
	if (mprotect(mapping + span, page_size, PROT_NONE) ||
	    mprotect(mapping + span + page_size * 2U, page_size, PROT_NONE) ||
	    munmap(mapping + span + page_size, page_size)) {
		check_result(0, "设置大量读取保护页");
		(void)munmap(mapping, total);
		return -1;
	}
	(void)test_read_matrix(fd, getpid(), (uintptr_t)mapping, mapping, span, "self");
	(void)test_read_speed(fd, getpid(), (uintptr_t)mapping, mapping, "self");
	if (pipe(parent_to_child) || pipe(child_to_parent)) {
		check_result(0, "创建大量读取管道");
		goto cleanup;
	}
	child = fork();
	if (child < 0) {
		check_result(0, "创建大量读取进程");
		goto cleanup;
	}
	if (!child) {
		unsigned char *remote;
		int child_status = 0;

		close_fd(&parent_to_child[1]);
		close_fd(&child_to_parent[0]);
		remote = mmap(NULL, total, PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (remote == MAP_FAILED) {
			report.error = errno;
		} else {
			fill_read_fixture(remote, span);
			if (mprotect(remote + span, page_size, PROT_NONE) ||
			    mprotect(remote + span + page_size * 2U, page_size, PROT_NONE) ||
			    munmap(remote + span + page_size, page_size))
				report.error = errno;
			else
				report.address = (uintptr_t)remote;
		}
		if (write_full(child_to_parent[1], &report, sizeof(report)))
			child_status = 2;
		if (!child_status && read_full(parent_to_child[0], &command, sizeof(command)))
			child_status = 2;
		if (remote != MAP_FAILED)
			(void)munmap(remote, total);
		close_fd(&parent_to_child[0]);
		close_fd(&child_to_parent[1]);
		_exit(child_status);
	}
	close_fd(&parent_to_child[0]);
	close_fd(&child_to_parent[1]);
	if (read_full(child_to_parent[0], &report, sizeof(report)) ||
	    report.error || !report.address) {
		fprintf(stderr, "  详细：远程测试进程准备失败，错误=%d\n", report.error);
		check_result(0, "远程大量读取进程准备");
		goto cleanup;
	}
	(void)test_read_matrix(fd, child, report.address, mapping, span, "remote");
	(void)test_read_speed(fd, child, report.address, mapping, "remote");
	section("大量读取清理");
	if (write_full(parent_to_child[1], &command, sizeof(command)) ||
	    wait_child(child, &status)) {
		check_result(0, "远程大量读取进程清理");
		goto cleanup;
	}
	child = -1;
	check_result(WIFEXITED(status) && !WEXITSTATUS(status), "远程测试进程清理");
cleanup:
	close_fd(&parent_to_child[0]);
	close_fd(&parent_to_child[1]);
	close_fd(&child_to_parent[0]);
	close_fd(&child_to_parent[1]);
	if (child > 0)
		kill_and_reap(child);
	(void)munmap(mapping, span + page_size);
	(void)munmap(mapping + span + page_size * 2U, page_size);
	return 0;
}

int main(void)
{
	struct sigaction action;
	struct nbdriver_abi_info info = { 0 };
	int fd = -1;
	int result;

	setvbuf(stdout, NULL, _IONBF, 0);
	(void)signal(SIGPIPE, SIG_IGN);
	memset(&action, 0, sizeof(action));
	action.sa_handler = alarm_handler;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGALRM, &action, NULL) != 0) {
		fprintf(stderr, "初始化失败：设置信号处理器，错误=%d(%s)\n", errno,
			error_text(errno));
		return 2;
	}
	if (geteuid() != 0) {
		fprintf(stderr, "初始化失败：需要root权限，当前用户=%u\n",
			(unsigned int)geteuid());
		return 2;
	}
	result = (int)sysconf(_SC_PAGESIZE);
	if (result <= 0) {
		fprintf(stderr, "初始化失败：获取页面大小，错误=%d(%s)\n",
			errno, error_text(errno));
		return 2;
	}
	page_size = (size_t)result;
	alarm(90);

	fd = nbdriver_open();
	if (fd < 0) {
		fprintf(stderr, "初始化失败：打开驱动，错误=%d(%s)\n", errno,
			error_text(errno));
		alarm(0);
		return 2;
	}
	if (nbdriver_get_abi(fd, &info) != 0) {
		fprintf(stderr, "初始化失败：读取接口版本，错误=%d(%s)\n", errno,
			error_text(errno));
		close_fd(&fd);
		alarm(0);
		return 2;
	}
	printf("测试程序：进程=%d，页面=%zu字节，接口版本=%u，功能=0x%x\n",
	       getpid(), page_size, info.version, info.features);
	if (info.version != NBDRIVER_ABI_VERSION || info.size != sizeof(info) ||
	    (info.features & (NBDRIVER_FEATURE_READ_PROCESS |
			      NBDRIVER_FEATURE_WRITE_PROCESS |
			      NBDRIVER_FEATURE_READ_FAULT_IN)) !=
			      (NBDRIVER_FEATURE_READ_PROCESS |
			       NBDRIVER_FEATURE_WRITE_PROCESS |
			       NBDRIVER_FEATURE_READ_FAULT_IN)) {
		fprintf(stderr, "初始化失败：驱动接口版本或功能不匹配\n");
		close_fd(&fd);
		alarm(0);
		return 2;
	}

	(void)test_abi_and_fd(fd, &info);
	(void)test_self_pages(fd);
	(void)test_fault_in_and_unresident_write(fd);
	(void)test_remote_fault_in(fd);
	(void)test_remote_write(fd);
	(void)test_exited_pid(fd);
	(void)test_cow_rejection(fd);
	(void)test_concurrent_madvise(fd);
	(void)test_executable_mapping(fd);
	(void)test_multi_reads_and_speed(fd);
	(void)test_inherited_fd_downgrade(fd);

	alarm(0);
	if (alarm_fired) {
		fprintf(stderr, "详细：测试超过90秒，已超时\n");
		failures++;
	}
	close_fd(&fd);
	section_finish();
	printf("\n结果：通过=%d，失败=%d，跳过=%d\n", passes, failures, skips);
	printf("测试结果：%s\n", failures ? "失败" : "通过");
	return failures ? 1 : 0;
}
