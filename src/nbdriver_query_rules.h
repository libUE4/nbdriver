/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_QUERY_RULES_H
#define NBDRIVER_QUERY_RULES_H
#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <stdbool.h>
#include <string.h>
#endif
#include "nbdriver_abi.h"

/* Selection rules ported from NBG-driver process.c/query.c. */
static inline bool nbdriver_pid_better(int threads, long rss, int pid,
				       int best_threads, long best_rss, int best_pid)
{
	return threads > best_threads ||
		(threads == best_threads && rss > best_rss) ||
		(threads == best_threads && rss == best_rss && pid > best_pid);
}

static inline bool nbdriver_thread_better(bool on_cpu, nbdriver_u64 exec_ns, int tid,
					 bool best_cpu, nbdriver_u64 best_exec, int best_tid)
{
	return !best_tid || (on_cpu && !best_cpu) ||
		(on_cpu == best_cpu && exec_ns > best_exec) ||
		(on_cpu == best_cpu && exec_ns == best_exec && tid > best_tid);
}

static inline bool nbdriver_module_name_matches(const char *path, const char *name)
{
	const char *base = strrchr(path, '/');
	size_t length = strlen(name);

	base = base ? base + 1 : path;
	if (!strcmp(base, name))
		return true;
	if (strncmp(base, name, length) || base[length] != '.' || !base[length + 1])
		return false;
	for (base += length + 1; *base; ++base)
		if ((*base < '0' || *base > '9') && *base != '.')
			return false;
	return true;
}
#endif
