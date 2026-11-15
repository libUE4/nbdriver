#!/usr/bin/env python3
"""Run production slot selection, patch lifecycle, fallback and runtime control."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/nbdriver_platform.c').read_text()
s=s[s.index('typedef asmlinkage long (*nbdriver_syscall_fn)'):s.index('/* ===== END src/platform/nbdriver_entry.c ===== */')]
control=(root/'src/nbdriver_runtime.c').read_text();control=control[control.index('long nbdriver_runtime_control_ioctl('):control.index('bool nbdriver_runtime_quiesced(')]
harness=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "nbdriver_abi.h"
typedef uint32_t u32;
typedef int32_t s32;
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define __user
#define asmlinkage
#define noinline
#define NBDRIVER_NO_CFI
#define __NR_syscalls 512U
#define nbd_warn(...) ((void)0)
#define nbd_info(...) ((void)0)
#define nbd_err(...) ((void)0)
#define READ_ONCE(x) (x)
#define WARN_ON_ONCE(x) assert(!(x))
#define IS_ALIGNED(a,n) (!((a)&((n)-1)))
#define PHYS_PFN(x) ((x)>>12)
#define offset_in_page(x) ((x)&4095U)
#define VM_MAP 0
#define PAGE_KERNEL 0
#define THIS_MODULE ((void *)1)
#define DEFINE_MUTEX(x) int x
#define mutex_lock(x) ((void)(x))
#define mutex_unlock(x) ((void)(x))
#define smp_wmb() ((void)0)
struct cpumask { int unused; };
struct page { int unused; };
struct pt_regs { unsigned long regs[31]; int syscallno; };
struct kprobe { const char *symbol_name; int (*pre_handler)(struct kprobe *,struct pt_regs *); };
typedef void (*nbdriver_task_work_add_fn_t)(void);
static nbdriver_task_work_add_fn_t nbdriver_task_work_add_fn;
static unsigned int refs,unmaps,syncs,patches,scheduled,unregistrations,udp_drained;
static int stop_error,write_error,read_error,probe_error,schedule_error;
static bool absent_table,absent_sync,absent_work,pin_fail,bad_pfn,map_fail;
static void *mapped;
static long (*table[512])(const struct pt_regs *) __attribute__((aligned(4096)));
static unsigned long image_offset;
static struct page fake_page;
static long ni_fn(const struct pt_regs *r) { (void)r; return -ENOSYS; }
static long cfi_ni_fn(const struct pt_regs *r) { (void)r; return -ENOSYS; }
static long used_fn(const struct pt_regs *r) { (void)r; return 123; }
static bool pfn_valid(unsigned long pfn) { assert(pfn==1000);return !bad_pfn; }
static struct page *pfn_to_page(unsigned long pfn) { assert(pfn==1000);return &fake_page; }
static void *vmap(struct page **p,unsigned int count,unsigned long flags,int prot)
{ assert(*p==&fake_page&&count==1&&!flags&&!prot&&!mapped);if(map_fail)return NULL;return mapped=table; }
static void vunmap(void *p) { assert(p&&p==mapped);mapped=NULL;++unmaps; }
static bool try_module_get(void *m) { assert(m==THIS_MODULE);if(pin_fail)return false;++refs;return true; }
static void module_put(void *m) { assert(m==THIS_MODULE&&refs);--refs; }
static long kernel_read(void *dst,const void *src,size_t n)
{ if(read_error)return read_error;memcpy(dst,src,n);return 0; }
static long kernel_write(void *dst,const void *src,size_t n)
{ assert(mapped&&n==sizeof(void *));if(write_error)return write_error;memcpy(dst,src,n);return 0; }
static int stop_machine_fn(int (*fn)(void *),void *arg,const struct cpumask *mask)
{ assert(!mask&&mapped);++patches;return stop_error?stop_error:fn(arg); }
static void sync_fn(void) { assert(mapped&&refs==1);++syncs; }
static unsigned long nbdriver_ksym_lookup_optional(const char *name)
{
 if(!strcmp(name,"sys_call_table"))return absent_table?0:(unsigned long)table;
 if(!strcmp(name,"stop_machine"))return (unsigned long)stop_machine_fn;
 if(!strcmp(name,"copy_from_kernel_nofault"))return (unsigned long)kernel_read;
 if(!strcmp(name,"copy_to_kernel_nofault"))return (unsigned long)kernel_write;
 if(!strcmp(name,"synchronize_rcu_tasks")||!strcmp(name,"synchronize_rcu_tasks_rude"))return absent_sync?0:(unsigned long)sync_fn;
 if(!strcmp(name,"kimage_voffset"))return (unsigned long)&image_offset;
 if(!strcmp(name,"__arm64_sys_ni_syscall"))return (unsigned long)ni_fn;
 if(!strcmp(name,"__arm64_sys_ni_syscall.cfi_jt"))return (unsigned long)cfi_ni_fn;
 return 0;
}
static unsigned long nbdriver_ksym_lookup(const char *n)
{ assert(!strcmp(n,"task_work_add"));return absent_work?0:1; }
static int register_kprobe(struct kprobe *p)
{ assert(!strcmp(p->symbol_name,"__arm64_sys_reboot")&&p->pre_handler);return probe_error; }
static void unregister_kprobe(struct kprobe *p)
{ assert(!strcmp(p->symbol_name,"__arm64_sys_reboot"));++unregistrations; }
static int nbdriver_schedule_fd(int *output)
{ assert(output);++scheduled;return schedule_error; }
static int copy_from_user(void *a,const void *b,size_t n) { memcpy(a,b,n);return 0; }
static int copy_to_user(void *a,const void *b,size_t n) { memcpy(a,b,n);return 0; }
static void nbdriver_udp_quiesce(void) { ++udp_drained; }
static struct { unsigned long long handle,token_lo,token_hi;unsigned int flags; } nbdriver_runtime={1,2,3,NBDRIVER_RUNTIME_READY};
static int nbdriver_runtime_lock;
'''+s+control+r'''
static void prepare(void)
{
 assert(!refs&&!mapped);
 for(unsigned int i=0;i<512;++i)table[i]=used_fn;
 image_offset=(unsigned long)table-(1000UL<<12);
 absent_table=absent_sync=absent_work=pin_fail=bad_pfn=map_fail=false;
 stop_error=write_error=read_error=probe_error=schedule_error=0;
 nbdriver_table_supported=false;nbdriver_runtime.flags=NBDRIVER_RUNTIME_READY;
}
static void control_call(unsigned int command,int expected)
{
 struct nbdriver_runtime_control r={0};r.version=NBDRIVER_ABI_VERSION;r.size=sizeof(r);
 r.handle=1;r.token_lo=2;r.token_hi=3;r.command=command;
 assert(nbdriver_runtime_control_ioctl((unsigned long)&r)==expected&&r.error==expected);
}
int main(void)
{
 prepare();table[18]=ni_fn;
 assert(!nbdriver_entry_init()&&nbdriver_syscall_number==18&&refs==1&&mapped);
 assert(table[18]==nbdriver_syscall_install&&nbdriver_entry_probe_mask()==3);
 struct pt_regs call={0},probe={0};int output=0;
 call.syscallno=18;call.regs[0]=NBDRIVER_INSTALL_MAGIC1;call.regs[1]=NBDRIVER_INSTALL_MAGIC2;call.regs[3]=(unsigned long)&output;
 assert(!table[18](&call)&&scheduled==1);
 call.regs[0]=0;assert(table[18](&call)==-ENOSYS&&scheduled==1);call.regs[0]=NBDRIVER_INSTALL_MAGIC1;
 probe.regs[0]=(unsigned long)&call;
 assert(!nbdriver_reboot_pre(&nbdriver_reboot_probe,&probe)&&scheduled==1);
 call.syscallno=142;assert(!nbdriver_reboot_pre(&nbdriver_reboot_probe,&probe)&&scheduled==2);
 probe.regs[0]=0;assert(!nbdriver_reboot_pre(&nbdriver_reboot_probe,&probe));
 struct nbdriver_runtime_control stale={0};stale.version=NBDRIVER_ABI_VERSION;stale.size=sizeof(stale);
 assert(nbdriver_runtime_control_ioctl((unsigned long)&stale)==-ESTALE&&refs==1);
 stop_error=-EIO;unsigned int drained=udp_drained;control_call(NBDRIVER_RUNTIME_CONTROL_QUIESCE,-EIO);
 assert(refs==1&&mapped&&table[18]==nbdriver_syscall_install&&nbdriver_runtime.flags==NBDRIVER_RUNTIME_READY&&udp_drained==drained);
 stop_error=0;control_call(NBDRIVER_RUNTIME_CONTROL_QUIESCE,0);
 assert(!refs&&!mapped&&table[18]==ni_fn&&syncs==1&&nbdriver_runtime.flags==NBDRIVER_RUNTIME_QUIESCED&&udp_drained==drained+1);
 control_call(NBDRIVER_RUNTIME_CONTROL_RESUME,0);assert(refs==1&&table[18]==nbdriver_syscall_install&&nbdriver_runtime.flags==NBDRIVER_RUNTIME_READY);
 /* Restore refuses an ownership mismatch and keeps its module reference. */
 table[18]=used_fn;assert(nbdriver_entry_quiesce()==-ESTALE&&refs==1);table[18]=nbdriver_syscall_install;
 assert(!nbdriver_entry_quiesce());nbdriver_entry_exit();assert(!nbdriver_entry_probe_mask());
 /* First slot is occupied; later preferred and both range endpoints work. */
 const unsigned int slots[]={38,42,163,164,244,259,295,423};
 for(unsigned int i=0;i<ARRAY_SIZE(slots);++i){prepare();unsigned int nr=slots[i];table[nr]=i&1?cfi_ni_fn:ni_fn;probe_error=-EINVAL;
  assert(!nbdriver_entry_init()&&nbdriver_syscall_number==(int)nr&&table[18]==used_fn&&refs==1&&nbdriver_entry_probe_mask()==1);
  assert(!nbdriver_entry_quiesce()&&!refs);nbdriver_entry_exit();}
 /* Missing patch dependency never prevents an independent reboot entry. */
 prepare();absent_sync=true;table[18]=ni_fn;assert(!nbdriver_entry_init()&&!refs&&table[18]==ni_fn&&nbdriver_entry_probe_mask()==2);nbdriver_entry_exit();
 prepare();absent_table=true;assert(!nbdriver_entry_init()&&!refs);nbdriver_entry_exit();
 prepare();assert(!nbdriver_entry_init()&&!refs);nbdriver_entry_exit();
 prepare();table[18]=ni_fn;write_error=-EFAULT;assert(!nbdriver_entry_init()&&!refs&&!mapped&&table[18]==ni_fn);nbdriver_entry_exit();
 prepare();table[18]=ni_fn;bad_pfn=true;assert(!nbdriver_entry_init()&&!refs&&!mapped);nbdriver_entry_exit();
 prepare();table[18]=ni_fn;map_fail=true;assert(!nbdriver_entry_init()&&!refs&&!mapped);nbdriver_entry_exit();
 prepare();table[18]=ni_fn;pin_fail=true;assert(!nbdriver_entry_init()&&!refs&&!mapped);nbdriver_entry_exit();
 prepare();absent_table=true;probe_error=-EPERM;assert(nbdriver_entry_init()==-EPERM&&!refs&&!mapped&&!nbdriver_entry_probe_mask());
 prepare();absent_work=true;assert(nbdriver_entry_init()==-ENOENT&&!refs&&!mapped);
 puts("PASS entry: dynamic/CFI empty slots, untouched occupied slots, independent reboot, task filtering, quiesce/resume, failed restore retains reference, init rollback");
}
'''
with tempfile.TemporaryDirectory(prefix='nbdriver-entry-')as directory:
 p=Path(directory);(p/'check.c').write_text(harness)
 subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=undefined,bounds','-I'+str(root/'include'),str(p/'check.c'),'-o',str(p/'check')],check=True)
 subprocess.run([str(p/'check')],check=True)
