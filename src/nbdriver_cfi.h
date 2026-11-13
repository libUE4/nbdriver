/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_CFI_H
#define NBDRIVER_CFI_H

/* Adapted from workspace lsdriver/export_fun.h: keep exemptions local.
 * Clang on older KMI branches does not recognize the "kcfi" sanitizer name;
 * only request it when this build actually enables the KCFI feature.
 */
#ifdef __clang__
#if __has_feature(kcfi)
#define NBDRIVER_NO_CFI __attribute__((no_sanitize("cfi", "kcfi")))
#else
#define NBDRIVER_NO_CFI __attribute__((no_sanitize("cfi")))
#endif
#else
#define NBDRIVER_NO_CFI
#endif

int nbdriver_cfi_init(void);
void nbdriver_cfi_exit(void);

#endif /* NBDRIVER_CFI_H */
