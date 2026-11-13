/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_WALK_H
#define NBDRIVER_WALK_H

#include <linux/mm_types.h>

/* Caller holds mmap_read_lock(mm) and a live mm_users reference. */
int nbdriver_walk_get_page(struct vm_area_struct *vma, unsigned long address,
			   bool write, bool allow_zero_page, unsigned long zero_pfn,
			   struct page **result);
/* Read-only resident normal RAM, including Normal-NC GPU PFN mappings.
 * Caller also checks VMA read access; no fault-in, MMIO or page-table writes. */
int nbdriver_walk_get_ram_page(struct vm_area_struct *vma, unsigned long address,
                               struct page **result, bool *uncached);
void nbdriver_walk_put_page(struct page *page);

#endif /* NBDRIVER_WALK_H */
