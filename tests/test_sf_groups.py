#!/usr/bin/env python3
"""Run the production group-reference release against both kernel field types.

Adapters check reference ownership and dispatch, not kernel atomic internals.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/nbdriver_sf_groups.c').read_text()
start = source.index('static noinline void NBDRIVER_NO_CFI\nnbdriver_put_group_info(')
end = source.index('static noinline int NBDRIVER_NO_CFI\nnbdriver_append_groups(', start)
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <limits.h>
#include <stdio.h>
#define noinline
#define NBDRIVER_NO_CFI
#define BUILD_BUG_ON(c) _Static_assert(!(c), "unsupported group reference type")
typedef struct { int value; } atomic_t;
typedef struct { int refs; } refcount_t;
#if REFCOUNT_MODE == 1
#define USAGE_TYPE refcount_t
#elif REFCOUNT_MODE == 0
#define USAGE_TYPE atomic_t
#else
#define USAGE_TYPE long
#endif
struct group_info { USAGE_TYPE usage; };
static unsigned atomic_calls, refcount_calls, frees;
static bool atomic_dec_and_test(atomic_t *value)
{ assert(REFCOUNT_MODE == 0);++atomic_calls;assert(value->value > 0);return --value->value == 0; }
static bool refcount_dec_and_test(refcount_t *value)
{ assert(REFCOUNT_MODE == 1);++refcount_calls;if(value->refs == INT_MAX)return false;assert(value->refs > 0);return --value->refs == 0; }
static void groups_free(struct group_info *group) { assert(group);++frees; }
static void (*nbdriver_groups_free_ptr)(struct group_info *) = groups_free;
'''
harness += source[start:end]
harness += r'''
int main(void) {
 struct group_info success = { {2} }, failure = { {1} };
 nbdriver_put_group_info(&success);assert(frees == 0);
 nbdriver_put_group_info(&success);assert(frees == 1);
 nbdriver_put_group_info(&failure);assert(frees == 2);
#if REFCOUNT_MODE == 1
 struct group_info saturated = { {INT_MAX} };
 nbdriver_put_group_info(&saturated);assert(frees == 2 && saturated.usage.refs == INT_MAX);
 assert(refcount_calls == 4 && atomic_calls == 0);
#else
 assert(atomic_calls == 3 && refcount_calls == 0);
#endif
 puts("PASS group refs: allocation reference, retained credential reference, final release, correct counter family");
}
'''
with tempfile.TemporaryDirectory(prefix='nbdriver-group-refs-') as directory:
    temp = Path(directory)
    path = temp / 'check.c'
    path.write_text(harness)
    for mode in (0, 1):
        binary = temp / ('check-' + str(mode))
        command = [os.environ.get('CC', 'cc'), '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                   '-DREFCOUNT_MODE=' + str(mode), str(path), '-o', str(binary)]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)
