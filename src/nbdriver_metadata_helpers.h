/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_METADATA_HELPERS_H
#define NBDRIVER_METADATA_HELPERS_H
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <stddef.h>
#include <string.h>
#endif
#include "nbdriver_abi.h"

struct nbdriver_maps_output {
	char *bytes;
	size_t capacity, used;
};

/* Count all text, but never expose a partial snapshot on ENOSPC. */
static inline int nbdriver_maps_append(struct nbdriver_maps_output *out,
				       const char *text, size_t length)
{
	if (length >= NBDRIVER_MAPS_MAX || out->used >= NBDRIVER_MAPS_MAX - length)
		return -EOVERFLOW;
	if (out->used + length < out->capacity)
		memcpy(out->bytes + out->used, text, length);
	out->used += length;
	return 0;
}

/* Octal escapes keep control characters from forging additional records. */
static inline int nbdriver_maps_escape(char *out, size_t capacity, const char *name)
{
	size_t used = 0;

	while (*name) {
		unsigned char ch = (unsigned char)*name++;
		if (ch == '\n' || ch == '\r' || ch == '\\') {
			if (capacity - used <= 4)
				return -EOVERFLOW;
			out[used++] = '\\';
			out[used++] = (char)('0' + (ch >> 6));
			out[used++] = (char)('0' + ((ch >> 3) & 7));
			out[used++] = (char)('0' + (ch & 7));
		} else {
			if (capacity - used <= 1)
				return -EOVERFLOW;
			out[used++] = (char)ch;
		}
	}
	if (used >= capacity)
		return -EOVERFLOW;
	out[used] = 0;
	return 0;
}

/* Retain the known implementation profile from NBG-driver. Other vendor
 * builds need a separately verified profile, not a guessed private layout. */
static inline int nbdriver_kgsl_profile_matches(const nbdriver_u32 code[7])
{
	return code[0] == 0xd503233fU && code[4] == 0xb9402841U &&
		code[6] == 0xf9400400U;
}
#endif
