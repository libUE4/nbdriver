/* SPDX-License-Identifier: GPL-2.0-only */
#ifdef TEST_LEGACY
#include "nbdriver/client.h"
#else
#include "nbdriver.h"
#endif
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stddef.h>
_Static_assert(sizeof(struct nbdriver_udp_packet)==2096,"packet ABI");
_Static_assert(offsetof(struct nbdriver_udp_packet,payload)==48,"payload ABI");
_Static_assert(sizeof(struct nbdriver_udp_status)==96,"status ABI");
_Static_assert(sizeof(struct nbdriver_udp_start_request)==32,"start ABI");
_Static_assert(sizeof(struct nbdriver_udp_read_request)==120,"read ABI");
_Static_assert(_IOC_NR(NBDRIVER_IOC_UDP_START)==0x25,"new command");
_Static_assert(_IOC_NR(NBDRIVER_IOC_UDP_STATUS)==0x28,"new command");
_Static_assert(NBDRIVER_ABI_VERSION==5,"additive change");
static int calls,error,malformed;
int __wrap_ioctl(int fd,unsigned long command,...){
    va_list args;va_start(args,command);assert(fd==17);++calls;
    if(error){va_end(args);errno=error;return -1;}
    if(command==NBDRIVER_IOC_UDP_STOP){assert(va_arg(args,int)==0);va_end(args);return 0;}
    if(command==NBDRIVER_IOC_UDP_START){
        struct nbdriver_udp_start_request *r=va_arg(args,struct nbdriver_udp_start_request *);
        assert(r->pid==123&&!r->reserved&&!r->generation&&r->expected_start_ns==42);
        assert(r->min_payload==16&&r->max_payload==2048);r->generation=7;
    }else if(command==NBDRIVER_IOC_UDP_READ){
        struct nbdriver_udp_read_request *r=va_arg(args,struct nbdriver_udp_read_request *);
        assert(r->packets&&r->capacity==2&&r->generation==7&&!r->count&&!r->status.size);
        r->count=malformed?3:1;r->status.size=sizeof(r->status);r->status.active=1;r->status.generation=7;
        struct nbdriver_udp_packet *p=(void *)(uintptr_t)r->packets;memset(p,0,sizeof(*p));p->sequence=9;p->payload_len=16;
    }else{
        assert(command==NBDRIVER_IOC_UDP_STATUS);struct nbdriver_udp_status *r=va_arg(args,struct nbdriver_udp_status *);
        assert(!r->size);r->size=malformed?0:sizeof(*r);r->active=1;r->generation=7;
    }
    va_end(args);return 0;
}
int main(void){
    struct nbdriver_udp_start_request started;struct nbdriver_udp_status status;struct nbdriver_udp_packet packets[2];
    assert(nbdriver_udp_capture_start(-1,123,0,0,42,&started)==-1&&errno==EINVAL);
    assert(nbdriver_udp_capture_start(17,123,2049,0,42,&started)==-1&&errno==EINVAL);
    assert(nbdriver_udp_capture_read(17,packets,33,7,&status)==-1&&errno==EINVAL);
    assert(nbdriver_udp_capture_read(17,packets,2,0,&status)==-1&&errno==EINVAL);
    assert(nbdriver_udp_capture_status(17,NULL)==-1&&errno==EINVAL);
    assert(!calls);
    assert(!nbdriver_udp_capture_start(17,123,0,0,42,&started)&&started.generation==7);
    assert(nbdriver_udp_capture_read(17,packets,2,7,&status)==1&&packets[0].sequence==9&&status.active);
    assert(!nbdriver_udp_capture_status(17,&status)&&status.generation==7);
    malformed=1;assert(nbdriver_udp_capture_read(17,packets,2,7,&status)==-1&&errno==EPROTO);
    assert(nbdriver_udp_capture_status(17,&status)==-1&&errno==EPROTO);malformed=0;
    error=ENOTTY;assert(nbdriver_udp_capture_start(17,123,0,0,42,&started)==-1&&errno==ENOTTY);
    assert(nbdriver_udp_capture_read(17,packets,2,7,&status)==-1&&errno==ENOTTY&&!status.size);
    error=0;assert(!nbdriver_udp_capture_stop(17));
    puts("PASS UDP SDK: additive ABI, defaults, validation, batches, errno, legacy-module detection and protocol checks");
}
