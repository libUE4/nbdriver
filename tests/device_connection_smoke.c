// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE 1
#include "nbdriver.h"
#include <dirent.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/wait.h>

static int failures;
static void check(int ok,const char *what)
{
    printf("%s: %s (errno=%d)\n",ok?"PASS":"FAIL",what,errno);
    if(!ok)++failures;
}
static int fd_count(void)
{
    DIR *dir=opendir("/proc/self/fd");
    struct dirent *entry;int count=0;
    if(!dir)return -1;
    while((entry=readdir(dir)))if(entry->d_name[0]!='.')++count;
    closedir(dir);return count;
}
int main(int argc,char **argv)
{
    struct rlimit saved,limited;
    int fd,status,before,after;pid_t child;
    setbuf(stdout,NULL);
    fd=nbdriver_open();check(fd>=0,"root connects and ABI handshake succeeds");
    if(fd<0)return 1;
    if(argc==2&&!strcmp(argv[1],"--hold")) {
        puts("READY: driver FD held");sleep(4);nbdriver_close(fd);return 0;
    }
    nbdriver_close(fd);
    before=fd_count();
    for(int i=0;i<16;i++)
        (void)syscall(SYS_reboot,NBDRIVER_INSTALL_MAGIC1,NBDRIVER_INSTALL_MAGIC2,0,(void *)(uintptr_t)1);
    after=fd_count();
    check(before>=0&&after==before,"invalid output pointer leaves no installed/leaked FD");
    if(getrlimit(RLIMIT_NOFILE,&saved)!=0) {check(0,"read FD limit");return 1;}
    check(1,"read FD limit");
    limited=saved;limited.rlim_cur=0;
    if(setrlimit(RLIMIT_NOFILE,&limited)==0) {
        errno=0;fd=nbdriver_open();status=errno;
        if(fd>=0)nbdriver_close(fd);
        check(setrlimit(RLIMIT_NOFILE,&saved)==0,"restore FD limit");
        errno=status;check(fd==-1&&status==EMFILE,"SDK preserves kernel EMFILE");
    }else check(0,"lower own FD limit");
    child=fork();
    if(child==0) {
        if(setgid(2000)||setuid(2000))_exit(2);
        errno=0;fd=nbdriver_open();
        if(fd>=0){nbdriver_close(fd);_exit(3);}
        _exit(errno==EPERM?0:4);
    }
    if(child>0)check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status),
                     "non-root connection returns EPERM");
    else check(0,"fork non-root fixture");
    printf("RESULT failures=%d\n",failures);return failures?1:0;
}
