// SPDX-License-Identifier: GPL-2.0-only
#include "nbdriver.h"
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#ifndef TIOCGPTN
#define TIOCGPTN _IOR('T', 0x30, unsigned int)
#endif
static int failures;
static void check(int ok, const char *name)
{ printf("%s: %s errno=%d(%s)\n",ok?"PASS":"FAIL",name,errno,strerror(errno));if(!ok)++failures; }
static int child_stat(const char *path)
{
    struct stat st;
    errno=0;
    if(stat(path,&st)==0)return 0;
    return errno;
}
static int run_child(const char *path)
{
    pid_t pid=fork();int status;
    if(pid<0)return -errno;
    if(!pid){int result=child_stat(path);_exit(result==0?0:(result==ENOENT?1:2));}
    if(waitpid(pid,&status,0)!=pid)return -errno;
    if(!WIFEXITED(status))return -EINTR;
    return WEXITSTATUS(status);
}
int main(void)
{
    int fd=-1,master=-1,slave=-1,result;
    unsigned int index=0;char path[64];
    struct nbdriver_abi_info abi={0};
    struct nbdriver_devpts_filter_request state={0};
    fd=nbdriver_open();check(fd>=0,"connect");if(fd<0)return 1;
    check(nbdriver_get_abi(fd,&abi)==0&&abi.version==NBDRIVER_ABI_VERSION,"ABI");
    check((abi.features&NBDRIVER_FEATURE_DEVPTS)!=0,"DEVPTS capability");
    master=posix_openpt(O_RDWR|O_NOCTTY|O_CLOEXEC);
    if(master>=0)grantpt(master),unlockpt(master),slave=posix_openpt(O_RDWR|O_NOCTTY|O_CLOEXEC);
    check(master>=0&&slave>=0,"create devpts fixture");if(master<0||slave<0)goto done;
    if(ioctl(slave,TIOCGPTN,&index)<0){check(0,"query pts index");goto done;}
    snprintf(path,sizeof(path),"/dev/pts/%u",index);
    result=nbdriver_configure_devpts_filter(fd,getpid(),NBDRIVER_DEVPTS_FILTER_ENABLE,index,&state);
    check(result==0&&state.enabled&&state.pid==getpid()&&state.max_index==index,"enable current PID");
    check(run_child(path)==0,"non-target process keeps pts visible");
    /* Make the child wait so the parent can bind the exact child PID before
     * it performs stat. This tests PID-scoped matching rather than merely
     * testing that a filter exists. */
    int gate[2];
    check(pipe(gate)==0,"create target gate");
    pid_t target=fork();
    if(target==0){char signal_byte;close(gate[1]);if(read(gate[0],&signal_byte,1)!=1)_exit(3);close(gate[0]);int r=child_stat(path);_exit(r==ENOENT?0:(r==0?1:2));}
    if(target>0){
        close(gate[0]);
        result=nbdriver_configure_devpts_filter(fd,target,NBDRIVER_DEVPTS_FILTER_ENABLE,index,&state);
        check(result==0&&state.enabled&&state.pid==target,"enable exact child PID");
        check(write(gate[1],"x",1)==1,"release target gate");close(gate[1]);
        waitpid(target,&result,0);check(WIFEXITED(result)&&WEXITSTATUS(result)==0,"target process sees pts hidden");
    }
    result=nbdriver_query_devpts_filter(fd,&state);
    printf("INFO query result=%d enabled=%u pid=%d max=%u permission=%llu getattr=%llu\n",result,state.enabled,state.pid,state.max_index,(unsigned long long)state.permission_hits,(unsigned long long)state.getattr_hits);
    check(result==0&&state.enabled&&state.pid==target&&state.permission_hits+state.getattr_hits>0,"query hit counters");
    result=nbdriver_disable_devpts_filter(fd,&state);
    printf("INFO disable result=%d enabled=%u pid=%d max=%u permission=%llu getattr=%llu errno=%d\n",result,state.enabled,state.pid,state.max_index,(unsigned long long)state.permission_hits,(unsigned long long)state.getattr_hits,errno);
    check(result==0&&!state.enabled&&state.pid==0&&state.permission_hits==0&&state.getattr_hits==0,"disable removes hooks and clears state");
    check(run_child(path)==0,"after disable pts visible");
    result=nbdriver_configure_devpts_filter(fd,getpid(),NBDRIVER_DEVPTS_FILTER_ENABLE,index+1,&state);
    check(result==0&&state.enabled&&state.max_index==index+1,"re-enable higher range");
    result=nbdriver_configure_devpts_filter(fd,getpid(),NBDRIVER_DEVPTS_FILTER_ENABLE,NBDRIVER_DEVPTS_FILTER_MAX_INDEX+1,&state);
    check(result<0&&errno==EINVAL,"reject max index overflow");
    result=nbdriver_configure_devpts_filter(fd,999999,NBDRIVER_DEVPTS_FILTER_ENABLE,index,&state);
    check(result<0&&errno==ESRCH,"reject dead PID");
    result=nbdriver_disable_devpts_filter(fd,&state);check(result==0&&!state.enabled,"final disable");
done:
    if(slave>=0) close(slave);
    if(master>=0) close(master);
    nbdriver_close(fd);
    printf("RESULT failures=%d\n",failures);return failures?1:0;
}
