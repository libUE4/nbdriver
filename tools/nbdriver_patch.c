/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Offline pre-load rewrites for a kernel module image. See nbdriver_patch.h.
 *
 * Layout assumptions are the kernel ABI for arm64 modules: a relocatable
 * Elf64 little-endian object with section headers present. Everything is
 * validated before use; on any mismatch the function fails instead of
 * emitting a half-written image.
 */
#define _GNU_SOURCE
#include "nbdriver_patch.h"

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define VERMAGIC_KEY "vermagic="
#define MODULE_SIG_MARKER "~Module signature appended~\n"

#ifndef EKEYREJECTED
#define EKEYREJECTED EPERM
#endif

static int read_whole(const char *path, unsigned char **out, size_t *size)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	struct stat st;
	unsigned char *buf;
	size_t done = 0;

	if (fd < 0)
		return -errno;
	if (fstat(fd, &st) < 0) {
		int e = -errno;
		close(fd);
		return e;
	}
	if (st.st_size <= 0 || (uintmax_t)st.st_size > SIZE_MAX) {
		close(fd);
		return -EINVAL;
	}
	buf = malloc((size_t)st.st_size);
	if (!buf) {
		close(fd);
		return -ENOMEM;
	}
	while (done < (size_t)st.st_size) {
		ssize_t n = read(fd, buf + done, (size_t)st.st_size - done);
		if (n < 0) {
			int error = errno;

			if (error == EINTR)
				continue;
			free(buf);
			close(fd);
			return error > 0 ? -error : -EIO;
		}
		if (n == 0)
			break;
		done += (size_t)n;
	}
	close(fd);
	if (done != (size_t)st.st_size) {
		free(buf);
		return -EIO;
	}
	*out = buf;
	*size = done;
	return 0;
}

static int write_whole(const char *path, unsigned char *buf, size_t size, int mode)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
		      0600);
	size_t done = 0;

	if (fd < 0)
		return -errno;
	while (done < size) {
		ssize_t n = write(fd, buf + done, size - done);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			{
				int e = -errno;
				close(fd);
				return e;
			}
		}
		if (n == 0) {
			close(fd);
			return -EIO;
		}
		done += (size_t)n;
	}
	if (fchmod(fd, (mode_t)(mode & 0600)) < 0) {
		int e = -errno;
		close(fd);
		return e;
	}
	close(fd);
	return 0;
}

static int range_ok(uint64_t offset, uint64_t length, size_t size)
{
	return offset <= size && length <= (uint64_t)size - offset;
}

static int has_module_signature(const unsigned char *buf, size_t size)
{
	const size_t marker_len = sizeof(MODULE_SIG_MARKER) - 1;

	return size >= marker_len &&
		memcmp(buf + size - marker_len, MODULE_SIG_MARKER, marker_len) == 0;
}

static int elf_ready(const unsigned char *buf, size_t size,
		     const Elf64_Ehdr **eh_out)
{
	const Elf64_Ehdr *eh;

	if (size < sizeof(Elf64_Ehdr))
		return -EINVAL;
	eh = (const Elf64_Ehdr *)buf;
	if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 ||
	    eh->e_ident[EI_VERSION] != EV_CURRENT || eh->e_version != EV_CURRENT)
		return -EINVAL;
	if (eh->e_ident[EI_CLASS] != ELFCLASS64 ||
	    eh->e_ident[EI_DATA] != ELFDATA2LSB)
		return -EINVAL;
	if (eh->e_machine != EM_AARCH64 || eh->e_type != ET_REL ||
	    eh->e_ehsize != sizeof(Elf64_Ehdr))
		return -EINVAL;
	if (eh->e_shentsize != sizeof(Elf64_Shdr) || eh->e_shnum == 0)
		return -EINVAL;
	if (eh->e_shoff == 0 || eh->e_shoff % _Alignof(Elf64_Shdr) != 0 ||
	    !range_ok(eh->e_shoff,
		      (uint64_t)eh->e_shnum * sizeof(Elf64_Shdr), size))
		return -EINVAL;
	if (eh->e_shstrndx == SHN_UNDEF || eh->e_shstrndx >= eh->e_shnum)
		return -EINVAL;
	{
		const Elf64_Shdr *sections = (const Elf64_Shdr *)(buf + eh->e_shoff);
		const Elf64_Shdr *names = &sections[eh->e_shstrndx];
		size_t i;

		if (names->sh_type != SHT_STRTAB ||
		    !range_ok(names->sh_offset, names->sh_size, size))
			return -EINVAL;
		for (i = 0; i < eh->e_shnum; ++i) {
			const Elf64_Shdr *section = &sections[i];

			if (section->sh_name >= names->sh_size ||
			    !memchr(buf + names->sh_offset + section->sh_name, '\0',
				    names->sh_size - section->sh_name) ||
			    (section->sh_addralign &&
			     (section->sh_addralign & (section->sh_addralign - 1))) ||
			    (section->sh_type != SHT_NOBITS && section->sh_type != SHT_NULL &&
			     (!range_ok(section->sh_offset, section->sh_size, size) ||
			      (section->sh_addralign && section->sh_offset % section->sh_addralign))))
				return -EINVAL;
		}
	}
	*eh_out = eh;
	return 0;
}

