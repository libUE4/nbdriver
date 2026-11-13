// SPDX-License-Identifier: GPL-2.0-only
/* Executes the actual metadata dispatcher with synthetic tasks, VMAs, files
 * and vendor backend. Does not claim real VFS, KGSL or kernel-lock validation. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include "nbdriver_abi.h"
#include "nbdriver_metadata_helpers.h"
typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;
#define __user
#define noinline
#define NBDRIVER_NO_CFI
#define CAP_SYS_PTRACE 0
#define GFP_KERNEL 0
#define STATX_SIZE 0x200
#define AT_STATX_SYNC_AS_STAT 0
#define PATH_MAX 4096
#define NAME_MAX 255
#define PAGE_SHIFT 12
#define PAGE_SIZE 4096UL
#define TASK_SIZE_64 (1UL << 48)
#define VM_READ 1
#define VM_WRITE 2
#define VM_EXEC 4
#define VM_MAYSHARE 8
#define KERNEL_VERSION(a,b,c) (((a)<<16)+((b)<<8)+(c))
#define LINUX_VERSION_CODE TEST_VERSION
#if TEST_VERSION >= KERNEL_VERSION(5,15,0)
#define CONFIG_ANON_VMA_NAME 1
#endif
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define BUILD_BUG_ON(x) _Static_assert(!(x), "build assertion")
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
#define offset_in_page(x) ((x)&(PAGE_SIZE-1))
#define untagged_addr(x) (x)
#define IS_ERR(x) ((uintptr_t)(x)>(uintptr_t)-4096)
#define ERR_PTR(x) ((void *)(intptr_t)(x))
#define PTR_ERR(x) ((long)(intptr_t)(x))
#define MAJOR(x) ((unsigned)major(x))
#define MINOR(x) ((unsigned)minor(x))
struct module { int unused; };
struct kstat { int64_t size; };
struct super_block { dev_t s_dev; };
struct inode { struct super_block *i_sb; unsigned long i_ino; mode_t i_mode; };
struct file;
struct path { const char *text; };
struct file_operations { struct module *owner; long (*unlocked_ioctl)(struct file *,unsigned int,unsigned long); };
struct file { struct path f_path; struct inode *inode; struct file_operations *f_op; void *private_data; };
struct anon_vma_name { const char *name; };
struct vm_area_struct;
struct vm_operations_struct { const char *(*name)(struct vm_area_struct *); };
struct mm_struct { unsigned long brk,start_brk,start_stack; struct vm_area_struct *mmap; };
struct vm_area_struct {
    struct mm_struct *vm_mm;
    unsigned long vm_start,vm_end,vm_flags,vm_pgoff;
    struct file *vm_file;
    const struct vm_operations_struct *vm_ops;
    struct anon_vma_name *anon_name;
    const char *legacy_name;
    struct vm_area_struct *vm_next;
};
struct task_struct { u64 start_boottime; };
static struct task_struct target={42};
static struct mm_struct target_mm;
static struct super_block sb;
static struct inode inode={&sb,123,S_IFREG};
static struct module vendor;
static long fake_ioctl(struct file *f,unsigned int c,unsigned long a)
{ (void)f;(void)c;(void)a;return 0; }
static struct file_operations fops={&vendor,fake_ioctl};
static struct file file={{"/tmp/map\nname\\suffix"},&inode,&fops,(void *)1};
static bool permission=true, backend=true, owner_ok=true, profile_ok=true;
static bool file_valid=true, stat_ok=true, no_mm, gpu_present=true;
static unsigned task_refs,mm_refs,file_refs,locks,rcu_locks,gpu_calls;
static int allocation_budget=-1;
static u64 time_now;
static long fake_gpu(void *p,unsigned int cmd,void *out)
{
    struct nbdriver_gpu_object_info *info=out;
    assert(p==(void *)1 && cmd==0xc0300947U);
    ++gpu_calls;
    if(info->id==999)return -EINVAL;
    info->size=4096;info->gpu_address=0x80000;info->virtual_length=8192;
    return 0;
}
static uint64_t read_once(const void *p,size_t n)
{
    uint64_t value=0;
    uintptr_t offset=(uintptr_t)p-(uintptr_t)fake_gpu;
    if(profile_ok && n==4) {
        if(offset==0)return 0xd503233fU;
        if(offset==16)return 0xb9402841U;
        if(offset==24)return 0xf9400400U;
    }
    memcpy(&value,p,n);return value;
}
#define READ_ONCE(x) ((__typeof__(x))read_once(&(x),sizeof(x)))
static bool capable(int cap){(void)cap;return permission;}
static bool accessible(const void *p){return (uintptr_t)p>=4096;}
static unsigned long copy_from_user(void *o,const void *i,size_t n)
{if(!accessible(i))return n;memcpy(o,i,n);return 0;}
static unsigned long copy_to_user(void *o,const void *i,size_t n)
{if(!accessible(o))return n;memcpy(o,i,n);return 0;}
static void *kmalloc(size_t n,int f)
{(void)f;if(allocation_budget==0)return NULL;if(allocation_budget>0)--allocation_budget;return malloc(n);}
static void *kvmalloc(size_t n,int f){return kmalloc(n,f);}
static void kfree(void *p){free(p);}
static void kvfree(void *p){free(p);}
static struct inode *file_inode(struct file *f){return f->inode;}
static struct task_struct *nbdriver_query_get_process(int pid,u64 expected)
{if(pid!=123)return ERR_PTR(-ESRCH);if(expected&&expected!=42)return ERR_PTR(-ESTALE);++task_refs;return &target;}
static void nbdriver_query_put_process(struct task_struct *t){assert(t==&target&&task_refs);--task_refs;}
static struct mm_struct *get_task_mm(struct task_struct *t)
{assert(t==&target);if(no_mm)return NULL;++mm_refs;return &target_mm;}
static void mmput(struct mm_struct *m){assert(m==&target_mm&&mm_refs);--mm_refs;}
static void mmap_read_lock(struct mm_struct *m){(void)m;assert(!locks);++locks;}
static void mmap_read_unlock(struct mm_struct *m){(void)m;assert(locks==1);--locks;}
static void rcu_read_lock(void){++rcu_locks;}
static void rcu_read_unlock(void){assert(rcu_locks);--rcu_locks;}
static struct file *fget_task(struct task_struct *t,unsigned int fd)
{assert(t==&target);if(!file_valid||fd!=7)return NULL;++file_refs;return &file;}
static void fput(struct file *f){assert(f==&file&&file_refs);--file_refs;}
static int vfs_getattr(const struct path *p,struct kstat *s,u32 mask,unsigned flags)
{assert(p==&file.f_path&&mask==STATX_SIZE&&!flags);if(!stat_ok)return -EACCES;s->size=12345;return 0;}
static struct module *__module_address(unsigned long addr)
{assert(rcu_locks);(void)addr;return owner_ok?&vendor:NULL;}
static unsigned long nbdriver_ksym_lookup_optional(const char *s)
{
    if(!strcmp(s,"fget_task"))return backend?(unsigned long)fget_task:0;
    if(!strcmp(s,"vfs_getattr"))return backend?(unsigned long)vfs_getattr:0;
    if(!strcmp(s,"__module_address"))return (unsigned long)__module_address;
    if(!strcmp(s,"kgsl_ioctl"))return gpu_present?(unsigned long)fake_ioctl:0;
    if(!strcmp(s,"kgsl_ioctl_gpuobj_info"))return gpu_present?(unsigned long)fake_gpu:0;
    abort();
}
static char *d_path(const struct path *p,char *buf,int size)
{assert(locks);if(strlen(p->text)>=(size_t)size)return ERR_PTR(-ENAMETOOLONG);strcpy(buf,p->text);return buf;}
#if TEST_VERSION < KERNEL_VERSION(5,11,0)
static const char *vma_get_anon_name(struct vm_area_struct *v){return v->legacy_name;}
static int nbdriver_read_mm_locked(struct mm_struct *m,struct nbdriver_read_request *r,u8 *out)
{assert(m==&target_mm&&locks);memcpy(out,(void *)(uintptr_t)r->address,r->size);r->copied=r->size;return 0;}
#endif
static struct vm_area_struct *find_vma(struct mm_struct *m,unsigned long a)
{struct vm_area_struct *v;for(v=m->mmap;v&&a>=v->vm_end;v=v->vm_next){}return v;}
#define VMA_ITERATOR(name,mm,start) struct vm_area_struct *name=(mm)->mmap
#define for_each_vma(iter,vma) for((vma)=(iter);(vma);(vma)=(vma)->vm_next)
static int nbdriver_mapping_locked(struct vm_area_struct *v,unsigned long a,struct nbdriver_mapping_request *r)
{assert(locks);r->physical=0x100000+(a&(PAGE_SIZE-1));r->page_size=PAGE_SIZE;r->vma_start=v->vm_start;r->vma_end=v->vm_end;r->flags=NBDRIVER_MAPPING_READ;return 0;}
static void get_random_bytes(void *out,size_t n){memset(out,0x5a,n);}
static void memzero_explicit(void *out,size_t n){memset(out,0,n);}
static u64 ktime_get_ns(void){return ++time_now;}
#include "metadata_dispatch.inc"
static long issue(unsigned cmd,void *arg)
{long ret=nbdriver_metadata_ioctl(cmd,(unsigned long)arg);assert(!task_refs&&!file_refs&&!mm_refs&&!locks&&!rcu_locks);return ret;}
int main(void)
{
    struct anon_vma_name anon={"fixture"};
    char legacy_name[256]="fixture";
    struct vm_area_struct vmas[3]={{0}};
    struct nbdriver_maps_request maps={0};
    struct nbdriver_mapping_request mapping={0};
    struct nbdriver_fd_size_request fd={0};
    struct nbdriver_gpu_info_request gpu={0};
    struct nbdriver_random_request random={0};
    unsigned char bytes[66];char text[4096];
    vmas[0].vm_mm=vmas[1].vm_mm=vmas[2].vm_mm=&target_mm;
    vmas[0].vm_start=0x1000;vmas[0].vm_end=0x2000;vmas[0].vm_flags=VM_READ|VM_MAYSHARE;vmas[0].vm_file=&file;vmas[0].vm_pgoff=2;
    vmas[1].vm_start=0x3000;vmas[1].vm_end=0x4000;vmas[1].vm_flags=VM_READ|VM_WRITE;vmas[1].anon_name=&anon;vmas[1].legacy_name=legacy_name;
    vmas[2].vm_start=0x5000;vmas[2].vm_end=0x6000;target_mm.start_stack=0x5800;
    vmas[0].vm_next=&vmas[1];vmas[1].vm_next=&vmas[2];target_mm.mmap=vmas;
    sb.s_dev=makedev(8,1);
    assert(nbdriver_metadata_init()==0);
    assert((nbdriver_metadata_features()&NBDRIVER_FEATURE_FD_SIZE)!=0);
    maps.pid=123;
    assert(issue(NBDRIVER_IOC_MAPS,&maps)==0&&maps.length>100&&!maps.copied&&maps.expected_start_ns==42);
    memset(text,0x61,sizeof(text));maps.buffer=(uintptr_t)text;maps.capacity=1;
    assert(issue(NBDRIVER_IOC_MAPS,&maps)==-ENOSPC&&maps.error==-ENOSPC&&!maps.copied&&text[0]=='a');
    maps.capacity=sizeof(text);
    assert(issue(NBDRIVER_IOC_MAPS,&maps)==0&&maps.copied==maps.length&&text[maps.length]==0);
    assert(strstr(text,"r--s 00002000 08:01 123 /tmp/map\\012name\\134suffix\n"));
    assert(strstr(text,"[anon:fixture]")&&strstr(text,"[stack]"));
    unsigned lines=0;for(char *p=text;*p;++p)lines+=*p=='\n';assert(lines==3);
    maps.expected_start_ns=43;
    assert(issue(NBDRIVER_IOC_MAPS,&maps)==-ESTALE&&!maps.copied);
    maps.expected_start_ns=0;allocation_budget=0;
    assert(issue(NBDRIVER_IOC_MAPS,&maps)==-ENOMEM);allocation_budget=-1;
    no_mm=true;assert(issue(NBDRIVER_IOC_MAPS,&maps)==-ESRCH);no_mm=false;
    maps.capacity=NBDRIVER_MAPS_MAX+1;assert(issue(NBDRIVER_IOC_MAPS,&maps)==-EINVAL);
    mapping.pid=123;mapping.address=0x1234;
    assert(issue(NBDRIVER_IOC_MAPPING,&mapping)==0&&mapping.physical==0x100234&&mapping.expected_start_ns==42);
    mapping.address=0x2345;assert(issue(NBDRIVER_IOC_MAPPING,&mapping)==-ENOENT&&!mapping.page_size);
    mapping.address=0x1234;mapping.expected_start_ns=43;assert(issue(NBDRIVER_IOC_MAPPING,&mapping)==-ESTALE);
    fd.pid=123;fd.target_fd=7;
    assert(issue(NBDRIVER_IOC_FD_SIZE,&fd)==0&&fd.size==12345&&fd.expected_start_ns==42);
    fd.target_fd=8;assert(issue(NBDRIVER_IOC_FD_SIZE,&fd)==-EBADF&&fd.error==-EBADF&&!fd.size);
    fd.target_fd=7;stat_ok=false;assert(issue(NBDRIVER_IOC_FD_SIZE,&fd)==-EACCES);stat_ok=true;
    gpu.pid=123;gpu.target_fd=7;gpu.object_id=1;
    assert(issue(NBDRIVER_IOC_GPU_INFO,&gpu)==-ENOTTY&&!gpu_calls);
    inode.i_mode=S_IFCHR;
    owner_ok=false;assert(issue(NBDRIVER_IOC_GPU_INFO,&gpu)==-EOPNOTSUPP&&!gpu_calls);owner_ok=true;
    profile_ok=false;assert(issue(NBDRIVER_IOC_GPU_INFO,&gpu)==-EOPNOTSUPP&&!gpu_calls);profile_ok=true;
    assert(issue(NBDRIVER_IOC_GPU_INFO,&gpu)==0&&!gpu.result&&gpu.info.size==4096&&gpu.sample_ns&&gpu_calls==1);
    gpu.object_id=999;assert(issue(NBDRIVER_IOC_GPU_INFO,&gpu)==0&&gpu.result==-EINVAL&&gpu.sample_ns&&gpu_calls==2);
    gpu.expected_start_ns=43;assert(issue(NBDRIVER_IOC_GPU_INFO,&gpu)==-ESTALE&&gpu_calls==2);
    gpu_present=false;gpu.expected_start_ns=0;assert(issue(NBDRIVER_IOC_GPU_INFO,&gpu)==-ENOTTY);gpu_present=true;
    memset(bytes,0xcc,sizeof(bytes));random.buffer=(uintptr_t)(bytes+1);random.size=64;
    assert(issue(NBDRIVER_IOC_RANDOM,&random)==0&&random.copied==64&&bytes[0]==0xcc&&bytes[65]==0xcc&&bytes[1]==0x5a);
    random.buffer=1;assert(issue(NBDRIVER_IOC_RANDOM,&random)==-EFAULT&&!random.copied&&random.error==-EFAULT);
    random.size=65;assert(issue(NBDRIVER_IOC_RANDOM,&random)==-EINVAL);
    permission=false;assert(issue(NBDRIVER_IOC_MAPS,&maps)==-EPERM);assert(issue(NBDRIVER_IOC_RANDOM,&random)==-EPERM);permission=true;
    assert(issue(0,NULL)==-ENOTTY);assert(issue(NBDRIVER_IOC_MAPS,NULL)==-EFAULT);
    backend=false;nbdriver_metadata_init();assert(!(nbdriver_metadata_features()&NBDRIVER_FEATURE_FD_SIZE));assert(issue(NBDRIVER_IOC_FD_SIZE,&fd)==-EOPNOTSUPP);
    nbdriver_metadata_exit();
    puts("PASS: actual metadata dispatcher, maps/escaping/ENOSPC, identity, FD, GPU validation/result, random bounds, cleanup");
    return 0;
}
