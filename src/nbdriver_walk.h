/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_WALK_H
#define NBDRIVER_WALK_H

#include <linux/mm_types.h>

/* Caller holds mmap_read_lock(mm) and a live mm_users reference. */
int nbdriver_walk_get_page(struct vm_area_struct *vma, unsigned long address,
			   struct page **result);

#endif /* NBDRIVER_WALK_H */
