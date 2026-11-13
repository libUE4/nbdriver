// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#include "nbdriver_query_rules.h"
int main(void)
{
	assert(nbdriver_pid_better(8, 10, 1, 7, 10000, 100));
	assert(!nbdriver_pid_better(7, 10000, 100, 8, 10, 1));
	assert(nbdriver_pid_better(8, 11, 1, 8, 10, 100));
	assert(nbdriver_pid_better(8, 10, 101, 8, 10, 100));
	assert(!nbdriver_pid_better(8, 10, 99, 8, 10, 100));
	assert(nbdriver_thread_better(true, 1, 1, false, 1000, 100));
	assert(!nbdriver_thread_better(false, 1000, 100, true, 1, 1));
	assert(nbdriver_thread_better(false, 11, 1, false, 10, 100));
	assert(nbdriver_thread_better(false, 10, 101, false, 10, 100));
	assert(nbdriver_module_name_matches("/data/app/pkg/lib/arm64/libUE4.so", "libUE4.so"));
	assert(nbdriver_module_name_matches("/system/lib64/libc.so.6", "libc.so"));
	assert(nbdriver_module_name_matches("libx.so.1.2", "libx.so"));
	assert(!nbdriver_module_name_matches("libUE4.so.evil", "libUE4.so"));
	assert(!nbdriver_module_name_matches("libUE4.so.", "libUE4.so"));
	assert(!nbdriver_module_name_matches("libUE4.so2", "libUE4.so"));
	assert(!nbdriver_module_name_matches("libUE4", "libUE4.so"));
	assert(!nbdriver_module_name_matches("libUE4.so.backup.6", "libUE4.so"));
	puts("PASS: PID/thread candidate ranking and precise SO matching");
}
