// SPDX-License-Identifier: GPL-2.0-only
#include "nbdriver.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
static unsigned calls;
static int fp_error;
#ifdef __cplusplus
extern "C" {
#endif
int __wrap_ioctl(int fd, unsigned long command, ...)
{
    va_list args;
    assert(fd==17); ++calls;
    va_start(args,command);
    if(command==NBDRIVER_IOC_FIND_PROCESS) {
        struct nbdriver_pid_request *r=va_arg(args,struct nbdriver_pid_request *);
        assert(!strcmp(r->name,"test.process") && !r->reserved && !r->start_ns);
        r->pid=123; r->start_ns=42;
    } else if(command==NBDRIVER_IOC_MODULE_BASE) {
        struct nbdriver_module_request *r=va_arg(args,struct nbdriver_module_request *);
        assert(r->pid==123 && !r->reserved && !r->base);
        assert(!strcmp(r->name,"libtest.so")); r->base=4096;
    } else if(command==NBDRIVER_IOC_PROCESS_ID) {
        struct nbdriver_process_identity *r=va_arg(args,struct nbdriver_process_identity *);
        assert(r->pid==123 && !r->reserved); r->start_ns=42;
    } else if(command==NBDRIVER_IOC_THREAD_ID) {
        struct nbdriver_thread_request *r=va_arg(args,struct nbdriver_thread_request *);
        assert(r->pid==123 && !strcmp(r->comm,"GameThread")); r->tid=124;
    } else if(command==NBDRIVER_IOC_THREAD_ENV) {
        struct nbdriver_thread_environment *r=va_arg(args,struct nbdriver_thread_environment *);
        assert(r->pid==123 && r->tid==124 && !r->reserved && !r->valid);
        r->valid=NBDRIVER_THREAD_TLS_VALID; r->tls=8192;
    } else if(command==NBDRIVER_IOC_THREAD_FP) {
        struct nbdriver_thread_fpstate *r=va_arg(args,struct nbdriver_thread_fpstate *);
        assert(r->pid==123 && r->tid==124 && !r->valid);
        if(fp_error) { va_end(args); errno=fp_error; return -1; }
        r->valid=1; r->fpsr=1; r->fpcr=2; r->sample_ns=43;
    } else {
        struct nbdriver_readv_request *r=va_arg(args,struct nbdriver_readv_request *);
        struct nbdriver_readv_entry *entries;
        assert(command==NBDRIVER_IOC_READV && r->pid==123 && r->count==2);
        assert(r->expected_start_ns==42 && !r->completed && !r->error);
        entries=(struct nbdriver_readv_entry *)(uintptr_t)r->entries;
        entries[0].copied=32; entries[0].error=0;
        entries[1].copied=16; entries[1].error=-EFAULT;
        r->completed=1;
    }
    va_end(args); return 0;
}
#ifdef __cplusplus
}
#endif
int main(void)
{
    nbdriver_u64 value;
    struct nbdriver_thread_environment env;
    struct nbdriver_thread_fpstate fp;
    struct nbdriver_readv_entry entries[2];
    memset(entries,0,sizeof(entries));
    assert(nbdriver_find_process(17,"test.process")==123);
    assert(nbdriver_get_module_base(17,123,"libtest.so",&value)==0 && value==4096);
    assert(nbdriver_get_process_start_time(17,123,&value)==0 && value==42);
    assert(nbdriver_get_thread_id(17,123,"GameThread")==124);
    assert(nbdriver_get_thread_environment(17,123,124,&env)==0 && env.tls==8192);
    assert(nbdriver_get_thread_fpstate(17,123,124,&fp)==0 && fp.valid && fp.fpsr==1 && fp.fpcr==2);
    fp_error=EAGAIN;
    assert(nbdriver_get_thread_fpstate(17,123,124,&fp)==-1 && errno==EAGAIN && !fp.valid);
    assert(nbdriver_read_process_vector(17,123,entries,2,42)==0);
    assert(entries[0].copied==32 && !entries[0].error && entries[1].error==-EFAULT);
    unsigned previous=calls;
    assert(nbdriver_find_process(-1,"test.process")==-1 && errno==EINVAL);
    assert(nbdriver_find_process(17,"")==-1 && errno==EINVAL);
    assert(nbdriver_get_thread_id(17,123,"1234567890123456")==-1 && errno==ENAMETOOLONG);
    assert(nbdriver_get_module_base(17,123,"libtest.so",NULL)==-1 && errno==EINVAL);
    assert(nbdriver_get_process_start_time(17,0,&value)==-1 && errno==EINVAL && !value);
    assert(nbdriver_get_thread_environment(17,123,0,&env)==-1 && errno==EINVAL && !env.valid);
    assert(nbdriver_read_process_vector(17,123,entries,0,0)==-1 && errno==EINVAL);
    assert(calls==previous);
    puts("PASS: single-header query/thread/readv APIs and validation");
}
