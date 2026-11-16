/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * 原生加载器：使用调用方选定的本地模块，校验原件 SHA256、私有副本与 ABI/runtime。
 * 不下载、不自动选择 KMI、不上报服务器，也不提供卸载或无中断模块切换。
 * --release-prefix 为可选字符串预检，--kallsyms 用于显式符号改写，不替换预检路径。
 * 本工具不改系统调用表、不绕过模块签名；内核连接入口由模块自身管理。
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
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "../sdk/include/nbdriver.h"
#include "nbdriver_patch.h"

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

static void hex_digest(const uint8_t d[32], char out[65])
{
	static const char x[]="0123456789abcdef"; unsigned int i;
	for (i=0;i<32;++i) { out[i*2]=x[d[i]>>4]; out[i*2+1]=x[d[i]&15U]; } out[64]='\0';
}

struct stage_paths {
	char dir[PATH_MAX];
	char module[PATH_MAX];
	char symbols[PATH_MAX];
	char vermagic[PATH_MAX];
};

static struct stage_paths staged;
static int stage_cleanup_registered;

static void cleanup_staged_files(void)
{
	if (staged.symbols[0])
		unlink(staged.symbols);
	if (staged.vermagic[0])
		unlink(staged.vermagic);
	if (staged.module[0])
		unlink(staged.module);
	if (staged.dir[0])
		rmdir(staged.dir);
	memset(&staged, 0, sizeof(staged));
}

static int write_all(int fd, const unsigned char *buf, size_t size)
{
	size_t done = 0;

	while (done < size) {
		ssize_t n = write(fd, buf + done, size - done);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (n == 0)
			return -EIO;
		done += (size_t)n;
	}
	return 0;
}

static int stage_verified_module(const char *source, const char *expected,
				 const char *temp_parent, char *digest_text,
				 size_t digest_text_size)
{
	char template[PATH_MAX], got[65];
	struct sha256_ctx hash;
	struct stat st;
	off_t source_size;
	uint64_t copied = 0;
	uint8_t digest[32];
	unsigned char buf[64 * 1024];
	int in_fd = -1, out_fd = -1, rc = 0;

	memset(&staged, 0, sizeof(staged));
	if (snprintf(template, sizeof(template), "%s/nbdriver-loader.XXXXXX",
		     temp_parent) >= (int)sizeof(template))
		return -ENAMETOOLONG;
	if (!mkdtemp(template))
		return -errno;
	strncpy(staged.dir, template, sizeof(staged.dir) - 1);
	if (!stage_cleanup_registered) {
		if (atexit(cleanup_staged_files) != 0) {
			rmdir(staged.dir);
			memset(&staged, 0, sizeof(staged));
			return -ENOMEM;
		}
		stage_cleanup_registered = 1;
	}
	if (snprintf(staged.module, sizeof(staged.module), "%s/module.ko",
		     staged.dir) >= (int)sizeof(staged.module)) {
		rc = -ENAMETOOLONG;
		goto fail;
	}
	in_fd = open(source, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (in_fd < 0) {
		rc = -errno;
		goto fail;
	}
	if (fstat(in_fd, &st) < 0) {
		rc = -errno;
		goto fail;
	}
	if (!S_ISREG(st.st_mode) || st.st_size <= 0) {
		rc = -EINVAL;
		goto fail;
	}
	source_size = st.st_size;
	out_fd = open(staged.module,
		      O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (out_fd < 0) {
		rc = -errno;
		goto fail;
	}
	sha256_init(&hash);
	for (;;) {
		ssize_t n = read(in_fd, buf, sizeof(buf));
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			rc = -errno;
			goto fail;
		}
		if (n == 0)
			break;
		sha256_update(&hash, buf, (size_t)n);
		copied += (uint64_t)n;
		rc = write_all(out_fd, buf, (size_t)n);
		if (rc < 0)
			goto fail;
	}
	if (fstat(in_fd, &st) < 0) {
		rc = -errno;
		goto fail;
	}
	if (st.st_size != source_size || copied != (uint64_t)source_size) {
		rc = -EINVAL;
		goto fail;
	}
	sha256_final(&hash, digest);
	hex_digest(digest, got);
	if (strcasecmp(got, expected)) {
		rc = -EBADMSG;
		goto fail;
	}
	if (fchmod(out_fd, 0600) < 0) {
		rc = -errno;
		goto fail;
	}
	if (close(out_fd) < 0) {
		out_fd = -1;
		rc = -errno;
		goto fail;
	}
	out_fd = -1;
	close(in_fd);
	if (digest_text && digest_text_size)
		snprintf(digest_text, digest_text_size, "%s", got);
	return 0;

fail:
	if (out_fd >= 0)
		close(out_fd);
	if (in_fd >= 0)
		close(in_fd);
	cleanup_staged_files();
	return rc;
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
	fprintf(stderr,"usage: %s --module PATH --sha256 HEX [--release-prefix PREFIX]"
		" [--patch-symbols --patch-symbol NAME ...] [--kallsyms PATH]"
		" [--vermagic-retry]\n",name);
}

static int valid_symbol_name(const char *name)
{
	const unsigned char *p = (const unsigned char *)name;

	if (!name || !*name || strlen(name) > 255)
		return 0;
	for (; *p; ++p)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
		       (*p >= '0' && *p <= '9') || *p == '_' || *p == '.' ||
		       *p == '$'))
			return 0;
	return 1;
}

