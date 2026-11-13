// SPDX-License-Identifier: GPL-2.0-only
#include "nbdriver.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
static unsigned calls;
static int mode;
int __wrap_ioctl(int fd,unsigned long command,...)
{
    va_list ap;struct nbdriver_devpts_status *s;
    assert(fd==17 && command==NBDRIVER_IOC_DEVPTS_STATUS);
    assert(_IOC_DIR(command)==_IOC_READ);
    va_start(ap,command);s=va_arg(ap,void *);va_end(ap);++calls;
    if(mode==1){errno=EIO;return -1;}
    s->size=mode==2?0:sizeof(*s);s->active=1;s->target_count=2;
    s->pids[0]=123;s->pids[1]=456;s->max_index=31;s->getattr_hits=5;
    s->reserved=mode==3?1:0;return 0;
}
int main(void)
{
    struct nbdriver_devpts_status s;
    assert(!nbdriver_get_devpts_status(17,&s)&&s.active&&s.pids[0]==123&&s.pids[1]==456&&s.getattr_hits==5);
    assert(nbdriver_get_devpts_status(-1,&s)==-1&&errno==EINVAL&&!s.active);
    assert(nbdriver_get_devpts_status(17,NULL)==-1&&errno==EINVAL&&calls==1);
    mode=1;assert(nbdriver_get_devpts_status(17,&s)==-1&&errno==EIO);
    mode=2;assert(nbdriver_get_devpts_status(17,&s)==-1&&errno==EPROTO);
    mode=3;assert(nbdriver_get_devpts_status(17,&s)==-1&&errno==EPROTO);
    puts("PASS: read-only automatic devpts status SDK, validation and error propagation");
    return 0;
}
