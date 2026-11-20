#!/usr/bin/env python3
"""Compile the real ioctl dispatcher with host-only backend probes.

No kernel module is loaded. Backend probes only model output ABI and route
identity; the switch and quiesce/GET_ABI branches come from production C.
"""
from pathlib import Path
import argparse
import os
import shlex
import subprocess
import tempfile

PROJECT = Path(__file__).resolve().parents[1]
PRELUDE = r'''
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "nbdriver_abi.h"
#define __user
#define nbd_warn(...) ((void)0)
struct file { int unused; };
static int paused, route, calls, failures;
static unsigned int observed_command;
static unsigned long observed_argument;
static long backend_result;
enum { METADATA=1, DEVPTS, QUERY, READV, READ, WRITE, UDP, TOUCH,
       RUNTIME, CONTROL, GROUPS };
static bool nbdriver_runtime_quiesced(void) { return paused != 0; }
static unsigned long copy_to_user(void *to,const void *from,size_t size) {
    if(!to)return size;
    memcpy(to,from,size);return 0;
}
static long dispatch_probe(int next,unsigned int command,unsigned long argument) {
    route=next; observed_command=command; observed_argument=argument; ++calls;
    return backend_result;
}
static long nbdriver_metadata_ioctl(unsigned int command,unsigned long argument) {
    long ret=dispatch_probe(METADATA,command,argument);
    if(ret)return ret;
    if(command==NBDRIVER_IOC_MAPPING) {
        struct nbdriver_mapping_request *r=(void *)argument;
        r->physical=0x12345000;r->page_size=4096;
        r->vma_start=r->address&~4095ULL;r->vma_end=r->vma_start+4096;
        r->flags=NBDRIVER_MAPPING_READ|NBDRIVER_MAPPING_EXEC;
    }
    if(command==NBDRIVER_IOC_VMA) {
        struct nbdriver_mapping_request *r=(void *)argument;
        r->physical=r->page_size=0;
        r->vma_start=r->address&~4095ULL;r->vma_end=r->vma_start+4096;
        r->flags=NBDRIVER_MAPPING_READ;
    }
    return 0;
}
static long nbdriver_devpts_status_ioctl(unsigned long argument) {
    long ret=dispatch_probe(DEVPTS,NBDRIVER_IOC_DEVPTS_STATUS,argument);
    struct nbdriver_devpts_status status={.size=sizeof(status),.active=1,.max_index=63};
    if(!ret)memcpy((void *)argument,&status,sizeof(status));
    return ret;
}
static long nbdriver_query_ioctl(unsigned int c,unsigned long a) {return dispatch_probe(QUERY,c,a);}
static long nbdriver_readv_ioctl(unsigned long a) {return dispatch_probe(READV,NBDRIVER_IOC_READV,a);}
static long nbdriver_read_ioctl(unsigned long a) {return dispatch_probe(READ,NBDRIVER_IOC_READ_PROCESS,a);}
static long nbdriver_write_ioctl(unsigned long a) {return dispatch_probe(WRITE,NBDRIVER_IOC_WRITE_PROCESS,a);}
static long nbdriver_udp_ioctl(struct file *f,unsigned int c,unsigned long a) {(void)f;return dispatch_probe(UDP,c,a);}
static long nbdriver_touch_ioctl(struct file *f,unsigned int c,unsigned long a) {(void)f;return dispatch_probe(TOUCH,c,a);}
static long nbdriver_runtime_ioctl(unsigned long a) {return dispatch_probe(RUNTIME,NBDRIVER_IOC_RUNTIME_STATUS,a);}
static long nbdriver_runtime_control_ioctl(unsigned long a) {return dispatch_probe(CONTROL,NBDRIVER_IOC_RUNTIME_CONTROL,a);}
static long nbdriver_sf_groups_ioctl(unsigned long a) {return dispatch_probe(GROUPS,NBDRIVER_IOC_SF_GROUPS_ACQUIRE,a);}
static unsigned int nbdriver_query_features(void) {return 0;}
static unsigned int nbdriver_metadata_features(void) {return NBDRIVER_FEATURE_MAPPING;}
static unsigned int nbdriver_devpts_features(void) {return NBDRIVER_FEATURE_DEVPTS;}
static unsigned int nbdriver_runtime_features(void) {return 0;}
static unsigned int nbdriver_udp_features(void) {return 0;}
static unsigned int nbdriver_touch_features(void) {return 0;}
static bool nbdriver_read_fault_in_ready(void) {return false;}
'''
MAIN = r'''
static void check(int ok,const char *name) {
    printf("%s %s\n",ok?"PASS":"FAIL",name);failures+=!ok;
}
int main(void) {
    struct file file={0};
    const unsigned int metadata[]={NBDRIVER_IOC_MAPS,NBDRIVER_IOC_MAPPING,
        NBDRIVER_IOC_VMA,NBDRIVER_IOC_FD_SIZE,NBDRIVER_IOC_GPU_SYNC,
        NBDRIVER_IOC_GPU_VISIBILITY,NBDRIVER_IOC_GPU_INFO,NBDRIVER_IOC_RANDOM};
    union { uint64_t alignment; unsigned char data[8192]; } storage;
    unsigned long argument=(unsigned long)storage.data;
    for(unsigned int i=0;i<sizeof(metadata)/sizeof(metadata[0]);++i) {
        memset(&storage,0xa5,sizeof(storage));route=calls=0;backend_result=0;
        long ret=nbdriver_ioctl(&file,metadata[i],argument);
        char name[120];snprintf(name,sizeof(name),"metadata command 0x%x reaches metadata backend exactly once",metadata[i]);
        check(ret==0&&calls==1&&route==METADATA&&observed_command==metadata[i]&&observed_argument==argument,name);
        int bounds_ok=1;
        for(size_t n=_IOC_SIZE(metadata[i]);n<sizeof(storage);++n)if(storage.data[n]!=0xa5)bounds_ok=0;
        check(bounds_ok,"metadata command never writes beyond its own ABI output size");
        backend_result=-EFAULT;route=calls=0;
        ret=nbdriver_ioctl(&file,metadata[i],argument);
        check(ret==-EFAULT&&calls==1&&route==METADATA,"metadata backend errors are not replaced by status success");
    }
    backend_result=0;memset(&storage,0xa5,sizeof(storage));
    struct nbdriver_mapping_request *mapping=(void *)storage.data;
    memset(mapping,0,sizeof(*mapping));mapping->pid=123;mapping->address=0x6000000123ULL;mapping->expected_start_ns=111;
    long ret=nbdriver_ioctl(&file,NBDRIVER_IOC_MAPPING,argument);
    check(ret==0&&mapping->pid==123&&mapping->address==0x6000000123ULL&&mapping->expected_start_ns==111&&
        mapping->physical==0x12345000&&mapping->page_size==4096&&mapping->vma_start==0x6000000000ULL&&
        mapping->vma_end==0x6000001000ULL&&mapping->flags==5,"MAPPING preserves request identity and returns page metadata");
    int intact=1;for(size_t i=sizeof(*mapping);i<sizeof(storage);++i)if(storage.data[i]!=0xa5)intact=0;
    check(intact,"MAPPING never writes a DEVPTS status beyond its output structure");
    memset(mapping,0,sizeof(*mapping));mapping->pid=123;mapping->address=0x6000000123ULL;mapping->expected_start_ns=111;
    ret=nbdriver_ioctl(&file,NBDRIVER_IOC_VMA,argument);
    check(ret==0&&route==METADATA&&mapping->pid==123&&mapping->physical==0&&mapping->page_size==0&&
        mapping->vma_start==0x6000000000ULL&&mapping->vma_end==0x6000001000ULL,"VMA uses its range-only metadata contract");
    memset(&storage,0,sizeof(storage));route=calls=0;
    ret=nbdriver_ioctl(&file,NBDRIVER_IOC_DEVPTS_STATUS,argument);
    struct nbdriver_devpts_status *status=(void *)storage.data;
    check(ret==0&&route==DEVPTS&&calls==1&&status->size==sizeof(*status)&&status->active==1,
        "DEVPTS status remains isolated from all metadata commands");
    const struct {unsigned int command;int expected;} other[]={
        {NBDRIVER_IOC_FIND_PROCESS,QUERY},{NBDRIVER_IOC_MODULE_BASE,QUERY},{NBDRIVER_IOC_PROCESS_ID,QUERY},
        {NBDRIVER_IOC_THREAD_ID,QUERY},{NBDRIVER_IOC_THREAD_SIGNAL,QUERY},{NBDRIVER_IOC_THREAD_ENV,QUERY},
        {NBDRIVER_IOC_THREAD_FP,QUERY},{NBDRIVER_IOC_READV,READV},{NBDRIVER_IOC_READ_PROCESS,READ},
        {NBDRIVER_IOC_WRITE_PROCESS,WRITE},{NBDRIVER_IOC_UDP_START,UDP},{NBDRIVER_IOC_UDP_READ,UDP},
        {NBDRIVER_IOC_UDP_STOP,UDP},{NBDRIVER_IOC_UDP_STATUS,UDP},{NBDRIVER_IOC_TOUCH_READ,TOUCH},
        {NBDRIVER_IOC_TOUCH_VIRTUAL,TOUCH},{NBDRIVER_IOC_RUNTIME_STATUS,RUNTIME},
        {NBDRIVER_IOC_RUNTIME_CONTROL,CONTROL},{NBDRIVER_IOC_SF_GROUPS_ACQUIRE,GROUPS}};
    for(unsigned int i=0;i<sizeof(other)/sizeof(other[0]);++i) {
        route=calls=0;backend_result=-EAGAIN;
        ret=nbdriver_ioctl(&file,other[i].command,argument);
        check(ret==-EAGAIN&&calls==1&&route==other[i].expected,"unrelated command route and errno remain unchanged");
    }
    backend_result=0;paused=1;
    for(unsigned int i=0;i<sizeof(metadata)/sizeof(metadata[0]);++i) {
        calls=0;ret=nbdriver_ioctl(&file,metadata[i],argument);
        check(ret==-ESHUTDOWN&&!calls,"quiesced driver still rejects metadata without invoking a backend");
    }
    for(unsigned int i=0;i<sizeof(other)/sizeof(other[0]);++i) {
        unsigned int c=other[i].command;calls=0;
        ret=nbdriver_ioctl(&file,c,argument);
        int allowed=c==NBDRIVER_IOC_RUNTIME_STATUS||c==NBDRIVER_IOC_RUNTIME_CONTROL||
                    c==NBDRIVER_IOC_UDP_STOP||c==NBDRIVER_IOC_UDP_STATUS;
        check(allowed?(ret==0&&calls==1):(ret==-ESHUTDOWN&&!calls),"quiesce command exceptions remain unchanged");
    }
    memset(&storage,0,sizeof(storage));
    ret=nbdriver_ioctl(&file,NBDRIVER_IOC_GET_ABI,argument);
    struct nbdriver_abi_info *abi=(void *)storage.data;
    check(ret==0&&abi->version==NBDRIVER_ABI_VERSION&&abi->size==sizeof(*abi),"ABI handshake still works while quiesced");
    check(nbdriver_ioctl(&file,NBDRIVER_IOC_GET_ABI,0)==-EFAULT,"ABI null output remains rejected");
    paused=0;calls=0;
    check(nbdriver_ioctl(&file,0xdeadbeef,argument)==-ENOTTY&&!calls,"unknown command never reaches a status backend");
    printf("SUMMARY failures=%d\n",failures);
    return failures?1:0;
}
'''

def extract_function(source):
    text = source.read_text()
    start = text.index('static long nbdriver_ioctl(')
    brace = text.index('{', start)
    depth = 0
    for index in range(brace, len(text)):
        depth += (text[index] == '{') - (text[index] == '}')
        if depth == 0:
            return text[start:index + 1]
    raise ValueError('incomplete production dispatcher')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=PROJECT/'src/nbdriver_core.c')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    work = args.output or Path(tempfile.mkdtemp(prefix='nbdriver-dispatch-'))
    work.mkdir(parents=True, exist_ok=True)
    source = work/'dispatch_harness.c'
    source.write_text(PRELUDE + '\n' + extract_function(args.source) + '\n' + MAIN)
    command = shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
        '-I'+str(PROJECT/'sdk/include'), str(source), '-o', str(work/'dispatch_harness')]
    print('BUILD', shlex.join(command), flush=True)
    subprocess.run(command, check=True)
    result = subprocess.run([str((work/'dispatch_harness').resolve())])
    raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()