/*
 * Read kernel log text produced by this load attempt and decide whether the
 * failure was specifically a vermagic rejection. Anything else must not
 * trigger a rewrite: the point of the retry is to be targeted.
 */
static int log_shows_vermagic_mismatch(const char *text)
{
	const char *line = text;

	while (line && *line) {
		const char *end = strchr(line, '\n');
		const char *module = strstr(line, "nbdriver:");
		size_t len = end ? (size_t)(end - line) : strlen(line);

		if (module && (size_t)(module - line) < len) {
			if ((memmem(module, len - (size_t)(module - line), "Unknown symbol",
				    sizeof("Unknown symbol") - 1)) ||
			    memmem(module, len - (size_t)(module - line),
				   "disagrees about version",
				   sizeof("disagrees about version") - 1)) {
				line = end ? end + 1 : NULL;
				continue;
			}
			if (memmem(module, len - (size_t)(module - line), "version magic",
				   sizeof("version magic") - 1) &&
			    (memmem(module, len - (size_t)(module - line), "should be",
				    sizeof("should be") - 1) ||
			     memmem(module, len - (size_t)(module - line), "disagrees",
				    sizeof("disagrees") - 1)))
				return 1;
			if (memmem(module, len - (size_t)(module - line), "vermagic",
				   sizeof("vermagic") - 1) &&
			    memmem(module, len - (size_t)(module - line), "mismatch",
				   sizeof("mismatch") - 1))
				return 1;
		}
		line = end ? end + 1 : NULL;
	}
	return 0;
}

static int open_klog_cursor(void)
{
	int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);

	if (fd < 0)
		return -errno;
	if (lseek(fd, 0, SEEK_END) < 0) {
		int e = errno;
		close(fd);
		return -e;
	}
	return fd;
}

static int collect_klog(int fd, char *out, size_t out_size)
{
	size_t used = 0;
	int seen = 0;

	if (!out || !out_size || fd < 0)
		return -EINVAL;
	out[0] = '\0';
	for (;;) {
		char record[4096];
		ssize_t len = read(fd, record, sizeof(record));
		size_t take;

		if (len < 0 && errno == EINTR)
			continue;
		if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			break;
		if (len < 0)
			return -errno;
		if (len == 0)
			break;
		seen = 1;
		take = (size_t)len;
		if (take > out_size - used - 1)
			take = out_size - used - 1;
		if (take == 0)
			break;
		memcpy(out + used, record, take);
		used += take;
		out[used] = '\0';
		if (take < (size_t)len)
			break;
	}
	return seen ? 0 : -ENODATA;
}

/*
 * Load one image via finit_module. Returns 0 on success, otherwise the
 * negative errno reported by the syscall.
 */
