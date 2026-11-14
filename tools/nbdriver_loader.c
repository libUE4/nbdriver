/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Small native loader for an authorized nbdriver build.  It deliberately
 * performs selection, digest verification and a post-load ABI handshake; it
 * does not patch syscall tables or bypass module-signature enforcement.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/limits.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "../sdk/include/nbdriver.h"

struct sha256_ctx {
	uint32_t h[8];
	uint64_t bits;
	uint8_t block[64];
	size_t used;
};

static const uint32_t sha_k[64] = {
	0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
	0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
	0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
	0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
	0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
	0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
	0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
	0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U
};

static uint32_t rotr(uint32_t x, unsigned int n) { return (x >> n) | (x << (32U - n)); }
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void put_be32(uint8_t *p, uint32_t x) { p[0]=(uint8_t)(x>>24); p[1]=(uint8_t)(x>>16); p[2]=(uint8_t)(x>>8); p[3]=(uint8_t)x; }

static void sha256_block(struct sha256_ctx *c, const uint8_t *p)
{
	uint32_t w[64], a, b, d, e, f, g, h, t1, t2, cc;
	unsigned int i;
	for (i=0; i<16; ++i) w[i]=be32(p+i*4);
	for (; i<64; ++i) { uint32_t s0=rotr(w[i-15],7)^rotr(w[i-15],18)^(w[i-15]>>3); uint32_t s1=rotr(w[i-2],17)^rotr(w[i-2],19)^(w[i-2]>>10); w[i]=w[i-16]+s0+w[i-7]+s1; }
	a=c->h[0]; b=c->h[1]; cc=c->h[2]; d=c->h[3]; e=c->h[4]; f=c->h[5]; g=c->h[6]; h=c->h[7];
	for (i=0; i<64; ++i) { uint32_t S1=rotr(e,6)^rotr(e,11)^rotr(e,25); uint32_t ch=(e&f)^((~e)&g); t1=h+S1+ch+sha_k[i]+w[i]; uint32_t S0=rotr(a,2)^rotr(a,13)^rotr(a,22); uint32_t maj=(a&b)^(a&cc)^(b&cc); t2=S0+maj; h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2; }
	c->h[0]+=a; c->h[1]+=b; c->h[2]+=cc; c->h[3]+=d; c->h[4]+=e; c->h[5]+=f; c->h[6]+=g; c->h[7]+=h;
}

static void sha256_init(struct sha256_ctx *c)
{
	static const uint32_t init[8]={0x6a09e667U,0xbb67ae85U,0x3c6ef372U,0xa54ff53aU,0x510e527fU,0x9b05688cU,0x1f83d9abU,0x5be0cd19U};
	memcpy(c->h, init, sizeof(init)); c->bits=0; c->used=0;
}

static void sha256_update(struct sha256_ctx *c, const uint8_t *p, size_t n)
{
	c->bits += (uint64_t)n * 8U;
	while (n) { size_t take=64U-c->used; if (take>n) take=n; memcpy(c->block+c->used,p,take); c->used+=take; p+=take; n-=take; if (c->used==64U) { sha256_block(c,c->block); c->used=0; } }
}

static void sha256_final(struct sha256_ctx *c, uint8_t out[32])
{
	uint64_t bits=c->bits; size_t i;
	uint8_t one=0x80U, zero=0;
	sha256_update(c,&one,1);
	while (c->used != 56U) sha256_update(c,&zero,1);
	uint8_t len[8]; for (i=0;i<8;++i) len[7-i]=(uint8_t)(bits>>(i*8)); sha256_update(c,len,8);
	for (i=0;i<8;++i) put_be32(out+i*4,c->h[i]);
}

static int hash_file(const char *path, uint8_t digest[32])
{
	struct sha256_ctx c; uint8_t buf[64*1024]; ssize_t n; int fd=open(path,O_RDONLY|O_CLOEXEC);
	if (fd < 0)
		return -errno;
	sha256_init(&c);
	for (;;) { n=read(fd,buf,sizeof(buf)); if (n==0) break; if (n<0) { int e=errno; close(fd); return -e; } sha256_update(&c,buf,(size_t)n); }
	close(fd); sha256_final(&c,digest); return 0;
}

static void hex_digest(const uint8_t d[32], char out[65])
{
	static const char x[]="0123456789abcdef"; unsigned int i;
	for (i=0;i<32;++i) { out[i*2]=x[d[i]>>4]; out[i*2+1]=x[d[i]&15U]; } out[64]='\0';
}

