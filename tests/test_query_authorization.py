#!/usr/bin/env python3
"""Exercise the production query dispatcher and caller policy with host credentials."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
core = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'src/nbdriver_core.c'


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


source = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include "nbdriver_abi.h"
static unsigned int effective_uid;
static bool ptrace_cap;
static int handler_calls;
static unsigned long received;
#define GLOBAL_ROOT_UID 0U
#define KUIDT_INIT(uid) (uid)
#define current_euid() effective_uid
#define uid_eq(a,b) ((a)==(b))
#define CAP_SYS_PTRACE 19
static bool capable(int cap) { assert(cap == CAP_SYS_PTRACE); return ptrace_cap; }
#define HANDLER(name, result) static long name(unsigned long arg) \
    { ++handler_calls; received = arg; return result; }
HANDLER(query_find_process, 101)
HANDLER(query_module, 102)
HANDLER(query_identity, 103)
HANDLER(query_thread, 104)
HANDLER(query_environment, 105)
HANDLER(query_signal, 106)
HANDLER(query_fpstate, 107)
'''
source += function((ROOT / 'src/nbdriver_read.h').read_text(),
                   'static inline bool nbdriver_sf_read_caller(') + '\n'
source += function(core.read_text(), 'long nbdriver_query_ioctl(') + '\n'
source += r'''
int main(void)
{
    const unsigned int commands[] = {
        NBDRIVER_IOC_FIND_PROCESS, NBDRIVER_IOC_MODULE_BASE,
        NBDRIVER_IOC_PROCESS_ID, NBDRIVER_IOC_THREAD_ID,
        NBDRIVER_IOC_THREAD_ENV, NBDRIVER_IOC_THREAD_SIGNAL,
        NBDRIVER_IOC_THREAD_FP
    };
    const unsigned int uids[] = {0, 1000, 1003, 10000};
    for (unsigned int c = 0; c < 2; ++c) {
        ptrace_cap = c;
        for (unsigned int u = 0; u < sizeof(uids)/sizeof(uids[0]); ++u) {
            effective_uid = uids[u];
            bool allowed = ptrace_cap || effective_uid == 0 || effective_uid == 1000;
            for (unsigned int i = 0; i < sizeof(commands)/sizeof(commands[0]); ++i) {
                handler_calls = 0;
                received = 0;
                long result = nbdriver_query_ioctl(commands[i], 0x12345678UL);
                assert(result == (allowed ? (long)(101+i) : -EPERM));
                assert(handler_calls == (allowed ? 1 : 0));
                assert(received == (allowed ? 0x12345678UL : 0));
            }
            handler_calls = 0;
            assert(nbdriver_query_ioctl(0, 0) == (allowed ? -ENOTTY : -EPERM));
            assert(!handler_calls);
        }
    }
    puts("PASS query authorization: root/system/capability callers, denied callers, all seven handlers and unknown command");
}
'''
with tempfile.TemporaryDirectory(prefix='nbdriver-query-auth-') as directory:
    path = Path(directory) / 'query_auth.c'
    binary = Path(directory) / 'query_auth'
    path.write_text(source)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O2', '-Wall',
                    '-Wextra', '-Werror', '-I' + str(ROOT / 'include'),
                    str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
