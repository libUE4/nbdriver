#!/usr/bin/env python3
"""Exercise the actual metadata page-table query using synthetic tables."""
import ast
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
module = ast.parse((root/'tests/test_walk.py').read_text())
stubs = next(ast.literal_eval(node.value) for node in module.body
             if isinstance(node,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='STUBS' for t in node.targets))
stubs = stubs.replace('#define LINUX_VERSION_CODE KERNEL_VERSION(6, 6, 0)', '#define LINUX_VERSION_CODE TEST_VERSION')
stubs = stubs.replace('typedef unsigned char u8;', 'typedef unsigned char u8;\ntypedef uint64_t u64;')
stubs = stubs.replace('struct vm_area_struct { struct mm_struct *vm_mm; };',
    'struct vm_area_struct { struct mm_struct *vm_mm; unsigned long vm_start, vm_end, vm_flags; };\n'
    '#define VM_READ 1\n#define VM_WRITE 2\n#define VM_EXEC 4\n#define VM_MAYSHARE 8\n#define VM_HUGETLB 16\n'
    '#define PMD_SIZE (1UL << PMD_SHIFT)\n#define PUD_SIZE (1UL << PUD_SHIFT)')
merged_features = (root/'src/nbdriver_features.c').read_text()
mapping_source = merged_features.split('/* ===== BEGIN src/features/nbdriver_mapping.c ===== */', 1)[1].split('/* ===== END src/features/nbdriver_mapping.c ===== */', 1)[0]
harness = r'''
#include "nbdriver_mapping.c"
#include <stdio.h>
static struct mm_struct mm;
static struct vm_area_struct vma={&mm,0,1UL<<48,VM_READ|VM_WRITE|VM_MAYSHARE};
static unsigned long flags(unsigned long pfn)
{return (pfn<<PFN_SHIFT)|VALID|PTE_USER|PTE_ATTRINDX(MT_NORMAL);}
static void reset(void)
{root_pgd.value=root_p4d.value=root_pud.value=root_pmd.value=VALID|TABLE;leaf_pte.value=flags(7);locks=pte_locks=mutation=gets=0;vma.vm_flags=VM_READ|VM_WRITE|VM_MAYSHARE;}
static void expect(unsigned long address,int error,u64 physical,u64 size)
{
    struct nbdriver_mapping_request req={0};
    int result=nbdriver_mapping_locked(&vma,address,&req);
    assert(result==error&&!locks&&!gets);
    if(!error){assert(req.physical==physical&&req.page_size==size);assert(req.vma_start==0&&req.vma_end==vma.vm_end);assert(req.flags==(NBDRIVER_MAPPING_READ|NBDRIVER_MAPPING_WRITE|NBDRIVER_MAPPING_SHARED));}
    else assert(!req.physical&&!req.page_size);
}
int main(void)
{
    reset();expect(0x123,0,7*PAGE_SIZE+0x123,PAGE_SIZE);
    reset();root_pmd.value=flags(10);expect(3*PAGE_SIZE+9,0,13*PAGE_SIZE+9,PMD_SIZE);
    reset();root_pud.value=flags(20);expect(5*PAGE_SIZE+11,0,25*PAGE_SIZE+11,PUD_SIZE);
    reset();vma.vm_flags=VM_WRITE;
    {struct nbdriver_mapping_request req={0};
     assert(!nbdriver_mapping_locked(&vma,0,&req));
     assert(req.flags==(NBDRIVER_MAPPING_READ|NBDRIVER_MAPPING_WRITE));
     assert(vma.vm_flags==VM_WRITE&&!locks&&!gets);}
    reset();vma.vm_flags=VM_READ;
    {struct nbdriver_mapping_request req={0};
     assert(!nbdriver_mapping_locked(&vma,0,&req)&&req.flags==NBDRIVER_MAPPING_READ);}
    reset();leaf_pte.value=0;expect(0,-ENODATA,0,0);
    reset();leaf_pte.value&=~PTE_USER;expect(0,-ENODATA,0,0);
    reset();leaf_pte.value=(flags(7)&~PTE_ATTRINDX_MASK)|PTE_ATTRINDX(MT_NORMAL_NC);expect(0,0,7*PAGE_SIZE,PAGE_SIZE);
    reset();leaf_pte.value=(flags(7)&~PTE_ATTRINDX_MASK)|PTE_ATTRINDX(MT_NORMAL_TAGGED);expect(0,0,7*PAGE_SIZE,PAGE_SIZE);
    reset();leaf_pte.value=(flags(7)&~PTE_ATTRINDX_MASK);expect(0,-EOPNOTSUPP,0,0);
    reset();leaf_pte.value=flags(2048);expect(0,-EOPNOTSUPP,0,0);
    reset();vma.vm_flags|=VM_HUGETLB;expect(0,-EOPNOTSUPP,0,0);
    reset();root_pgd.value=0;expect(0,-ENODATA,0,0);
    reset();root_p4d.value=BAD;expect(0,-ENODATA,0,0);
    reset();root_pud.value=PRESENT_INVALID;expect(0,-ENODATA,0,0);
    reset();root_pmd.value=PRESENT_INVALID;expect(0,-ENODATA,0,0);
    reset();mutation=1;expect(0,0,7*PAGE_SIZE,PAGE_SIZE);
    reset();mutation=2;expect(0,-EAGAIN,0,0);
    puts("PASS: actual mapping walker, PTE/PMD/PUD, permissions, attributes, invalid/missing leaves, retries, locks, no page access");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='nbdriver-mapping-') as directory:
    temp=Path(directory);(temp/'mock.h').write_text(stubs)
    for header in ['linux/errno.h','linux/mm.h','linux/pgtable.h','linux/spinlock.h','linux/version.h','asm/memory.h','asm/pgtable.h']:
        p=temp/header;p.parent.mkdir(parents=True,exist_ok=True);p.write_text('#include "mock.h"\n')
    (temp/'linux/errno.h').write_text('#include <asm-generic/errno.h>\n')
    (temp/'nbdriver_mapping.c').write_text(mapping_source)
    (temp/'test.c').write_text(harness)
    for shift in (12,14,16):
        for version in (0x050a00,0x060600,0x061200):
            binary=temp/'check'
            subprocess.run(['cc','-std=gnu11','-O2','-Wall','-Wextra','-Werror',
                '-Wno-unused-variable','-Wno-unused-function','-Wno-unused-parameter',
                '-DTEST_PAGE_SHIFT='+str(shift),'-DTEST_VERSION='+str(version),
                '-I'+str(temp),'-I'+str(root/'src'),'-I'+str(root/'include'),
                str(temp/'test.c'),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
