/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_HWBP_RULES_H
#define NBDRIVER_HWBP_RULES_H
#include "nbdriver_abi.h"

/* Shared by the real register writer and host boundary tests. */
static inline int nbd_hwb_shape(nbdriver_u64 address, nbdriver_u32 type,
    nbdriver_u32 length, nbdriver_u64 *base, nbdriver_u64 *control)
{
    nbdriver_u64 bytes;
    if (!address || type < NBDRIVER_HWBP_R || type > NBDRIVER_HWBP_X)
        return 0;
    if (type == NBDRIVER_HWBP_X) {
        if (length != 4 || (address & 3)) return 0;
        *base = address;
        *control = 0x1e5U; /* E=1, PMC=EL0, BAS=1111, old NBG-driver */
        return 1;
    }
    if (length != 1 && length != 2 && length != 4 && length != 8) return 0;
    if ((address & 7) + length > 8) return 0;
    bytes = ((1ULL << length) - 1) << (address & 7);
    *base = address & ~7ULL;
    *control = 1ULL | (2ULL << 1) | ((nbdriver_u64)type << 3) | (bytes << 5);
    return 1;
}

static inline nbdriver_u64 nbd_hwb_distance(nbdriver_u64 fault,
    nbdriver_u64 address, nbdriver_u32 length)
{
    if (fault < address) return address - fault;
    if (fault - address >= length) return fault - address - length + 1;
    return 0;
}
#endif
