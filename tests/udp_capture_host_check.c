/* SPDX-License-Identifier: GPL-2.0-only */
/* Executes the production capture backend with bounded kernel adapters.
 * Scheduler/probe/IP-stack correctness still requires an actual kernel test. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <sys/epoll.h>
#include "nbdriver_abi.h"
typedef unsigned long long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef u32 __poll_t;
typedef int poll_table;
typedef unsigned int kuid_t;
#define __percpu
#define __user
#define GFP_KERNEL 0
#define ETH_P_IP 0x0800
#define PF_EXITING 1U
#define PIDTYPE_TGID 1
#define CAP_SYS_PTRACE 19
#define READ_ONCE(x) __atomic_load_n(&(x),__ATOMIC_ACQUIRE)
#define WRITE_ONCE(x,v) __atomic_store_n(&(x),(v),__ATOMIC_RELEASE)
#define smp_load_acquire(p) __atomic_load_n((p),__ATOMIC_ACQUIRE)
#define smp_store_release(p,v) __atomic_store_n((p),(v),__ATOMIC_RELEASE)
#define BUILD_BUG_ON(c) _Static_assert(!(c),#c)
#define NOKPROBE_SYMBOL(fn)
#define min_t(type,a,b) ((type)(a)<(type)(b)?(type)(a):(type)(b))
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define PTR_ERR(p) ((long)(intptr_t)(p))
#define IS_ERR(p) ((uintptr_t)(p)>(uintptr_t)-4096)
#define IS_ERR_OR_NULL(p) (!(p)||IS_ERR(p))
#define nbd_info(...) ((void)0)
#define nbd_warn(...) ((void)0)
#define __kuid_val(x) (x)
#define uid_eq(a,b) ((a)==(b))
struct file {int id;};
struct mm_struct {int users,refs;};
struct cred {kuid_t uid;};
struct task_struct {u64 start_boottime;unsigned long flags;struct mm_struct *mm;bool alive;};
struct pid {int value;};
static struct mm_struct target_mm={1,1},other_mm={1,1};
static struct task_struct task={42,0,&target_mm,true};
static struct cred creds={23456};
static struct pid target_pid={123};
static int task_refs,pid_refs,cred_refs;
static int caller_uid,allow_cap,deny_mm;
static bool quiesced;
static struct pid *find_get_pid(int pid){if(pid!=123)return NULL;++pid_refs;return &target_pid;}
static struct task_struct *get_pid_task(struct pid *p,int type){assert(p==&target_pid&&type==PIDTYPE_TGID);++task_refs;return &task;}
static void put_pid(struct pid *p){assert(p==&target_pid&&pid_refs>0);--pid_refs;}
static void put_task_struct(struct task_struct *t){assert(t==&task&&task_refs>0);--task_refs;}
static bool pid_alive(struct task_struct *t){return READ_ONCE(t->alive);}
static struct mm_struct *nbdriver_access_mm(struct task_struct *t){assert(t==&task);if(deny_mm)return ERR_PTR(-EACCES);++t->mm->users;return t->mm;}
static void mmgrab(struct mm_struct *m){++m->refs;}
static void mmput(struct mm_struct *m){assert(m->users>0);--m->users;}
static void mmdrop(struct mm_struct *m){assert(m->refs>1);--m->refs;}
static const struct cred *get_task_cred(struct task_struct *t){assert(t==&task);++cred_refs;return &creds;}
static void put_cred(const struct cred *c){assert(c==&creds&&cred_refs>0);--cred_refs;}
static bool nbdriver_sf_read_caller(void){return caller_uid==0||caller_uid==1000;}
static bool capable(int cap){assert(cap==CAP_SYS_PTRACE);return allow_cap;}
static bool nbdriver_runtime_quiesced(void){return quiesced;}
static u64 ktime_get_ns(void){struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));return (u64)t.tv_sec*1000000000ULL+t.tv_nsec;}
static _Thread_local int spin_depth,cpu_id;
typedef pthread_mutex_t spinlock_t;
#define DEFINE_MUTEX(name) pthread_mutex_t name=PTHREAD_MUTEX_INITIALIZER
#define spin_lock_init(lock) assert(!pthread_mutex_init((lock),NULL))
#define mutex_lock(lock) assert(!pthread_mutex_lock(lock))
#define mutex_unlock(lock) assert(!pthread_mutex_unlock(lock))
#define spin_lock_irqsave(lock,flags) do{(flags)=0;assert(!spin_depth);assert(!pthread_mutex_lock(lock));++spin_depth;}while(0)
#define spin_unlock_irqrestore(lock,flags) do{(void)(flags);assert(spin_depth==1);--spin_depth;assert(!pthread_mutex_unlock(lock));}while(0)
#define CPUS 2
#define num_possible_cpus() CPUS
#define for_each_possible_cpu(cpu) for((cpu)=0;(cpu)<CPUS;++(cpu))
#define per_cpu_ptr(p,cpu) (&(p)[cpu])
#define this_cpu_ptr(p) (&(p)[cpu_id])
static int allocations,alloc_budget=-1;
static void *allocate(size_t n,size_t size){assert(!spin_depth);if(alloc_budget==0)return NULL;if(alloc_budget>0)--alloc_budget;void *p=calloc(n,size);if(p)++allocations;return p;}
static void dispose(void *p){assert(!spin_depth);if(p){assert(allocations>0);--allocations;free(p);}}
#define kzalloc(size,flags) allocate(1,(size))
#define kvcalloc(n,size,flags) allocate((n),(size))
#define alloc_percpu(type) ((type *)allocate(CPUS,sizeof(type)))
#define free_percpu(p) dispose(p)
#define kfree(p) dispose(p)
#define kvfree(p) dispose(p)
static const void *fail_copy_from,*fail_copy_to;
static bool partial_copy;
static int copy_from_user(void *to,const void *from,size_t n){assert(!spin_depth);if(!from||from==fail_copy_from)return 1;memcpy(to,from,n);return 0;}
static int copy_to_user(void *to,const void *from,size_t n){assert(!spin_depth);if(!to||to==fail_copy_to){if(to&&partial_copy)memcpy(to,from,n/2);return 1;}memcpy(to,from,n);return 0;}
static atomic_uint notifications;
#define DECLARE_WAIT_QUEUE_HEAD(name) int name
static void wake_up_interruptible_poll(int *q,unsigned events){(void)q;(void)events;atomic_fetch_add(&notifications,1);}
static void poll_wait(struct file *f,int *q,poll_table *p){(void)f;(void)q;(void)p;}
struct pt_regs {unsigned long regs[31];};
struct kprobe {const char *symbol_name;int (*pre_handler)(struct kprobe *,struct pt_regs *);unsigned long nmissed;};
static pthread_rwlock_t probe_gate=PTHREAD_RWLOCK_INITIALIZER;
static struct kprobe *registered;
static int registration_error,registrations,unregistrations;
static int register_kprobe(struct kprobe *p){assert(!spin_depth&&!strcmp(p->symbol_name,"udp_queue_rcv_one_skb"));if(registration_error)return registration_error;assert(!pthread_rwlock_wrlock(&probe_gate));assert(!registered);registered=p;++registrations;assert(!pthread_rwlock_unlock(&probe_gate));return 0;}
static void unregister_kprobe(struct kprobe *p){assert(!spin_depth);assert(!pthread_rwlock_wrlock(&probe_gate));assert(registered==p);registered=NULL;++unregistrations;assert(!pthread_rwlock_unlock(&probe_gate));}
struct sock {kuid_t sk_uid;};
struct skb_shared_info {unsigned short gso_size;};
struct sk_buff {unsigned short protocol;unsigned int len;int transport;bool gro,copy_error;struct iphdr ip;struct udphdr udp;struct skb_shared_info shinfo;unsigned char bytes[4104];};
#define ip_hdr(skb) (&(skb)->ip)
#define udp_hdr(skb) (&(skb)->udp)
#define skb_transport_offset(skb) ((skb)->transport)
#define skb_is_gso(skb) ((skb)->gro)
#define skb_shinfo(skb) (&(skb)->shinfo)
static int skb_copy_bits(struct sk_buff *skb,int offset,void *out,int n){assert(!spin_depth);if(skb->copy_error||offset<0||(unsigned int)(offset+n)>skb->len)return -EFAULT;memcpy(out,skb->bytes+offset,n);return 0;}
#include "udp_production.inc"
static struct file owner={1},other={2};
static void no_resources(void){assert(!udp_active&&!registered&&!allocations&&!task_refs&&!pid_refs&&!cred_refs&&target_mm.users==1&&target_mm.refs==1);}
static long start(struct file *f,struct nbdriver_udp_start_request *r){return nbdriver_udp_ioctl(f,NBDRIVER_IOC_UDP_START,(unsigned long)r);}
static long stop(struct file *f){return nbdriver_udp_ioctl(f,NBDRIVER_IOC_UDP_STOP,0);}
static struct nbdriver_udp_start_request new_start(void){struct nbdriver_udp_start_request r={.pid=123,.expected_start_ns=42};assert(!start(&owner,&r)&&r.generation&&r.max_payload==2048);return r;}
static struct sk_buff packet(unsigned length,unsigned tag){struct sk_buff s={.protocol=htons(ETH_P_IP),.len=8+(int)length};assert(length<=4096);s.ip.version=4;s.ip.protocol=IPPROTO_UDP;s.ip.saddr=htonl(0x7f000001);s.ip.daddr=htonl(0x7f000001);s.udp.len=htons(8+length);s.udp.source=htons(32123);s.udp.dest=htons(32124);for(unsigned i=0;i<length;++i)s.bytes[8+i]=(unsigned char)(tag+i);return s;}
static void deliver(struct sk_buff *s,kuid_t uid){struct sock sk={uid};struct pt_regs r={0};r.regs[0]=(unsigned long)&sk;r.regs[1]=(unsigned long)s;assert(!pthread_rwlock_rdlock(&probe_gate));if(registered)assert(!registered->pre_handler(registered,&r));assert(!pthread_rwlock_unlock(&probe_gate));}
static long read_packets(struct file *f,u64 generation,struct nbdriver_udp_packet *out,unsigned capacity,struct nbdriver_udp_read_request *r){memset(r,0,sizeof(*r));r->packets=(uintptr_t)out;r->generation=generation;r->capacity=capacity;return nbdriver_udp_ioctl(f,NBDRIVER_IOC_UDP_READ,(unsigned long)r);}
static atomic_int producers;
static void *produce(void *arg){cpu_id=(int)(intptr_t)arg;for(unsigned i=0;i<10000;++i){struct sk_buff s=packet(64,(unsigned)cpu_id+i);s.bytes[8]=cpu_id;s.bytes[9]=i&255;deliver(&s,creds.uid);}atomic_fetch_sub(&producers,1);return NULL;}
int main(void){
 registration_error=-ENOENT;nbdriver_udp_init();assert(!nbdriver_udp_features());struct nbdriver_udp_start_request r={.pid=123};assert(start(&owner,&r)==-EOPNOTSUPP);nbdriver_udp_exit();no_resources();
 registration_error=0;nbdriver_udp_init();assert(nbdriver_udp_features()==NBDRIVER_FEATURE_UDP_CAPTURE&&!registered);
 registration_error=-EBUSY;r=(struct nbdriver_udp_start_request){.pid=123};assert(start(&owner,&r)==-EBUSY);registration_error=0;no_resources();
 caller_uid=1000;r=new_start();assert(!stop(&owner));no_resources();caller_uid=10001;allow_cap=1;r=new_start();assert(!stop(&owner));no_resources();allow_cap=0;caller_uid=0;
 assert(start(&owner,NULL)==-EFAULT);r=(struct nbdriver_udp_start_request){.pid=-1};assert(start(&owner,&r)==-EINVAL);r.pid=123;r.min_payload=2049;assert(start(&owner,&r)==-EINVAL);r.min_payload=0;r.expected_start_ns=43;assert(start(&owner,&r)==-ESTALE);no_resources();r.expected_start_ns=42;
 deny_mm=1;assert(start(&owner,&r)==-EACCES);deny_mm=0;no_resources();
 for(int i=0;i<4;++i){alloc_budget=i;assert(start(&owner,&r)==-ENOMEM);no_resources();}alloc_budget=-1;
 caller_uid=10001;assert(start(&owner,&r)==-EPERM);caller_uid=0;
 fail_copy_to=&r;assert(start(&owner,&r)==-EFAULT);fail_copy_to=NULL;no_resources();
 r=new_start();struct nbdriver_udp_status status;assert(!nbdriver_udp_ioctl(&owner,NBDRIVER_IOC_UDP_STATUS,(unsigned long)&status)&&status.active&&!status.queued);assert(!nbdriver_udp_poll(&owner,NULL));
 struct nbdriver_udp_start_request busy={.pid=123};assert(start(&other,&busy)==-EBUSY);assert(stop(&other)==-EPERM);nbdriver_udp_release(&other);assert(registered&&udp_active);assert(nbdriver_udp_poll(&other,NULL)==EPOLLERR);
 struct nbdriver_udp_packet out[32];struct nbdriver_udp_read_request read;
 assert(read_packets(&other,r.generation,out,32,&read)==-EPERM);assert(read_packets(&owner,r.generation+1,out,32,&read)==-ESTALE&&!read.count);
 struct sk_buff s=packet(64,3);deliver(&s,creds.uid+1);assert(udp_active->captured==0);s=packet(8,3);deliver(&s,creds.uid);assert(udp_active->captured==0);s=packet(64,3);s.udp.len=htons(200);deliver(&s,creds.uid);assert(udp_active->malformed==1);s=packet(64,3);deliver(&s,creds.uid);assert(nbdriver_udp_poll(&owner,NULL)&EPOLLIN);
 assert(!read_packets(&owner,r.generation,out,32,&read)&&read.count==1&&read.status.queued==0);assert(out[0].sequence==1&&out[0].timestamp_ns&&out[0].src_port==32123&&out[0].payload_len==64&&!out[0].flags);assert(!memcmp(out[0].payload,s.bytes+8,64));for(unsigned i=64;i<2048;++i)assert(!out[0].payload[i]);
 s=packet(3200,7);s.gro=true;s.shinfo.gso_size=1440;deliver(&s,creds.uid);assert(!read_packets(&owner,r.generation,out,32,&read)&&read.count==1);assert(out[0].flags==(NBDRIVER_UDP_F_GRO|NBDRIVER_UDP_F_TRUNCATED)&&out[0].gso_size==1440&&out[0].original_len==3200&&out[0].payload_len==2048);
 for(unsigned i=0;i<266;++i){s=packet(64,i);deliver(&s,creds.uid);}assert(udp_active->captured==268&&udp_active->overflow==10);
 fail_copy_to=out;partial_copy=true;assert(read_packets(&owner,r.generation,out,32,&read)==-EFAULT&&!read.count&&read.status.queued==256);fail_copy_to=NULL;partial_copy=false;
 assert(!read_packets(&owner,r.generation,out,1,&read)&&read.count==1&&out[0].sequence==13&&read.status.queued==255);
 for(unsigned i=0;i<600;++i){s=packet(64,i);deliver(&s,creds.uid);}
 assert(!read_packets(&owner,r.generation,out,32,&read)&&read.count==31&&out[0].sequence==14&&out[30].sequence==44);
 /* Reply-copy failure must preserve the detached batch too. */
 memset(&read,0,sizeof(read));read.packets=(uintptr_t)out;read.generation=r.generation;read.capacity=32;fail_copy_to=&read;assert(nbdriver_udp_ioctl(&owner,NBDRIVER_IOC_UDP_READ,(unsigned long)&read)==-EFAULT);u64 first=out[0].sequence;fail_copy_to=NULL;assert(!read_packets(&owner,r.generation,out,32,&read)&&read.count==32&&out[0].sequence==first);
 WRITE_ONCE(task.mm,&other_mm);s=packet(64,9);u64 before=udp_active->captured;deliver(&s,creds.uid);assert(udp_active->captured==before);assert(read_packets(&owner,r.generation,out,32,&read)==-ESRCH);assert(nbdriver_udp_poll(&owner,NULL)==EPOLLHUP);WRITE_ONCE(task.mm,&target_mm);
 WRITE_ONCE(task.alive,false);deliver(&s,creds.uid);assert(udp_active->captured==before&&read_packets(&owner,r.generation,out,32,&read)==-ESRCH);WRITE_ONCE(task.alive,true);
 WRITE_ONCE(task.flags,PF_EXITING);deliver(&s,creds.uid);assert(udp_active->captured==before);WRITE_ONCE(task.flags,0);nbdriver_udp_release(&owner);no_resources();
 r=new_start();quiesced=true;nbdriver_udp_quiesce();no_resources();r=(struct nbdriver_udp_start_request){.pid=123};assert(start(&owner,&r)==-ESHUTDOWN);quiesced=false;r=new_start();assert(!udp_active->count);assert(!stop(&owner));no_resources();assert(!stop(&owner));
 r=new_start();pthread_t threads[2];atomic_store(&producers,2);for(int i=0;i<2;++i)assert(!pthread_create(&threads[i],NULL,produce,(void *)(intptr_t)i));u64 received=0,sequence=0;
 while(atomic_load(&producers)||udp_active->count||udp_active->pending){assert(!read_packets(&owner,r.generation,out,32,&read));for(unsigned i=0;i<read.count;++i){assert(out[i].sequence>sequence);sequence=out[i].sequence;assert(out[i].payload_len==64&&!out[i].flags);unsigned tag=out[i].payload[0]+out[i].payload[1];for(unsigned j=2;j<64;++j)assert(out[i].payload[j]==(unsigned char)(tag+j));++received;}}
 for(int i=0;i<2;++i)assert(!pthread_join(threads[i],NULL));assert(udp_active->captured==20000&&received+udp_active->overflow==20000);printf("PASS UDP production backend: 20000 concurrent packets, delivered=%llu overflow=%llu; owner isolation, identity, rollback, copy retries, metadata, GRO/truncation, poll and cleanup\n",received,udp_active->overflow);
 nbdriver_udp_exit();no_resources();assert(registrations==unregistrations);return 0;
}