/*
 * Parse a kallsyms dump into caller-owned arrays.
 *
 * /proc/kallsyms prints "<address> <type> <name>" with the address zeroed
 * when kptr_restrict hides it. Only non-zero addresses are usable here.
 */
struct ksym {
	char *name;
	uint64_t addr;
};

static void free_syms(struct ksym *syms, size_t count)
{
	size_t i;

	for (i = 0; i < count; ++i)
		free(syms[i].name);
	free(syms);
}

static int load_kallsyms(const char *path, struct ksym **out, size_t *count_out,
			 char *log, size_t log_size)
{
	FILE *f = fopen(path, "re");
	struct ksym *syms = NULL;
	size_t count = 0, cap = 0;
	char line[512];

	if (!f) {
		snprintf(log, log_size, "kallsyms_unreadable");
		return -errno;
	}
	while (fgets(line, sizeof(line), f)) {
		char *p = line, *type, *name, *end, *nl;
		uint64_t addr;
		size_t len, type_len;

		if (!strchr(line, '\n') && !feof(f)) {
			int ch;

			while ((ch = fgetc(f)) != '\n' && ch != EOF)
				;
			snprintf(log, log_size, "kallsyms_line_too_long");
			free_syms(syms, count);
			fclose(f);
			return -EOVERFLOW;
		}

		errno = 0;
		addr = strtoull(p, &end, 16);
		if (end == p || errno == ERANGE || (*end != ' ' && *end != '\t')) {
			snprintf(log, log_size, "kallsyms_bad_address");
			free_syms(syms, count);
			fclose(f);
			return -EINVAL;
		}
		p = end;
		while (*p == ' ' || *p == '\t')
			++p;
		type = p;
		while (*p && *p != ' ' && *p != '\t')
			++p;
		type_len = (size_t)(p - type);
		if (type_len != 1 || !*p) {
			snprintf(log, log_size, "kallsyms_bad_type");
			free_syms(syms, count);
			fclose(f);
			return -EINVAL;
		}
		while (*p == ' ' || *p == '\t')
			++p;
		name = p;
		while (*p && *p != ' ' && *p != '\t' && *p != '\n')
			++p;
		if (p == name) {
			snprintf(log, log_size, "kallsyms_missing_name");
			free_syms(syms, count);
			fclose(f);
			return -EINVAL;
		}
		nl = strchr(p, '\n');
		if (nl)
			*nl = '\0';
		/* Module-owned addresses need normal dependency tracking; never turn
		 * them into unreferenced absolute imports. */
		{
			char *owner = p;
			while (*owner == ' ' || *owner == '\t')
				++owner;
			if (*owner == '[')
				continue;
		}
		*p = '\0';
		if (!addr)
			continue;
		len = strlen(name) + 1;
		if (count == cap) {
			size_t next;

			if (cap > SIZE_MAX / 2) {
				free_syms(syms, count);
				fclose(f);
				return -EOVERFLOW;
			}
			next = cap ? cap * 2 : 256;
			if (next > SIZE_MAX / sizeof(*syms)) {
				free_syms(syms, count);
				fclose(f);
				return -EOVERFLOW;
			}
			struct ksym *grown = realloc(syms, next * sizeof(*syms));
			if (!grown) {
				free_syms(syms, count);
				fclose(f);
				return -ENOMEM;
			}
			syms = grown;
			cap = next;
		}
		syms[count].name = malloc(len);
		if (!syms[count].name) {
			free_syms(syms, count);
			fclose(f);
			return -ENOMEM;
		}
		memcpy(syms[count].name, name, len);
		syms[count].addr = addr;
		++count;
	}
	if (ferror(f)) {
		free_syms(syms, count);
		fclose(f);
		return -EIO;
	}
	fclose(f);
	if (!count) {
		free(syms);
		snprintf(log, log_size, "kallsyms_empty");
		return -ENOENT;
	}
	*out = syms;
	*count_out = count;
	return 0;
}

