// SPDX-License-Identifier: GPL-2.0-only
/* Executes the actual batch dispatcher with synthetic task/mm and copy helpers.
 * The memory walker is stubbed; its own tests cover page-table behavior. */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nbdriver_abi.h"

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define __user
#define GFP_KERNEL 0
#define CAP_SYS_PTRACE 0
#define PIDTYPE_TGID 0
#define READ_ONCE(x) (x)
#define min_t(type,a,b) ((type)(a) < (type)(b) ? (type)(a) : (type)(b))
#define u64_to_user_ptr(x) ((void *)(uintptr_t)(x))
#define IS_ERR_OR_NULL(p) (!(p) || (uintptr_t)(p) > (uintptr_t)-4096)
#define PTR_ERR(p) ((long)(intptr_t)(p))
struct task_struct { u64 start_boottime; };
struct mm_struct { int unused; };
struct pid { int unused; };
static struct task_struct target = {42};
static struct mm_struct mm;
static struct pid id;
static bool permission = true;
static unsigned resolves, mm_refs, task_refs, reads;
static unsigned char remote[512 * 1024];
static size_t readable = sizeof(remote);
static bool capable(int capability) { (void)capability; return permission; }
static bool access_ok(const void *p, size_t size) { (void)size; return p && p != (void *)0xdead; }
static unsigned long copy_from_user(void *out, const void *in, size_t size)
{ if (!access_ok(in,size)) return size; memcpy(out,in,size); return 0; }
static unsigned long copy_to_user(void *out, const void *in, size_t size)
{ if (!access_ok(out,size)) return size; memcpy(out,in,size); return 0; }
static void *kvmalloc(size_t size, int flags) { (void)flags; return malloc(size); }
static void kvfree(void *p) { free(p); }
static struct pid *find_get_pid(int pid) { ++resolves; return pid == 123 ? &id : NULL; }
static struct task_struct *get_pid_task(struct pid *p, int type)
{ (void)p; (void)type; ++task_refs; return &target; }
static void put_pid(struct pid *p) { (void)p; }
static void put_task_struct(struct task_struct *p) { (void)p; --task_refs; }
static struct mm_struct *nbdriver_access_mm(struct task_struct *p)
{ (void)p; ++mm_refs; return &mm; }
static void mmput(struct mm_struct *p) { (void)p; --mm_refs; }
static int nbdriver_read_mm(struct mm_struct *m, struct nbdriver_read_request *r,
                            u8 *out, bool fault)
{
    uintptr_t begin = (uintptr_t)remote, end = begin + readable;
    (void)m;
    assert(fault && r->size <= NBDRIVER_READ_MAX);
    ++reads;
    if (r->address < begin || r->address >= end) return -EFAULT;
    r->copied = (u32)((end-r->address) < r->size ? end-r->address : r->size);
    memcpy(out,(void *)(uintptr_t)r->address,r->copied);
    return r->copied == r->size ? 0 : -EFAULT;
}
#include "readv_dispatch.inc"
static long issue(struct nbdriver_readv_request *request)
{
    long result = nbdriver_readv_ioctl((unsigned long)request);
    assert(!task_refs && !mm_refs);
    return result;
}
int main(void)
{
    unsigned char *output = malloc(sizeof(remote));
    struct nbdriver_readv_entry entries[33];
    struct nbdriver_readv_request request;
    memset(remote,0x5a,sizeof(remote));
    memset(entries,0,sizeof(entries));
    memset(&request,0,sizeof(request));
    request.pid=123; request.count=1; request.entries=(uintptr_t)entries;
    entries[0].address=(uintptr_t)remote; entries[0].buffer=(uintptr_t)output;
    entries[0].size=sizeof(remote);
    assert(issue(&request)==0 && request.completed==1 && !request.error);
    assert(entries[0].copied==sizeof(remote) && !entries[0].error);
    assert(!memcmp(output,remote,sizeof(remote)) && reads==8 && resolves==1);
    assert(request.expected_start_ns==42);
    readable=70000;
    assert(issue(&request)==0 && request.completed==0 && !request.error);
    assert(entries[0].copied==70000 && entries[0].error==-EFAULT);
    assert(entries[0].failed_address==(uintptr_t)remote+70000);
    readable=sizeof(remote);
    request.expected_start_ns=43;
    assert(issue(&request)==-ESTALE && request.error==-ESTALE);
    assert(!entries[0].copied && entries[0].error==-ESTALE);
    request.expected_start_ns=42; request.count=3;
    entries[0].size=32;
    entries[1]=entries[0]; entries[1].buffer=0xdead;
    entries[2]=entries[0]; entries[2].address=1;
    assert(issue(&request)==0 && request.completed==1);
    assert(entries[0].copied==32 && !entries[0].error);
    assert(entries[1].error==-EFAULT && entries[2].error==-EFAULT);
    for(unsigned i=0;i<33;i++) {
        entries[i]=entries[0]; entries[i].size=NBDRIVER_READV_ITEM_MAX;
    }
    request.count=33;
    assert(issue(&request)==-E2BIG && request.error==-E2BIG);
    request.count=1; entries[0].reserved=1;
    assert(issue(&request)==0 && !request.completed && entries[0].error==-EINVAL);
    entries[0].reserved=0; entries[0].size=NBDRIVER_READV_ITEM_MAX+1;
    assert(issue(&request)==0 && entries[0].error==-EINVAL);
    entries[0].size=32; request.pid=999;
    assert(issue(&request)==-ESRCH && request.error==-ESRCH);
    request.pid=123; permission=false;
    assert(issue(&request)==-EPERM);
    permission=true; request.count=NBDRIVER_READV_MAX+1;
    assert(issue(&request)==-EINVAL);
    request.count=0;
    assert(issue(&request)==-EINVAL);
    free(output);
    puts("PASS: actual batch dispatcher, 512 KiB chunking, partial/mixed failure, identity, limits, reference cleanup");
}
