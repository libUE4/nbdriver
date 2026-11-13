// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE 1
#include "nbdriver.h"
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#ifndef TIOCGPTN
#define TIOCGPTN _IOR('T',0x30,unsigned int)
#endif
struct child {pid_t pid;int command,reply;};
struct observation {int stat_error,access_error,open_error;};
static int failures;
static void check(int ok,const char *what)
{printf("%s: %s errno=%d\n",ok?"PASS":"FAIL",what,errno);if(!ok)++failures;}
static int transfer(int fd,void *data,size_t length,int writing)
{
    unsigned char *p=data;
    while(length){ssize_t n=writing?write(fd,p,length):read(fd,p,length);if(n<0&&errno==EINTR)continue;if(n<=0)return -1;p+=n;length-=(size_t)n;}
    return 0;
}
static int probe_main(char **argv)
{
    int input=atoi(argv[3]),output=atoi(argv[4]);char command;
    while(!transfer(input,&command,1,0)) {
        if(command=='E')return 0;
        if(command=='X'){
            char *args[]={"nbdriver.not-target","--probe",argv[2],argv[3],argv[4],NULL};
            execv("/proc/self/exe",args);return 3;
        }
        if(command!='P')return 4;
        struct observation o={0};struct stat st;
        if(stat(argv[2],&st))o.stat_error=errno;
        if(access(argv[2],F_OK))o.access_error=errno;
        int fd=open(argv[2],O_RDONLY|O_NOCTTY|O_NONBLOCK);
        if(fd<0)o.open_error=errno;else close(fd);
        if(transfer(output,&o,sizeof(o),1))return 5;
    }
    return 0;
}
static struct child spawn(const char *package,const char *path)
{
    int command[2],reply[2];struct child c={.pid=-1,.command=-1,.reply=-1};
    if(pipe(command)||pipe(reply))return c;
    c.pid=fork();
    if(c.pid==0){
        close(command[1]);close(reply[0]);char in[24],out[24];
        snprintf(in,sizeof(in),"%d",command[0]);snprintf(out,sizeof(out),"%d",reply[1]);
        char *args[]={(char *)package,"--probe",(char *)path,in,out,NULL};
        execv("/proc/self/exe",args);_exit(127);
    }
    close(command[0]);close(reply[1]);c.command=command[1];c.reply=reply[0];return c;
}
static int sample(struct child c,struct observation *o)
{
    char command='P';struct pollfd p={.fd=c.reply,.events=POLLIN};
    if(transfer(c.command,&command,1,1)||poll(&p,1,2000)<=0)return -1;
    return transfer(c.reply,o,sizeof(*o),0);
}
static void stop_child(struct child *c)
{
    if(c->pid>0){char command='E';int status;
        (void)transfer(c->command,&command,1,1);close(c->command);close(c->reply);
        if(waitpid(c->pid,&status,0)!=c->pid||!WIFEXITED(status)||WEXITSTATUS(status))++failures;
        c->pid=-1;
    }
}
static int wait_hidden(struct child c)
{
    struct observation o;
    for(int i=0;i<100;i++){
        if(sample(c,&o))return 0;
        if(o.stat_error==ENOENT&&o.access_error==ENOENT&&!o.open_error)return 1;
        usleep(100000);
    }
    printf("last stat=%d access=%d open=%d\n",o.stat_error,o.access_error,o.open_error);return 0;
}
int main(int argc,char **argv)
{
    if(argc==5&&!strcmp(argv[1],"--probe"))return probe_main(argv);
    if(argc==2&&!strcmp(argv[1],"--status")) {
        struct nbdriver_devpts_status s;int driver=nbdriver_open();
        if(driver<0){perror("nbdriver_open");return 1;}
        int result=nbdriver_get_devpts_status(driver,&s);
        if(result)perror("devpts status");
        else printf("STATUS active=%u targets=%u dfm=%d rmcn=%d generation=%llu permission=%llu getattr=%llu error=%d\n",
                    s.active,s.target_count,s.pids[0],s.pids[1],(unsigned long long)s.generation,
                    (unsigned long long)s.permission_hits,(unsigned long long)s.getattr_hits,s.last_error);
        nbdriver_close(driver);return result?1:0;
    }
    if(argc>2){fprintf(stderr,"usage: %s [gate-file]\n",argv[0]);return 2;}
    int master=-1,fd=-1;unsigned index=0;char path[64];
    struct child dfm={.pid=-1},rmcn={.pid=-1},other={.pid=-1};
    struct nbdriver_devpts_status state;struct observation o;
    setbuf(stdout,NULL);signal(SIGPIPE,SIG_IGN);
    master=posix_openpt(O_RDWR|O_NOCTTY|O_CLOEXEC);
    if(master<0||grantpt(master)||unlockpt(master)||ioctl(master,TIOCGPTN,&index)) {check(0,"create devpts fixture");goto done;}
    check(index<=NBDRIVER_DEVPTS_MAX_INDEX,"fixture falls within fixed automatic range");
    if(index>NBDRIVER_DEVPTS_MAX_INDEX)goto done;
    snprintf(path,sizeof(path),"/dev/pts/%u",index);
    dfm=spawn("com.tencent.tmgp.dfm",path);rmcn=spawn("com.tencent.rmcn",path);
    other=spawn("com.tencent.tmgp.dfm:helper",path);
    check(dfm.pid>0&&rmcn.pid>0&&other.pid>0,"create two exact packages and near-match fixture");
    if(dfm.pid<=0||rmcn.pid<=0||other.pid<=0)goto done;
    /* Optional gate proves both fixtures existed before the host loads the
     * new module. No FD or configuration ioctl is held while waiting. */
    if(argc==2){
        check(!sample(dfm,&o)&&!o.stat_error&&!o.access_error,"before reload DFM fixture remains visible");
        check(!sample(rmcn,&o)&&!o.stat_error&&!o.access_error,"before reload RMCN fixture remains visible");
        printf("READY parent=%d fixtures dfm=%d rmcn=%d other=%d\n",getpid(),dfm.pid,rmcn.pid,other.pid);
        int ready=0;
        for(int i=0;i<600;i++){if(!access(argv[1],F_OK)){ready=1;break;}usleep(100000);}
        if(!ready){check(0,"host load gate");goto done;}
    }
    fd=nbdriver_open();check(fd>=0,"connect after automatic module load");if(fd<0)goto done;
    check(!nbdriver_get_devpts_status(fd,&state)&&state.active&&state.max_index==31&&!state.last_error,
          "automatic hooks armed without configuration ioctl");
    check(wait_hidden(dfm),"DFM auto-bound: stat/access hidden and ordinary open succeeds");
    check(wait_hidden(rmcn),"RMCN auto-bound simultaneously");
    check(!sample(other,&o)&&!o.stat_error&&!o.access_error&&!o.open_error,"package suffix is not included");
    check(!stat(path,&(struct stat){0})&&!access(path,F_OK),"non-target parent keeps devpts visible");
    check(!nbdriver_get_devpts_status(fd,&state)&&state.target_count>=2&&state.pids[0]>0&&state.pids[1]>0&&state.getattr_hits&&state.permission_hits,
          "read-only status reports both packages and actual probe hits");
    printf("STATUS active=%u targets=%u dfm=%d rmcn=%d generation=%llu permission=%llu getattr=%llu error=%d\n",
           state.active,state.target_count,state.pids[0],state.pids[1],(unsigned long long)state.generation,
           (unsigned long long)state.permission_hits,(unsigned long long)state.getattr_hits,state.last_error);
    {unsigned char retired[32]={0};errno=0;
     check(ioctl(fd,_IOC(_IOC_READ|_IOC_WRITE,'N',0x1c,32),retired)==-1&&errno==ENOTTY,"retired command is rejected");}
    stop_child(&rmcn);rmcn=spawn("com.tencent.rmcn",path);
    check(rmcn.pid>0&&wait_hidden(rmcn),"RMCN restart is auto-bound to new PID");
    {char command='X';check(!transfer(dfm.command,&command,1,1),"exec target fixture into non-target");
     int visible=0;
     for(int i=0;i<100;i++){if(!sample(dfm,&o)&&!o.stat_error&&!o.access_error&&!o.open_error){visible=1;break;}usleep(10000);}
     check(visible,"exec into new mm stops old binding immediately");}
    stop_child(&rmcn);stop_child(&dfm);stop_child(&other);
    check(!nbdriver_get_devpts_status(fd,&state)&&state.active&&!state.last_error,"automatic hooks remain armed after fixtures exit");
done:
    stop_child(&dfm);stop_child(&rmcn);stop_child(&other);
    nbdriver_close(fd);if(master>=0)close(master);
    printf("RESULT failures=%d\n",failures);return failures?1:0;
}
