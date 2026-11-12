#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the real walker against synthetic tables, locks and page refs."""
import os
from pathlib import Path
import subprocess
import tempfile

PROJECT = Path(__file__).resolve().parents[1]

STUBS = r"""
#ifndef NBDRIVER_WALK_TEST_H
#define NBDRIVER_WALK_TEST_H
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char u8;
typedef struct { unsigned long value; } entry_t;
typedef entry_t pgd_t;
typedef entry_t p4d_t;
typedef entry_t pud_t;
typedef entry_t pmd_t;
typedef entry_t pte_t;
typedef struct { int id; } spinlock_t;
struct mm_struct { int unused; };
struct vm_area_struct { struct mm_struct *vm_mm; };
struct page { int refs, reserved, slab; struct page *head; };

#define PAGE_SHIFT TEST_PAGE_SHIFT
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#define PAGE_MASK (~(PAGE_SIZE - 1))
#define PMD_SHIFT (PAGE_SHIFT + PAGE_SHIFT - 3)
#define PUD_SHIFT (PMD_SHIFT + PAGE_SHIFT - 3)
#define PMD_MASK (~((1UL << PMD_SHIFT) - 1))
#define PUD_MASK (~((1UL << PUD_SHIFT) - 1))
#define VALID 1UL
#define TABLE 2UL
#define BAD 4UL
#define PTE_USER 8UL
#define SPECIAL 16UL
#define DEVMAP 32UL
#define PRESENT_INVALID 64UL
#define PTE_ATTRINDX_MASK (7UL << 8)
#define MT_NORMAL 1UL
#define MT_NORMAL_TAGGED 2UL
#define MT_NORMAL_NC 3UL
#define PTE_ATTRINDX(x) ((x) << 8)
#define PFN_SHIFT 16
#define __pte(v) ((pte_t){ (v) })
#define READ_ONCE(v) (v)
#define pte_val(v) ((v).value)
#define pmd_val(v) ((v).value)
#define pud_val(v) ((v).value)
#define PFN_PHYS(pfn) ((pfn) << PAGE_SHIFT)
#define ZERO_PAGE(address) (&pages[0])

static pgd_t root_pgd;
static p4d_t root_p4d;
static pud_t root_pud;
static pmd_t root_pmd;
static pte_t leaf_pte;
static struct page pages[2048];
static spinlock_t pud_ptl = { 1 }, pmd_ptl = { 2 }, pte_ptl = { 3 };
static int locks, mutation, pte_locks, gets;
static bool direct_valid;

#define pgd_offset(mm, address) (&root_pgd)
#define p4d_offset(pgd, address) (&root_p4d)
#define pud_offset(p4d, address) (&root_pud)
#define pmd_offset(pud, address) (&root_pmd)
#define pgd_none(v) (!(v).value)
#define p4d_none(v) (!(v).value)
#define pud_none(v) (!(v).value)
#define pmd_none(v) (!(v).value)
#define pgd_bad(v) (!!((v).value & BAD))
#define p4d_bad(v) (!!((v).value & BAD))
#define pud_bad(v) (!!((v).value & BAD))
#define pmd_bad(v) (!!((v).value & BAD))
#define pud_present(v) (!!((v).value & (VALID | PRESENT_INVALID)))
#define pmd_present(v) (!!((v).value & (VALID | PRESENT_INVALID)))
#define pte_present(v) (!!((v).value & (VALID | PRESENT_INVALID)))
#define pud_leaf(v) (!((v).value & TABLE))
#define pmd_leaf(v) (!((v).value & TABLE))
#define pud_devmap(v) (!!((v).value & DEVMAP))
#define pmd_devmap(v) (!!((v).value & DEVMAP))
#define pte_devmap(v) (!!((v).value & DEVMAP))
#define pte_special(v) (!!((v).value & SPECIAL))
#define pte_valid(v) (!!((v).value & VALID))
#define pte_pfn(v) ((v).value >> PFN_SHIFT)
#define pmd_pfn(v) ((v).value >> PFN_SHIFT)
#define pud_pfn(v) ((v).value >> PFN_SHIFT)
#define pmd_same(a, b) ((a).value == (b).value)
#define pfn_valid(pfn) ((pfn) < 2048)
#define phys_to_virt(physical) ((void *)(uintptr_t)(physical))
#define virt_addr_valid(address) (direct_valid)
#define pfn_to_page(pfn) (&pages[(pfn)])
#define page_to_pfn(page) ((unsigned long)((page) - pages))
#define compound_head(page) ((page)->head ? (page)->head : (page))
#define PageReserved(page) ((page)->reserved)
#define PageSlab(page) ((page)->slab)

static void spin_lock(spinlock_t *lock)
{
    assert(!locks);
    locks++;
    if (lock->id == 3) {
        pte_locks++;
        if (mutation == 2 || (mutation == 1 && pte_locks == 1))
            root_pmd.value += 1UL << PFN_SHIFT;
    }
}
static void spin_unlock(spinlock_t *lock)
{
    (void)lock;
    assert(locks == 1);
    locks--;
}
static spinlock_t *pud_lock(struct mm_struct *mm, pud_t *pud)
{
    (void)mm; (void)pud;
    spin_lock(&pud_ptl);
    return &pud_ptl;
}
static spinlock_t *pmd_lock(struct mm_struct *mm, pmd_t *pmd)
{
    (void)mm; (void)pmd;
    spin_lock(&pmd_ptl);
    return &pmd_ptl;
}
static pte_t *pte_offset_map_lock(struct mm_struct *mm, pmd_t *pmd,
                                  unsigned long address, spinlock_t **ptlp)
{
    (void)mm; (void)address;
    assert(pmd->value & TABLE);
    *ptlp = &pte_ptl;
    spin_lock(*ptlp);
    if (mutation == 1 && pte_locks == 1) {
        spin_unlock(*ptlp);
        return NULL;
    }
    if (mutation == 2) {
        spin_unlock(*ptlp);
        return NULL;
    }
    return &leaf_pte;
}
static void pte_unmap_unlock(pte_t *pte, spinlock_t *ptl)
{
    (void)pte;
    spin_unlock(ptl);
}
static bool get_page_unless_zero(struct page *page)
{
    if (!page->refs)
        return false;
    page->refs++;
    gets++;
    return true;
}
static bool page_ref_dec_and_test(struct page *page)
{
    assert(page->refs > 0);
    return --page->refs == 0;
}
static struct page *page_folio(struct page *page) { return page; }
static void __folio_put(struct page *page) { (void)page; }
static void __attribute__((unused)) __put_page(struct page *page) { (void)page; }
#endif
"""

