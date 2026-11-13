#define _GNU_SOURCE
#include "nbdriver.h"
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
static atomic_bool stop;
static atomic_int child_tid;
static void handler(int n,siginfo_t *i,void *c){(void)n;(void)i;(void)c;}
static void handler2(int n,siginfo_t *i,void *c){(void)n;(void)i;(void)c;}
static void *worker(void *unused){
 (void)unused;sigset_t b;sigemptyset(&b);sigaddset(&b,SIGUSR2);assert(!pthread_sigmask(SIG_BLOCK,&b,NULL));
 atomic_store(&child_tid,(int)syscall(SYS_gettid));while(!atomic_load(&stop))sched_yield();return NULL;
}
static void *actions(void *unused){
 (void)unused;unsigned n=0;while(!atomic_load(&stop)){
  struct sigaction a;memset(&a,0,sizeof(a));a.sa_sigaction=(n++&1)?handler:handler2;a.sa_flags=SA_SIGINFO;sigemptyset(&a.sa_mask);sigaddset(&a.sa_mask,a.sa_sigaction==handler?SIGUSR1:SIGUSR2);
  assert(!sigaction(SIGUSR1,&a,NULL));
 }return NULL;
}
int main(void){
 setbuf(stdout,NULL);int fd=nbdriver_open();if(fd<0){perror("nbdriver_open");return 1;}
 struct nbdriver_abi_info abi;assert(!nbdriver_get_abi(fd,&abi));printf("abi=%u features=0x%x\n",abi.version,abi.features);
 if((abi.features&(NBDRIVER_FEATURE_THREAD_SIGNAL|NBDRIVER_FEATURE_VMA))!=(NBDRIVER_FEATURE_THREAD_SIGNAL|NBDRIVER_FEATURE_VMA)){
  struct nbdriver_thread_signal s;int rc=nbdriver_get_thread_signal(fd,getpid(),getpid(),SIGUSR1,0,0,&s);printf("extensions_absent signal_query=%d errno=%d\n",rc,errno);nbdriver_close(fd);return 77;
 }
 int pid=getpid();struct nbdriver_process_identity id={.pid=pid};assert(!ioctl(fd,NBDRIVER_IOC_PROCESS_ID,&id));
 struct sigaction a;memset(&a,0,sizeof(a));a.sa_sigaction=handler;a.sa_flags=SA_SIGINFO|SA_RESTART;sigemptyset(&a.sa_mask);sigaddset(&a.sa_mask,SIGUSR1);assert(!sigaction(SIGUSR1,&a,NULL));
 sigset_t block;sigemptyset(&block);sigaddset(&block,SIGUSR2);assert(!pthread_sigmask(SIG_BLOCK,&block,NULL));
 struct nbdriver_thread_signal s;assert(!nbdriver_get_thread_signal(fd,pid,pid,SIGUSR1,id.start_ns,id.start_ns,&s));
 assert(s.handler==(uintptr_t)handler&&(s.flags&SA_SIGINFO)&&s.action_mask==(1ULL<<(SIGUSR1-1))&&(s.blocked_mask&(1ULL<<(SIGUSR2-1)))&&s.sample_ns);
 struct sigaction observed;assert(!sigaction(SIGUSR1,NULL,&observed));assert((unsigned long)observed.sa_flags==s.flags);
 printf("PASS main action + mask + blocked + sample match normal self query\n");
 assert(nbdriver_get_thread_signal(fd,pid,pid,SIGUSR1,id.start_ns+1,id.start_ns,&s)<0&&errno==ESTALE&&!s.handler&&!s.sample_ns);
 assert(nbdriver_get_thread_signal(fd,pid,pid,SIGUSR1,id.start_ns,id.start_ns+1,&s)<0&&errno==ESTALE&&!s.handler);
 assert(nbdriver_get_thread_signal(fd,pid,pid,0,id.start_ns,id.start_ns,&s)<0&&errno==EINVAL);
 memset(&s,0,sizeof(s));s.pid=pid;s.tid=pid;s.signo=65;assert(ioctl(fd,NBDRIVER_IOC_THREAD_SIGNAL,&s)<0&&errno==EINVAL);
 s.signo=11;s.reserved=1;assert(ioctl(fd,NBDRIVER_IOC_THREAD_SIGNAL,&s)<0&&errno==EINVAL);
 assert(!pthread_sigmask(SIG_UNBLOCK,&block,NULL));pthread_t t,writer;assert(!pthread_create(&t,NULL,worker,NULL));while(!atomic_load(&child_tid))sched_yield();
 int tid=atomic_load(&child_tid);assert(!nbdriver_get_thread_signal(fd,pid,tid,SIGUSR1,id.start_ns,0,&s));assert(s.blocked_mask&(1ULL<<(SIGUSR2-1)));
 assert(!nbdriver_get_thread_signal(fd,pid,pid,SIGUSR1,id.start_ns,id.start_ns,&s));assert(!(s.blocked_mask&(1ULL<<(SIGUSR2-1))));
 assert(!pthread_create(&writer,NULL,actions,NULL));
 for(unsigned i=0;i<10000;++i){assert(!nbdriver_get_thread_signal(fd,pid,tid,SIGUSR1,id.start_ns,0,&s));assert(s.handler==(uintptr_t)handler||s.handler==(uintptr_t)handler2);assert(s.action_mask==(1ULL<<((s.handler==(uintptr_t)handler?SIGUSR1:SIGUSR2)-1)));}
 atomic_store(&stop,true);assert(!pthread_join(t,NULL));assert(!pthread_join(writer,NULL));
 assert(nbdriver_get_thread_signal(fd,pid,tid,SIGUSR1,id.start_ns,0,&s)<0&&errno==ESRCH&&!s.handler);
 printf("PASS thread-specific blocked masks, 10000 concurrent action snapshots, stale identity, exited thread, invalid inputs\n");
 size_t bytes=(size_t)sysconf(_SC_PAGESIZE);void *p=mmap(NULL,bytes,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(p!=MAP_FAILED);
 struct nbdriver_mapping_request v;assert(!nbdriver_query_vma(fd,pid,(uintptr_t)p,id.start_ns,&v));assert(!v.physical&&!v.page_size&&!v.flags&&v.vma_start<=(uintptr_t)p&&v.vma_end>=(uintptr_t)p+bytes);
 assert(nbdriver_query_mapping(fd,pid,(uintptr_t)p,id.start_ns,&v)<0&&errno==ENODATA);
 assert(!mprotect(p,bytes,PROT_READ|PROT_WRITE));*(volatile char*)p=1;
 assert(!nbdriver_query_vma(fd,pid,(uintptr_t)p,id.start_ns,&v));assert((v.flags&3)==3&&!v.physical&&!v.page_size);
 assert(!mprotect(p,bytes,PROT_NONE));assert(!nbdriver_query_vma(fd,pid,(uintptr_t)p,id.start_ns,&v)&&!v.flags);
 assert(!munmap(p,bytes));assert(nbdriver_query_vma(fd,pid,(uintptr_t)p,id.start_ns,&v)<0&&errno==ENOENT&&!v.vma_start);
 printf("PASS cold/resident PROT_NONE, RW permissions, unmapped address; target game untouched\n");nbdriver_close(fd);return 0;
}
