/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_MAPPING_H
#define NBDRIVER_MAPPING_H
struct vm_area_struct;
struct nbdriver_mapping_request;
/* Caller holds mmap_lock and an mm_users reference. No faults or page access. */
int nbdriver_mapping_locked(struct vm_area_struct *vma, unsigned long address,
			    struct nbdriver_mapping_request *request);
#endif
