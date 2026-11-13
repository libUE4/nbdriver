/* Execute the actual kernel signal query body; no copied implementation. */
#include "nbdriver_abi.h"
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
typedef uint64_t u64;
#define __user
#define BUILD_BUG_ON(x) _Static_assert(!(x), "size")
#define READ_ONCE(x) (x)
#define _NSIG 64
#define UAPI_SA_FLAGS 0xffffffffUL
#define __ARCH_HAS_SA_RESTORER 1
#define ERR_PTR(x) ((void *)(intptr_t)(x))
#define IS_ERR(x) ((uintptr_t)(x)>(uintptr_t)-4096)
#define PTR_ERR(x) ((long)(intptr_t)(x))
/* Avoid libc's 128-byte userspace sigset_t (kernel native arm64 sigset is 8). */
#define sigset_t kernel_sigset
typedef struct kernel_sigset {u64 value;} kernel_sigset;
struct sigaction {void (*sa_handler)(int);unsigned long sa_flags;void (*sa_restorer)(void);struct kernel_sigset sa_mask;};
struct sighand_struct {pthread_mutex_t siglock;struct {struct sigaction sa;} action[64];};
struct task_struct {struct task_struct *group_leader;u64 start_boottime;struct kernel_sigset blocked;};
static struct task_struct leader={.start_boottime=42},thread={.group_leader=&leader,.start_boottime=43};
static struct sighand_struct sighand={.siglock=PTHREAD_MUTEX_INITIALIZER};
static bool query_lock_sighand=true,gone;
static unsigned refs;
static _Thread_local bool locked;
static struct task_struct *query_task(int pid,int tid,u64 start,u64 tstart){
 if(pid!=123||tid!=124)return ERR_PTR(-ESRCH);
 if((start&&start!=42)||(tstart&&tstart!=43))return ERR_PTR(-ESTALE);
 ++refs;return &thread;
}
static void query_put_task(struct task_struct *task){assert(task==&thread&&refs);--refs;}
static struct sighand_struct *query_signal_lock(struct task_struct *task,unsigned long *flags){
 assert(task==&thread);if(gone)return NULL;assert(!pthread_mutex_lock(&sighand.siglock));locked=true;*flags=0;return &sighand;
}
#define spin_unlock_irqrestore(m,f) do { (void)(f);assert(locked);locked=false;assert(!pthread_mutex_unlock(m)); }while(0)
static int copy_from_user(void *to,const void *from,size_t n){assert(!locked);if(!from)return 1;memcpy(to,from,n);return 0;}
static int copy_to_user(void *to,const void *from,size_t n){assert(!locked&&!refs);memcpy(to,from,n);return 0;}
static u64 ktime_get_ns(void){assert(locked);return 999;}
#include "signal_query.inc"
static long issue(struct nbdriver_thread_signal *r){long e=query_signal((unsigned long)r);assert(!refs&&!locked);return e;}
static atomic_bool stop;
static void *writer(void *unused){
 (void)unused;while(!atomic_load(&stop)){
  assert(!pthread_mutex_lock(&sighand.siglock));
  struct sigaction *a=&sighand.action[10].sa;u64 v=a->sa_flags==4?8:4;
  a->sa_flags=v;a->sa_handler=(void (*)(int))(uintptr_t)(v*256);a->sa_mask.value=v*16;thread.blocked.value=v*32;
  assert(!pthread_mutex_unlock(&sighand.siglock));
 }return NULL;
}
int main(void){
 struct nbdriver_thread_signal r={.pid=123,.tid=124,.signo=11,.expected_start_ns=42,.expected_thread_start_ns=43};
 assert(query_signal(0)==-EFAULT);
 r.signo=0;assert(issue(&r)==-EINVAL&&!r.sample_ns&&!r.handler);
 r.signo=65;assert(issue(&r)==-EINVAL);
 r.signo=11;r.reserved=1;assert(issue(&r)==-EINVAL);r.reserved=0;
 query_lock_sighand=false;assert(issue(&r)==-EOPNOTSUPP);query_lock_sighand=true;
 r.expected_thread_start_ns=44;assert(issue(&r)==-ESTALE&&!r.sample_ns);r.expected_thread_start_ns=43;
 r.expected_start_ns=44;assert(issue(&r)==-ESTALE);r.expected_start_ns=42;
 r.pid=125;assert(issue(&r)==-ESRCH);r.pid=123;
 gone=true;assert(issue(&r)==-ESRCH&&!r.handler&&!r.sample_ns);gone=false;
 struct sigaction *a=&sighand.action[10].sa;a->sa_flags=4;a->sa_handler=(void (*)(int))1024;a->sa_mask.value=64;thread.blocked.value=128;
 pthread_t t;assert(!pthread_create(&t,NULL,writer,NULL));
 for(unsigned i=0;i<20000;i++){
  assert(issue(&r)==0&&r.sample_ns==999&&r.expected_start_ns==42&&r.expected_thread_start_ns==43);
  assert(r.handler==r.flags*256&&r.action_mask==r.flags*16&&r.blocked_mask==r.flags*32);
 }
 atomic_store(&stop,true);assert(!pthread_join(t,NULL));
 puts("PASS actual signal query: coherent concurrent snapshots, identity, bounds, absent backend, exit, cleanup, usercopy outside lock");
}