static int load_image(const char *path)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	int rc;

	if (fd < 0)
		return -errno;
#ifdef SYS_finit_module
	rc = (int)syscall(SYS_finit_module, fd, "", 0);
#else
	rc = -1;
	errno = ENOSYS;
#endif
	{
		int saved = errno;
		close(fd);
		if (rc < 0)
			return -saved;
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *module = NULL, *expected = NULL, *prefix = NULL;
	const char *kallsyms_path = "/proc/kallsyms";
	const char *allow_symbols[64];
	size_t allow_count = 0;
	struct utsname u;
	char got[65], detail[PATH_MAX + 256];
	char work[PATH_MAX], retry[PATH_MAX], klog[8192], plog[256];
	int patch_symbols = 0, vermagic_retry = 0;
	int fd = -1, kmsg_fd = -1, rc, i;

	for (i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "--module") && i + 1 < argc)
			module = argv[++i];
		else if (!strcmp(argv[i], "--sha256") && i + 1 < argc)
			expected = argv[++i];
		else if (!strcmp(argv[i], "--release-prefix") && i + 1 < argc)
			prefix = argv[++i];
		else if (!strcmp(argv[i], "--patch-symbols"))
			patch_symbols = 1;
		else if (!strcmp(argv[i], "--patch-symbol") && i + 1 < argc &&
			 valid_symbol_name(argv[i + 1])) {
			if (allow_count == sizeof(allow_symbols) / sizeof(allow_symbols[0])) {
				usage(argv[0]);
				return 2;
			}
			allow_symbols[allow_count++] = argv[++i];
		} else if (!strcmp(argv[i], "--kallsyms") && i + 1 < argc)
			kallsyms_path = argv[++i];
		else if (!strcmp(argv[i], "--vermagic-retry"))
			vermagic_retry = 1;
		else {
			usage(argv[0]);
			return 2;
		}
	}
	if (!module || !valid_hex64(expected) || (patch_symbols && !allow_count) ||
	    (!patch_symbols && allow_count)) {
		usage(argv[0]);
		return 2;
	}
	if (geteuid() != 0) {
		log_line("preflight.fail", "reason=not_root");
		return 77;
	}
	if (uname(&u) < 0) {
		log_line("preflight.fail", "reason=uname");
		return 1;
	}
	if (prefix && strncmp(u.release, prefix, strlen(prefix))) {
		log_line("preflight.fail", "reason=release_mismatch");
		return 78;
	}
	if (check_kallsyms() < 0) {
		log_line("preflight.fail", "reason=required_symbol_missing");
		return 79;
	}

	/*
	 * The digest always covers the artifact as shipped. Every rewrite below
	 * writes to a separate path, so a patched image can never masquerade as
	 * the verified original.
	 */
	rc = stage_verified_module(module, expected, "/data/local/tmp", got,
				   sizeof(got));
	if (rc < 0) {
		if (rc == -EBADMSG) {
			log_line("preflight.fail", "reason=sha256_mismatch");
			return 80;
		}
		snprintf(detail, sizeof(detail), "reason=verified_stage_failed error=%d",
			 -rc);
		log_line("preflight.fail", detail);
		return 1;
	}
	snprintf(detail, sizeof(detail),
		 "release=%s module=%s sha256=%s kptr_restrict=%d",
		 u.release, module, got, read_kptr_restrict());
	log_line("preflight.pass", detail);

	snprintf(work, sizeof(work), "%s", staged.module);

	/* Every absolute import must be individually named by the caller. */
	if (patch_symbols) {
		int patched = 0;

		if (snprintf(staged.symbols, sizeof(staged.symbols),
			     "%s.symabs.%ld", staged.module,
			     (long)getpid()) >= (int)sizeof(staged.symbols)) {
			log_line("patch.fail", "reason=staged_path_too_long");
			return 84;
		}
		rc = nbdriver_patch_symbols(staged.module, staged.symbols, kallsyms_path,
					    allow_symbols, allow_count, &patched,
					    plog, sizeof(plog));
		if (rc < 0) {
			snprintf(detail, sizeof(detail),
				 "reason=symbol_patch_failed detail=%s", plog);
			log_line("patch.fail", detail);
			return 84;
		}
		for (i = 0; (size_t)i < allow_count; ++i)
			log_line("patch.symbol.allow", allow_symbols[i]);
		snprintf(detail, sizeof(detail), "symbols=%d allowlist_count=%zu source=%s",
			 patched, allow_count, kallsyms_path);
		log_line("patch.symbols", detail);
		snprintf(work, sizeof(work), "%s", staged.symbols);
	} else {
		log_line("patch.symbols", "skipped");
	}

	if (vermagic_retry) {
		kmsg_fd = open_klog_cursor();
		if (kmsg_fd < 0)
			log_line("klog", "attempt_cursor=unavailable retry_disabled_on_failure=1");
	}
	rc = load_image(work);
	if (rc == 0) {
		if (kmsg_fd >= 0)
			close(kmsg_fd);
		kmsg_fd = -1;
		log_line("load.pass", "module=nbdriver");
	} else {
		snprintf(detail, sizeof(detail), "errno=%d", -rc);
		log_line("load.fail", detail);

		/*
		 * Step 3: retry only when this attempt's kernel log proves the
		 * rejection was a vermagic mismatch. Rebuilding .modinfo is
		 * pointless for any other error, so the log is the gate.
		 */
		if (!vermagic_retry) {
			return 81;
		}

		if (kmsg_fd < 0 || collect_klog(kmsg_fd, klog, sizeof(klog)) < 0) {
			if (kmsg_fd >= 0)
				close(kmsg_fd);
			log_line("patch.fail", "reason=klog_unavailable");
			return 81;
		}
		close(kmsg_fd);
		kmsg_fd = -1;
		if (!log_shows_vermagic_mismatch(klog)) {
			log_line("patch.vermagic", "skipped reason=not_vermagic");
			return 81;
		}

		if (snprintf(staged.vermagic, sizeof(staged.vermagic),
			     "%s.vermagic.%ld", work, (long)getpid()) >=
		    (int)sizeof(staged.vermagic)) {
			log_line("patch.fail", "reason=retry_path_too_long");
			return 85;
		}
		snprintf(retry, sizeof(retry), "%s", staged.vermagic);
		{
			int changed = 0;

			rc = nbdriver_patch_vermagic(work, retry, u.release,
						     &changed, plog, sizeof(plog));
			if (rc < 0) {
				snprintf(detail, sizeof(detail),
					 "reason=vermagic_patch_failed detail=%s", plog);
				log_line("patch.fail", detail);
				return 85;
			}
			snprintf(detail, sizeof(detail), "magic=%s changed=%d",
				 u.release, changed);
			log_line("patch.vermagic", detail);
		}
		rc = load_image(retry);
		if (rc < 0) {
			snprintf(detail, sizeof(detail), "errno=%d", -rc);
			log_line("load.fail", detail);
			return 81;
		}
		log_line("load.pass", "module=nbdriver vermagic_retry=1");
	}

	fd = nbdriver_open();
	if (fd < 0) {
		snprintf(detail, sizeof(detail), "errno=%d", errno);
		log_line("handshake.fail", detail);
		return 82;
	}
	{
		struct nbdriver_abi_info abi;
		struct nbdriver_runtime_status status;

		if (nbdriver_get_abi(fd, &abi) < 0 ||
		    nbdriver_get_runtime_status(fd, &status) < 0 ||
		    abi.version != NBDRIVER_ABI_VERSION ||
		    !(status.flags & NBDRIVER_RUNTIME_READY)) {
			log_line("handshake.fail", "reason=abi_or_runtime_not_ready");
			close(fd);
			return 83;
		}
		snprintf(detail, sizeof(detail),
			 "abi=%u features=0x%08x handle=%" PRIu64
			 " generation=%" PRIu64 " flags=0x%08x",
			 abi.version, abi.features, status.handle,
			 status.generation, status.flags);
		log_line("ready", detail);
	}
	close(fd);
	return 0;
}
