#!/usr/bin/env python3
"""Run real query/auth/PID sampling functions with bounded kernel adapters.

These probes verify the behavior of production function bodies, not kernel
scheduler/RCU implementations. Module lifetime is checked separately in ELF.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CORE = (ROOT / 'src/nbdriver_core.c').read_text()
FEATURES = (ROOT / 'src/nbdriver_features.c').read_text()

def section(source, start, end):
    return source[source.index(start):source.index(end, source.index(start))]

COMMON = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "nbdriver_abi.h"
typedef unsigned char u8;
typedef uint64_t u64;
#define noinline
#define NBDRIVER_NO_CFI
#define READ_ONCE(x) (x)
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define IS_ERR(p) ((uintptr_t)(p)>(uintptr_t)-4096)
#define min_t(type,a,b) ((type)(a)<(type)(b)?(type)(a):(type)(b))
'''
ARGV = COMMON + r'''
struct mm_struct { unsigned long arg_start, arg_end; };
static const char *prefix;
static unsigned copied;
static int read_error;
static int nbdriver_read_mm(struct mm_struct *mm, struct nbdriver_read_request *r,u8 *out,bool fault)
{ (void)mm;assert(!fault&&copied<=r->size);memcpy(out,prefix,copied);r->copied=copied;return read_error; }
''' + section(CORE,'static int query_argv0(', 'static long query_find_process(') + r'''
int main(void) {
 struct mm_struct mm={0x1ff9,0x2100};char name[256]={0};
 prefix="target\0";copied=7;read_error=-ENODATA;
 assert(!query_argv0(&mm,name,sizeof(name))&&!strcmp(name,"target"));
 prefix="target";copied=6;
 assert(query_argv0(&mm,name,sizeof(name))==-ENODATA);
 read_error=0;
 assert(query_argv0(&mm,name,sizeof(name))==-ENAMETOOLONG);
 prefix="x\0";copied=2;
 assert(!query_argv0(&mm,name,sizeof(name)));
 mm.arg_end=mm.arg_start;
 assert(query_argv0(&mm,name,sizeof(name))==-ENOENT);
 puts("PASS: actual argv0 reader accepts a full prefix before later-page failure; rejects incomplete input");
}
'''
TASKS = COMMON + r'''
typedef int pid_t;
#define PIDTYPE_PID 0
#define PF_EXITING 1UL
#define CONFIG_SMP 1
#define CONFIG_ARM64_PTR_AUTH 1
struct signal_struct {int exec_update_lock;};
struct task_struct {
 int tgid, visible_tgid, visible_tid, on_cpu;
 unsigned long flags;
 u64 start_boottime, saved_tls;
 struct task_struct *group_leader;
 struct signal_struct *signal;
 struct {struct {struct {u64 lo,hi;} apga;} keys_user;} thread;
};
struct pid {int value;};
static struct signal_struct signal_state;
static struct task_struct leader={.tgid=9000,.visible_tgid=10,.visible_tid=10,.start_boottime=42,.signal=&signal_state};
static struct task_struct worker={.tgid=9000,.visible_tgid=10,.visible_tid=11,.start_boottime=43,.signal=&signal_state};
static struct task_struct self={.tgid=9100,.visible_tgid=20,.visible_tid=20,.on_cpu=1,.start_boottime=44,.signal=&signal_state};
static struct task_struct *current=&self;
static struct pid id;
static int refs, locks, preemption, scheduler_lock, task_calls, ipi_calls, ipi_misses, ipi_error;
static u64 live_tls, live_lo, live_hi;
#define SYS_APGAKEYLO_EL1 1
#define SYS_APGAKEYHI_EL1 2
static u64 sample_key(int key){assert(preemption);return key==SYS_APGAKEYLO_EL1?live_lo:live_hi;}
#define read_sysreg_s(reg) sample_key(reg)
#define task_cpu(task) ((void)(task),3)
static int smp_call_function_single(int cpu,void (*fn)(void *),void *arg,int wait)
{
 struct task_struct *saved=current;
 assert(cpu==3&&wait&&!scheduler_lock&&!preemption);++ipi_calls;
 if(ipi_error)return ipi_error;
 current=ipi_misses?&self:&worker;if(ipi_misses)--ipi_misses;
 ++preemption;fn(arg);--preemption;current=saved;return 0;
}
static bool auth=true;
static struct pid *find_get_pid(int nr){if(nr!=11)return NULL;id.value=nr;return &id;}
static struct task_struct *get_pid_task(struct pid *p,int type){(void)type;assert(p==&id);++refs;return &worker;}
static void put_pid(struct pid *p){assert(p==&id);}
static int query_lock_task(struct task_struct *p){assert(p==&worker);++locks;return 0;}
static void up_read(int *l){assert(l==&signal_state.exec_update_lock&&locks);--locks;}
static void put_task_struct(struct task_struct *p){assert(p==&worker&&refs);--refs;}
static int task_tgid_vnr(struct task_struct *p){return p->visible_tgid;}
static bool pid_alive(struct task_struct *p){(void)p;return true;}
#define task_user_tls(p) (&(p)->saved_tls)
static void preempt_disable(void){++preemption;}
static void preempt_enable(void){assert(preemption);--preemption;}
static u64 sample_register(void){assert(preemption);return live_tls;}
#define read_sysreg(reg) sample_register()
static bool system_supports_generic_auth(void){assert(preemption||scheduler_lock);return auth;}
static int task_call(struct task_struct *p,int (*fn)(struct task_struct *,void *),void *arg)
{int r;assert(!scheduler_lock);++scheduler_lock;++task_calls;r=fn(p,arg);--scheduler_lock;return r;}
static int (*query_task_call)(struct task_struct *,int (*)(struct task_struct *,void *),void *)=task_call;
''' + section(CORE,'static struct task_struct *query_task(', 'struct task_struct *nbdriver_query_get_process(') + section(CORE,'static int query_environment_sample(', 'static long query_environment(') + r'''
int main(void) {
 struct task_struct *t;struct nbdriver_thread_environment req={0};
 leader.group_leader=&leader;worker.group_leader=&leader;self.group_leader=&self;
 t=query_task(10,11,42,43);assert(t==&worker&&refs==1&&locks==1);query_put_task(t);assert(!refs&&!locks);
 assert(PTR_ERR(query_task(9000,11,0,0))==-ESRCH&&!refs&&!locks);
 assert(PTR_ERR(query_task(10,11,41,0))==-ESTALE&&!refs&&!locks);
 assert(PTR_ERR(query_task(10,11,42,44))==-ESTALE&&!refs&&!locks);
 assert(PTR_ERR(query_task(10,9001,0,0))==-ESRCH&&!refs&&!locks);
 assert(PTR_ERR(query_task(10,0,0,0))==-EINVAL);
 assert(!query_environment_call(&worker,&req));
 assert(req.valid==(NBDRIVER_THREAD_TLS_VALID|NBDRIVER_THREAD_APGA_VALID)&&!req.tls);
 assert(task_calls==1&&!preemption&&!scheduler_lock);
 worker.on_cpu=1;memset(&req,0,sizeof(req));
 live_tls=999;worker.saved_tls=123;
 assert(!query_environment_call(&worker,&req)&&req.tls==999&&ipi_calls==1);
 int before=ipi_calls;ipi_misses=2;memset(&req,0,sizeof(req));
 assert(!query_environment_call(&worker,&req)&&req.tls==999&&ipi_calls==before+3);
 before=ipi_calls;ipi_misses=3;memset(&req,0,sizeof(req));
 assert(query_environment_call(&worker,&req)==-EAGAIN&&!req.valid&&ipi_calls==before+3);
 before=ipi_calls;ipi_error=-ENXIO;
 assert(query_environment_call(&worker,&req)==-EAGAIN&&!req.valid&&ipi_calls==before+3);
 before=ipi_calls;ipi_error=-EIO;
 assert(query_environment_call(&worker,&req)==-EIO&&ipi_calls==before+1);ipi_error=0;
 worker.on_cpu=0;worker.flags=PF_EXITING;
 assert(query_environment_call(&worker,&req)==-ESRCH&&!req.valid);worker.flags=0;
 self.saved_tls=1;live_tls=999;
 assert(!query_environment_call(&self,&req)&&req.tls==999&&!preemption&&!scheduler_lock);
 live_lo=1;memset(&req,0,sizeof(req));
 assert(query_environment_call(&self,&req)==-EAGAIN&&!req.valid);live_lo=0;
 live_hi=1;assert(query_environment_call(&self,&req)==-EAGAIN&&!req.valid);live_hi=0;
 live_tls=0;auth=false;memset(&req,0,sizeof(req));
 assert(!query_environment_call(&self,&req)&&!req.tls&&req.valid==NBDRIVER_THREAD_TLS_VALID);
 query_task_call=NULL;memset(&req,0,sizeof(req));
 assert(query_environment_call(&worker,&req)==-EOPNOTSUPP&&!req.valid);
 assert(!query_environment_call(&self,&req)&&req.valid==NBDRIVER_THREAD_TLS_VALID);
 puts("PASS: actual PID identity uses namespace-visible TGID; references balance; TLS current/live and remote IPI, off-CPU, migration/CPU-offline bounded retries, key reset rejection, zero and missing backend");
}
'''
AUTH = COMMON + r'''
#define PTRACE_MODE_READ_FSCREDS 1U
#define PTRACE_MODE_ATTACH_FSCREDS 2U
struct task_struct {int dummy;};struct mm_struct {int dummy;};
static struct mm_struct mm;static unsigned mode;static bool permit_attach;
static struct mm_struct *access_mm(struct task_struct *t,unsigned m)
{(void)t;mode=m;return m==PTRACE_MODE_ATTACH_FSCREDS&&!permit_attach?ERR_PTR(-EACCES):&mm;}
static struct mm_struct *(*nbdriver_mm_access)(struct task_struct *,unsigned)=access_mm;
''' + section(CORE,'static noinline struct mm_struct *NBDRIVER_NO_CFI\nnbdriver_access_mm_mode(', '/* fixup_user_fault may drop') + r'''
int main(void) {
 struct task_struct t={0};
 assert(nbdriver_access_mm(&t)==&mm&&mode==PTRACE_MODE_READ_FSCREDS);
 assert(PTR_ERR(nbdriver_access_mm_mode(&t,true))==-EACCES&&mode==PTRACE_MODE_ATTACH_FSCREDS);
 permit_attach=true;assert(nbdriver_access_mm_mode(&t,true)==&mm&&mode==PTRACE_MODE_ATTACH_FSCREDS);
 puts("PASS: actual mm authorization selects READ for reads, ATTACH for writes, and preserves denial");
}
'''
with tempfile.TemporaryDirectory(prefix='nbdriver-audit-fixes-') as directory:
    temp=Path(directory)
    for name,source in [('argv0',ARGV),('namespace-tls',TASKS),('authorization',AUTH)]:
        path=temp/(name+'.c');path.write_text(source)
        binary=temp/name
        subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-O2','-Wall','-Wextra','-Werror',
                        '-I'+str(ROOT/'include'),str(path),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
