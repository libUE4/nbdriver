#!/usr/bin/env python3
"""Exercise the production lock-owner adapter across the kernel layout changes.

Minimal type fixtures make missing headers and wrong member names compile errors.
Runtime checks pass the iterator's interior list node, as fs/locks.c does.
The CI KMI matrix separately builds against real target headers.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
COMMON = r'''
#ifndef TEST_LOCK_TYPES_H
#define TEST_LOCK_TYPES_H
#include <stddef.h>
typedef int pid_t;
struct hlist_node { void *next; void **pprev; };
#define hlist_entry(ptr, type, member) \
    ((type *)((const char *)(ptr) - offsetof(type, member)))
#endif
'''
LEGACY = r'''
struct file_lock {
    void *blocker;
    unsigned long list[2];
    struct hlist_node fl_link;
    unsigned long blocked[4];
    unsigned int fl_pid;
    unsigned long tail;
};
'''
MODERN = r'''
struct file_lock_core {
    void *blocker;
    unsigned long list[2];
    struct hlist_node flc_link;
    unsigned long blocked[4];
    pid_t flc_pid;
    unsigned long tail;
};
/* Neither wrapper is the iterator container. Make assumptions fail loudly. */
struct file_lock { unsigned long prefix[3]; struct file_lock_core c; };
struct file_lease { unsigned long prefix[5]; struct file_lock_core c; };
'''
MAIN = r'''
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "nbdriver_filelock.h"
int main(void) {
    assert(nbdriver_filelock_owner(NULL) == 0);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
    struct file_lock lock;
    struct file_lease lease;
    memset(&lock, 0xA5, sizeof(lock));
    memset(&lease, 0x5A, sizeof(lease));
    lock.c.flc_pid = 12345;
    lease.c.flc_pid = 67890;
    assert(nbdriver_filelock_owner(&lock.c.flc_link) == 12345);
    assert(nbdriver_filelock_owner(&lease.c.flc_link) == 67890);
    lock.c.flc_pid = -1;
    assert(nbdriver_filelock_owner(&lock.c.flc_link) == -1);
    lock.c.flc_pid = INT_MAX;
    assert(nbdriver_filelock_owner(&lock.c.flc_link) == INT_MAX);
#else
    struct file_lock lock;
    memset(&lock, 0xA5, sizeof(lock));
    lock.fl_pid = 12345;
    assert(nbdriver_filelock_owner(&lock.fl_link) == 12345);
    lock.fl_pid = (unsigned int)-1;
    assert(nbdriver_filelock_owner(&lock.fl_link) == -1);
    lock.fl_pid = INT_MAX;
    assert(nbdriver_filelock_owner(&lock.fl_link) == INT_MAX);
#endif
    puts("PASS: iterator node -> target lock container -> owner pid");
}
'''

with tempfile.TemporaryDirectory(prefix='nbdriver-filelock-test-') as directory:
    folder = Path(directory)
    for version in [(5, 10), (5, 15), (6, 1), (6, 2), (6, 3), (6, 6), (6, 8),
                    (6, 9), (6, 12), (6, 18)]:
        base = folder / ('%d.%d' % version)
        headers = base / 'linux'
        headers.mkdir(parents=True)
        code = version[0] * 65536 + version[1] * 256
        (headers / 'version.h').write_text(
            '#define KERNEL_VERSION(a,b,c) (((a)<<16)+((b)<<8)+(c))\n'
            f'#define LINUX_VERSION_CODE {code}\n')
        types = MODERN if version >= (6, 9) else LEGACY
        (headers / 'fs.h').write_text(COMMON + (types if version < (6, 3) else ''))
        if version >= (6, 3):
            (headers / 'filelock.h').write_text(COMMON + types)
        source = base / 'test.c'
        source.write_text(MAIN)
        binary = base / 'test'
        command = [os.environ.get('CC', 'cc'), '-std=gnu11', '-O2', '-Wall',
                   '-Wextra', '-Werror', '-I' + str(base), '-I' + str(ROOT / 'src'),
                   str(source), '-o', str(binary)]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)
        print('kernel header layout %d.%d: PASS' % version, flush=True)
