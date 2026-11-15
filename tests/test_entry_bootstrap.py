#!/usr/bin/env python3
"""Exercise real entry selection and syscall filtering with kernel adapters."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/nbdriver_platform.c').read_text()
s=s[s.index('static int nbdriver_sf_pre('):s.index('int nbdriver_entry_init(')]
harness=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "nbdriver_abi.h"
typedef uint32_t u32;
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define __user
#define nbd_warn(...) ((void)0)
#define nbd_info(...) ((void)0)
struct pt_regs { unsigned long regs[31]; int syscallno; };
struct kprobe { const char *symbol_name; int (*pre_handler)(struct kprobe *,struct pt_regs *); };
typedef void (*nbdriver_task_work_add_fn_t)(void);
static nbdriver_task_work_add_fn_t nbdriver_task_work_add_fn;
static struct kprobe nbdriver_sf_probe;
static const char *nbdriver_sf_probe_symbol;
static bool no_work;
static int legacy_error,ni_error,attempts,unregistrations,scheduled,schedule_error;
static int *expected_output;
static unsigned long nbdriver_ksym_lookup(const char *name)
{ assert(!strcmp(name,"task_work_add"));return no_work?0:1; }
static int register_kprobe(struct kprobe *probe)
{
 assert(probe->pre_handler);++attempts;
 if(!strcmp(probe->symbol_name,"__arm64_sys_lookup_dcookie"))return legacy_error;
 assert(!strcmp(probe->symbol_name,"__arm64_sys_ni_syscall"));return ni_error;
}
static void unregister_kprobe(struct kprobe *probe)
{ assert(probe==&nbdriver_sf_probe);++unregistrations; }
static int nbdriver_schedule_fd(int *output)
{ assert(output==expected_output);++scheduled;return schedule_error; }
'''+s+r'''
static void invoke(int number, unsigned long first, unsigned long second)
{
 struct pt_regs call={0},probe={0};call.syscallno=number;
 call.regs[0]=first;call.regs[1]=second;call.regs[3]=(unsigned long)expected_output;
 probe.regs[0]=(unsigned long)&call;assert(!nbdriver_sf_pre(&nbdriver_sf_probe,&probe));
 assert(call.regs[0]==first&&call.regs[1]==second);
}
int main(void)
{
 int output=0;expected_output=&output;
 assert(!nbdriver_sf_entry_init()&&attempts==1);
 assert(!strcmp(nbdriver_sf_entry_target(),"__arm64_sys_lookup_dcookie"));
 invoke(NBDRIVER_INSTALL_SYSCALL,NBDRIVER_INSTALL_MAGIC1,NBDRIVER_INSTALL_MAGIC2);
 assert(scheduled==1);nbdriver_sf_entry_exit();assert(unregistrations==1&&!nbdriver_sf_entry_target());
 legacy_error=-ENOENT;attempts=0;
 assert(!nbdriver_sf_entry_init()&&attempts==2);
 assert(!strcmp(nbdriver_sf_entry_target(),"__arm64_sys_ni_syscall"));
 invoke(0,NBDRIVER_INSTALL_MAGIC1,NBDRIVER_INSTALL_MAGIC2);
 invoke(999,NBDRIVER_INSTALL_MAGIC1,NBDRIVER_INSTALL_MAGIC2);
 invoke(NBDRIVER_INSTALL_SYSCALL,0,NBDRIVER_INSTALL_MAGIC2);
 invoke(NBDRIVER_INSTALL_SYSCALL,NBDRIVER_INSTALL_MAGIC1,0);
 struct pt_regs empty={0};assert(!nbdriver_sf_pre(&nbdriver_sf_probe,&empty));assert(scheduled==1);
 invoke(NBDRIVER_INSTALL_SYSCALL,NBDRIVER_INSTALL_MAGIC1,NBDRIVER_INSTALL_MAGIC2);assert(scheduled==2);
 schedule_error=-ENOMEM;invoke(NBDRIVER_INSTALL_SYSCALL,NBDRIVER_INSTALL_MAGIC1,NBDRIVER_INSTALL_MAGIC2);assert(scheduled==3);
 nbdriver_sf_entry_exit();assert(unregistrations==2&&!nbdriver_sf_entry_target());nbdriver_sf_entry_exit();assert(unregistrations==2);
 ni_error=-ENOENT;attempts=0;assert(nbdriver_sf_entry_init()==-ENOENT&&attempts==2&&!nbdriver_sf_entry_target());
 legacy_error=-EINVAL;attempts=0;assert(nbdriver_sf_entry_init()==-EINVAL&&attempts==1&&!nbdriver_sf_entry_target());
 legacy_error=-EPERM;attempts=0;assert(nbdriver_sf_entry_init()==-EPERM&&attempts==1&&!nbdriver_sf_entry_target());
 legacy_error=-ENOENT;ni_error=-EINVAL;attempts=0;assert(nbdriver_sf_entry_init()==-EINVAL&&attempts==2&&!nbdriver_sf_entry_target());
 no_work=true;attempts=0;assert(nbdriver_sf_entry_init()==-ENOENT&&attempts==0&&!nbdriver_sf_entry_target());
 puts("PASS entry bootstrap: legacy and NI wrappers, syscall18/magic filters, absent targets, restricted probes, work failure and cleanup");
}
'''
with tempfile.TemporaryDirectory(prefix='nbdriver-entry-')as directory:
 p=Path(directory);(p/'check.c').write_text(harness)
 subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=undefined,bounds','-I'+str(root/'include'),str(p/'check.c'),'-o',str(p/'check')],check=True)
 subprocess.run([str(p/'check')],check=True)