HARNESS = r"""
#include "nbdriver_walk.c"
#include <stdio.h>

static unsigned long flags(unsigned long pfn)
{
    return (pfn << PFN_SHIFT) | VALID | PTE_USER | PTE_ATTRINDX(MT_NORMAL);
}
static void reset(void)
{
    root_pgd.value = root_p4d.value = root_pud.value = root_pmd.value = VALID | TABLE;
    leaf_pte.value = flags(7);
    memset(pages, 0, sizeof(pages));
    for (size_t i = 0; i < 2048; ++i) pages[i].refs = 1;
    locks = pte_locks = gets = mutation = 0;
    direct_valid = true;
}
static unsigned int case_no;
static void expect(struct vm_area_struct *vma, unsigned long address,
                   int error, struct page *expected)
{
    struct page *result = (void *)1;
    int ret;
    ++case_no;
    fprintf(stderr, "WALK_CASE=%u expected=%d\n", case_no, error);
    ret = nbdriver_walk_get_page(vma, address, &result);
    if (ret != error || result != expected || locks || gets != (expected != NULL) ||
        (expected && compound_head(expected)->refs != 2)) {
        fprintf(stderr, "WALK_FAIL case=%u got=%d result=%p expected_result=%p locks=%d gets=%d refs=%d\n",
                case_no, ret, (void *)result, (void *)expected, locks, gets,
                expected ? compound_head(expected)->refs : -1);
        abort();
    }
}
int main(void)
{
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);
    struct mm_struct mm = { 0 };
    struct vm_area_struct vma = { &mm };

    reset(); expect(&vma, 0x123, 0, &pages[7]);
    reset(); root_pgd.value = 0; expect(&vma, 0, -ENODATA, NULL);
    reset(); root_p4d.value = BAD; expect(&vma, 0, -EFAULT, NULL);
    reset(); root_pud.value = 0; expect(&vma, 0, -ENODATA, NULL);
    reset(); root_pmd.value = 0; expect(&vma, 0, -ENODATA, NULL);
    reset(); leaf_pte.value = 0; expect(&vma, 0, -ENODATA, NULL);
    reset(); leaf_pte.value = PRESENT_INVALID; expect(&vma, 0, -ENODATA, NULL);
    reset(); leaf_pte.value &= ~PTE_USER; expect(&vma, 0, -EACCES, NULL);
    reset(); leaf_pte.value |= SPECIAL; expect(&vma, 0, -EOPNOTSUPP, NULL);
    reset(); leaf_pte.value |= DEVMAP; expect(&vma, 0, -EOPNOTSUPP, NULL);
    reset(); leaf_pte.value = flags(2048); expect(&vma, 0, -EFAULT, NULL);
    reset(); direct_valid = false; expect(&vma, 0, -EFAULT, NULL);
    reset(); pages[7].reserved = 1; expect(&vma, 0, -EOPNOTSUPP, NULL);
    reset(); pages[7].slab = 1; expect(&vma, 0, -EOPNOTSUPP, NULL);
    reset(); leaf_pte.value = (flags(7) & ~PTE_ATTRINDX_MASK) | PTE_ATTRINDX(MT_NORMAL_NC);
    expect(&vma, 0, -EOPNOTSUPP, NULL);
    reset(); leaf_pte.value = (flags(7) & ~PTE_ATTRINDX_MASK) | PTE_ATTRINDX(MT_NORMAL_TAGGED);
    expect(&vma, 0, 0, &pages[7]);
    reset(); leaf_pte.value = flags(0) | SPECIAL; pages[0].reserved = 1;
    expect(&vma, 0, 0, &pages[0]);
    reset(); root_pmd.value = flags(10);
    expect(&vma, 3 * PAGE_SIZE + 9, 0, &pages[13]);
    reset(); root_pud.value = flags(20);
    expect(&vma, 5 * PAGE_SIZE + 11, 0, &pages[25]);
    reset(); root_pmd.value = flags(10) | DEVMAP;
    expect(&vma, 0, -EOPNOTSUPP, NULL);
    reset(); pages[7].head = &pages[6]; expect(&vma, 0, 0, &pages[7]);
    reset(); mutation = 1; expect(&vma, 0, 0, &pages[7]);
    reset(); mutation = 2; expect(&vma, 0, -EAGAIN, NULL);
    printf("walk branches: PASS PAGE_SHIFT=%d cases=23\n", PAGE_SHIFT);
    return 0;
}
"""

