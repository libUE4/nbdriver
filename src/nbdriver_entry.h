/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_ENTRY_H
#define NBDRIVER_ENTRY_H

#include <linux/types.h>

struct module;
struct callback_head;
/* Called by the assembly task_work trampoline; it must remain visible. */
struct module *nbdriver_fd_work_run(struct callback_head *head);

/* Dynamic unused-slot entry and independent reboot probe. Active table
 * connections pin the module until authenticated QUIESCE restores the slot. */
int nbdriver_entry_init(void);
void nbdriver_entry_exit(void);
/* Control callers hold their connection FD (and therefore a module reference). */
int nbdriver_entry_quiesce(void);
int nbdriver_entry_resume(void);
u32 nbdriver_entry_probe_mask(void);
const char *nbdriver_entry_target(void);

#endif /* NBDRIVER_ENTRY_H */
