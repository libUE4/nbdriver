// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE 1
#include "nbdriver.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int ready, stop;
static int worker_tid;
static void *worker(void *unused)
{
    (void)unused;
    prctl(PR_SET_NAME,"nbd-query-test",0,0,0);
    worker_tid=(int)syscall(SYS_gettid);
#ifdef __aarch64__
    { unsigned long flags=0x08000000UL; __asm__ volatile("msr fpsr, %0" :: "r"(flags)); }
#endif
    pthread_mutex_lock(&lock);
    ready=1;
    pthread_cond_signal(&changed);
    while(!stop) pthread_cond_wait(&changed,&lock);
    pthread_mutex_unlock(&lock);
    return NULL;
}
#define CHECK(condition,label) do { if(!(condition)) { \
    fprintf(stderr,"FAIL: %s errno=%d\n",label,errno); failed=1; goto done; \
    } puts("PASS: " label); } while(0)
int main(void)
{
    int fd=-1, failed=0, started=0;
    pthread_t thread;
    struct nbdriver_abi_info info;
    struct nbdriver_thread_environment environment;
    struct nbdriver_thread_fpstate fp;
    struct nbdriver_readv_entry entries[3];
    struct nbdriver_readv_request batch;
    struct nbdriver_process_identity stale;
    nbdriver_u64 start=0, base=0;
    unsigned char *source=NULL,*output=NULL;
    unsigned char small[64], magic[4];
    char unique[64];
    const char *identity=getenv("NBDRIVER_QUERY_FIXTURE");
    if(!identity) {
        char *args[2];
        snprintf(unique,sizeof(unique),"nbdriver.query.%d",getpid());
        if(setenv("NBDRIVER_QUERY_FIXTURE",unique,1)) return 2;
        args[0]=unique; args[1]=NULL;
        execv("/proc/self/exe",args);
        perror("exec fixture"); return 2;
    }
    fd=nbdriver_open();
    CHECK(fd>=0,"connect");
    CHECK(nbdriver_get_abi(fd,&info)==0,"get ABI");
    const unsigned required=NBDRIVER_FEATURE_FIND_PROCESS|NBDRIVER_FEATURE_MODULE_BASE|
        NBDRIVER_FEATURE_PROCESS_ID|NBDRIVER_FEATURE_THREAD_ID|NBDRIVER_FEATURE_THREAD_ENV|
        NBDRIVER_FEATURE_READV;
    CHECK((info.features&required)==required,"required feature bits");
    CHECK(nbdriver_find_process(fd,identity)==getpid(),"exact argv[0] PID lookup");
    CHECK(nbdriver_get_process_start_time(fd,getpid(),&start)==0 && start,"process identity");
    memset(&stale,0,sizeof(stale)); stale.pid=getpid(); stale.start_ns=start^1U;
    CHECK(nbdriver_ioctl(fd,NBDRIVER_IOC_PROCESS_ID,&stale)==-1 && errno==ESTALE,"stale identity rejected");
    CHECK(nbdriver_get_module_base(fd,getpid(),"libc.so",&base)==0 && base,"libc.so base");
    CHECK(nbdriver_read_process_memory(fd,getpid(),base,magic,sizeof(magic))==0 &&
          !memcmp(magic,"\177ELF",4),"module base contains ELF header");
    CHECK(pthread_create(&thread,NULL,worker,NULL)==0,"create named fixture thread");
    started=1;
    pthread_mutex_lock(&lock);
    while(!ready) pthread_cond_wait(&changed,&lock);
    pthread_mutex_unlock(&lock);
    CHECK(nbdriver_get_thread_id(fd,getpid(),"nbd-query-test")==worker_tid,"named TID lookup");
    CHECK(nbdriver_get_thread_environment(fd,getpid(),worker_tid,&environment)==0 &&
          (environment.valid&NBDRIVER_THREAD_TLS_VALID) && environment.tls,"thread TLS environment");
    CHECK(environment.expected_start_ns==start && environment.expected_thread_start_ns,
          "thread/process identity in environment");
    if(info.features&NBDRIVER_FEATURE_THREAD_FP) {
        int result=-1;
        for(int attempt=0;attempt<100;attempt++) {
            result=nbdriver_get_thread_fpstate(fd,getpid(),worker_tid,&fp);
            if(result==0 || errno!=EAGAIN) break;
            usleep(1000);
        }
        CHECK(result==0 && fp.valid && fp.sample_ns && fp.tls==environment.tls,
              "stable thread FP sample");
#ifdef __aarch64__
        CHECK((fp.fpsr&0x08000000U)!=0,"worker saved FPSR QC flag");
#endif
        printf("INFO: fpsr=0x%x fpcr=0x%x APGA_valid=%u\n",fp.fpsr,fp.fpcr,
               !!(environment.valid&NBDRIVER_THREAD_APGA_VALID));
    } else puts("SKIP: task_call_func/FP sampling unavailable on this kernel");
    source=malloc(NBDRIVER_READV_ITEM_MAX); output=malloc(NBDRIVER_READV_ITEM_MAX);
    CHECK(source && output,"allocate batch fixture");
    memset(source,0x5a,NBDRIVER_READV_ITEM_MAX); memset(output,0,NBDRIVER_READV_ITEM_MAX);
    printf("INFO: malloc source=%p output=%p\n",(void *)source,(void *)output);
    CHECK(nbdriver_read_process_memory(fd,getpid(),(uintptr_t)source,small,sizeof(small))==0 &&
          !memcmp(source,small,sizeof(small)),"malloc pointer single read");
    CHECK(nbdriver_write_process_memory(fd,getpid(),(uintptr_t)source,small,sizeof(small))==0,
          "malloc pointer single write");
    memset(small,0,sizeof(small)); memset(entries,0,sizeof(entries)); memset(&batch,0,sizeof(batch));
    entries[0].address=(uintptr_t)source; entries[0].buffer=(uintptr_t)output;
    entries[0].size=NBDRIVER_READV_ITEM_MAX;
    entries[1].address=1; entries[1].buffer=(uintptr_t)small; entries[1].size=sizeof(small);
    entries[2].address=(uintptr_t)source; entries[2].buffer=(uintptr_t)small; entries[2].size=sizeof(small);
    batch.pid=getpid(); batch.count=3; batch.entries=(uintptr_t)entries; batch.expected_start_ns=start;
    errno=0;
    int batch_result=nbdriver_readv_process(fd,&batch);
    if(batch_result!=0 || batch.completed!=2) {
        fprintf(stderr,"BATCH ret=%d errno=%d completed=%u error=%d start=%llu expected=%llu source=%p output=%p\n",
            batch_result,errno,batch.completed,batch.error,(unsigned long long)start,
            (unsigned long long)batch.expected_start_ns,(void *)source,(void *)output);
        for(unsigned i=0;i<3;++i)
            fprintf(stderr,"ENTRY %u copied=%u error=%d address=%llx failed=%llx\n",i,
                entries[i].copied,entries[i].error,(unsigned long long)entries[i].address,
                (unsigned long long)entries[i].failed_address);
    }
    CHECK(batch_result==0 && batch.completed==2,"mixed batch dispatched");
    CHECK(!entries[0].error && entries[0].copied==NBDRIVER_READV_ITEM_MAX &&
          !memcmp(source,output,NBDRIVER_READV_ITEM_MAX),"512 KiB batch content");
    CHECK(entries[1].error<0 && !entries[1].copied && entries[1].failed_address==1,
          "invalid entry reports failure");
    CHECK(!entries[2].error && !memcmp(source,small,sizeof(small)),"batch continues after failed entry");
 done:
    if(started) {
        pthread_mutex_lock(&lock); stop=1; pthread_cond_signal(&changed); pthread_mutex_unlock(&lock);
        pthread_join(thread,NULL);
    }
    free(output); free(source); nbdriver_close(fd);
    return failed;
}