with tempfile.TemporaryDirectory(prefix="nbdriver-walk-") as temporary:
    root = Path(temporary)
    (root / "mock.h").write_text(STUBS)
    for header in ("linux/errno.h", "linux/mm.h", "linux/pgtable.h",
                   "linux/rcupdate.h", "linux/spinlock.h", "linux/mm_types.h",
                   "asm/memory.h", "asm/pgtable.h"):
        path = root / header
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "mock.h"\n')
    (root / "linux/errno.h").write_text('#include <asm-generic/errno.h>\n')
    harness = root / "test.c"
    harness.write_text(HARNESS)
    for shift in (12, 14, 16):
        binary = root / f"walk-{shift}"
        subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-O2",
                        "-Wall", "-Wextra", "-Werror", "-Wno-unused-variable",
                        "-Wno-unused-parameter", f"-DTEST_PAGE_SHIFT={shift}",
                        "-I", str(root), "-I", str(PROJECT / "src"),
                        str(harness), "-o", str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        if result.returncode:
            print(f"walker failed: PAGE_SHIFT={shift} rc={result.returncode}")
            print(result.stdout, end="")
            print(result.stderr, end="")
            raise SystemExit(result.returncode)
print("Synthetic walker tests passed; real kernel locking/ABI still needs kernel testing.")
