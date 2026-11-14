// SPDX-License-Identifier: GPL-2.0-only
/* Executes production binding, path matching, iterator contexts and lifecycle
 * using bounded host adapters; no real kprobe or scheduler validation. */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include "nbdriver_abi.h"
typedef uint32_t u32;typedef uint64_t u64;
#define KERNEL_VERSION(a,b,c) (((a)<<16)|((b)<<8)|(c))
#ifndef TEST_VERSION
#define TEST_VERSION KERNEL_VERSION(6,6,0)
#endif
#define LINUX_VERSION_CODE TEST_VERSION
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define SYSFS_MAGIC 0x62656572U
#define DEBUGFS_MAGIC 0x64626720U
#define PF_EXITING 1U
#define PIDTYPE_TGID 0
#define HZ 100
#define __user
#define IS_ERR(p) ((uintptr_t)(p)>(uintptr_t)-4096)
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
typedef unsigned spinlock_t;
#define DEFINE_SPINLOCK(n) spinlock_t n
static unsigned lock_depth,rcu_depth,task_refs;
static void rcu_read_lock(void){rcu_depth++;}
static void rcu_read_unlock(void){assert(rcu_depth);rcu_depth--;}
#define spin_lock_irqsave(p,f) do{(f)=0;assert(!*(p));*(p)=1;lock_depth++;}while(0)
#define spin_unlock_irqrestore(p,f) do{(void)(f);assert(*(p)&&lock_depth);*(p)=0;lock_depth--;}while(0)
typedef uint64_t atomic64_t;
#define ATOMIC64_INIT(x) (x)
static void atomic64_inc(atomic64_t *n){++*n;}
static u64 atomic64_read(atomic64_t *n){return *n;}
struct task_struct;
struct pid {unsigned refs;int nr;struct task_struct *task;};
struct task_struct {u64 start_boottime;unsigned flags;struct pid *id;};
static struct pid identity;
static struct task_struct task={42,0,&identity};
static struct task_struct *pid_task(struct pid *p,int type){(void)type;assert(rcu_depth);return p->task;}
static bool pid_alive(struct task_struct *t){return t->id->task==t;}
static struct pid *get_pid(struct pid *p){if(p)p->refs++;return p;}
static void put_pid(struct pid *p){if(p){assert(p->refs);p->refs--;}}
static struct pid *task_tgid(struct task_struct *t){return t->id;}
static int pid_nr(struct pid *p){return p->nr;}
static struct task_struct *nbdriver_query_get_process(int pid,u64 expected){if(pid!=123||!identity.task)return ERR_PTR(-ESRCH);if(expected&&expected!=42)return ERR_PTR(-ESTALE);task_refs++;return &task;}
static void nbdriver_query_put_process(struct task_struct *t){assert(t==&task&&task_refs);task_refs--;}
struct super_block {unsigned long s_magic;};
struct qstr {const unsigned char *name;unsigned len;};
struct dentry {struct qstr d_name;struct dentry *d_parent;struct super_block *d_sb;};
struct path {struct dentry *dentry;};
struct file {struct path f_path;};
struct dir_context;
#if TEST_VERSION >= KERNEL_VERSION(6,1,0)
typedef bool (*fill_fn)(struct dir_context *,const char *,int,loff_t,u64,unsigned);
#else
typedef int (*fill_fn)(struct dir_context *,const char *,int,loff_t,u64,unsigned);
#endif
struct dir_context {fill_fn actor;loff_t pos;};
struct pt_regs {unsigned long regs[31],pc;};
struct kprobe {const char *symbol_name;int (*pre_handler)(struct kprobe *,struct pt_regs *);unsigned nmissed;};
struct kretprobe_instance {_Alignas(max_align_t) unsigned char data[128];};
struct kretprobe {struct kprobe kp;int (*entry_handler)(struct kretprobe_instance *,struct pt_regs *);int (*handler)(struct kretprobe_instance *,struct pt_regs *);unsigned data_size,maxactive,nmissed;};
static unsigned registered,attempts,fail_at,queued;
static int register_kretprobe(struct kretprobe *p){assert(p->data_size<=128);if(++attempts==fail_at)return -EINVAL;registered++;return 0;}
static void unregister_kretprobe(struct kretprobe *p){(void)p;assert(registered);registered--;}
static int register_kprobe(struct kprobe *p){(void)p;if(++attempts==fail_at)return -EINVAL;registered++;return 0;}
static void unregister_kprobe(struct kprobe *p){(void)p;assert(registered);registered--;}
struct work_struct {int unused;};struct delayed_work {struct work_struct work;void (*fn)(struct work_struct *);};
#define DECLARE_DELAYED_WORK(n,f) struct delayed_work n={.fn=f}
static void schedule_delayed_work(struct delayed_work *w,int ticks){(void)w;assert(ticks==HZ);queued=1;}
static void cancel_delayed_work_sync(struct delayed_work *w){(void)w;queued=0;}
static unsigned long copy_from_user(void *to,const void *from,size_t n){if((uintptr_t)from<4096)return n;memcpy(to,from,n);return 0;}
static unsigned long copy_to_user(void *to,const void *from,size_t n){if((uintptr_t)to<4096)return n;memcpy(to,from,n);return 0;}
#include "gpu_visibility_production.inc"
static struct super_block sysfs={SYSFS_MAGIC},debugfs={DEBUGFS_MAGIC},other={0};
static struct dentry root,proc,pid_dir,leaf;
static void make_path(void){root=(struct dentry){{(const unsigned char *)"kgsl-3d0",9},NULL,&sysfs};root.d_parent=&root;proc=(struct dentry){{(const unsigned char *)"proc",4},&root,&sysfs};pid_dir=(struct dentry){{(const unsigned char *)"9000",4},&proc,&sysfs};leaf=(struct dentry){{(const unsigned char *)"mem",3},&pid_dir,&sysfs};}
static unsigned emitted;
static GPU_FILL_RETURN sink(struct dir_context *c,const char *n,int l,loff_t off,u64 ino,unsigned type){(void)c;(void)n;(void)l;(void)off;(void)ino;(void)type;emitted++;return GPU_FILL_CONTINUE;}
static void no_refs(void){assert(!lock_depth&&!rcu_depth&&!task_refs);}
int main(void){
 struct nbdriver_gpu_visibility_request r={.pid=123,.operation=NBDRIVER_GPU_VISIBILITY_HIDE};
 struct file file;struct pt_regs regs={};struct kretprobe_instance a,b;
 struct dir_context context={sink,0},second={sink,12};
 identity=(struct pid){0,9000,&task};make_path();
 for(fail_at=1;fail_at<=3;fail_at++){attempts=0;nbdriver_kgsl_visibility_init();assert(!gpu_visibility_active&&!registered&&!queued);nbdriver_kgsl_visibility_exit();}
 fail_at=0;attempts=0;nbdriver_kgsl_visibility_init();assert(registered==3&&gpu_visibility_active&&queued);
 assert(nbdriver_kgsl_visibility_features()==NBDRIVER_FEATURE_GPU_VISIBILITY);
 assert(!nbdriver_kgsl_visibility_ioctl((unsigned long)&r)&&r.hidden&&r.target_count==1&&identity.refs==1);
 assert(!nbdriver_kgsl_visibility_ioctl((unsigned long)&r)&&identity.refs==1);no_refs();
 assert(gpu_visibility_number("9000",4)&&!gpu_visibility_number("123",3));
 assert(!gpu_visibility_number("09000",5)&&!gpu_visibility_number("2147483648",10)&&!gpu_visibility_number("9000x",5));
 assert(gpu_visibility_path(&leaf));root.d_name.name=(const unsigned char *)"unrelated";assert(!gpu_visibility_path(&leaf));root.d_name.name=(const unsigned char *)"kgsl-3d0";
 proc.d_sb=&other;assert(!gpu_visibility_path(&leaf));proc.d_sb=&debugfs;assert(gpu_visibility_path(&leaf));proc.d_sb=&sysfs;
 r.expected_start_ns=43;assert(nbdriver_kgsl_visibility_ioctl((unsigned long)&r)==-ESTALE&&identity.refs==1);r.expected_start_ns=0;
 file.f_path.dentry=&proc;regs.regs[0]=(unsigned long)&file;regs.regs[1]=(unsigned long)&context;
 assert(!gpu_visibility_iter_enter(&a,&regs)&&regs.regs[1]!=(unsigned long)&context);
 struct dir_context *wrapped=(void *)regs.regs[1];wrapped->pos=20;
 assert(wrapped->actor(wrapped,"9000",4,1,1,4)==GPU_FILL_CONTINUE&&!emitted);
 assert(wrapped->actor(wrapped,"9001",4,2,2,4)==GPU_FILL_CONTINUE&&emitted==1&&context.pos==20);
 regs.regs[1]=(unsigned long)&second;assert(!gpu_visibility_iter_enter(&b,&regs));
 struct dir_context *other_context=(void *)regs.regs[1];assert(other_context!=wrapped);other_context->pos=99;
 gpu_visibility_iter_leave(&b,&regs);gpu_visibility_iter_leave(&a,&regs);assert(second.pos==99&&context.pos==20);
 file.f_path.dentry=&leaf;regs.regs[0]=(unsigned long)&file;regs.regs[30]=1234;
 assert(gpu_visibility_open_pre(NULL,&regs)==1&&(long)regs.regs[0]==-ENOENT&&regs.pc==1234);
 struct path path={&leaf};regs.regs[0]=(unsigned long)&path;assert(gpu_visibility_getattr_pre(NULL,&regs)==1);
 r.operation=NBDRIVER_GPU_VISIBILITY_SHOW;assert(!nbdriver_kgsl_visibility_ioctl((unsigned long)&r)&&!r.hidden&&!r.target_count&&!identity.refs&&!gpu_visibility_path(&leaf));
 /* Previously wrapped directory contexts immediately see restored names. */
 assert(wrapped->actor(wrapped,"9000",4,1,1,4)==GPU_FILL_CONTINUE&&emitted==2);
 r.operation=NBDRIVER_GPU_VISIBILITY_HIDE;assert(!nbdriver_kgsl_visibility_ioctl((unsigned long)&r));
 task.flags=PF_EXITING;assert(!gpu_visibility_number("9000",4));gpu_visibility_reap(NULL);assert(!identity.refs&&!gpu_visibility_count);task.flags=0;
 r.operation=NBDRIVER_GPU_VISIBILITY_QUERY;r.pid=0;gpu_visibility_iter_probe.nmissed=2;
 assert(!nbdriver_kgsl_visibility_ioctl((unsigned long)&r)&&r.missed==2&&r.directory_hits&&r.access_hits);
 r.pid=123;r.operation=NBDRIVER_GPU_VISIBILITY_HIDE;assert(!nbdriver_kgsl_visibility_ioctl((unsigned long)&r));
 identity.task=NULL;assert(!gpu_visibility_number("9000",4));gpu_visibility_reap(NULL);assert(!identity.refs);identity.task=&task;
 nbdriver_kgsl_visibility_exit();assert(!registered&&!queued&&!gpu_visibility_count&&!identity.refs);no_refs();
 puts("PASS: actual GPU visibility binding, PID generation/namespace number, exact filesystem ancestry, enumeration contexts, open/stat, immediate restore, exit cleanup, hook rollback and missed-probe reporting");return 0;
}
