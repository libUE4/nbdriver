// SPDX-License-Identifier: GPL-2.0-only
#include "nbdriver.h"
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
static unsigned calls;static int mode;
int __wrap_ioctl(int fd,unsigned long command,...)
{
 va_list ap;struct nbdriver_devpts_filter_request *r;assert(fd==17&&command==NBDRIVER_IOC_DEVPTS_FILTER);va_start(ap,command);r=va_arg(ap,void *);va_end(ap);++calls;
 assert(r->abi_version==1);if(mode==0){assert(r->enabled==1&&r->pid==123&&r->max_index==31);r->permission_hits=4;r->getattr_hits=5;}
 else if(mode==1){assert(r->enabled==2);r->enabled=1;r->pid=123;r->max_index=31;r->permission_hits=4;r->getattr_hits=5;}
 else {assert(r->enabled==0);r->enabled=0;r->pid=0;r->max_index=0;r->permission_hits=0;r->getattr_hits=0;}return 0;
}
int main(void){struct nbdriver_devpts_filter_request r;memset(&r,0,sizeof(r));assert(nbdriver_configure_devpts_filter(17,123,1,31,&r)==0&&r.enabled==1&&r.permission_hits==4);mode=1;assert(nbdriver_query_devpts_filter(17,&r)==0&&r.pid==123&&r.getattr_hits==5);mode=2;assert(nbdriver_disable_devpts_filter(17,&r)==0&&!r.enabled&&!r.pid);assert(calls==3);puts("PASS: devpts C ABI wrappers");return 0;}
