// SPDX-License-Identifier: GPL-2.0-only
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
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

static int failures;

static void check(int passed, const char *name)
{
	printf("%s: %s\n", name, passed ? "PASS" : "FAIL");
	if (!passed)
		failures++;
}

static struct nbdriver_read_request request_for(const void *address,
						void *buffer, size_t size)
{
	struct nbdriver_read_request request = {
		.pid = getpid(),
		.size = (nbdriver_u32)size,
		.address = (uintptr_t)address,
		.buffer = (uintptr_t)buffer,
	};

	return request;
}

struct race_region {
	void *address;
	size_t size;
	atomic_int stop;
};

static void *remap_thread(void *argument)
{
	struct race_region *region = argument;

	while (!atomic_load(&region->stop)) {
		void *mapped;

		munmap(region->address, region->size);
		mapped = mmap(region->address, region->size, PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
		if (mapped == MAP_FAILED)
			break;
		memset(mapped, 0x5a, region->size);
	}
	return NULL;
}

int main(void)
{
	struct nbdriver_abi_info info;
	struct nbdriver_read_request request;
	struct race_region region;
	pthread_t thread;
	size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
	uint8_t *source, *destination;
	int fd, result;
	size_t i;

	setvbuf(stdout, NULL, _IONBF, 0);
	fd = nbdriver_open();
	if (fd < 0) {
		perror("nbdriver_open");
		return 1;
	}
	if (nbdriver_get_abi(fd, &info) ||
	    info.version != NBDRIVER_ABI_VERSION ||
	    !(info.features & NBDRIVER_FEATURE_READ_PROCESS)) {
		fputs("read interface unavailable\n", stderr);
		close(fd);
		return 1;
	}
	source = mmap(NULL, page_size * 3, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	destination = malloc(page_size * 3);
	if (source == MAP_FAILED || !destination) {
		perror("allocate");
		close(fd);
		return 1;
	}
	for (i = 0; i < page_size * 3; ++i)
		source[i] = (uint8_t)(i * 17U + 3U);

	request = request_for(source + page_size - 13, destination, 40);
	result = nbdriver_read_process(fd, &request);
	check(!result && request.copied == 40 && !request.error &&
	      !memcmp(source + page_size - 13, destination, 40), "cross_page");
	check(request.expected_start_ns != 0, "process_identity");

	request.expected_start_ns++;
	result = nbdriver_read_process(fd, &request);
	check(result == -1 && errno == ESTALE && request.error == -ESTALE &&
	      request.copied == 0, "stale_identity");

	{
		int ready[2], finish[2], status;
		uintptr_t remote_address;
		pid_t child;
		char token = 0;

		if (pipe(ready) || pipe(finish))
			return 1;
		child = fork();
		if (!child) {
			void *remote = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
					    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

			close(ready[0]);
			close(finish[1]);
			if (remote == MAP_FAILED)
				_exit(2);
			memset(remote, 0xa7, page_size);
			remote_address = (uintptr_t)remote;
			if (write(ready[1], &remote_address, sizeof(remote_address)) !=
			    (ssize_t)sizeof(remote_address))
				_exit(2);
			if (read(finish[0], &token, 1) != 1)
				_exit(2);
			_exit(0);
		}
		close(ready[1]);
		close(finish[0]);
		if (child < 0 || read(ready[0], &remote_address,
				      sizeof(remote_address)) != (ssize_t)sizeof(remote_address))
			return 1;
		close(ready[0]);
		request = request_for((void *)remote_address, destination, 32);
		request.pid = child;
		result = nbdriver_read_process(fd, &request);
		for (i = 0; i < 32 && destination[i] == 0xa7; ++i)
			;
		check(!result && request.copied == 32 && i == 32, "child_process");
		if (write(finish[1], &token, 1) != 1)
			return 1;
		close(finish[1]);
		if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
		    WEXITSTATUS(status))
			return 1;
		result = nbdriver_read_process(fd, &request);
		check(result == -1 && (errno == ESRCH || errno == ESTALE) &&
		      request.copied == 0, "exited_process");
	}

	request = request_for(source, destination, NBDRIVER_READ_MAX + 1U);
	check(nbdriver_read_process(fd, &request) == -1 && errno == EINVAL,
	      "length_limit");

	/* Disposable self-owned pages reproduce DFM's -w-p descriptor VMA. */
	{
		uint8_t expected[16];
		struct nbdriver_mapping_request mapping;
		memcpy(expected, source, sizeof(expected));
		if (mprotect(source, page_size, PROT_WRITE))
			return 1;
		request = request_for(source, destination, sizeof(expected));
		result = nbdriver_read_process(fd, &request);
		check(!result && !request.error && request.copied == sizeof(expected) &&
		      !memcmp(destination, expected, sizeof(expected)), "write_only_vma_read");
		result = nbdriver_query_mapping(fd, getpid(), (uintptr_t)source,
					      request.expected_start_ns, &mapping);
		check(!result && (mapping.flags & (NBDRIVER_MAPPING_READ | NBDRIVER_MAPPING_WRITE)) ==
		      (NBDRIVER_MAPPING_READ | NBDRIVER_MAPPING_WRITE), "write_only_effective_mapping");
		if (mprotect(source, page_size, PROT_READ | PROT_WRITE))
			return 1;
		check(!memcmp(source, expected, sizeof(expected)), "write_only_source_unchanged");
	}

	if (mprotect(source + page_size, page_size, PROT_NONE))
		return 1;
	memset(destination, 0xcc, 40);
	request = request_for(source + page_size - 13, destination, 40);
	result = nbdriver_read_process(fd, &request);
	check(result == -1 && errno == EACCES && request.error == -EACCES &&
	      request.copied == 13 && request.failed_address ==
	      (uintptr_t)(source + page_size) && destination[13] == 0xcc &&
	      !memcmp(source + page_size - 13, destination, 13), "partial_protected");

	if (munmap(source + page_size, page_size))
		return 1;
	request = request_for(source + page_size - 13, destination, 40);
	result = nbdriver_read_process(fd, &request);
	check(result == -1 && errno == EFAULT && request.copied == 13 &&
	      request.failed_address == (uintptr_t)(source + page_size), "partial_hole");

	{
		void *untouched = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
				       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		unsigned char before = 0, after = 0;
		uint8_t zeros[16] = { 0 };

		if (untouched == MAP_FAILED || mincore(untouched, page_size, &before))
			return 1;
		memset(destination, 0xcc, sizeof(zeros));
		request = request_for(untouched, destination, sizeof(zeros));
		result = nbdriver_read_process(fd, &request);
		if (mincore(untouched, page_size, &after))
			return 1;
		check(!result && !request.error && request.copied == sizeof(zeros) &&
		      !(before & 1) && (after & 1) &&
		      !memcmp(destination, zeros, sizeof(zeros)), "automatic_fault_in");
		munmap(untouched, page_size);
	}

	region.address = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	region.size = page_size;
	atomic_init(&region.stop, 0);
	if (region.address == MAP_FAILED || pthread_create(&thread, NULL,
							 remap_thread, &region))
		return 1;
	for (i = 0; i < 1000; ++i) {
		request = request_for(region.address, destination, 32);
		result = nbdriver_read_process(fd, &request);
		if (!result && request.copied != 32)
			break;
		if (result && errno != EFAULT && errno != ENODATA && errno != EAGAIN)
			break;
	}
	atomic_store(&region.stop, 1);
	pthread_join(thread, NULL);
	check(i == 1000, "concurrent_unmap_1000");
	munmap(region.address, page_size);

	{
		pid_t child = fork();
		int status;

		if (!child) {
			int ok;

			if (setgid(65534) || setuid(65534))
				_exit(2);
			request = request_for(destination, destination + 32, 16);
			ok = nbdriver_read_process(fd, &request) == -1 && errno == EPERM;
			_exit(ok ? 0 : 1);
		}
		if (child < 0 || waitpid(child, &status, 0) != child)
			return 1;
		check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "inherited_fd_denied");
	}

	munmap(source, page_size);
	munmap(source + page_size * 2, page_size);
	free(destination);
	close(fd);
	printf("device_read_smoke: %s failures=%d\n", failures ? "FAIL" : "PASS",
	       failures);
	return failures ? 1 : 0;
}