static int valid_hex64(const char *s)
{
	unsigned int i; if (!s || strlen(s)!=64U) return 0;
	for (i=0;i<64;++i) if (!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')||(s[i]>='A'&&s[i]<='F'))) return 0;
	return 1;
}

static void log_line(const char *event, const char *detail)
{
	fprintf(stderr,"nbdriver-loader event=%s %s\n",event,detail?detail:"");
}

static int check_kallsyms(void)
{
	static const char *const required[]={"register_kprobe","kallsyms_lookup_name","__arm64_sys_reboot"};
	char line[512]; unsigned int found=0; FILE *f=fopen("/proc/kallsyms","re");
	if (!f) return -errno;
	while (fgets(line,sizeof(line),f)) { unsigned int i; for(i=0;i<sizeof(required)/sizeof(required[0]);++i) if(strstr(line,required[i])) found|=1U<<i; }
	fclose(f); return found==7U?0:-ENOENT;
}

static int read_kptr_restrict(void)
{
	FILE *f = fopen("/proc/sys/kernel/kptr_restrict", "re");
	int value = -1;
	if (f) {
		(void)fscanf(f, "%d", &value);
		fclose(f);
	}
	return value;
}

static void usage(const char *name)
{
	fprintf(stderr,"usage: %s --module PATH --sha256 HEX [--release-prefix PREFIX]\n",name);
}

int main(int argc, char **argv)
{
	const char *module=NULL,*expected=NULL,*prefix=NULL; struct utsname u; uint8_t digest[32]; char got[65], detail[PATH_MAX+128]; int fd=-1, rc, i;
	for(i=1;i<argc;++i) { if(!strcmp(argv[i],"--module")&&i+1<argc) module=argv[++i]; else if(!strcmp(argv[i],"--sha256")&&i+1<argc) expected=argv[++i]; else if(!strcmp(argv[i],"--release-prefix")&&i+1<argc) prefix=argv[++i]; else { usage(argv[0]); return 2; } }
	if(!module||!valid_hex64(expected)) { usage(argv[0]); return 2; }
	if(geteuid()!=0) { log_line("preflight.fail","reason=not_root"); return 77; }
	if(uname(&u)<0) { log_line("preflight.fail","reason=uname"); return 1; }
	if(prefix && strncmp(u.release,prefix,strlen(prefix))) { log_line("preflight.fail","reason=release_mismatch"); return 78; }
	if(check_kallsyms()<0) { log_line("preflight.fail","reason=required_symbol_missing"); return 79; }
	rc=hash_file(module,digest); if(rc<0) { snprintf(detail,sizeof(detail),"reason=hash_read error=%d",-rc); log_line("preflight.fail",detail); return 1; }
	hex_digest(digest,got); if(strcasecmp(got,expected)) { log_line("preflight.fail","reason=sha256_mismatch"); return 80; }
	snprintf(detail,sizeof(detail),"release=%s module=%s sha256=%s kptr_restrict=%d",u.release,module,got,read_kptr_restrict()); log_line("preflight.pass",detail);
	fd=open(module,O_RDONLY|O_CLOEXEC); if(fd<0) { log_line("load.fail","reason=open"); return 1; }
#ifdef SYS_finit_module
	rc=(int)syscall(SYS_finit_module,fd,"",0);
#else
	rc=-1; errno=ENOSYS;
#endif
	close(fd); if(rc<0) { snprintf(detail,sizeof(detail),"errno=%d",errno); log_line("load.fail",detail); return 81; }
	log_line("load.pass","module=nbdriver");
	fd=nbdriver_open(); if(fd<0) { snprintf(detail,sizeof(detail),"errno=%d",errno); log_line("handshake.fail",detail); return 82; }
	struct nbdriver_abi_info abi; struct nbdriver_runtime_status status;
	if(nbdriver_get_abi(fd,&abi)<0 || nbdriver_get_runtime_status(fd,&status)<0 || abi.version!=NBDRIVER_ABI_VERSION || !(status.flags&NBDRIVER_RUNTIME_READY)) { log_line("handshake.fail","reason=abi_or_runtime_not_ready"); close(fd); return 83; }
	snprintf(detail,sizeof(detail),"abi=%u features=0x%08x handle=%" PRIu64 " generation=%" PRIu64 " flags=0x%08x",abi.version,abi.features,status.handle,status.generation,status.flags); log_line("ready",detail);
	close(fd); return 0;
}
