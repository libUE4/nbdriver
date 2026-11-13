/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_CLIENT_H
#define NBDRIVER_CLIENT_H

#include "nbdriver_abi.h"

int nbdriver_open(void);
int nbdriver_get_abi(int fd, struct nbdriver_abi_info *info);
/* On an operational error, request still reports copied/error/failed_address. */
int nbdriver_read_process(int fd, struct nbdriver_read_request *request);
int nbdriver_write_process(int fd, struct nbdriver_write_request *request);

#endif /* NBDRIVER_CLIENT_H */
