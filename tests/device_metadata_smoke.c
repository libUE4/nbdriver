// SPDX-License-Identifier: GPL-2.0-only
/* Self-owned fixtures only. KGSL ABI follows OnePlusOSS graphics-kernel's
 * include/uapi/linux/msm_kgsl.h; source hashes are in the validation archive. */
#define _GNU_SOURCE
#include "nbdriver.h"
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#ifndef PR_SET_VMA
#define PR_SET_VMA 0x53564d41
#define PR_SET_VMA_ANON_NAME 0
#endif
static unsigned failures, skipped;
static void check(int ok, const char *name)
{
    printf("%s: %s",ok?"PASS":"FAIL",name);
    if(!ok){printf(" (errno=%d: %s)",errno,strerror(errno));++failures;}
    putchar('\n');
}
static void skip(const char *name,int required)
{
    printf("%s: %s (errno=%d: %s)\n",required?"FAIL":"SKIP",name,errno,strerror(errno));
    if(required)++failures;else ++skipped;
}
/* Native KGSL UAPI payloads: alloc 48 bytes, free 32 bytes, info 48 bytes. */
struct gpu_alloc { uint64_t size,flags,va_len,mmapsize;uint32_t id,metadata_len;uint64_t metadata; };
struct gpu_free { uint64_t flags,priv;uint32_t id,type,len,padding; };
_Static_assert(sizeof(struct gpu_alloc)==48,"KGSL alloc ABI");
_Static_assert(sizeof(struct gpu_free)==32,"KGSL free ABI");
static void gpu_test(int driver_fd,int pid,nbdriver_u64 start,int required)
{
    struct gpu_alloc alloc;
    struct gpu_free release;
    struct nbdriver_gpu_object_info direct;
    struct nbdriver_gpu_info_request request;
    int fd=open("/dev/kgsl-3d0",O_RDWR|O_CLOEXEC),ret;
    if(fd<0){skip("KGSL device unavailable",required);return;}
    memset(&alloc,0,sizeof(alloc));alloc.size=4096;
    if(ioctl(fd,_IOWR(9,0x45,struct gpu_alloc),&alloc)<0){skip("KGSL fixture allocation",required);close(fd);return;}
    memset(&direct,0,sizeof(direct));direct.id=alloc.id;
    ret=ioctl(fd,_IOWR(9,0x47,struct nbdriver_gpu_object_info),&direct);
    check(ret==0,"native KGSL GPUOBJ_INFO fixture");
    if(ret==0){
        ret=nbdriver_read_gpu_object_info(driver_fd,pid,fd,alloc.id,start,&request);
        if(ret<0&&errno==EOPNOTSUPP){skip("KGSL implementation profile unsupported",required);}
        else {
            check(ret==0&&request.result==0&&request.sample_ns&&
                request.expected_start_ns==start&&request.info.id==direct.id&&
                request.info.gpu_address==direct.gpu_address&&request.info.size==direct.size&&
                request.info.flags==direct.flags&&request.info.virtual_length==direct.virtual_length&&
                request.info.virtual_address==direct.virtual_address,
                "driver GPU metadata matches native KGSL");
            if(ret==0&&request.result==0){
                ret=nbdriver_read_gpu_object_info(driver_fd,pid,fd,UINT32_MAX,start,&request);
                check(ret==0&&request.result<0&&request.sample_ns,"GPU invalid object preserves vendor error");
            }
        }
    }
    memset(&release,0,sizeof(release));release.id=alloc.id;
    check(ioctl(fd,_IOW(9,0x46,struct gpu_free),&release)==0,"release KGSL fixture");
    close(fd);
}
static int maps_contains(const char *maps,uintptr_t address,const char *permissions,uint64_t offset)
{
    const char *line=maps;
    while(*line){
        unsigned long long start,end,file_offset;char perms[5];
        if(sscanf(line,"%llx-%llx %4s %llx",&start,&end,perms,&file_offset)==4&&
            address>=start&&address<end)return !strcmp(perms,permissions)&&file_offset==offset;
        line=strchr(line,'\n');if(!line)break;++line;
    }
    return 0;
}
int main(int argc,char **argv)
{
    int required=argc==2&&!strcmp(argv[1],"--require-gpu");
    int driver_fd=-1,fixture_fd=-1,pid=(int)getpid(),ret;
    long page=sysconf(_SC_PAGESIZE);
    uint64_t fixture_size;
    nbdriver_u64 start=0,size=0;
    struct nbdriver_abi_info abi;
    struct nbdriver_maps_request maps;
    struct nbdriver_mapping_request mapping,adjacent;
    struct nbdriver_fd_size_request bad_fd;
    unsigned char random_a[66],random_b[64];
    char *text=NULL,*anon=MAP_FAILED,*file_map=MAP_FAILED;
    char temporary[PATH_MAX],renamed[PATH_MAX];
    const char *directory;
    uint32_t needed;
    if(argc>2||(argc==2&&!required)){fprintf(stderr,"usage: %s [--require-gpu]\n",argv[0]);return 2;}
#ifdef __ANDROID__
    directory="/data/local/tmp";
#else
    directory="/tmp";
#endif
    setbuf(stdout,NULL);
    driver_fd=nbdriver_open();
    if(driver_fd<0){perror("nbdriver_open");return 1;}
    memset(&abi,0,sizeof(abi));
    check(nbdriver_get_abi(driver_fd,&abi)==0&&abi.version==NBDRIVER_ABI_VERSION,"driver ABI");
    needed=NBDRIVER_FEATURE_MAPS|NBDRIVER_FEATURE_MAPPING|NBDRIVER_FEATURE_FD_SIZE|NBDRIVER_FEATURE_RANDOM;
    if((abi.features&needed)!=needed){fprintf(stderr,"missing required features: 0x%x\n",needed&~abi.features);failures++;goto cleanup;}
    if(nbdriver_get_process_start_time(driver_fd,pid,&start)<0||!start){check(0,"self process identity");goto cleanup;}
    if(page<=0){check(0,"system page size");goto cleanup;}
    fixture_size=3*(uint64_t)page+123;
    snprintf(temporary,sizeof(temporary),"%s/nbdriver-meta-XXXXXX",directory);
    fixture_fd=mkstemp(temporary);
    if(fixture_fd<0){check(0,"create fixture file");goto cleanup;}
    snprintf(renamed,sizeof(renamed),"%s/nbdriver-meta-%d\nfixture",directory,pid);
    ret=rename(temporary,renamed);
    if(ret<0){unlink(temporary);check(0,"rename fixture with newline");goto cleanup;}
    unlink(renamed); /* Deleted pathname is retained by the open/mapped file. */
    if(ftruncate(fixture_fd,(off_t)fixture_size)<0){check(0,"resize fixture");goto cleanup;}
    file_map=mmap(NULL,(size_t)page,PROT_READ,MAP_SHARED,fixture_fd,(off_t)page);
    anon=mmap(NULL,(size_t)page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(file_map==MAP_FAILED||anon==MAP_FAILED){check(0,"map self-owned fixtures");goto cleanup;}
#ifdef MADV_NOHUGEPAGE
    (void)madvise(anon,(size_t)page*2,MADV_NOHUGEPAGE);
#endif
    anon[0]=0x5a;
    ret=prctl(PR_SET_VMA,PR_SET_VMA_ANON_NAME,(unsigned long)anon,(unsigned long)page*2,"nbdriver-meta-fixture");
    if(ret<0)skip("anonymous mapping names unsupported by this kernel",0);
    int named=ret==0;
    text=malloc(NBDRIVER_MAPS_MAX);
    if(!text){check(0,"allocate maps output");goto cleanup;}
    memset(&maps,0,sizeof(maps));maps.pid=pid;maps.expected_start_ns=start;
    check(nbdriver_process_maps(driver_fd,&maps)==0&&maps.length>0&&!maps.copied&&maps.expected_start_ns==start,"maps required-length query");
    char sentinel='X';maps.buffer=(uintptr_t)&sentinel;maps.capacity=1;
    check(nbdriver_process_maps(driver_fd,&maps)<0&&errno==ENOSPC&&maps.error==-ENOSPC&&maps.length>1&&!maps.copied&&sentinel=='X',"maps ENOSPC without truncated output");
    maps.buffer=(uintptr_t)text;maps.capacity=NBDRIVER_MAPS_MAX;
    ret=nbdriver_process_maps(driver_fd,&maps);
    check(ret==0&&maps.copied==maps.length&&maps.length<maps.capacity,"complete maps snapshot");
    if(ret==0){
        check(text[maps.length]==0&&maps_contains(text,(uintptr_t)file_map,"r--s",(uint64_t)page)&&strstr(text,"\\012fixture (deleted)"),"file maps permissions, offset and escaped path");
        check(maps_contains(text,(uintptr_t)anon,"rw-p",0),"anonymous mapping range");
        if(named)check(strstr(text,"[anon:nbdriver-meta-fixture]")!=NULL,"anonymous mapping name");
    }
    maps.expected_start_ns=start+1;
    check(nbdriver_process_maps(driver_fd,&maps)<0&&errno==ESTALE&&!maps.copied,"maps reject stale process identity");
    ret=nbdriver_query_mapping(driver_fd,pid,(uintptr_t)(anon+37),start,&mapping);
    check(ret==0&&mapping.page_size>=(uint64_t)page&&mapping.physical%page==37&&
        mapping.vma_start<=(uintptr_t)anon&&mapping.vma_end>(uintptr_t)anon&&
        (mapping.flags&(NBDRIVER_MAPPING_READ|NBDRIVER_MAPPING_WRITE))==(NBDRIVER_MAPPING_READ|NBDRIVER_MAPPING_WRITE),"resident page mapping/permissions");
    if(ret==0)check(nbdriver_query_mapping(driver_fd,pid,(uintptr_t)(anon+38),start,&adjacent)==0&&adjacent.physical==mapping.physical+1,"physical offset within same page");
    check(nbdriver_query_mapping(driver_fd,pid,(uintptr_t)(anon+page),start,&mapping)<0&&errno==ENODATA&&!mapping.physical,"mapping query does not fault untouched page in");
    check(nbdriver_query_mapping(driver_fd,pid,(uintptr_t)anon,start+1,&mapping)<0&&errno==ESTALE,"mapping rejects stale identity");
    check(nbdriver_get_file_descriptor_size(driver_fd,pid,fixture_fd,start,&size)==0&&size==fixture_size,"FD size matches fixture");
    memset(&bad_fd,0,sizeof(bad_fd));bad_fd.pid=pid;bad_fd.target_fd=INT_MAX;bad_fd.expected_start_ns=start;
    check(nbdriver_ioctl(driver_fd,NBDRIVER_IOC_FD_SIZE,&bad_fd)<0&&errno==EBADF&&bad_fd.error==-EBADF&&!bad_fd.size,"invalid target FD");
    check(nbdriver_get_file_descriptor_size(driver_fd,pid,fixture_fd,start+1,&size)<0&&errno==ESTALE&&!size,"FD query rejects stale identity");
    memset(random_a,0xcc,sizeof(random_a));
    check(nbdriver_read_random(driver_fd,random_a+1,64)==64&&random_a[0]==0xcc&&random_a[65]==0xcc,"64 random bytes with buffer boundaries");
    check(nbdriver_read_random(driver_fd,random_b,64)==64&&memcmp(random_a+1,random_b,64)!=0,"two random requests");
    check(nbdriver_read_random(driver_fd,random_b,65)<0&&errno==EINVAL,"random size limit");
    if(abi.features&NBDRIVER_FEATURE_GPU_INFO){
        struct nbdriver_gpu_info_request gpu;
        check(nbdriver_read_gpu_object_info(driver_fd,pid,fixture_fd,1,start,&gpu)<0&&errno==ENOTTY,"GPU query rejects ordinary files");
        gpu_test(driver_fd,pid,start,required);
    }else{errno=EOPNOTSUPP;skip("KGSL backend absent",required);}
cleanup:
    free(text);
    if(anon!=MAP_FAILED)munmap(anon,(size_t)page*2);
    if(file_map!=MAP_FAILED)munmap(file_map,(size_t)page);
    if(fixture_fd>=0)close(fixture_fd);
    nbdriver_close(driver_fd);
    printf("RESULT failures=%u skipped=%u\n",failures,skipped);
    return failures?1:0;
}
