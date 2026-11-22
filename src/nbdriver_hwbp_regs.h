/* SPDX-License-Identifier: GPL-2.0-only */
/* Register programming adapted from NBG-driver/src/hwbp/hwbp_{core,watch}.c.
 * Disable before changing an address; program EL0 only. No perf registration.
 */
#ifndef NBDRIVER_HWBP_REGS_H
#define NBDRIVER_HWBP_REGS_H
#include <linux/types.h>
#include <asm/sysreg.h>

#define NBD_HWB_BP_BASE 2U
#define NBD_HWB_SLOTS 4U
static inline void nbd_hwb_counts(unsigned int *brps, unsigned int *wrps)
{
    u64 value = read_sysreg(id_aa64dfr0_el1);
    *brps = ((value >> 12) & 15) + 1;
    *wrps = ((value >> 20) & 15) + 1;
}
#define NBD_HWB_WRITE_CASE(n, prefix) \
    case n: \
        write_sysreg(0, prefix##cr##n##_el1); \
        isb(); \
        write_sysreg(address, prefix##vr##n##_el1); \
        write_sysreg(control, prefix##cr##n##_el1); \
        break

static inline void nbd_hwb_write_bp(unsigned int slot, u64 address, u64 control)
{
    dsb(ish);
    switch (slot) {
    NBD_HWB_WRITE_CASE(2, dbgb);
    NBD_HWB_WRITE_CASE(3, dbgb);
    NBD_HWB_WRITE_CASE(4, dbgb);
    NBD_HWB_WRITE_CASE(5, dbgb);
    default: return;
    }
    isb();
}

static inline void nbd_hwb_write_wp(unsigned int slot, u64 address, u64 control)
{
    dsb(ish);
    switch (slot) {
    NBD_HWB_WRITE_CASE(0, dbgw);
    NBD_HWB_WRITE_CASE(1, dbgw);
    NBD_HWB_WRITE_CASE(2, dbgw);
    NBD_HWB_WRITE_CASE(3, dbgw);
    default: return;
    }
    isb();
}
#undef NBD_HWB_WRITE_CASE

#define NBD_HWB_READ_CASE(n, prefix) \
    case n: *address = read_sysreg(prefix##vr##n##_el1); \
        *control = read_sysreg(prefix##cr##n##_el1); break
#define NBD_HWB_READ_ALL(prefix) \
    NBD_HWB_READ_CASE(0, prefix); NBD_HWB_READ_CASE(1, prefix); \
    NBD_HWB_READ_CASE(2, prefix); NBD_HWB_READ_CASE(3, prefix); \
    NBD_HWB_READ_CASE(4, prefix); NBD_HWB_READ_CASE(5, prefix); \
    NBD_HWB_READ_CASE(6, prefix); NBD_HWB_READ_CASE(7, prefix); \
    NBD_HWB_READ_CASE(8, prefix); NBD_HWB_READ_CASE(9, prefix); \
    NBD_HWB_READ_CASE(10, prefix); NBD_HWB_READ_CASE(11, prefix); \
    NBD_HWB_READ_CASE(12, prefix); NBD_HWB_READ_CASE(13, prefix); \
    NBD_HWB_READ_CASE(14, prefix); NBD_HWB_READ_CASE(15, prefix)
static inline void nbd_hwb_read(bool execute, unsigned int slot,
    u64 *address, u64 *control)
{
    *address = *control = 0;
    if (execute) {
        switch (slot) { NBD_HWB_READ_ALL(dbgb); }
    } else {
        switch (slot) { NBD_HWB_READ_ALL(dbgw); }
    }
}
#undef NBD_HWB_READ_ALL
#undef NBD_HWB_READ_CASE

static inline void nbd_hwb_enable(void)
{
    u64 value = read_sysreg(mdscr_el1);
    /* Never clear MDE on stop: it may also belong to another debug client. */
    if (!(value & (1ULL << 15))) {
        write_sysreg(value | (1ULL << 15), mdscr_el1);
        isb();
    }
}
#endif