static const struct ksym *find_sym(const struct ksym *syms, size_t count,
					   const char *name, int *ambiguous)
{
	const struct ksym *hit = NULL;
	size_t i;

	*ambiguous = 0;
	for (i = 0; i < count; ++i) {
		if (strcmp(syms[i].name, name) != 0)
			continue;
		if (hit && hit->addr != syms[i].addr) {
			*ambiguous = 1;
			return NULL;
		}
		hit = &syms[i];
	}
	return hit;
}

static size_t allowed_index(const char *name, const char *const *allow,
			    size_t allow_count)
{
	size_t i;

	for (i = 0; i < allow_count; ++i)
		if (allow[i] && strcmp(allow[i], name) == 0)
			return i;
	return SIZE_MAX;
}

int nbdriver_patch_symbols(const char *in_path, const char *out_path,
			   const char *kallsyms_path, const char *const *allow,
			   size_t allow_count, int *patched_out,
			   char *log, size_t log_size)
{
	unsigned char *buf = NULL;
	size_t size = 0;
	const Elf64_Ehdr *eh = NULL;
	struct ksym *syms = NULL;
	size_t sym_count = 0;
	unsigned char *matched = NULL;
	Elf64_Shdr *sh;
	const Elf64_Shdr *symtab;
	const char *strtab;
	size_t sym_entries, i;
	int rc, patched = 0;

	if (patched_out)
		*patched_out = 0;
	if (log && log_size)
		log[0] = '\0';

	if (!allow || !allow_count) {
		snprintf(log, log_size, "empty_symbol_allowlist");
		return -EINVAL;
	}
	rc = read_whole(in_path, &buf, &size);
	if (rc < 0) {
		snprintf(log, log_size, "read_failed");
		return rc;
	}
	if (has_module_signature(buf, size)) {
		snprintf(log, log_size, "signed_module_must_be_resigned");
		free(buf);
		return -EKEYREJECTED;
	}
	rc = elf_ready(buf, size, &eh);
	if (rc < 0) {
		snprintf(log, log_size, "not_arm64_relocatable_elf");
		free(buf);
		return rc;
	}
	rc = load_kallsyms(kallsyms_path, &syms, &sym_count, log, log_size);
	if (rc < 0) {
		free(buf);
		return rc;
	}
	matched = calloc(allow_count, sizeof(*matched));
	if (!matched) {
		free_syms(syms, sym_count);
		free(buf);
		return -ENOMEM;
	}

	sh = (Elf64_Shdr *)(buf + eh->e_shoff);
	symtab = NULL;
	for (i = 0; i < eh->e_shnum; ++i) {
		if (sh[i].sh_type == SHT_SYMTAB) {
			/* Only one .symtab is expected in a module image. */
			if (symtab) {
				symtab = NULL;
				break;
			}
			symtab = &sh[i];
		}
	}
	if (!symtab || symtab->sh_entsize != sizeof(Elf64_Sym) ||
	    symtab->sh_link >= eh->e_shnum) {
		snprintf(log, log_size, "no_symtab");
		free(matched);
		free_syms(syms, sym_count);
		free(buf);
		return -ENOENT;
	}
	if (sh[symtab->sh_link].sh_type != SHT_STRTAB ||
	    symtab->sh_size % sizeof(Elf64_Sym) != 0 ||
	    symtab->sh_offset % _Alignof(Elf64_Sym) != 0 ||
	    !range_ok(sh[symtab->sh_link].sh_offset,
		      sh[symtab->sh_link].sh_size, size) ||
	    !range_ok(symtab->sh_offset, symtab->sh_size, size)) {
		snprintf(log, log_size, "symtab_out_of_range");
		free(matched);
		free_syms(syms, sym_count);
		free(buf);
		return -EINVAL;
	}
	strtab = (const char *)buf + sh[symtab->sh_link].sh_offset;

	sym_entries = symtab->sh_size / sizeof(Elf64_Sym);
	for (i = 1; i < sym_entries; ++i) {
		Elf64_Sym *sym = &((Elf64_Sym *)(buf + symtab->sh_offset))[i];
		const char *name;
		const struct ksym *hit;
		size_t allow_idx;
		int ambiguous;

		/* Only undefined imports are candidates. */
		if (sym->st_shndx != SHN_UNDEF || sym->st_name == 0)
			continue;
		if (sym->st_name >= sh[symtab->sh_link].sh_size) {
			snprintf(log, log_size, "symbol_name_out_of_range");
			free(matched);
			free_syms(syms, sym_count);
			free(buf);
			return -EINVAL;
		}
		name = strtab + sym->st_name;
		if (!memchr(name, '\0', sh[symtab->sh_link].sh_size - sym->st_name)) {
			snprintf(log, log_size, "symbol_name_unterminated");
			free(matched);
			free_syms(syms, sym_count);
			free(buf);
			return -EINVAL;
		}
		if (!*name)
			continue;
		allow_idx = allowed_index(name, allow, allow_count);
		if (allow_idx == SIZE_MAX)
			continue;
		hit = find_sym(syms, sym_count, name, &ambiguous);
		if (!hit) {
			snprintf(log, log_size, "%s", ambiguous ?
				 "allowlisted_symbol_ambiguous" : "allowlisted_symbol_not_in_kallsyms");
			free(matched);
			free_syms(syms, sym_count);
			free(buf);
			return -ENOENT;
		}
		matched[allow_idx] = 1;
		/* Absolute value short-circuits export resolution. */
		sym->st_shndx = SHN_ABS;
		sym->st_value = (Elf64_Addr)hit->addr;
		++patched;
	}
	for (i = 0; i < allow_count; ++i) {
		if (!matched[i]) {
			snprintf(log, log_size, "allowlisted_symbol_not_imported");
			free(matched);
			free_syms(syms, sym_count);
			free(buf);
			return -ENOENT;
		}
	}

	rc = write_whole(out_path, buf, size, 0600);
	free(matched);
	if (rc < 0) {
		snprintf(log, log_size, "%s errno=%d",
			 rc == -EEXIST ? "output_exists" : "write_failed", -rc);
	} else {
		snprintf(log, log_size, "patched=%d", patched);
		if (patched_out)
			*patched_out = patched;
	}
	free_syms(syms, sym_count);
	free(buf);
	return rc;
}

