// SPDX-License-Identifier: GPL-2.0-only
#include "nbdriver.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
static unsigned calls;
#ifdef __cplusplus
extern "C"
#endif
int __wrap_ioctl(int fd,unsigned long command,...)
{
    va_list args;void *p;
    assert(fd==17);++calls;va_start(args,command);p=va_arg(args,void *);va_end(args);
    if(command==NBDRIVER_IOC_MAPS){
        struct nbdriver_maps_request *r=(struct nbdriver_maps_request *)p;
        assert(r->pid==123&&!r->length&&!r->copied&&!r->error&&r->expected_start_ns==42);
        r->length=100;
        if(r->capacity&&r->capacity<101){r->error=-ENOSPC;errno=ENOSPC;return -1;}
        if(r->capacity)r->copied=100;
    }else if(command==NBDRIVER_IOC_MAPPING){
        struct nbdriver_mapping_request *r=(struct nbdriver_mapping_request *)p;
        assert(r->pid==123&&r->address==4096&&r->expected_start_ns==42&&!r->physical&&!r->reserved&&!r->reserved2);
        r->physical=8192;r->page_size=4096;
    }else if(command==NBDRIVER_IOC_FD_SIZE){
        struct nbdriver_fd_size_request *r=(struct nbdriver_fd_size_request *)p;
        assert(r->pid==123&&r->target_fd==7&&r->expected_start_ns==42&&!r->size&&!r->reserved);
        r->size=12345;
    }else if(command==NBDRIVER_IOC_GPU_INFO){
        struct nbdriver_gpu_info_request *r=(struct nbdriver_gpu_info_request *)p;
        assert(r->pid==123&&r->target_fd==7&&r->object_id==9&&r->expected_start_ns==42&&!r->sample_ns&&!r->reserved);
        r->result=-EINVAL;r->sample_ns=1;
    }else{
        struct nbdriver_random_request *r=(struct nbdriver_random_request *)p;
        assert(command==NBDRIVER_IOC_RANDOM&&r->size==64&&r->buffer&&!r->copied&&!r->error&&!r->reserved);
        memset((void *)(uintptr_t)r->buffer,0x5a,64);r->copied=64;
    }
    return 0;
}
int main(void)
{
    char text[128];unsigned char random[64];nbdriver_u32 length=999;nbdriver_u64 size=999;
    struct nbdriver_mapping_request mapping;
    struct nbdriver_gpu_info_request gpu;
    assert(nbdriver_get_process_maps(-1,123,text,sizeof(text),&length,42)==-1&&errno==EINVAL&&length==0);
    assert(nbdriver_get_process_maps(17,123,NULL,1,&length,42)==-1&&errno==EINVAL);
    assert(nbdriver_query_mapping(17,123,0,42,&mapping)==-1&&errno==EINVAL&&!mapping.physical);
    assert(nbdriver_get_file_descriptor_size(17,123,-1,42,&size)==-1&&errno==EINVAL&&size==0);
    assert(nbdriver_read_gpu_object_info(17,123,7,0,42,&gpu)==-1&&errno==EINVAL);
    assert(nbdriver_read_random(17,random,65)==-1&&errno==EINVAL&&calls==0);
    assert(nbdriver_get_process_maps(17,123,NULL,0,&length,42)==0&&length==100);
    assert(nbdriver_get_process_maps(17,123,text,1,&length,42)==-1&&errno==ENOSPC&&length==100);
    assert(nbdriver_get_process_maps(17,123,text,sizeof(text),&length,42)==0&&length==100);
    assert(nbdriver_query_mapping(17,123,4096,42,&mapping)==0&&mapping.physical==8192&&mapping.page_size==4096);
    assert(nbdriver_get_file_descriptor_size(17,123,7,42,&size)==0&&size==12345);
    assert(nbdriver_read_gpu_object_info(17,123,7,9,42,&gpu)==0&&gpu.result==-EINVAL&&gpu.sample_ns==1);
    assert(nbdriver_read_random(17,random,sizeof(random))==64&&random[63]==0x5a);
    assert(calls==7);
    puts("PASS: C/C++ metadata SDK validation, maps sizing/ENOSPC, mapping, FD, GPU result, random");
    return 0;
}
