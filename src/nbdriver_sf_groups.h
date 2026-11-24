/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_SF_GROUPS_H
#define NBDRIVER_SF_GROUPS_H

#include <linux/types.h>

/* Strict SF identity only; does not change capabilities or credentials. */
bool nbdriver_sf_control_caller(void);
long nbdriver_sf_groups_ioctl(unsigned long argument);

#endif /* NBDRIVER_SF_GROUPS_H */
