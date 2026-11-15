#!/usr/bin/env python3
"""Check the isolated single-header SDK against the legacy SDK with mocked syscalls."""

import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PROBE = r'''
#include "SDK_HEADER"
#include "SDK_HEADER"
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>

static int installed_fd = 17;
static unsigned int selected_nr = 18, bootstrap_calls, last_nr;
static int bootstrap_errno = EINVAL;
static int abi_error, abi_mismatch;
static int ioctl_count;
static unsigned expected_start = 42;
static int close_count;
int sdk_second_tu(void);

#ifdef __cplusplus
extern "C" {
#endif
long __wrap_syscall(long number, ...)
{
    va_list args;
    assert((unsigned long)number == (bootstrap_calls < NBDRIVER_INSTALL_CANDIDATE_COUNT
        ? nbdriver_install_candidate(bootstrap_calls) : NBDRIVER_INSTALL_REBOOT_SYSCALL));
    ++bootstrap_calls;last_nr=(unsigned int)number;
    va_start(args, number);
    assert(va_arg(args, unsigned int) == NBDRIVER_INSTALL_MAGIC1);
    assert(va_arg(args, unsigned int) == NBDRIVER_INSTALL_MAGIC2);
    assert(va_arg(args, int) == 0);
    int *out = va_arg(args, int *);
    assert(*out == NBDRIVER_INSTALL_PENDING);
    if ((unsigned int)number == selected_nr) *out = installed_fd;
    assert(va_arg(args, unsigned long) == 0UL);
    assert(va_arg(args, unsigned long) == 0UL);
    va_end(args);
    errno = bootstrap_errno; /* The output, rather than reboot return, is authoritative. */
    return -1;
}

int __wrap_close(int fd)
{
    assert(fd == 17);
    ++close_count;
    return 0;
}

int __wrap_ioctl(int fd, unsigned long command, ...)
{
    va_list args;
    assert(fd == installed_fd);
    ++ioctl_count;
    va_start(args, command);
    if (command == NBDRIVER_IOC_GET_ABI) {
        struct nbdriver_abi_info *info = va_arg(args, struct nbdriver_abi_info *);
        if (abi_error) { va_end(args); errno=abi_error; return -1; }
        info->version = NBDRIVER_ABI_VERSION + abi_mismatch;
        info->size = sizeof(*info);
        info->features = NBDRIVER_FEATURE_READ_PROCESS | NBDRIVER_FEATURE_WRITE_PROCESS;
        va_end(args);
        return 0;
    }
    if (command == NBDRIVER_IOC_READ_PROCESS) {
        struct nbdriver_read_request *r = va_arg(args, struct nbdriver_read_request *);
        assert(r->copied == 0 && r->error == 0 && r->failed_address == 0);
        assert(r->pid == 123 && r->address == 4096 && r->size == 32);
        assert(r->buffer == 8192 && r->expected_start_ns == expected_start);
        r->copied = 16;
        r->error = -EFAULT;
        r->failed_address = r->address + 16;
        va_end(args);
        errno = EFAULT;
        return -1;
    }
    assert(command == NBDRIVER_IOC_WRITE_PROCESS);
    struct nbdriver_write_request *w = va_arg(args, struct nbdriver_write_request *);
    assert(w->copied == 0 && w->error == 0 && w->failed_address == 0);
    assert(w->pid == 123 && w->address == 4096 && w->size == 32);
    assert(w->buffer == 8192 && w->expected_start_ns == expected_start);
    w->copied = w->size;
    va_end(args);
    return 0;
}
#ifdef __cplusplus
}
#endif

int main(void)
{
    struct nbdriver_abi_info info;
    struct nbdriver_read_request r;
    struct nbdriver_write_request w;
    memset(&info, 0, sizeof(info));
    memset(&r, 0, sizeof(r));
    memset(&w, 0, sizeof(w));
    assert(sizeof(info) == 16 && sizeof(r) == 48 && sizeof(w) == 48);
    assert(_IOC_SIZE(NBDRIVER_IOC_READ_PROCESS) == sizeof(r));
    assert(_IOC_SIZE(NBDRIVER_IOC_WRITE_PROCESS) == sizeof(w));
    bootstrap_calls=0;assert(nbdriver_open() == 17 && errno == 0);
    /* Validate every candidate, both range boundaries, reboot last and FD 0. */
    for (unsigned int i=0;i<=NBDRIVER_INSTALL_CANDIDATE_COUNT;++i) {
        selected_nr=i<NBDRIVER_INSTALL_CANDIDATE_COUNT?nbdriver_install_candidate(i):NBDRIVER_INSTALL_REBOOT_SYSCALL;
        bootstrap_calls=0;
        assert(nbdriver_open()==17&&bootstrap_calls==i+1&&last_nr==selected_nr);
    }
    installed_fd=0;selected_nr=423;bootstrap_calls=0;
    assert(nbdriver_open()==0&&last_nr==423);
    installed_fd=17;selected_nr=18;
    installed_fd = NBDRIVER_INSTALL_PENDING;
    bootstrap_calls=0;assert(nbdriver_open() == -1 && errno == ENODEV);
    bootstrap_errno = EPERM;
    bootstrap_calls=0;assert(nbdriver_open() == -1 && errno == EPERM);
    bootstrap_errno = EINVAL;
    installed_fd = -EMFILE;
    bootstrap_calls=0;assert(nbdriver_open() == -1 && errno == EMFILE);
    installed_fd = -EPERM;
    bootstrap_calls=0;assert(nbdriver_open() == -1 && errno == EPERM);
    installed_fd = 17;
    abi_error = EIO;
    bootstrap_calls=0;assert(nbdriver_open() == -1 && errno == EIO && close_count == 1);
    abi_error = 0; abi_mismatch = 1;
    bootstrap_calls=0;assert(nbdriver_open() == -1 && errno == EPROTO && close_count == 2);
    abi_mismatch = 0;
    ioctl_count = close_count = 0;
    assert(nbdriver_get_abi(-1, &info) == -1 && errno == EINVAL);
    assert(nbdriver_get_abi(17, NULL) == -1 && errno == EINVAL);
    assert(nbdriver_read_process(-1, &r) == -1 && errno == EINVAL);
    assert(nbdriver_read_process(17, NULL) == -1 && errno == EINVAL);
    assert(nbdriver_write_process(-1, &w) == -1 && errno == EINVAL);
    assert(nbdriver_write_process(17, NULL) == -1 && errno == EINVAL);
    assert(sdk_second_tu() == -1 && errno == EINVAL);
    assert(ioctl_count == 0);
    assert(nbdriver_get_abi(17, &info) == 0);
    assert(info.version == NBDRIVER_ABI_VERSION && info.size == sizeof(info));
    assert(info.features == (NBDRIVER_FEATURE_READ_PROCESS | NBDRIVER_FEATURE_WRITE_PROCESS));
    r.pid = w.pid = 123;
    r.address = w.address = 4096;
    r.size = w.size = 32;
    r.buffer = w.buffer = 8192;
    r.expected_start_ns = w.expected_start_ns = 42;
    r.copied = w.copied = 99;
    r.error = w.error = -1;
    r.failed_address = w.failed_address = 99;
    assert(nbdriver_read_process(17, &r) == -1 && errno == EFAULT);
    assert(r.copied == 16 && r.error == -EFAULT && r.failed_address == 4112);
    assert(nbdriver_write_process(17, &w) == 0 && w.copied == 32);
    assert(ioctl_count == 3);
#ifndef TEST_LEGACY
    assert(nbdriver_ioctl(17, NBDRIVER_IOC_GET_ABI, &info) == 0);
    expected_start = 0;
    assert(nbdriver_read_process_memory(17, 123, 4096, (void *)(uintptr_t)8192, 32) == -1);
    assert(errno == EFAULT);
    assert(nbdriver_write_process_memory(17, 123, 4096, (void *)(uintptr_t)8192, 32) == 0);
    assert(ioctl_count == 6);
    assert(nbdriver_read_process_memory(-1, 123, 4096, &r, 32) == -1 && errno == EINVAL);
    assert(nbdriver_read_process_memory(17, 0, 4096, &r, 32) == -1 && errno == EINVAL);
    assert(nbdriver_read_process_memory(17, 123, 4096, NULL, 32) == -1 && errno == EINVAL);
    assert(nbdriver_read_process_memory(17, 123, 4096, &r, 0) == -1 && errno == EINVAL);
    assert(nbdriver_read_process_memory(17, 123, 4096, &r, NBDRIVER_READ_MAX + 1) == -1);
    assert(nbdriver_write_process_memory(-1, 123, 4096, &w, 32) == -1 && errno == EINVAL);
    assert(nbdriver_write_process_memory(17, 0, 4096, &w, 32) == -1 && errno == EINVAL);
    assert(nbdriver_write_process_memory(17, 123, 4096, NULL, 32) == -1 && errno == EINVAL);
    assert(nbdriver_write_process_memory(17, 123, 4096, &w, 0) == -1 && errno == EINVAL);
    assert(nbdriver_write_process_memory(17, 123, 4096, &w, NBDRIVER_WRITE_MAX + 1) == -1);
    assert(ioctl_count == 6);
    nbdriver_close(-1);
    assert(close_count == 0);
    nbdriver_close(17);
    assert(close_count == 1);
#endif
    puts("PASS: 150-slot scan, reboot fallback, FD 0, ABI, validation, partial read, write, multiple translation units");
    return 0;
}
'''


