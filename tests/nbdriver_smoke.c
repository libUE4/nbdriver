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
static size_t page_size;
static volatile sig_atomic_t alarm_fired;

static void alarm_handler(int signal_number)
{
	(void)signal_number;
	alarm_fired = 1;
}

static void section(const char *name)
{
	printf("\n[%s]\n", name);
}

static void check_result(int ok, const char *name)
{
	printf("  %-42s %s\n", name, ok ? "PASS" : "FAIL");
	if (ok)
		passes++;
	else
		failures++;
}

static void skip_result(const char *name)
{
	printf("  %-42s SKIP\n", name);
	skips++;
}

static void print_fault_read(const char *name, int rc, int saved_errno,
			     const struct nbdriver_read_request *request)
{
	printf("  %s: rc=%d errno=%d(%s) copied=%u error=%d "
	       "failed_address=0x%llx\n",
	       name, rc, saved_errno, strerror(saved_errno), request->copied,
	       request->error, (unsigned long long)request->failed_address);
}

static void print_fault_write(const char *name, int rc, int saved_errno,
			      const struct nbdriver_write_request *request)
{
	printf("  %s: rc=%d errno=%d(%s) copied=%u error=%d "
	       "failed_address=0x%llx\n",
	       name, rc, saved_errno, strerror(saved_errno), request->copied,
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

	section("ABI / FD / IOCTL");
	check_result(info->version == NBDRIVER_ABI_VERSION, "abi_version");
	check_result(info->size == sizeof(*info) && info->reserved == 0,
		     "abi_layout");
	check_result((info->features & NBDRIVER_FEATURE_READ_PROCESS) != 0,
		     "read_feature");
	check_result((info->features & NBDRIVER_FEATURE_WRITE_PROCESS) != 0,
		     "write_feature");
	check_result((info->features & NBDRIVER_FEATURE_READ_FAULT_IN) != 0,
		     "read_fault_in_feature");
	check_result(_IOC_SIZE(NBDRIVER_IOC_GET_ABI) == sizeof(*info) &&
		     _IOC_SIZE(NBDRIVER_IOC_READ_PROCESS) ==
			     sizeof(struct nbdriver_read_request) &&
		     _IOC_SIZE(NBDRIVER_IOC_WRITE_PROCESS) ==
			     sizeof(struct nbdriver_write_request),
		     "ioctl_layout");

	flags = fcntl(fd, F_GETFD);
	check_result(flags >= 0 && (flags & FD_CLOEXEC) != 0,
		     "fd_cloexec");
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
	check_result(i == 3, "repeated_open_close");

	memset(&request_info, 0, sizeof(request_info));
	errno = 0;
	result = ioctl(fd, _IO(NBDRIVER_IOC_MAGIC, 0x7f), NULL);
	saved_errno = errno;
	check_result(result == -1 && saved_errno == ENOTTY,
		     "unknown_ioctl");

	errno = 0;
	result = ioctl(fd, NBDRIVER_IOC_GET_ABI, NULL);
	saved_errno = errno;
	check_result(result == -1 && saved_errno == EFAULT,
		     "null_get_abi");

	errno = 0;
	result = ioctl(fd, NBDRIVER_IOC_GET_ABI, (void *)(uintptr_t)1);
	saved_errno = errno;
	check_result(result == -1 && saved_errno == EFAULT,
		     "bad_get_abi_pointer");

	memset(&bad_read, 0, sizeof(bad_read));
	bad_read.pid = getpid();
	bad_read.address = (nbdriver_u64)(uintptr_t)&bad_read;
	bad_read.buffer = (nbdriver_u64)(uintptr_t)&request_info;
	result = nbdriver_read_process(fd, &bad_read);
	saved_errno = errno;
	check_result(result == -1 && saved_errno == EINVAL &&
		     bad_read.copied == 0 && bad_read.error == 0,
		     "bad_read_size");

	memset(&bad_write, 0, sizeof(bad_write));
	bad_write.pid = getpid();
	bad_write.size = NBDRIVER_WRITE_MAX + 1U;
	bad_write.address = (nbdriver_u64)(uintptr_t)&bad_write;
	bad_write.buffer = (nbdriver_u64)(uintptr_t)&request_info;
	result = nbdriver_write_process(fd, &bad_write);
	saved_errno = errno;
	check_result(result == -1 && saved_errno == EINVAL &&
		     bad_write.copied == 0 && bad_write.error == 0,
		     "bad_write_size");

	errno = 0;
	result = nbdriver_read_process(-1, NULL);
	saved_errno = errno;
	check_result(result == -1 && saved_errno == EINVAL,
		     "sdk_bad_read_arguments");
	errno = 0;
	result = nbdriver_write_process(-1, NULL);
	saved_errno = errno;
	check_result(result == -1 && saved_errno == EINVAL,
		     "sdk_bad_write_arguments");

	closed_fd = nbdriver_open();
	ok = closed_fd >= 0;
	if (ok) {
		(void)close(closed_fd);
		errno = 0;
		result = ioctl(closed_fd, NBDRIVER_IOC_GET_ABI, &request_info);
		saved_errno = errno;
		ok = result == -1 && saved_errno == EBADF;
	}
	check_result(ok, "closed_fd_rejected");
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

	section("SELF READ / WRITE / BOUNDARIES");
	mapping = mmap(NULL, page_size * 3, PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	output = malloc(128);
	if (mapping == MAP_FAILED || !output) {
		check_result(0, "allocate_self_mapping");
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
		     "self_cross_page_read");

	for (i = 0; i < sizeof(write_pattern); ++i)
		write_pattern[i] = (unsigned char)(0xa0U + i);
	write_req = write_request(getpid(), (void *)(boundary + 3),
					 write_pattern, sizeof(write_pattern));
	result = nbdriver_write_process(fd, &write_req);
	saved_errno = errno;
	if (result != 0 || write_req.error != 0 ||
	    write_req.copied != sizeof(write_pattern))
		print_fault_write("self_cross_page_write", result, saved_errno,
				  &write_req);
	check_result(result == 0 && write_req.error == 0 &&
		     write_req.copied == sizeof(write_pattern),
		     "self_cross_page_write");
	memset(readback, 0, sizeof(readback));
	read_req = read_request(getpid(), (void *)(boundary + 3), readback,
				 sizeof(readback));
	result = nbdriver_read_process(fd, &read_req);
	check_result(result == 0 && read_req.error == 0 &&
		     read_req.copied == sizeof(readback) &&
		     memcmp(readback, write_pattern, sizeof(readback)) == 0,
		     "self_write_readback");

	read_req = read_request(getpid(), mapping, output, 16);
	read_req.expected_start_ns = read_req.expected_start_ns + 1U;
	result = nbdriver_read_process(fd, &read_req);
	saved_errno = errno;
	print_fault_read("stale_process_identity", result, saved_errno, &read_req);
	check_result(result == -1 && saved_errno == ESTALE &&
		     read_req.error == -ESTALE && read_req.copied == 0 &&
		     read_req.failed_address == (nbdriver_u64)(uintptr_t)mapping,
		     "stale_process_identity");

	if (mprotect(mapping + page_size, page_size, PROT_NONE) != 0) {
		check_result(0, "protect_partial_page");
	} else {
		memset(output, 0xcc, 64);
		read_req = read_request(getpid(), mapping + page_size - 13,
					output, 40);
		result = nbdriver_read_process(fd, &read_req);
		saved_errno = errno;
		print_fault_read("partial_prot_none", result, saved_errno, &read_req);
		ok = result == -1 && saved_errno == EACCES &&
			read_req.error == -EACCES && read_req.copied == 13 &&
			read_req.failed_address == (nbdriver_u64)(uintptr_t)(mapping +
									page_size) &&
				output[13] == 0xcc;
		check_result(ok, "cross_page_partial_protection");

		memset(output, 0xcc, 16);
		read_req = read_request(getpid(), mapping + page_size, output, 16);
		result = nbdriver_read_process(fd, &read_req);
		saved_errno = errno;
		print_fault_read("prot_none_read", result, saved_errno, &read_req);
		check_result(result == -1 && saved_errno == EACCES &&
			     read_req.error == -EACCES && read_req.copied == 0 &&
			     read_req.failed_address == (nbdriver_u64)(uintptr_t)(mapping +
										page_size),
			     "prot_none_read_rejected");

		if (munmap(mapping + page_size, page_size) != 0) {
			check_result(0, "create_hole");
		} else {
			memset(output, 0xcc, 64);
			read_req = read_request(getpid(), mapping + page_size - 13,
						output, 40);
			result = nbdriver_read_process(fd, &read_req);
			saved_errno = errno;
			print_fault_read("partial_hole", result, saved_errno, &read_req);
			check_result(result == -1 && saved_errno == EFAULT &&
				     read_req.error == -EFAULT && read_req.copied == 13 &&
				     read_req.failed_address ==
					 (nbdriver_u64)(uintptr_t)(mapping + page_size) &&
				     output[13] == 0xcc,
				     "cross_page_partial_hole");
		}
	}

	{
		void *readonly = mmap(NULL, page_size, PROT_READ,
				       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

		if (readonly == MAP_FAILED) {
			check_result(0, "allocate_readonly_mapping");
		} else {
			write_req = write_request(getpid(), readonly, write_pattern,
						  sizeof(write_pattern));
			result = nbdriver_write_process(fd, &write_req);
			saved_errno = errno;
			print_fault_write("readonly_write", result, saved_errno, &write_req);
			check_result(result == -1 && saved_errno == EACCES &&
				     write_req.error == -EACCES && write_req.copied == 0 &&
				     write_req.failed_address ==
					 (nbdriver_u64)(uintptr_t)readonly,
				     "readonly_write_rejected");
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

	section("FAULT-IN / NONRESIDENT WRITE");
	mapping = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED) {
		check_result(0, "allocate_fault_mapping");
		return -1;
	}
	if (madvise(mapping, page_size, MADV_DONTNEED) != 0 ||
	    mincore_page(mapping, &before) != 0) {
		check_result(0, "mincore_fault_mapping");
		(void)munmap(mapping, page_size);
		return -1;
	}
	if (before) {
		skip_result("self_fault_in_nonresident_transition");
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
		check_result(ok, "self_fault_in_zero_read");
		if (after && madvise(mapping, page_size, MADV_DONTNEED) == 0 &&
			mincore_page(mapping, &before) == 0 && !before) {
			memset(output, 0xcc, sizeof(output));
			read_req = read_request(getpid(), mapping, output, sizeof(output));
			result = nbdriver_read_process(fd, &read_req);
			saved_errno = errno;
			if (result != 0 || read_req.error != 0 ||
			    read_req.copied != sizeof(output))
				print_fault_read("madvise_dontneed_refault", result,
						 saved_errno, &read_req);
			ok = mincore_page(mapping, &after) == 0 && after &&
				result == 0 && read_req.error == 0 &&
				read_req.copied == sizeof(output) &&
				!memcmp(output, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0",
				       sizeof(output));
			check_result(ok, "madvise_dontneed_refault");
		} else {
			skip_result("madvise_dontneed_nonresident_transition");
		}
	}
	(void)munmap(mapping, page_size);

	write_mapping = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
				     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (write_mapping == MAP_FAILED) {
		check_result(0, "allocate_nonresident_write_mapping");
		return -1;
	}
	if (madvise(write_mapping, page_size, MADV_DONTNEED) != 0 ||
	    mincore_page(write_mapping, &before) != 0) {
		check_result(0, "mincore_nonresident_write_mapping");
		(void)munmap(write_mapping, page_size);
		return -1;
	}
	if (before) {
		skip_result("nonresident_write_no_fault_transition");
	} else {
		for (i = 0; i < sizeof(write_data); ++i)
			write_data[i] = (unsigned char)(0x70U + i);
		write_req = write_request(getpid(), write_mapping, write_data,
					 sizeof(write_data));
		result = nbdriver_write_process(fd, &write_req);
		saved_errno = errno;
		print_fault_write("nonresident_write", result, saved_errno, &write_req);
		ok = result == -1 && saved_errno == ENODATA &&
			write_req.error == -ENODATA && write_req.copied == 0 &&
			write_req.failed_address ==
				(nbdriver_u64)(uintptr_t)write_mapping &&
			mincore_page(write_mapping, &after) == 0 && !after;
		check_result(ok, "nonresident_write_not_faulted");
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

	section("REMOTE READ FAULT-IN");
	if (pipe(parent_to_child) != 0 || pipe(child_to_parent) != 0) {
		close_fd(&parent_to_child[0]);
		close_fd(&parent_to_child[1]);
		close_fd(&child_to_parent[0]);
		close_fd(&child_to_parent[1]);
		check_result(0, "create_remote_fault_pipes");
		return -1;
	}
	child = fork();
	if (child < 0) {
		close_fd(&parent_to_child[0]);
		close_fd(&parent_to_child[1]);
		close_fd(&child_to_parent[0]);
		close_fd(&child_to_parent[1]);
		check_result(0, "fork_remote_fault_child");
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
		check_result(0, "remote_fault_initial_report");
		return -1;
	}
	if (report.error != 0 || report.address == 0) {
		command = 0;
		(void)write_full(parent_to_child[1], &command, sizeof(command));
		if (wait_child(child, &status) != 0)
			kill_and_reap(child);
		close_fd(&parent_to_child[1]);
		close_fd(&child_to_parent[0]);
		check_result(0, "remote_fault_initial_mincore");
		return -1;
	}
	if (report.resident) {
		command = 0;
		(void)write_full(parent_to_child[1], &command, sizeof(command));
		if (wait_child(child, &status) != 0)
			kill_and_reap(child);
		close_fd(&parent_to_child[1]);
		close_fd(&child_to_parent[0]);
		skip_result("remote_fault_nonresident_transition");
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
	check_result(ok, "remote_fault_in_zero_read");

	command = 1U;
	if (write_full(parent_to_child[1], &command, sizeof(command)) != 0 ||
	    read_full(child_to_parent[0], &after_report, sizeof(after_report)) != 0) {
		kill_and_reap(child);
		child = -1;
		reaped = 1;
		close_fd(&parent_to_child[1]);
		close_fd(&child_to_parent[0]);
		check_result(0, "remote_fault_after_report");
		return -1;
	}
	check_result(after_report.error == 0 && after_report.resident != 0,
		     "remote_fault_in_resident_transition");
	if (wait_child(child, &status) == 0)
		reaped = 1;
	else
		kill_and_reap(child);
	check_result(reaped && WIFEXITED(status) && WEXITSTATUS(status) == 0,
		     "remote_fault_child_cleanup");
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

	section("REMOTE WRITE / READBACK");
	memset(expected, 0x6d, sizeof(expected));
	if (pipe(parent_to_child) != 0 || pipe(child_to_parent) != 0) {
		close_fd(&parent_to_child[0]);
		close_fd(&parent_to_child[1]);
		close_fd(&child_to_parent[0]);
		close_fd(&child_to_parent[1]);
		check_result(0, "create_remote_write_pipes");
		return -1;
	}
	child = fork();
	if (child < 0) {
		close_fd(&parent_to_child[0]);
		close_fd(&parent_to_child[1]);
		close_fd(&child_to_parent[0]);
		close_fd(&child_to_parent[1]);
		check_result(0, "fork_remote_write_child");
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
		check_result(0, "remote_write_initial_report");
		return -1;
	}
	write_req = write_request(child, (void *)address, expected,
					 sizeof(expected));
	result = nbdriver_write_process(fd, &write_req);
	saved_errno = errno;
	if (result != 0 || write_req.error != 0 ||
	    write_req.copied != sizeof(expected))
		print_fault_write("remote_write", result, saved_errno, &write_req);
	check_result(result == 0 && write_req.error == 0 &&
		     write_req.copied == sizeof(expected), "remote_write");

	memset(output, 0, sizeof(output));
	read_req = read_request(child, (void *)address, output, sizeof(output));
	result = nbdriver_read_process(fd, &read_req);
	saved_errno = errno;
	if (result != 0 || read_req.error != 0 || read_req.copied != sizeof(output))
		print_fault_read("remote_write_readback", result, saved_errno, &read_req);
	check_result(result == 0 && read_req.error == 0 &&
		     read_req.copied == sizeof(output) &&
		     memcmp(output, expected, sizeof(output)) == 0,
		     "remote_write_readback");

	command = 1U;
	if (write_full(parent_to_child[1], &command, sizeof(command)) != 0 ||
	    read_full(child_to_parent[0], &child_ok, sizeof(child_ok)) != 0) {
		kill_and_reap(child);
		child = -1;
		reaped = 1;
		close_fd(&parent_to_child[1]);
		close_fd(&child_to_parent[0]);
		check_result(0, "remote_write_child_report");
		return -1;
	}
	check_result(child_ok != 0, "remote_write_child_direct_verify");
	if (wait_child(child, &status) == 0)
		reaped = 1;
	else
		kill_and_reap(child);
	check_result(reaped && WIFEXITED(status) && WEXITSTATUS(status) == 0,
		     "remote_write_child_cleanup");
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

	section("EXITED PID");
	child = fork();
	if (child < 0) {
		check_result(0, "fork_exited_child");
		return -1;
	}
	if (child == 0)
		_exit(0);
	if (wait_child(child, &status) != 0) {
		kill_and_reap(child);
		check_result(0, "reap_exited_child");
		return -1;
	}
	request = read_request(child, (void *)(uintptr_t)page_size, output,
				       sizeof(output));
	result = nbdriver_read_process(fd, &request);
	saved_errno = errno;
	print_fault_read("exited_pid_read", result, saved_errno, &request);
	check_result(result == -1 && saved_errno == ESRCH &&
		     request.error == -ESRCH && request.copied == 0 &&
		     request.failed_address == (nbdriver_u64)page_size,
		     "exited_pid_rejected");
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

	section("COW WRITE REJECTION");
	mapping = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED) {
		check_result(0, "allocate_cow_mapping");
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
		check_result(0, "create_cow_pipes");
		return -1;
	}
	child = fork();
	if (child < 0) {
		close_fd(&parent_to_child[0]);
		close_fd(&parent_to_child[1]);
		close_fd(&child_to_parent[0]);
		close_fd(&child_to_parent[1]);
		(void)munmap(mapping, page_size);
		check_result(0, "fork_cow_child");
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
		check_result(0, "cow_initial_report");
		return -1;
	}
	write_req = write_request(child, (void *)address, replacement,
					 sizeof(replacement));
	result = nbdriver_write_process(fd, &write_req);
	saved_errno = errno;
	print_fault_write("cow_write", result, saved_errno, &write_req);
	check_result(result == -1 && saved_errno == EACCES &&
		     write_req.error == -EACCES && write_req.copied == 0 &&
		     write_req.failed_address == (nbdriver_u64)address,
		     "cow_write_rejected");

	memset(output, 0, sizeof(output));
	read_req = read_request(getpid(), mapping, output, sizeof(output));
	result = nbdriver_read_process(fd, &read_req);
	check_result(result == 0 && read_req.error == 0 &&
		     read_req.copied == sizeof(output) &&
		     !memcmp(output, "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
			     sizeof(output)),
		     "cow_parent_unchanged");

	command = 1U;
	if (write_full(parent_to_child[1], &command, sizeof(command)) != 0 ||
	    read_full(child_to_parent[0], &child_ok, sizeof(child_ok)) != 0) {
		kill_and_reap(child);
		child = -1;
		reaped = 1;
		close_fd(&parent_to_child[1]);
		close_fd(&child_to_parent[0]);
		(void)munmap(mapping, page_size);
		check_result(0, "cow_child_report");
		return -1;
	}
	check_result(child_ok != 0, "cow_child_unchanged");
	if (wait_child(child, &status) == 0)
		reaped = 1;
	else
		kill_and_reap(child);
	check_result(reaped && WIFEXITED(status) && WEXITSTATUS(status) == 0,
		     "cow_child_cleanup");
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

	section("CONCURRENT MADV_DONTNEED / READ");
	mapping = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED) {
		check_result(0, "allocate_concurrent_mapping");
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
		check_result(0, "create_madvise_thread");
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
		printf("  madvise_thread: errno=%d(%s) iterations=%u\n",
		       atomic_load(&state.thread_errno),
		       strerror(atomic_load(&state.thread_errno)),
		       atomic_load(&state.iterations));
	}
	printf("  concurrent_reads=%u transient_errors=%u madvise_iterations=%u\n",
	       completed, transient_errors, atomic_load(&state.iterations));
	check_result(completed == 1000U && !unexpected &&
		     !atomic_load(&state.thread_error),
		     "concurrent_madvise_1000_cleanup");
	(void)munmap(mapping, page_size);
	return 0;
}

static int test_inherited_fd_downgrade(int fd)
{
	pid_t child;
	int status;

	section("INHERITED FD DOWNGRADE");
	child = fork();
	if (child < 0) {
		check_result(0, "fork_downgrade_child");
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
		check_result(0, "reap_downgrade_child");
		return -1;
	}
	if (WIFEXITED(status) && WEXITSTATUS(status) == 2) {
		skip_result("inherited_fd_downgrade_capability_drop");
		return 0;
	}
	check_result(WIFEXITED(status) && WEXITSTATUS(status) == 0,
		     "inherited_fd_downgrade_rejected");
	return 0;
}

static int test_executable_mapping(int fd)
{
	section("EXECUTABLE ANONYMOUS PAGE");
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
			skip_result("anonymous_rwx_mapping_policy");
			return 0;
		}
		code[0] = 0x52800000U | (old_value << 5);
		code[1] = 0xd65f03c0U;
		(void)code[0];
		__builtin___clear_cache((char *)code,
					(char *)code + sizeof(instructions));
		function = (int (*)(void))(uintptr_t)code;
		check_result(function() == (int)old_value, "rwx_prefill_execute");
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
			     "rwx_driver_write_and_execute");
		(void)munmap(code, page_size);
	}
#else
	(void)fd;
	skip_result("anonymous_rwx_arm64_instruction_test");
#endif
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
		fprintf(stderr, "INIT FAIL: sigaction errno=%d(%s)\n", errno,
			strerror(errno));
		return 2;
	}
	if (geteuid() != 0) {
		fprintf(stderr, "INIT FAIL: root is required, euid=%u\n",
			(unsigned int)geteuid());
		return 2;
	}
	result = (int)sysconf(_SC_PAGESIZE);
	if (result <= 0) {
		fprintf(stderr, "INIT FAIL: sysconf(_SC_PAGESIZE) errno=%d(%s)\n",
			errno, strerror(errno));
		return 2;
	}
	page_size = (size_t)result;
	alarm(90);

	fd = nbdriver_open();
	if (fd < 0) {
		fprintf(stderr, "INIT FAIL: nbdriver_open errno=%d(%s)\n", errno,
			strerror(errno));
		alarm(0);
		return 2;
	}
	if (nbdriver_get_abi(fd, &info) != 0) {
		fprintf(stderr, "INIT FAIL: nbdriver_get_abi errno=%d(%s)\n", errno,
			strerror(errno));
		close_fd(&fd);
		alarm(0);
		return 2;
	}
	printf("nbdriver_smoke: pid=%d page_size=%zu abi=%u size=%u features=0x%x\n",
	       getpid(), page_size, info.version, info.size, info.features);
	if (info.version != NBDRIVER_ABI_VERSION || info.size != sizeof(info) ||
	    (info.features & (NBDRIVER_FEATURE_READ_PROCESS |
			      NBDRIVER_FEATURE_WRITE_PROCESS |
			      NBDRIVER_FEATURE_READ_FAULT_IN)) !=
			      (NBDRIVER_FEATURE_READ_PROCESS |
			       NBDRIVER_FEATURE_WRITE_PROCESS |
			       NBDRIVER_FEATURE_READ_FAULT_IN)) {
		fprintf(stderr, "INIT FAIL: required ABI/features unavailable\n");
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
	(void)test_inherited_fd_downgrade(fd);

	alarm(0);
	if (alarm_fired) {
		printf("  global_alarm: FAIL (a test exceeded 90 seconds)\n");
		failures++;
	}
	close_fd(&fd);
	printf("\n========================================\n");
	printf("SUMMARY: PASS=%d FAIL=%d SKIP=%d\n", passes, failures, skips);
	printf("nbdriver_smoke: %s\n", failures ? "FAIL" : "PASS");
	return failures ? 1 : 0;
}
