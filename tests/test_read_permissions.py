#!/usr/bin/env python3
"""Exercise the actual kernel read loop with bounded synthetic VMAs/pages."""
from pathlib import Path
import subprocess,tempfile,os
root=Path(__file__).resolve().parents[1]
s=(root/'src/nbdriver_core.c').read_text()
s=s[s.index('static int nbdriver_read_mm_inner('):s.index('\nint nbdriver_read_mm_locked(')]
harness=r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "nbdriver_abi.h"
typedef unsigned char u8;
#define KERNEL_VERSION(a,b,c) (((a)<<16)|((b)<<8)|(c))
#define LINUX_VERSION_CODE TEST_VERSION
#define PAGE_SIZE (1UL<<12)
#define VM_READ 1UL
#define VM_WRITE 2UL
#define VM_EXEC 4UL
#define VM_IO 8UL
#define VM_PFNMAP 16UL
#define VM_MIXEDMAP 32UL
#define VM_HUGETLB 64UL
#define READ_ONCE(x) (x)
#define untagged_addr(x) ((x)&0x00ffffffffffffffUL)
#define offset_in_page(x) ((x)&(PAGE_SIZE-1))
#define min_t(type,a,b) ((type)(a)<(type)(b)?(type)(a):(type)(b))
struct mm_struct{int dummy;};
struct vm_area_struct{unsigned long vm_start,vm_end,vm_flags;};
struct page{unsigned char bytes[PAGE_SIZE];int refs;};
static struct vm_area_struct regions[2];
static struct page pages[2];
static unsigned long* nbdriver_zero_pfn;
static bool present=true,hole=false,ram_uncached=true,map_ok=true;
static int ram_error,ram_calls,vmap_calls,vunmap_calls;
static int fault_calls=0,walk_calls=0,failures=0;
static struct vm_area_struct* find_vma(struct mm_struct* mm,unsigned long at){
    (void)mm;
    if(at<regions[0].vm_end)return &regions[0];
    return hole?NULL:at<regions[1].vm_end?&regions[1]:NULL;
}
static int nbdriver_walk_get_page(struct vm_area_struct* vma,unsigned long at,bool write,
                                  bool allow_zero,unsigned long zero,struct page** out){
    (void)at;(void)allow_zero;(void)zero;assert(!write);++walk_calls;
    if(!present)return -ENODATA;
    *out=&pages[vma==&regions[1]];++(*out)->refs;return 0;
}
#define VM_MAP 1
#define PAGE_KERNEL 0
#define pgprot_writecombine(x) ((x)+7)
static int nbdriver_walk_get_ram_page(struct vm_area_struct* vma,unsigned long at,struct page** out,bool* uncached){
 ++ram_calls;if(ram_error)return ram_error;*uncached=ram_uncached;
 return nbdriver_walk_get_page(vma,at,false,false,0,out);
}
static void* vmap(struct page** p,unsigned count,unsigned flags,unsigned prot){
 assert(count==1&&flags==VM_MAP&&prot==7&&(*p)->refs>0);++vmap_calls;return map_ok?(*p)->bytes:NULL;
}
static void vunmap(void* p){assert(p);++vunmap_calls;}
static int nbdriver_fault_in_page(struct mm_struct* mm,unsigned long at){(void)mm;(void)at;++fault_calls;present=true;return 0;}
static void* kmap_local_page(struct page* page){return page->bytes;}
static void kunmap_local(void* p){(void)p;}
static void* kmap_atomic(struct page* page){return page->bytes;}
static void kunmap_atomic(void* p){(void)p;}
static void nbdriver_walk_put_page(struct page* page){assert(page->refs>0);--page->refs;}
#include "read_loop.inc"
static void check(bool ok,const char* label){printf("%s %s\n",ok?"PASS":"FAIL",label);failures+=!ok;}
static void reset(void){
    regions[0]=(struct vm_area_struct){0x10000,0x11000,VM_READ|VM_WRITE};
    regions[1]=(struct vm_area_struct){0x11000,0x12000,VM_READ|VM_WRITE};
    memset(pages,0,sizeof(pages));memset(pages[0].bytes,0x5a,PAGE_SIZE);memset(pages[1].bytes,0x6b,PAGE_SIZE);
    present=true;hole=false;fault_calls=walk_calls=0;ram_error=ram_calls=vmap_calls=vunmap_calls=0;ram_uncached=map_ok=true;
}
int main(void){
    struct mm_struct mm={0};unsigned char out[32];int result;
    struct nbdriver_read_request r;
    reset();regions[0].vm_flags=VM_WRITE;
    r=(struct nbdriver_read_request){.address=0x10010,.size=16};memset(out,0xcc,sizeof(out));
    result=nbdriver_read_mm_inner(&mm,&r,out,true);
    check(result==0&&r.copied==16&&out[0]==0x5a&&out[15]==0x5a&&out[16]==0xcc,"resident write-only VMA is read without changing permissions");
    check(regions[0].vm_flags==VM_WRITE&&pages[0].bytes[0]==0x5a&&!pages[0].refs,"read preserves VMA, source bytes and page references");
    reset();regions[1].vm_flags=VM_WRITE;
    r=(struct nbdriver_read_request){.address=0x10ff8,.size=16};
    result=nbdriver_read_mm_inner(&mm,&r,out,true);
    check(!result&&r.copied==16&&out[7]==0x5a&&out[8]==0x6b&&!pages[0].refs&&!pages[1].refs,"cross-page read enters write-only VMA safely");
    reset();regions[1].vm_flags=0;memset(out,0xcc,sizeof(out));
    r=(struct nbdriver_read_request){.address=0x10ff8,.size=16};result=nbdriver_read_mm_inner(&mm,&r,out,true);
    check(result==-EACCES&&r.copied==8&&out[8]==0xcc&&!pages[0].refs,"PROT_NONE still rejects with exact partial length");
    reset();regions[0].vm_flags=VM_EXEC;
    r=(struct nbdriver_read_request){.address=0x10010,.size=16};result=nbdriver_read_mm_inner(&mm,&r,out,true);
    check(result==-EACCES&&!r.copied&&!walk_calls,"execute-only VMA is not silently made readable");
    reset();regions[0].vm_flags=VM_READ|VM_HUGETLB;
    r=(struct nbdriver_read_request){.address=0x10010,.size=16};result=nbdriver_read_mm_inner(&mm,&r,out,true);
    check(result==-EOPNOTSUPP&&!r.copied&&!walk_calls&&!ram_calls,"hugetlb remains unsupported");
    for(unsigned long bit=VM_IO;bit<=VM_MIXEDMAP;bit<<=1){
      reset();regions[0].vm_flags=VM_WRITE|bit;ram_error=-EOPNOTSUPP;
      r=(struct nbdriver_read_request){.address=0x10010,.size=16};result=nbdriver_read_mm_inner(&mm,&r,out,true);
      check(result==-EOPNOTSUPP&&!r.copied&&ram_calls==1&&!walk_calls&&!vmap_calls,"special VMA requires successful normal-RAM leaf validation");
    }
    reset();regions[0].vm_flags=VM_READ|VM_PFNMAP;
    r=(struct nbdriver_read_request){.address=0x10010,.size=16};result=nbdriver_read_mm_inner(&mm,&r,out,true);
    check(!result&&r.copied==16&&out[0]==0x5a&&vmap_calls==1&&vunmap_calls==1&&!pages[0].refs&&!fault_calls,"resident Normal-NC RAM uses matching temporary mapping and releases reference");
    reset();regions[0].vm_flags=VM_READ|VM_IO;map_ok=false;
    r=(struct nbdriver_read_request){.address=0x10010,.size=16};result=nbdriver_read_mm_inner(&mm,&r,out,true);
    check(result==-ENOMEM&&!r.copied&&!pages[0].refs&&vmap_calls==1&&!vunmap_calls,"failed mapping allocation releases the pinned page");
    reset();regions[0].vm_flags=VM_READ|VM_IO;ram_error=-ENODATA;
    r=(struct nbdriver_read_request){.address=0x10010,.size=16};result=nbdriver_read_mm_inner(&mm,&r,out,true);
    check(result==-ENODATA&&!r.copied&&!fault_calls&&!walk_calls&&!vmap_calls,"nonresident GPU/device mapping never faults in pages");
    reset();regions[0].vm_flags=VM_READ|VM_IO;ram_uncached=false;
    r=(struct nbdriver_read_request){.address=0x10010,.size=16};result=nbdriver_read_mm_inner(&mm,&r,out,true);
    check(!result&&r.copied==16&&!vmap_calls&&!pages[0].refs,"validated cached RAM keeps the ordinary mapping path");
    reset();present=false;r=(struct nbdriver_read_request){.address=0x10010,.size=16};
    result=nbdriver_read_mm_inner(&mm,&r,out,true);
    check(!result&&r.copied==16&&fault_calls==1&&walk_calls==2&&!pages[0].refs,"cold page faults once then reacquires and releases page");
    reset();present=false;r=(struct nbdriver_read_request){.address=0x10010,.size=16};
    result=nbdriver_read_mm_inner(&mm,&r,out,false);
    check(result==-ENODATA&&!r.copied&&!fault_calls,"no-fault path remains no-fault");
    reset();hole=true;r=(struct nbdriver_read_request){.address=0x10ff8,.size=16};
    result=nbdriver_read_mm_inner(&mm,&r,out,true);
    check(result==-EFAULT&&r.copied==8&&!pages[0].refs,"unmapped hole preserves partial read contract");
    reset();r=(struct nbdriver_read_request){.address=0xab00000000010010ULL,.size=16};
    result=nbdriver_read_mm_inner(&mm,&r,out,true);
    check(!result&&r.copied==16&&out[0]==0x5a,"tagged pointer behavior preserved");
    return failures?1:0;
}
'''
with tempfile.TemporaryDirectory(prefix='nbdriver-read-permissions-') as d:
 p=Path(d);(p/'read_loop.inc').write_text(s);(p/'test.c').write_text(harness)
 for version in (0x050a00,0x060600):
  subprocess.run([os.environ.get('CC','clang-18'),'-std=gnu11','-O1','-Wall','-Wextra','-Werror','-Wno-unused-function','-DTEST_VERSION='+str(version),'-I'+str(root/'include'),'-I'+str(p),str(p/'test.c'),'-o',str(p/'test')],check=True)
  subprocess.run([str(p/'test')],check=True)