def run(command):
    print("+", " ".join(map(str, command)), flush=True)
    result = subprocess.run(list(map(str, command)), text=True, capture_output=True)
    print(result.stdout, end="")
    print(result.stderr, end="")
    print("exit:", result.returncode, flush=True)
    result.check_returncode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--legacy", action="store_true", help="check the original SDK baseline")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="nbdriver-header-") as directory:
        isolated = Path(directory)
        name = "nbdriver/client.h" if args.legacy else "nbdriver.h"
        if not args.legacy:
            shutil.copy2(ROOT / "sdk/include/nbdriver.h", isolated / name)
        for compiler, language, standard, suffix in [
            ("cc", "c", "c11", "c"), ("c++", "c++", "c++17", "cpp")
        ]:
            probe = isolated / ("probe." + suffix)
            second = isolated / ("second." + suffix)
            probe.write_text(PROBE.replace("SDK_HEADER", name))
            second.write_text(f'#include "{name}"\nint sdk_second_tu(void)\n'
                              '{ return nbdriver_get_abi(-1, 0); }\n')
            command = [compiler, "-x", language, f"-std={standard}", "-Wall", "-Wextra",
                       "-Werror", "-pedantic", "-I", isolated, probe, second]
            if args.legacy:
                command += ["-DTEST_LEGACY", "-D_GNU_SOURCE", "-I", ROOT / "include", "-I",
                            ROOT / "sdk/include", ROOT / "sdk/src/client.c"]
            executable = isolated / ("check-" + suffix)
            run(command + ["-Wl,--wrap=syscall", "-Wl,--wrap=ioctl", "-Wl,--wrap=close", "-o", executable])
            run([executable])
        if not args.legacy:
            # Check the bundled ABI against the kernel header in separate programs.
            expression = ('printf("%u %u %u %u %u %u %zu %zu %zu %zu %zu %zu %zu %zu %zu\\n", '
                          'NBDRIVER_ABI_VERSION, NBDRIVER_INSTALL_MAGIC1, NBDRIVER_INSTALL_MAGIC2, '
                          'NBDRIVER_FEATURE_READ_PROCESS, NBDRIVER_FEATURE_WRITE_PROCESS, '
                          'NBDRIVER_FEATURE_READ_FAULT_IN, (size_t)NBDRIVER_READ_MAX, '
                          '(size_t)NBDRIVER_WRITE_MAX, sizeof(struct nbdriver_abi_info), '
                          'sizeof(struct nbdriver_read_request), sizeof(struct nbdriver_write_request), '
                          '(size_t)NBDRIVER_IOC_GET_ABI, (size_t)NBDRIVER_IOC_READ_PROCESS, '
                          '(size_t)NBDRIVER_IOC_WRITE_PROCESS, '
                          'offsetof(struct nbdriver_read_request, expected_start_ns));')
            for value in ['NBDRIVER_FEATURE_UDP_CAPTURE', 'NBDRIVER_IOC_UDP_START',
                          'NBDRIVER_IOC_UDP_READ', 'NBDRIVER_IOC_UDP_STOP', 'NBDRIVER_IOC_UDP_STATUS',
                          'NBDRIVER_FEATURE_FIND_PROCESS', 'NBDRIVER_FEATURE_MODULE_BASE',
                          'NBDRIVER_FEATURE_PROCESS_ID', 'NBDRIVER_FEATURE_THREAD_ID',
                          'NBDRIVER_FEATURE_THREAD_ENV', 'NBDRIVER_FEATURE_THREAD_FP',
                          'NBDRIVER_FEATURE_THREAD_SIGNAL', 'NBDRIVER_FEATURE_VMA',
                          'NBDRIVER_IOC_THREAD_SIGNAL', 'NBDRIVER_IOC_VMA',
                          'NBDRIVER_FEATURE_READV', 'NBDRIVER_FEATURE_MAPS',
                          'NBDRIVER_FEATURE_MAPPING', 'NBDRIVER_FEATURE_FD_SIZE',
                          'NBDRIVER_FEATURE_GPU_SYNC', 'NBDRIVER_FEATURE_GPU_VISIBILITY',
                          'NBDRIVER_IOC_GPU_SYNC', 'NBDRIVER_IOC_GPU_VISIBILITY',
                          'NBDRIVER_GPU_SYNC_MAX_ID', 'NBDRIVER_GPU_SYNC_WORDS',
                          'NBDRIVER_FEATURE_GPU_INFO', 'NBDRIVER_FEATURE_RANDOM',
                          'NBDRIVER_IOC_MAPS', 'NBDRIVER_IOC_MAPPING', 'NBDRIVER_IOC_FD_SIZE',
                          'NBDRIVER_IOC_GPU_INFO', 'NBDRIVER_IOC_RANDOM', 'NBDRIVER_MAPS_MAX',
                          'NBDRIVER_RANDOM_MAX', 'NBDRIVER_MAPPING_READ', 'NBDRIVER_MAPPING_WRITE',
                          'NBDRIVER_MAPPING_EXEC', 'NBDRIVER_MAPPING_SHARED', 'NBDRIVER_FEATURE_DEVPTS',
                          'NBDRIVER_DEVPTS_PACKAGE_COUNT', 'NBDRIVER_DEVPTS_MAX_INDEX',
                          'NBDRIVER_IOC_DEVPTS_STATUS',
                          'NBDRIVER_IOC_FIND_PROCESS',
                          'NBDRIVER_IOC_MODULE_BASE', 'NBDRIVER_IOC_PROCESS_ID',
                          'NBDRIVER_IOC_THREAD_ID', 'NBDRIVER_IOC_THREAD_ENV',
                          'NBDRIVER_IOC_THREAD_FP', 'NBDRIVER_IOC_READV']:
                expression += f'printf("%zu ", (size_t){value});'
            for value in ['nbdriver_udp_packet','nbdriver_udp_start_request','nbdriver_udp_status','nbdriver_udp_read_request','nbdriver_pid_request', 'nbdriver_module_request',
                          'nbdriver_process_identity', 'nbdriver_thread_request',
                          'nbdriver_thread_environment', 'nbdriver_thread_fpstate', 'nbdriver_thread_signal',
                          'nbdriver_readv_entry', 'nbdriver_readv_request',
                          'nbdriver_maps_request', 'nbdriver_mapping_request',
                          'nbdriver_fd_size_request', 'nbdriver_gpu_object_info',
                          'nbdriver_gpu_info_request', 'nbdriver_gpu_sync_request', 'nbdriver_gpu_visibility_request', 'nbdriver_random_request', 'nbdriver_devpts_status']:
                expression += f'printf("%zu ", sizeof(struct {value}));'
            for structure, fields in {
                'nbdriver_udp_packet': ['sequence', 'timestamp_ns', 'src_ip', 'original_len', 'payload_len', 'flags', 'gso_size', 'payload'],
                'nbdriver_udp_start_request': ['expected_start_ns', 'min_payload', 'max_payload', 'generation'],
                'nbdriver_udp_status': ['start_ns', 'generation', 'captured', 'overflow', 'truncated', 'malformed', 'probe_missed', 'queued', 'error'],
                'nbdriver_udp_read_request': ['packets', 'generation', 'capacity', 'count', 'status'],
                'nbdriver_pid_request': ['pid', 'start_ns'],
                'nbdriver_module_request': ['expected_start_ns', 'base', 'name'],
                'nbdriver_thread_request': ['tid', 'expected_start_ns', 'thread_start_ns', 'comm'],
                'nbdriver_thread_environment': ['valid', 'expected_start_ns', 'tls', 'apga_lo', 'apga_hi'],
                'nbdriver_thread_signal': ['signo', 'expected_start_ns', 'expected_thread_start_ns', 'handler', 'flags', 'restorer', 'action_mask', 'blocked_mask', 'sample_ns'],
                'nbdriver_thread_fpstate': ['valid', 'expected_start_ns', 'tls', 'sample_ns', 'fpsr', 'fpcr'],
                'nbdriver_readv_entry': ['buffer', 'size', 'copied', 'error', 'failed_address'],
                'nbdriver_readv_request': ['entries', 'expected_start_ns', 'completed', 'error'],
                'nbdriver_maps_request': ['buffer', 'expected_start_ns', 'length', 'copied', 'error', 'reserved'],
                'nbdriver_mapping_request': ['address', 'expected_start_ns', 'physical', 'page_size', 'vma_start', 'vma_end', 'flags', 'reserved2'],
                'nbdriver_fd_size_request': ['target_fd', 'expected_start_ns', 'size', 'error', 'reserved'],
                'nbdriver_gpu_object_info': ['flags', 'size', 'virtual_length', 'virtual_address', 'id', 'reserved'],
                'nbdriver_gpu_sync_request': ['expected_start_ns', 'max_id', 'first_free', 'backend', 'error', 'sample_ns', 'occupied'],
                'nbdriver_gpu_visibility_request': ['operation', 'expected_start_ns', 'hidden', 'directory_hits', 'access_hits', 'missed'],
                'nbdriver_gpu_info_request': ['object_id', 'expected_start_ns', 'result', 'sample_ns', 'info'],
                'nbdriver_random_request': ['size', 'copied', 'error', 'reserved'],
                'nbdriver_devpts_status': ['size', 'active', 'max_index', 'target_count', 'pids', 'permission_hits', 'getattr_hits', 'generation', 'last_error', 'reserved'],
            }.items():
                for field in fields:
                    expression += f'printf("%zu ", offsetof(struct {structure}, {field}));'
            expression += 'printf("%u %u ", NBDRIVER_INSTALL_CANDIDATE_COUNT, NBDRIVER_INSTALL_REBOOT_SYSCALL);'
            expression += 'for (unsigned int i=0;i<NBDRIVER_INSTALL_CANDIDATE_COUNT;++i) printf("%u ",nbdriver_install_candidate(i));'
            outputs = []
            for name, include_dir in [("nbdriver.h", isolated), ("nbdriver_abi.h", ROOT / "include")]:
                source = isolated / "abi.c"
                source.write_text(f'#include "{name}"\n#include <stddef.h>\n#include <stdio.h>\n'
                                  'int main(void) { ' + expression + ' return 0; }\n')
                executable = isolated / "abi-check"
                run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", include_dir,
                     source, "-o", executable])
                outputs.append(subprocess.check_output([executable], text=True))
            assert outputs[0] == outputs[1], "single-header ABI differs from kernel ABI"
            print("PASS: standalone ABI matches kernel ABI:", outputs[0].strip())


if __name__ == "__main__":
    main()
