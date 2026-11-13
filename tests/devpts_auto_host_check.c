// SPDX-License-Identifier: GPL-2.0-only
/* Executes the production automatic discovery, probes and teardown through
 * bounded task/mm/RCU/workqueue adapters; not a real scheduler stress test. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/stat.h>
#include "nbdriver_abi.h"
typedef uint32_t u32;typedef uint64_t u64;typedef unsigned char u8;typedef int pid_t;
#define __user
#define NBDRIVER_NO_CFI
#define GFP_KERNEL 0
#define CAP_SYS_PTRACE 1
#define PF_KTHREAD 1
#define PF_EXITING 2
#define PIDTYPE_TGID 1
#define DEVPTS_SUPER_MAGIC 0x1cd1
#define MAY_ACCESS 16
#define MAY_READ 4
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
#define msecs_to_jiffies(x) (x)
#define DEFINE_MUTEX(n) int n
#define rcu_dereference(p) (p)
#define rcu_dereference_protected(p,condition) ((void)(condition),(p))
#define rcu_assign_pointer(p,v) ((p)=(v))
#define RCU_INIT_POINTER(p,v) ((p)=(v))
#define __rcu
#define ATOMIC64_INIT(x) (x)
typedef u64 atomic64_t;
static void atomic64_inc(atomic64_t *a){++*a;}
static u64 atomic64_read(atomic64_t *a){return *a;}
struct mm_struct {unsigned long arg_start,arg_end;const char *argv;int users,refs;bool partial,missing;};
struct pid {int nr,refs;};
struct task_struct {unsigned long flags;struct mm_struct *mm;struct pid *pid;int refs,visible;bool alive;};
static struct mm_struct mms[5];static struct pid ids[5];static struct task_struct fixture_tasks[5];
static struct task_struct *current=&fixture_tasks[0];
static int rcu_depth,mutex_depth,allocations,fail_alloc;
static bool allowed=true,pending;
#define for_each_process(t) for(size_t task_index=0;task_index<ARRAY_SIZE(fixture_tasks)&&((t)=&fixture_tasks[task_index]);++task_index)
#define task_tgid(t) ((t)->pid)
static void rcu_read_lock(void){++rcu_depth;}
static void rcu_read_unlock(void){assert(rcu_depth);--rcu_depth;}
static void synchronize_rcu(void){assert(!rcu_depth);}
static bool lockdep_is_held(int *m){(void)m;return mutex_depth>0;}
static void mutex_lock(int *m){(void)m;assert(!rcu_depth);++mutex_depth;}
static void mutex_unlock(int *m){(void)m;assert(mutex_depth);--mutex_depth;}
static void *allocate(size_t n){assert(!rcu_depth);if(++allocations==fail_alloc)return NULL;return calloc(1,n);}
static void *kvcalloc(size_t n,size_t z,int f){(void)f;return allocate(n*z);}
static void *kzalloc(size_t n,int f){(void)f;return allocate(n);}
#define kvfree free
#define kfree free
static bool capable(int c){(void)c;return allowed;}
static void get_task_struct(struct task_struct *t){assert(rcu_depth);++t->refs;}
static void put_task_struct(struct task_struct *t){assert(t->refs);--t->refs;}
static struct mm_struct *get_task_mm(struct task_struct *t){if(!t->alive||!t->mm)return NULL;++t->mm->users;return t->mm;}
static void mmput(struct mm_struct *m){assert(m->users>1);--m->users;}
static void mmgrab(struct mm_struct *m){++m->refs;}
static void mmdrop(struct mm_struct *m){assert(m->refs>1);--m->refs;}
static bool pid_alive(struct task_struct *t){return t->alive;}
static struct pid *get_pid(struct pid *p){assert(rcu_depth);++p->refs;return p;}
static void put_pid(struct pid *p){assert(p->refs>1);--p->refs;}
static struct task_struct *get_pid_task(struct pid *p,int type)
{assert(type==PIDTYPE_TGID);for(size_t i=0;i<ARRAY_SIZE(fixture_tasks);++i)if(fixture_tasks[i].alive&&fixture_tasks[i].pid==p){++fixture_tasks[i].refs;return &fixture_tasks[i];}return NULL;}
static int task_tgid_vnr(struct task_struct *t){return t->visible;}
static int nbdriver_read_mm(struct mm_struct *m,struct nbdriver_read_request *r,u8 *out,bool fault)
{assert(!fault);if(m->missing){r->copied=0;return -ENODATA;}size_t n=strlen(m->argv)+1;if(n>r->size)n=r->size;memcpy(out,m->argv,n);r->copied=n;return m->partial?-ENODATA:0;}
struct pt_regs {unsigned long regs[4],pc,lr,result;};
struct super_block {unsigned long s_magic;};
struct inode {unsigned mode,major,minor;struct super_block *i_sb;};
#define i_mode mode
#define imajor(i) ((i)->major)
#define iminor(i) ((i)->minor)
struct dentry {struct inode *inode;};struct path {struct dentry *dentry;};
static struct inode *d_backing_inode(struct dentry *d){return d->inode;}
static void regs_set_return_value(struct pt_regs *r,unsigned long x){r->result=x;}
static void instruction_pointer_set(struct pt_regs *r,unsigned long x){r->pc=x;}
static unsigned long procedure_link_pointer(struct pt_regs *r){return r->lr;}
struct kprobe {const char *symbol_name;int (*pre_handler)(struct kprobe *,struct pt_regs *);};
static int registrations,registered,fail_register;
static int register_kprobe(struct kprobe *p){assert(p->pre_handler);if(++registrations==fail_register)return -EACCES;++registered;return 0;}
static void unregister_kprobe(struct kprobe *p){(void)p;assert(registered>0);--registered;}
struct work_struct {void (*fn)(struct work_struct *);};struct delayed_work {struct work_struct work;};
#define DECLARE_DELAYED_WORK(name,fn) struct delayed_work name={{fn}}
static bool schedule_delayed_work(struct delayed_work *w,unsigned delay){assert(w->work.fn&&(delay==0||delay==500));pending=true;return true;}
static bool cancel_delayed_work_sync(struct delayed_work *w){(void)w;bool had=pending;pending=false;return had;}
static int copy_to_user(void *dst,const void *src,size_t size){if(!dst)return 1;memcpy(dst,src,size);return 0;}
static void test_log(const char *fmt,...){(void)fmt;}
#define nbd_info(...) test_log(__VA_ARGS__)
#define nbd_warn(...) test_log(__VA_ARGS__)
#define nbd_err(...) test_log(__VA_ARGS__)
#include "devpts_production.c"

static void refresh(void){assert(pending);pending=false;nbdriver_devpts_work.work.fn(&nbdriver_devpts_work.work);assert(pending);}
static struct nbdriver_devpts_status status(void)
{struct nbdriver_devpts_status s;assert(!nbdriver_devpts_status_ioctl((unsigned long)&s));assert(s.size==sizeof(s)&&!s.reserved);return s;}
int main(void)
{
    const char *names[]={"ordinary","com.tencent.tmgp.dfm","com.tencent.rmcn","com.tencent.tmgp.dfm:helper","com.tencent.rmcn"};
    for(size_t i=0;i<ARRAY_SIZE(fixture_tasks);++i){mms[i]=(struct mm_struct){1,64,names[i],1,1,false,false};ids[i]=(struct pid){100+(int)i,1};fixture_tasks[i]=(struct task_struct){0,&mms[i],&ids[i],0,10+(int)i,true};}
    fail_register=2;assert(nbdriver_devpts_init()==-EACCES&&!registered&&!pending);
    fail_register=0;assert(!nbdriver_devpts_init()&&registered==2&&pending);
    assert(status().active&&!status().target_count);
    mms[1].partial=true;refresh();struct nbdriver_devpts_status s=status();
    assert(s.target_count==3&&s.pids[0]==11&&s.pids[1]&&s.generation==1&&!s.last_error);
    refresh();assert(status().generation==1);
    struct super_block sb={DEVPTS_SUPER_MAGIC};struct inode inode={S_IFCHR,136,31,&sb};
    assert(!nbdriver_devpts_match_inode(&inode));
    current=&fixture_tasks[1];assert(nbdriver_devpts_match_inode(&inode));
    current=&fixture_tasks[2];assert(nbdriver_devpts_match_inode(&inode));
    current=&fixture_tasks[3];assert(!nbdriver_devpts_match_inode(&inode));
    current=&fixture_tasks[4];assert(nbdriver_devpts_match_inode(&inode));
    current=&fixture_tasks[1];inode.minor=32;assert(!nbdriver_devpts_match_inode(&inode));inode.minor=31;
    struct pt_regs regs={.regs={(unsigned long)&inode,MAY_READ},.lr=123};
    assert(!nbdriver_devpts_permission_pre(NULL,&regs));regs.regs[1]=MAY_ACCESS;
    assert(nbdriver_devpts_permission_pre(NULL,&regs)==1&&regs.pc==123&&(long)regs.result==-ENOENT);
    struct dentry dent={&inode};struct path path={&dent};regs.regs[0]=(unsigned long)&path;
    assert(nbdriver_devpts_getattr_pre(NULL,&regs)==1&&status().getattr_hits==1&&status().permission_hits==1);
    struct pid recycled={ids[1].nr,1};fixture_tasks[1].pid=&recycled;assert(!nbdriver_devpts_match_inode(&inode));fixture_tasks[1].pid=&ids[1];
    fixture_tasks[1].mm=&mms[0];assert(!nbdriver_devpts_match_inode(&inode));fixture_tasks[1].mm=&mms[1];
    fixture_tasks[2].alive=false;fixture_tasks[4].visible=0;s=status();assert(s.target_count==1&&s.pids[1]==0);fixture_tasks[4].visible=14;
    fixture_tasks[1].flags=PF_EXITING;refresh();s=status();assert(s.target_count==1&&!s.pids[0]&&s.pids[1]==14);
    fixture_tasks[1].flags=0;fixture_tasks[2].alive=true;refresh();assert(status().target_count==3);
    allocations=0;fail_alloc=3;refresh();assert(status().last_error==-ENOMEM&&status().target_count==3);
    fail_alloc=0;refresh();assert(!status().last_error&&status().target_count==3);
    mms[1].missing=true;refresh();assert(!status().pids[0]);mms[1].missing=false;refresh();assert(status().pids[0]==11);
    allowed=false;assert(nbdriver_devpts_status_ioctl((unsigned long)&s)==-EPERM);allowed=true;
    assert(nbdriver_devpts_status_ioctl(0)==-EFAULT);
    nbdriver_devpts_exit();assert(!pending&&!registered&&!nbdriver_devpts_state&&!rcu_depth&&!mutex_depth);
    nbdriver_devpts_work.work.fn(&nbdriver_devpts_work.work);assert(!pending);
    for(size_t i=0;i<ARRAY_SIZE(fixture_tasks);++i)assert(!fixture_tasks[i].refs&&ids[i].refs==1&&mms[i].users==1&&mms[i].refs==1);
    puts("PASS: production auto discovery, exact two-package scope, multiple instances, delayed retry, PID reuse/exec, namespace/exit, access/stat-only probes, OOM retention, hook rollback and teardown");
    return 0;
}
