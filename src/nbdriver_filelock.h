/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_FILELOCK_H
#define NBDRIVER_FILELOCK_H

#include <linux/version.h>
#include <linux/fs.h>
/* File-lock declarations moved out of fs.h in Linux 6.3. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
#include <linux/filelock.h>
#endif

/* locks_show's iterator value is the list node, not the containing lock.
 * Linux 6.9 split that container into file_lock_core (also used by leases).
 * Use the target headers for both conversions; never assume a byte offset.
 */
static inline pid_t nbdriver_filelock_owner(const void *entry)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
    const struct file_lock_core *lock;
#else
    const struct file_lock *lock;
#endif

    if (!entry)
        return 0;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
    lock = hlist_entry(entry, const struct file_lock_core, flc_link);
    return lock->flc_pid;
#else
    lock = hlist_entry(entry, const struct file_lock, fl_link);
    return (pid_t)lock->fl_pid;
#endif
}

#endif /* NBDRIVER_FILELOCK_H */
