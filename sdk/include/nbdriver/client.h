/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_CLIENT_H
#define NBDRIVER_CLIENT_H

#include "nbdriver_abi.h"

int nbdriver_open(void);
int nbdriver_get_abi(int fd, struct nbdriver_abi_info *info);
int nbdriver_get_runtime_status(int fd, struct nbdriver_runtime_status *status);
int nbdriver_runtime_control(int fd, struct nbdriver_runtime_control *control);
/* Root-only loader primitive; policy/preflight belongs to the caller. */
int nbdriver_load_module_path(const char *path, const char *parameters);
/* On an operational error, request still reports copied/error/failed_address. */
int nbdriver_read_process(int fd, struct nbdriver_read_request *request);
int nbdriver_write_process(int fd, struct nbdriver_write_request *request);

int nbdriver_udp_capture_start(int fd, int pid, nbdriver_u32 min_payload,
    nbdriver_u32 max_payload, nbdriver_u64 expected_start_ns,
    struct nbdriver_udp_start_request *out);
int nbdriver_udp_capture_read(int fd, struct nbdriver_udp_packet *packets,
    nbdriver_u32 capacity, nbdriver_u64 generation, struct nbdriver_udp_status *status);
int nbdriver_udp_capture_status(int fd, struct nbdriver_udp_status *out);
int nbdriver_udp_capture_stop(int fd);

#endif /* NBDRIVER_CLIENT_H */
