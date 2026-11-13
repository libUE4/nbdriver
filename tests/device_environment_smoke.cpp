// Self-owned always-running thread: compare live TLS and PACGA to driver data.
#include "nbdriver.h"
#include "jiemi/qarma_pac.h"
#include <atomic>
#include <cstdio>
#include <pthread.h>
#include <sched.h>
#include <time.h>
static std::atomic<bool> running{true},ready{false};
static uint64_t expectedTls,expectedPac;
static constexpr uint64_t data=0x123456789abcdef0ULL,modifier=0xfedcba9876543210ULL;
static void *worker(void*)
{
 cpu_set_t cpus;CPU_ZERO(&cpus);CPU_SET(6,&cpus);
 if(sched_setaffinity(0,sizeof(cpus),&cpus))return nullptr;
 pthread_setname_np(pthread_self(),"nbd-live-env");
 expectedTls=reinterpret_cast<uintptr_t>(__builtin_thread_pointer());
 asm volatile(".arch_extension pauth\n pacga %0,%1,%2":"=r"(expectedPac):"r"(data),"r"(modifier));
 ready.store(true,std::memory_order_release);
 while(running.load(std::memory_order_relaxed))asm volatile("yield");
 return nullptr;
}
int main(int argc,char**argv)
{
 bool baseline=argc==2&&!strcmp(argv[1],"--baseline");
 cpu_set_t cpus;CPU_ZERO(&cpus);CPU_SET(4,&cpus);
 if(sched_setaffinity(0,sizeof(cpus),&cpus))return 2;
 int fd=nbdriver_open();if(fd<0)return 3;
 nbdriver_u64 start=0;if(nbdriver_get_process_start_time(fd,getpid(),&start))return 4;
 pthread_t thread;if(pthread_create(&thread,nullptr,worker,nullptr))return 5;
 for(unsigned n=0;n<1000&&!ready.load(std::memory_order_acquire);++n)usleep(1000);
 if(!ready.load(std::memory_order_acquire)){running=false;pthread_join(thread,nullptr);return 6;}
 nbdriver_thread_request target{};target.pid=getpid();target.expected_start_ns=start;strcpy(target.comm,"nbd-live-env");
 if(nbdriver_ioctl(fd,NBDRIVER_IOC_THREAD_ID,&target)){running=false;pthread_join(thread,nullptr);return 7;}
 unsigned ok=0,retry=0,wrong=0,failed=0;
 for(unsigned n=0;n<2000;++n){
  nbdriver_thread_environment sample{};sample.pid=getpid();sample.tid=target.tid;sample.expected_start_ns=start;sample.expected_thread_start_ns=target.thread_start_ns;
  int ret=nbdriver_ioctl(fd,NBDRIVER_IOC_THREAD_ENV,&sample);
  if(ret<0){if(errno==EAGAIN)++retry;else ++failed;continue;}
  uint64_t pac=CoordDecrypt::ComputeQarma5Pac(data,modifier,{sample.apga_lo,sample.apga_hi})&0xffffffff00000000ULL;
  if(sample.valid!=(NBDRIVER_THREAD_TLS_VALID|NBDRIVER_THREAD_APGA_VALID)||sample.tls!=expectedTls||pac!=expectedPac||sample.expected_start_ns!=start||sample.expected_thread_start_ns!=target.thread_start_ns)++wrong;else ++ok;
 }
 running=false;pthread_join(thread,nullptr);
 nbdriver_thread_environment dead{};dead.pid=getpid();dead.tid=target.tid;dead.expected_start_ns=start;dead.expected_thread_start_ns=target.thread_start_ns;
 bool rejectsDead=nbdriver_ioctl(fd,NBDRIVER_IOC_THREAD_ENV,&dead)<0&&errno==ESRCH;
 nbdriver_close(fd);
 printf("environment_smoke mode=%s samples=2000 valid=%u transient=%u wrong=%u other_failures=%u exited_thread_rejected=%d\n",baseline?"baseline":"candidate",ok,retry,wrong,failed,rejectsDead);
 return !wrong&&!failed&&rejectsDead&&(baseline?retry>0:ok==2000)?0:8;
}
