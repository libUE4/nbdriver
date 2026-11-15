// SPDX-License-Identifier: GPL-2.0-only
/* Executes production decoder/backend with synthetic kernel adapters.
 * Does not emulate real kernel scheduling, CFI, vendor locks or GPU hardware. */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "nbdriver_abi.h"
typedef uint8_t u8;typedef uint32_t u32;typedef uint64_t u64;typedef int64_t s64;typedef int32_t s32;typedef unsigned gfp_t;
#define __user
#define noinline
#define NBDRIVER_NO_CFI
#define CONFIG_COMPAT 1
#define MAX_ERRNO 4095
#define GFP_KERNEL 0
#define KSYM_NAME_LEN 512
#define BITS_PER_LONG 64
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#define READ_ONCE(x) (x)
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
#define BUILD_BUG_ON(x) _Static_assert(!(x),"build assertion")
#define IS_ERR(p) ((uintptr_t)(p)>(uintptr_t)-4096)
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define ERR_PTR(e) ((void *)(intptr_t)(e))
struct module {unsigned refs;bool going;};
static struct module vendor,foreign;
static bool owner_ok=true,symbols_present=true,info_present=true,bad_copy;
static unsigned preempt,task_refs,file_refs,spin_depth,rcu_depth,gpu_calls;
static int allocations=-1;
static void *kmalloc(size_t n,int f){(void)f;if(allocations==0)return NULL;if(allocations>0)--allocations;return malloc(n);}
static void *kzalloc(size_t n,int f){void *p=kmalloc(n,f);if(p)memset(p,0,n);return p;}
static void kfree(void *p){free(p);}
static void preempt_disable(void){++preempt;}
static void preempt_enable(void){assert(preempt);--preempt;}
static bool try_module_get(struct module *m){if(!m)return true;if(m->going)return false;m->refs++;return true;}
static void module_put(struct module *m){if(m){assert(m->refs);m->refs--;}}
typedef unsigned spinlock_t;
#define DEFINE_MUTEX(n) int n
static void mutex_lock(int *m){assert(!*m);*m=1;}
static void mutex_unlock(int *m){assert(*m);*m=0;}
static void spin_lock(spinlock_t *l){assert(!*l);*l=1;spin_depth++;}
static void spin_unlock(spinlock_t *l){assert(*l&&spin_depth);*l=0;spin_depth--;}
#define spin_lock_irqsave(l,f) do {(f)=0;spin_lock(l);}while(0)
#define spin_unlock_irqrestore(l,f) do {(void)(f);spin_unlock(l);}while(0)
static void rcu_read_lock(void){rcu_depth++;}
static void rcu_read_unlock(void){assert(rcu_depth);rcu_depth--;}
#define rcu_dereference_raw(x) (x)
struct file;
struct file_operations {struct module *owner;long (*unlocked_ioctl)(struct file *,unsigned int,unsigned long);};
struct inode {unsigned i_mode;};
struct file {struct file_operations *f_op;struct inode *inode;void *private_data;};
struct fdtable {unsigned max_fds;struct file **fd;};
struct files_struct {spinlock_t file_lock;struct fdtable table;};
struct task_struct {u64 start_boottime;struct files_struct *files;};
static struct task_struct task;
static struct inode inode={S_IFCHR};
static long fake_ioctl(struct file *f,unsigned int c,unsigned long a){(void)f;(void)c;(void)a;return 0;}
static struct file_operations fops={&vendor,fake_ioctl};
static struct file file={&fops,&inode,NULL};
static struct file *fds[8];
static struct files_struct files={.table={8,fds}};
static void task_lock(struct task_struct *t){assert(t==&task);}
static void task_unlock(struct task_struct *t){assert(t==&task);}
static struct fdtable *files_fdtable(struct files_struct *f){return &f->table;}
static void get_file(struct file *f){assert(f==&file);file_refs++;}
static void fput(struct file *f){assert(f==&file&&file_refs);file_refs--;}
static struct inode *file_inode(struct file *f){return f->inode;}
static struct task_struct *nbdriver_query_get_process(int pid,u64 expected){if(pid!=123)return ERR_PTR(-ESRCH);if(expected&&expected!=42)return ERR_PTR(-ESTALE);task_refs++;return &task;}
static void nbdriver_query_put_process(struct task_struct *t){assert(t==&task&&task_refs);task_refs--;}
static unsigned long copy_from_user(void *to,const void *from,size_t n){if((uintptr_t)from<4096)return n;memcpy(to,from,n);return 0;}
static unsigned long copy_to_user(void *to,const void *from,size_t n){if((uintptr_t)to<4096)return n;memcpy(to,from,n);return 0;}
static u64 clock_value;
static u64 ktime_get_ns(void){return ++clock_value;}
#define IDR_RT_MARKER 3U
#define XA_CHUNK_SHIFT 6U
#define XA_CHUNK_SIZE 64U
#define XA_FREE_MARK 0
#define XAS_RESTART ((void *)1)
struct xarray {gfp_t xa_flags;void *xa_head;};
struct idr {struct xarray idr_rt;unsigned idr_base,idr_next;};
struct xa_node {struct xa_node *parent;struct xarray *array;u8 shift,offset,count,nr_values;};
static struct xa_node node;
struct xa_state {struct xarray *array;unsigned long xa_index;void *xa_node;};
#define XA_STATE(n,a,i) struct xa_state n={a,i,NULL}
static u64 allocated[64];
static bool xa_is_retry(void *p){return p==(void *)3;}
static bool xa_is_node(void *p){return p==&node;}
static struct xa_node *xa_to_node(void *p){return p;}
static void *xas_find_marked(struct xa_state *s,unsigned long limit,int mark){(void)mark;assert(rcu_depth);while(s->xa_index<=4096&&s->xa_index<=limit){unsigned long id=s->xa_index;if(!id||!(allocated[(id-1)/64]&(1ULL<<((id-1)%64))))return NULL;s->xa_index++;}if(s->xa_index>limit)s->xa_node=XAS_RESTART;return NULL;}
static bool xas_retry(struct xa_state *s,void *p){(void)s;(void)p;return false;}
static int xas_error(struct xa_state *s){(void)s;return 0;}
static struct {unsigned char padding[64];struct idr idr;spinlock_t lock;} process;
static struct {void *device;void *process;} device;
static u32 create_code[40],destroy_code[40],jump_code[4];
static unsigned create_len,destroy_len;
static bool cfi_identity,cfi_info;
static int vendor_return;
static long fake_info(void *p,unsigned int c,void *out){struct nbdriver_gpu_object_info *i=out;assert(p==&device&&c==_IOWR(9,0x47,struct nbdriver_gpu_object_info));gpu_calls++;if(vendor_return)return vendor_return;i->size=4096;i->reserved=0xdead;return i->id==999?-EINVAL:0;}
static void __attribute__((aligned(4))) idr_alloc_stub(void){}
static void __attribute__((aligned(4))) idr_find_stub(void){}
static void __attribute__((aligned(4))) spin_stub(void){}
static int fake_nofault(void *to,const void *from,size_t n){if(bad_copy||(uintptr_t)from<4096)return -EFAULT;memcpy(to,from,n);return 0;}
static int fake_size(unsigned long a,unsigned long *size,unsigned long *offset){*offset=0;if(a==(unsigned long)create_code)*size=create_len*4;else if(a==(unsigned long)destroy_code)*size=destroy_len*4;else if(a==(unsigned long)jump_code)*size=8;else return 0;return 1;}
static struct module *fake_module(unsigned long a){(void)a;assert(preempt);return owner_ok?&vendor:&foreign;}
static bool xas_symbol_present=true;
static unsigned long nbdriver_ksym_lookup_optional(const char *s){
 if(!strcmp(s,"xas_find_marked"))return xas_symbol_present?(unsigned long)xas_find_marked:0;
 if(!strcmp(s,"copy_from_kernel_nofault"))return (unsigned long)fake_nofault;
 if(!strcmp(s,"kallsyms_lookup_size_offset"))return (unsigned long)fake_size;
 if(!strcmp(s,"__module_address"))return (unsigned long)fake_module;
 if(!symbols_present)return 0;
 if(!strcmp(s,"kgsl_fops"))return cfi_identity?0:(unsigned long)&fops;
 if(!strcmp(s,"kgsl_ioctl"))return cfi_identity?0:(unsigned long)fake_ioctl;
 if(!strcmp(s,"kgsl_ioctl.cfi_jt"))return cfi_identity?(unsigned long)fake_ioctl:0;
 if(!strcmp(s,"kgsl_ioctl_gpuobj_info"))return info_present&&!cfi_info?(unsigned long)fake_info:0;
 if(!strcmp(s,"kgsl_ioctl_gpuobj_info.cfi_jt"))return info_present&&cfi_info?(unsigned long)fake_info:0;
 if(!strcmp(s,"kgsl_ioctl_syncsource_create"))return (unsigned long)create_code;
 if(!strcmp(s,"kgsl_ioctl_syncsource_destroy"))return (unsigned long)destroy_code;
 if(!strcmp(s,"idr_alloc"))return (unsigned long)idr_alloc_stub;
 if(!strcmp(s,"idr_find"))return (unsigned long)idr_find_stub;
 if(!strcmp(s,"_raw_spin_lock"))return (unsigned long)spin_stub;
 return 0;
}
static long strscpy(char *to,const char *from,size_t n){size_t len=strlen(from);if(len>=n)return -E2BIG;memcpy(to,from,len+1);return (long)len;}
static void nbdriver_kgsl_visibility_init(void){}
static void nbdriver_kgsl_visibility_exit(void){}
static u32 nbdriver_kgsl_visibility_features(void){return 0;}
#include "kgsl_production.inc"
static u32 branch(u32 *pc,unsigned long to){intptr_t diff=(intptr_t)to-(intptr_t)pc;assert(!(diff&3)&&diff>=-(1<<27)&&diff<(1<<27));return 0x94000000U|((u32)(diff/4)&0x03ffffff);}
static u32 add(unsigned offset){assert(offset<4096);return 0x91000000U|(offset<<10)|(19U<<5);}
static void make_shape(unsigned prefix){unsigned n=0;memset(create_code,0,sizeof(create_code));memset(destroy_code,0,sizeof(destroy_code));for(unsigned i=0;i<prefix;i++){create_code[n]=destroy_code[n]=i==1?0xf800865eU:0xd503245fU;n++;}
 create_code[n]=destroy_code[n]=0xf9400413U;n++;
 create_code[n]=destroy_code[n]=add(64+sizeof(struct idr));n++;
 create_code[n]=branch(&create_code[n],(unsigned long)spin_stub);destroy_code[n]=branch(&destroy_code[n],(unsigned long)spin_stub);n++;
 create_code[n]=destroy_code[n]=add(64);n++;
 create_code[n++]=0x52800022U;create_code[n++]=0x52800003U;
 create_code[n]=branch(&create_code[n],(unsigned long)idr_alloc_stub);create_code[++n]=0xd65f03c0U;create_len=n+1;
 n=prefix+4;destroy_code[n]=branch(&destroy_code[n],(unsigned long)idr_find_stub);destroy_code[++n]=0xd65f03c0U;destroy_len=n+1;
}
static void reset_backend(void){nbdriver_kgsl_exit();assert(!vendor.refs);nbdriver_kgsl_init();}
static void no_refs(void){assert(!task_refs&&!file_refs&&!preempt&&!spin_depth&&!rcu_depth);}
int main(void){
 struct nbdriver_gpu_info_request info={.pid=123,.target_fd=7,.object_id=9};
 struct nbdriver_gpu_sync_request sync={.pid=123,.target_fd=7,.max_id=130};
 struct kgsl_layout layout;u32 stage;unsigned calls;
 task.start_boottime=42;task.files=&files;fds[7]=&file;device.process=&process;file.private_data=&device;
 process.idr.idr_rt.xa_flags=IDR_RT_MARKER;process.idr.idr_rt.xa_head=&node;node.array=&process.idr.idr_rt;node.count=1;
 make_shape(0);nbdriver_kgsl_init();
 assert((nbdriver_kgsl_features()&(NBDRIVER_FEATURE_GPU_INFO|NBDRIVER_FEATURE_GPU_SYNC))==(NBDRIVER_FEATURE_GPU_INFO|NBDRIVER_FEATURE_GPU_SYNC));
 assert(!nbdriver_kgsl_info_ioctl((unsigned long)&info)&&info.result==0&&info.info.size==4096&&!info.info.reserved&&info.expected_start_ns==42);
 assert(vendor.refs==1);no_refs();
 info.object_id=999;assert(!nbdriver_kgsl_info_ioctl((unsigned long)&info)&&info.result==-EINVAL&&!info.info.size);no_refs();
 vendor_return=1;assert(nbdriver_kgsl_info_ioctl((unsigned long)&info)==-EPROTO&&!info.info.id);vendor_return=0;
 info.expected_start_ns=43;calls=gpu_calls;assert(nbdriver_kgsl_info_ioctl((unsigned long)&info)==-ESTALE&&gpu_calls==calls);info.expected_start_ns=0;
 info.target_fd=6;assert(nbdriver_kgsl_info_ioctl((unsigned long)&info)==-EBADF);info.target_fd=7;
 inode.i_mode=S_IFREG;assert(nbdriver_kgsl_info_ioctl((unsigned long)&info)==-ENOTTY);inode.i_mode=S_IFCHR;
 reset_backend();owner_ok=false;assert(nbdriver_kgsl_info_ioctl((unsigned long)&info)==-EOPNOTSUPP&&!vendor.refs);owner_ok=true;
 vendor.going=true;assert(nbdriver_kgsl_info_ioctl((unsigned long)&info)==-ENODEV);vendor.going=false;
 cfi_identity=cfi_info=true;assert(!nbdriver_kgsl_info_ioctl((unsigned long)&info));reset_backend();cfi_identity=cfi_info=false;
 info_present=false;assert(nbdriver_kgsl_info_ioctl((unsigned long)&info)==-EOPNOTSUPP);info_present=true;reset_backend();
 assert(!kgsl_resolve(&file,true,&layout,NULL,&stage)&&layout.process_priv_offset==8&&layout.syncsource_idr_offset==64);
 allocated[0]=1ULL|(1ULL<<63);allocated[1]=1ULL; /* IDs 1,64,65; ID 65 models allocated NULL. */
 assert(!nbdriver_kgsl_sync_ioctl((unsigned long)&sync)&&sync.backend==1&&sync.occupied_count==3&&sync.first_free==2&&sync.occupied[0]==allocated[0]&&sync.occupied[1]==allocated[1]&&!sync.error&&!sync.fail_stage);no_refs();
 xas_symbol_present=false;reset_backend();
 assert(!(nbdriver_kgsl_features()&NBDRIVER_FEATURE_GPU_SYNC));
 assert(nbdriver_kgsl_features()&NBDRIVER_FEATURE_GPU_INFO);
 {u32 free_id=123;assert(kgsl_find_lowest_free_once(&process.idr,1,&free_id)==-EOPNOTSUPP&&free_id==123);}
 assert(nbdriver_kgsl_sync_ioctl((unsigned long)&sync)==-EOPNOTSUPP&&!sync.occupied_count&&!sync.occupied[0]);no_refs();
 assert(!nbdriver_kgsl_info_ioctl((unsigned long)&info));no_refs();
 xas_symbol_present=true;reset_backend();
 assert(nbdriver_kgsl_features()&NBDRIVER_FEATURE_GPU_SYNC);
 assert(!nbdriver_kgsl_sync_ioctl((unsigned long)&sync)&&sync.first_free==2&&sync.occupied_count==3);no_refs();
 sync.expected_start_ns=43;assert(nbdriver_kgsl_sync_ioctl((unsigned long)&sync)==-ESTALE&&!sync.occupied_count&&!sync.occupied[0]);sync.expected_start_ns=0;
 sync.reserved2=1;assert(nbdriver_kgsl_sync_ioctl((unsigned long)&sync)==-EINVAL);sync.reserved2=0;
 sync.max_id=4097;assert(nbdriver_kgsl_sync_ioctl((unsigned long)&sync)==-EINVAL);sync.max_id=130;
 process.idr.idr_base=1;assert(nbdriver_kgsl_sync_ioctl((unsigned long)&sync)==-EOPNOTSUPP&&sync.fail_stage==NBDRIVER_GPU_STAGE_IDR);process.idr.idr_base=0;
 for(unsigned prefix=0;prefix<=3;prefix++){reset_backend();make_shape(prefix);assert(!kgsl_resolve(&file,true,&layout,NULL,&stage));}
 reset_backend();make_shape(0);destroy_code[3]=add(128);assert(kgsl_resolve(&file,true,&layout,NULL,&stage)==-EOPNOTSUPP&&stage==NBDRIVER_GPU_STAGE_LAYOUT);
 reset_backend();make_shape(0);create_code[4]=0x52800042U;assert(kgsl_resolve(&file,true,&layout,NULL,&stage)==-EOPNOTSUPP); /* native start must equal 1 */
 reset_backend();make_shape(0);create_code[2]=branch(&create_code[2],(unsigned long)idr_find_stub);assert(kgsl_resolve(&file,true,&layout,NULL,&stage)==-EOPNOTSUPP);
 reset_backend();make_shape(0);bad_copy=true;assert(kgsl_resolve(&file,true,&layout,NULL,&stage)<0);bad_copy=false;
 reset_backend();make_shape(0);allocations=0;assert(nbdriver_kgsl_sync_ioctl((unsigned long)&sync)==-ENOMEM);allocations=-1;
 symbols_present=false;reset_backend();assert(!(nbdriver_kgsl_features()&NBDRIVER_FEATURE_GPU_INFO));assert(nbdriver_kgsl_info_ioctl((unsigned long)&info)==-EOPNOTSUPP);symbols_present=true;
 nbdriver_kgsl_exit();assert(!vendor.refs);no_refs();
 puts("PASS: actual KGSL backend; INFO, CFI identity, owner lifetime, stale PID/FD, vendor errors, bounded bitmaps including reserved NULL, decoder shifts/cross-checks/native bounds/call targets, unsupported and allocation failures");return 0;
}
