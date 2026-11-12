/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_CLIENT_H
#define NBDRIVER_CLIENT_H

#include "nbdriver_abi.h"

int nbdriver_open(void);
int nbdriver_get_abi(int fd, struct nbdriver_abi_info *info);

#endif /* NBDRIVER_CLIENT_H */
