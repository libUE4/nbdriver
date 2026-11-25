/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Offline pre-load rewrites for a kernel module image.
 *
 * Both rewrites operate on a copy of the .ko; the original file and its
 * SHA256 stay untouched so the digest check keeps proving what was shipped.
 */
#ifndef NBDRIVER_PATCH_H
#define NBDRIVER_PATCH_H

#include <stddef.h>

/* Complete kernel vermagic values can end in spaces. Preserve those bytes:
 * vendor kernels use both "aarch64 vivo " and "vivo aarch64". Reject log
 * delimiters and control characters instead of trimming valid metadata. */
#define NBDRIVER_VERMAGIC_MAX 1024U
static inline int nbdriver_valid_vermagic(const char *magic)
{
	size_t i;

	if (!magic || !*magic || *magic == ' ')
		return 0;
	for (i = 0; i <= NBDRIVER_VERMAGIC_MAX; ++i) {
		unsigned char c = (unsigned char)magic[i];

		if (!c)
			return 1;
		if (c < ' ' || c == '\'' || c == 127)
			return 0;
	}
	return 0;
}

/*
 * Rewrite only explicitly allowlisted undefined symbols to absolute
 * addresses taken from a kallsyms dump. This bypasses normal export/KMI
 * checks; callers must validate each symbol's ABI for the target kernel.
 *
 * A symbol whose Elf64_Sym.st_shndx is SHN_UNDEF forces the kernel loader to
 * resolve it through the export table. Setting st_shndx to SHN_ABS and
 * st_value to the live address makes the loader treat it as already defined,
 * which covers functions that exist but carry no EXPORT_SYMBOL.
 *
 * No other imported symbols are modified. An empty allowlist is rejected.
 * Returns 0 on success and fills @patched_out with the number of rewritten
 * symbols plus a human readable @log.
 */
int nbdriver_patch_symbols(const char *in_path, const char *out_path,
			   const char *kallsyms_path, const char *const *allow,
			   size_t allow_count, int *patched_out,
			   char *log, size_t log_size);

/*
 * Replace the release token in the .modinfo "vermagic=" entry with @magic,
 * preserving the original space-separated feature/architecture suffix when
 * @magic is a single token. If @magic contains spaces, use it as the exact
 * complete vermagic string (only after verifying it against the kernel log).
 * Trailing spaces in a complete string are significant and retained.
 * The section is rebuilt
 * rather than truncated, so the new string may be longer or shorter than the
 * original; other section offsets stay unchanged. Returns 0 on success and sets @changed_out to 1 when the entry
 * actually differed.
 */
int nbdriver_patch_vermagic(const char *in_path, const char *out_path,
			    const char *magic, int *changed_out,
			    char *log, size_t log_size);

#endif /* NBDRIVER_PATCH_H */
