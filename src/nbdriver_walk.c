// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include <linux/version.h>
#include <linux/mm.h>
#include <linux/pgtable.h>
#include <linux/spinlock.h>

#include <asm/memory.h>
#include <asm/pgtable.h>

#include "nbdriver_walk.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
#define nbdriver_pud_devmap(entry) false
#define nbdriver_pmd_devmap(entry) false
#define nbdriver_pte_devmap(entry) false
#else
#define nbdriver_pud_devmap(entry) pud_devmap(entry)
#define nbdriver_pmd_devmap(entry) pmd_devmap(entry)
#define nbdriver_pte_devmap(entry) pte_devmap(entry)
#endif

static bool nbdriver_page_ref_get(struct page *page)
{
	struct page *head = compound_head(page);

	return get_page_unless_zero(head);
}

void nbdriver_walk_put_page(struct page *page)
{
	struct page *head = compound_head(page);

	if (!page_ref_dec_and_test(head))
		return;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
	__folio_put(page_folio(head));
#else
	__put_page(head);
#endif
}

static int nbdriver_leaf_get(pte_t entry, unsigned long pfn, bool block,
			     struct page **result)
{
	struct page *page, *head;
	unsigned long attr = pte_val(entry) & PTE_ATTRINDX_MASK;

	if (!pte_valid(entry))
		return -ENODATA;
	if (!(pte_val(entry) & PTE_USER))
		return -EACCES;
	if (attr != PTE_ATTRINDX(MT_NORMAL)
#ifdef MT_NORMAL_TAGGED
	    && attr != PTE_ATTRINDX(MT_NORMAL_TAGGED)
#endif
	    )
		return -EOPNOTSUPP;
	if (nbdriver_pte_devmap(entry))
		return -EOPNOTSUPP;
	if (!block && pte_special(entry))
		return -EOPNOTSUPP;
	if (!pfn_valid(pfn) || !virt_addr_valid(phys_to_virt(PFN_PHYS(pfn))))
		return -EFAULT;

	page = pfn_to_page(pfn);
	head = compound_head(page);
	if (page != ZERO_PAGE(0) && (PageReserved(head) || PageSlab(head)))
		return -EOPNOTSUPP;
	/* The leaf lock keeps the mapping's page reference alive during get_page. */
	if (!nbdriver_page_ref_get(page))
		return -EAGAIN;
	*result = page;
	return 0;
}

int nbdriver_walk_get_page(struct vm_area_struct *vma, unsigned long address,
			   struct page **result)
{
	struct mm_struct *mm = vma->vm_mm;
	pgd_t *pgdp, pgd;
	p4d_t *p4dp, p4d;
	pud_t *pudp, pud;
	pmd_t *pmdp, pmd;
	pte_t *ptep, pte;
	spinlock_t *ptl;
	unsigned long pfn;
	int attempt;
	int ret;

	*result = NULL;
	pgdp = pgd_offset(mm, address);
	pgd = READ_ONCE(*pgdp);
	if (pgd_none(pgd))
		return -ENODATA;
	if (pgd_bad(pgd))
		return -EFAULT;
	p4dp = p4d_offset(pgdp, address);
	p4d = READ_ONCE(*p4dp);
	if (p4d_none(p4d))
		return -ENODATA;
	if (p4d_bad(p4d))
		return -EFAULT;
	pudp = pud_offset(p4dp, address);

	ptl = pud_lock(mm, pudp);
	pud = READ_ONCE(*pudp);
	if (pud_none(pud) || !pud_present(pud)) {
		spin_unlock(ptl);
		return -ENODATA;
	}
	if (pud_leaf(pud)) {
		if (nbdriver_pud_devmap(pud)) {
			spin_unlock(ptl);
			return -EOPNOTSUPP;
		}
		pfn = pud_pfn(pud) + ((address & ~PUD_MASK) >> PAGE_SHIFT);
		ret = nbdriver_leaf_get(__pte(pud_val(pud)), pfn, true, result);
		spin_unlock(ptl);
		return ret;
	}
	if (pud_bad(pud)) {
		spin_unlock(ptl);
		return -EFAULT;
	}
	spin_unlock(ptl);

	/* The kernel helper owns the PTE mapping and its lock/RCU protocol. */
	for (attempt = 0; attempt < 3; ++attempt) {
		pmdp = pmd_offset(pudp, address);
		ptl = pmd_lock(mm, pmdp);
		pmd = READ_ONCE(*pmdp);
		if (pmd_none(pmd) || !pmd_present(pmd)) {
			spin_unlock(ptl);
			return -ENODATA;
		}
		if (pmd_leaf(pmd)) {
			if (nbdriver_pmd_devmap(pmd)) {
				spin_unlock(ptl);
				return -EOPNOTSUPP;
			}
			pfn = pmd_pfn(pmd) + ((address & ~PMD_MASK) >> PAGE_SHIFT);
			ret = nbdriver_leaf_get(__pte(pmd_val(pmd)), pfn, true,
						result);
			spin_unlock(ptl);
			return ret;
		}
		if (pmd_bad(pmd)) {
			spin_unlock(ptl);
			return -EFAULT;
		}
		spin_unlock(ptl);
		ptep = pte_offset_map_lock(mm, pmdp, address, &ptl);
		if (!ptep)
			continue;
		pte = READ_ONCE(*ptep);
		if (!pte_present(pte))
			ret = -ENODATA;
		else
			ret = nbdriver_leaf_get(pte, pte_pfn(pte), false, result);
		pte_unmap_unlock(ptep, ptl);
		return ret;
	}
	return -EAGAIN;
}