/*
 * Rebuild the .modinfo section content with a replaced vermagic entry.
 *
 * The section is a run of NUL terminated "key=value" strings. Rebuilding
 * keeps every other entry byte-identical and allows a different length.
 * The new body is appended; other section offsets and alignments never move.
 */
int nbdriver_patch_vermagic(const char *in_path, const char *out_path,
			    const char *magic, int *changed_out,
			    char *log, size_t log_size)
{
	unsigned char *buf = NULL;
	size_t size = 0;
	const Elf64_Ehdr *eh = NULL;
	Elf64_Shdr *sh;
	long modinfo = -1;
	size_t i;
	const char *old;
	size_t old_len, new_len, new_total;
	unsigned char *out = NULL;
	char *body = NULL;
	size_t cursor = 0, selected = 0;
	int rc, replaced = 0, changed = 0;

	if (changed_out)
		*changed_out = 0;
	if (log && log_size)
		log[0] = '\0';
	if (!magic || !*magic || strpbrk(magic, "\t\r\n\f\v") ||
	    magic[0] == ' ' || magic[strlen(magic) - 1] == ' ') {
		snprintf(log, log_size, "invalid_release");
		return -EINVAL;
	}
	/* A single token means the traditional uname -r rewrite and preserves the
	 * module's feature suffix. A space-bearing value is an exact, kernel-log-
	 * verified complete vermagic (vendor kernels may require extra tokens). */

	rc = read_whole(in_path, &buf, &size);
	if (rc < 0) {
		snprintf(log, log_size, "read_failed");
		return rc;
	}
	if (has_module_signature(buf, size)) {
		snprintf(log, log_size, "signed_module_must_be_resigned");
		free(buf);
		return -EKEYREJECTED;
	}
	rc = elf_ready(buf, size, &eh);
	if (rc < 0) {
		snprintf(log, log_size, "not_arm64_relocatable_elf");
		free(buf);
		return rc;
	}
	sh = (Elf64_Shdr *)(buf + eh->e_shoff);
	if (sh[eh->e_shstrndx].sh_type != SHT_STRTAB ||
	    !range_ok(sh[eh->e_shstrndx].sh_offset,
		      sh[eh->e_shstrndx].sh_size, size)) {
		snprintf(log, log_size, "section_names_out_of_range");
		free(buf);
		return -EINVAL;
	}
	for (i = 0; i < eh->e_shnum; ++i) {
		const Elf64_Shdr *str = &sh[eh->e_shstrndx];
		const char *name;
		size_t left;

		if (sh[i].sh_name >= str->sh_size)
			goto bad_section_name;
		name = (const char *)buf + str->sh_offset + sh[i].sh_name;
		left = (size_t)(str->sh_size - sh[i].sh_name);
		if (!memchr(name, '\0', left))
			goto bad_section_name;
		if (strcmp(name, ".modinfo") == 0) {
			if (modinfo >= 0) {
				snprintf(log, log_size, "duplicate_modinfo");
				free(buf);
				return -EINVAL;
			}
			modinfo = (long)i;
		}
	}
	if (modinfo < 0) {
		snprintf(log, log_size, "no_modinfo");
		free(buf);
		return -ENOENT;
	}
	if (sh[modinfo].sh_type != SHT_PROGBITS ||
	    (sh[modinfo].sh_flags & SHF_COMPRESSED) ||
	    !range_ok(sh[modinfo].sh_offset, sh[modinfo].sh_size, size)) {
		snprintf(log, log_size, "modinfo_out_of_range");
		free(buf);
		return -EINVAL;
	}

	{
		const uint64_t start = sh[modinfo].sh_offset;
		const uint64_t end = start + sh[modinfo].sh_size;
		const uint64_t table_end = eh->e_shoff +
			(uint64_t)eh->e_shnum * sizeof(Elf64_Shdr);

		if (start < sizeof(Elf64_Ehdr) || (start < table_end && end > eh->e_shoff)) {
			free(buf);
			return -EINVAL;
		}
		for (i = 0; i < eh->e_shnum; ++i) {
			if (i == (size_t)modinfo || sh[i].sh_type == SHT_NOBITS || !sh[i].sh_size)
				continue;
			if (sh[i].sh_offset < end && sh[i].sh_offset + sh[i].sh_size > start) {
				snprintf(log, log_size, "section_overlaps_modinfo");
				free(buf);
				return -EINVAL;
			}
		}
	}

	old = (const char *)buf + sh[modinfo].sh_offset;
	old_len = sh[modinfo].sh_size;

	/* Find the existing vermagic entry inside the section body. */
	for (cursor = 0; cursor < old_len;) {
		const char *entry = old + cursor;
		size_t entry_len = strnlen(entry, old_len - cursor);

		if (entry_len == old_len - cursor) {
			/* Missing terminator means a malformed section. */
			snprintf(log, log_size, "modinfo_malformed");
			free(buf);
			return -EINVAL;
		}
		if (entry_len >= strlen(VERMAGIC_KEY) &&
		    memcmp(entry, VERMAGIC_KEY, strlen(VERMAGIC_KEY)) == 0) {
			if (replaced || entry_len == strlen(VERMAGIC_KEY)) {
				snprintf(log, log_size, "invalid_or_duplicate_vermagic");
				free(buf);
				return -EINVAL;
			}
			replaced = 1;
			selected = cursor;
		}
		cursor += entry_len + 1;
	}
	if (!replaced) {
		snprintf(log, log_size, "no_vermagic");
		free(buf);
		return -ENOENT;
	}

	cursor = selected;
	if (strlen(magic) > SIZE_MAX - strlen(VERMAGIC_KEY) - 1) {
		free(buf);
		return -EOVERFLOW;
	}
	{
		size_t body_len;

		/* The no_vermagic guard above guarantees a selected entry here. */
		const size_t old_entry_len = strnlen(old + cursor, old_len - cursor);
		const char *value = old + cursor + strlen(VERMAGIC_KEY);
		const size_t value_len = old_entry_len - strlen(VERMAGIC_KEY);
		const int exact = strpbrk(magic, " ") != NULL;
		const char *suffix = exact ? NULL : memchr(value, ' ', value_len);
		const size_t suffix_len = suffix ? old_entry_len -
			(size_t)(suffix - (old + cursor)) : 0;
		const size_t prefix_len = strlen(VERMAGIC_KEY);
		char *replacement;

		if (strlen(magic) > SIZE_MAX - prefix_len - suffix_len - 1) {
			free(buf);
			return -EOVERFLOW;
		}
		new_len = prefix_len + strlen(magic) + suffix_len + 1;
		if (new_len > SIZE_MAX - (old_len - old_entry_len - 1)) {
			free(buf);
			return -EOVERFLOW;
		}
		body_len = old_len - (old_entry_len + 1) + new_len;
		body = malloc(body_len ? body_len : 1);
		if (!body) {
			free(buf);
			return -ENOMEM;
		}
		replacement = malloc(new_len);
		if (!replacement) {
			free(body);
			free(buf);
			return -ENOMEM;
		}
		memcpy(replacement, VERMAGIC_KEY, prefix_len);
		memcpy(replacement + prefix_len, magic, strlen(magic));
		if (suffix_len)
			memcpy(replacement + prefix_len + strlen(magic), suffix,
			       suffix_len);
		replacement[new_len - 1] = '\0';
		memcpy(body, old, cursor);
		memcpy(body + cursor, replacement, new_len);
		memcpy(body + cursor + new_len,
		       old + cursor + old_entry_len + 1,
		       old_len - (cursor + old_entry_len + 1));
		changed = new_len != old_entry_len + 1 ||
			memcmp(old + cursor, replacement, new_len) != 0;
		free(replacement);

		/* Append only the rebuilt metadata. All existing section offsets,
		 * alignment, relocation targets and the section table stay intact. */
		{
			const uint64_t limit = 16U * 1024U * 1024U;
			const uint64_t alignment = sh[modinfo].sh_addralign ?
				sh[modinfo].sh_addralign : 1;
			uint64_t offset;
			Elf64_Shdr *n_sh;

			if (size > limit || alignment > limit) {
				free(body);
				free(buf);
				return -EFBIG;
			}
			offset = ((uint64_t)size + alignment - 1) & ~(alignment - 1);
			if (!range_ok(offset, body_len, (size_t)limit)) {
				free(body);
				free(buf);
				return -EFBIG;
			}
			new_total = (size_t)offset + body_len;
			out = calloc(1, new_total);
			if (!out) {
				free(body);
				free(buf);
				return -ENOMEM;
			}
			memcpy(out, buf, size);
			memcpy(out + offset, body, body_len);
			n_sh = (Elf64_Shdr *)(out + eh->e_shoff);
			n_sh[modinfo].sh_offset = offset;
			n_sh[modinfo].sh_size = body_len;
		}
	}

	rc = write_whole(out_path, out, new_total, 0600);
	if (rc < 0) {
		snprintf(log, log_size, "%s errno=%d",
			 rc == -EEXIST ? "output_exists" : "write_failed", -rc);
	} else {
		snprintf(log, log_size, "vermagic=%s replaced=%d", magic, replaced);
		if (changed_out)
			*changed_out = changed;
	}
	free(out);
	free(body);
	free(buf);
	return rc;

bad_section_name:
	snprintf(log, log_size, "section_name_unterminated");
	free(buf);
	return -EINVAL;
}
