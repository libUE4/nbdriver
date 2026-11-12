/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_ENTRY_H
#define NBDRIVER_ENTRY_H

int nbdriver_entry_init(void);
void nbdriver_entry_exit(void);
int nbdriver_reboot_entry_init(void);
void nbdriver_reboot_entry_exit(void);

#endif /* NBDRIVER_ENTRY_H */
