/* ===== BEGIN src/features/nbdriver_metadata.c ===== */
// SPDX-License-Identifier: GPL-2.0-only
/* Read-only metadata adapted from workspace NBG-driver readonly.c/query.c
 * and socket_ioctl.c; use nbdriver's ABI, task identity and ptrace checks. */
#include <linux/capability.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kdev_t.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/mm_inline.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/preempt.h>
#include <linux/pid.h>
#include <linux/random.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <asm/memory.h>
#include "nbdriver_abi.h"
#include "nbdriver_cfi.h"
#include "nbdriver_ksym.h"
#include "nbdriver_mapping.h"
#include "nbdriver_metadata.h"
#include "nbdriver_metadata_helpers.h"
#include "nbdriver_query.h"
#include "nbdriver_read.h"

static struct file *(*metadata_fget)(struct task_struct *, unsigned int);
static __typeof__(&vfs_getattr) metadata_getattr;
static __typeof__(&__module_address) metadata_module_address;
typedef long (*metadata_gpu_fn)(void *, unsigned int, void *);

/* One backend, never a target file or GPU object. The module reference keeps
 * both text and the cache key alive; failures are deliberately not cached. */
static DEFINE_MUTEX(metadata_gpu_lock);
static const struct file_operations *metadata_gpu_fops;
static struct module *metadata_gpu_module;
static unsigned long metadata_gpu_query, metadata_gpu_ioctl;

int nbdriver_metadata_init(void)
{
	metadata_fget = (__typeof__(metadata_fget))nbdriver_ksym_lookup_optional("fget_task");
	metadata_getattr = (__typeof__(metadata_getattr))nbdriver_ksym_lookup_optional("vfs_getattr");
	metadata_module_address = (__typeof__(metadata_module_address))
		nbdriver_ksym_lookup_optional("__module_address");
	/* Optional backends must not stop the base driver from initializing. */
	return 0;
}

void nbdriver_metadata_exit(void)
{
	struct module *owner;

	mutex_lock(&metadata_gpu_lock);
	owner = metadata_gpu_module;
	metadata_gpu_fops = NULL;
	metadata_gpu_module = NULL;
	metadata_gpu_query = 0;
	metadata_gpu_ioctl = 0;
	mutex_unlock(&metadata_gpu_lock);
	module_put(owner);
	metadata_module_address = NULL;
	metadata_getattr = NULL;
	metadata_fget = NULL;
}

u32 nbdriver_metadata_features(void)
{
	u32 features = NBDRIVER_FEATURE_MAPS | NBDRIVER_FEATURE_MAPPING | NBDRIVER_FEATURE_VMA |
		NBDRIVER_FEATURE_RANDOM;

	if (metadata_fget && metadata_getattr)
		features |= NBDRIVER_FEATURE_FD_SIZE;
	/* Cold feature discovery must still support a vendor module loading later. */
	if (metadata_fget && metadata_module_address &&
	    nbdriver_ksym_lookup_optional("kgsl_ioctl") &&
	    nbdriver_ksym_lookup_optional("kgsl_ioctl_gpuobj_info"))
		features |= NBDRIVER_FEATURE_GPU_INFO;
	return features;
}

static noinline struct file *NBDRIVER_NO_CFI
metadata_get_file(struct task_struct *task, unsigned int fd)
{
	return metadata_fget(task, fd);
}

static noinline int NBDRIVER_NO_CFI metadata_stat(struct file *file, struct kstat *stat)
{
	return metadata_getattr(&file->f_path, stat, STATX_SIZE, AT_STATX_SYNC_AS_STAT);
}

/* Called with mmap_lock held. Keep older kernels' anonymous-name union out
 * of file-backed VMAs, and never import unexported anon_vma_name helpers. */
static const char *metadata_anon_name(struct vm_area_struct *vma, char *scratch)
{
#ifdef CONFIG_ANON_VMA_NAME
	struct anon_vma_name *anon;

	(void)scratch;
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 2, 0)
	anon = vma->vm_file ? NULL : vma->anon_name;
#else
	anon = vma->anon_name;
#endif
	return anon ? anon->name : NULL;
#elif LINUX_VERSION_CODE < KERNEL_VERSION(5, 11, 0)
	unsigned long address = (unsigned long)vma_get_anon_name(vma);
	size_t used = 0;

	if (!address)
		return NULL;
	while (used < NAME_MAX) {
		struct nbdriver_read_request read = {};
		size_t want = min_t(size_t, NAME_MAX - used,
				   PAGE_SIZE - offset_in_page(address));
		size_t length;
		int ret;

		read.address = address;
		read.size = want;
		ret = nbdriver_read_mm_locked(vma->vm_mm, &read, (u8 *)scratch + used);
		length = strnlen(scratch + used, read.copied);
		used += length;
		if (length < read.copied)
			break;
		if (ret)
			return ERR_PTR(ret);
		address += length;
	}
	scratch[used] = 0;
	return scratch;
#else
	(void)vma;
	(void)scratch;
	return NULL;
#endif
}

struct metadata_maps_buffer {
	struct nbdriver_maps_output output;
	char *path, *escaped, *line;
};
#define METADATA_ESCAPED_MAX (4 * PATH_MAX + 1)
#define METADATA_LINE_MAX (METADATA_ESCAPED_MAX + 256)

static int metadata_map_line(struct metadata_maps_buffer *out, struct vm_area_struct *vma)
{
	struct file *file = vma->vm_file;
	struct inode *inode = file ? file_inode(file) : NULL;
	dev_t dev = inode ? inode->i_sb->s_dev : 0;
	const char *anon = metadata_anon_name(vma, out->path);
	const char *name = "", *prefix = "", *suffix = "";
	u64 offset = file ? (u64)vma->vm_pgoff << PAGE_SHIFT : 0;
	int count, ret;

	if (IS_ERR(anon))
		return PTR_ERR(anon);
	if (anon) {
		name = anon;
		prefix = file ? "[anon_shmem:" : "[anon:";
		suffix = "]";
	} else if (file) {
		name = d_path(&file->f_path, out->path, PATH_MAX);
		if (IS_ERR(name))
			return PTR_ERR(name);
	} else {
		if (vma->vm_ops && vma->vm_ops->name)
			name = vma->vm_ops->name(vma);
		if (!name || !*name) {
			if (vma->vm_start <= vma->vm_mm->brk &&
			    vma->vm_end > vma->vm_mm->start_brk)
				name = "[heap]";
			else if (vma->vm_start <= vma->vm_mm->start_stack &&
				 vma->vm_end > vma->vm_mm->start_stack)
				name = "[stack]";
			else
				name = "";
		}
	}
	ret = nbdriver_maps_escape(out->escaped, METADATA_ESCAPED_MAX, name);
	if (ret)
		return ret;
	count = snprintf(out->line, METADATA_LINE_MAX,
		"%016lx-%016lx %c%c%c%c %08llx %02x:%02x %lu %s%s%s\n",
		vma->vm_start, vma->vm_end,
		vma->vm_flags & VM_READ ? 'r' : '-', vma->vm_flags & VM_WRITE ? 'w' : '-',
		vma->vm_flags & VM_EXEC ? 'x' : '-', vma->vm_flags & VM_MAYSHARE ? 's' : 'p',
		offset, MAJOR(dev), MINOR(dev), inode ? inode->i_ino : 0,
		prefix, out->escaped, suffix);
	if (count < 0 || count >= METADATA_LINE_MAX)
		return -EOVERFLOW;
	return nbdriver_maps_append(&out->output, out->line, count);
}

static long metadata_maps(unsigned long argument)
{
	struct nbdriver_maps_request req;
	struct metadata_maps_buffer out = {};
	struct task_struct *task;
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	int ret = 0;

	if (copy_from_user(&req, (void __user *)argument, sizeof(req)))
		return -EFAULT;
	if (req.pid <= 0 || req.reserved || req.capacity > NBDRIVER_MAPS_MAX ||
	    (!!req.capacity != !!req.buffer))
		return -EINVAL;
	req.length = req.copied = 0;
	req.error = 0;
	task = nbdriver_query_get_process(req.pid, req.expected_start_ns);
	if (IS_ERR(task)) {
		ret = PTR_ERR(task);
		goto reply;
	}
	req.expected_start_ns = READ_ONCE(task->start_boottime);
	mm = get_task_mm(task);
	if (!mm) {
		ret = -ESRCH;
		goto put_task;
	}
	out.output.capacity = req.capacity;
	if (req.capacity)
		out.output.bytes = kvmalloc(req.capacity, GFP_KERNEL);
	out.path = kmalloc(PATH_MAX, GFP_KERNEL);
	out.escaped = kvmalloc(METADATA_ESCAPED_MAX, GFP_KERNEL);
	out.line = kvmalloc(METADATA_LINE_MAX, GFP_KERNEL);
	if ((req.capacity && !out.output.bytes) || !out.path || !out.escaped || !out.line) {
		ret = -ENOMEM;
		goto free_output;
	}
	mmap_read_lock(mm);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
	{
		VMA_ITERATOR(iterator, mm, 0);
		for_each_vma(iterator, vma) {
#else
		for (vma = mm->mmap; vma; vma = vma->vm_next) {
#endif
			ret = metadata_map_line(&out, vma);
			if (ret)
				break;
		}
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
	}
#endif
	mmap_read_unlock(mm);
	req.length = out.output.used;
	if (!ret && req.capacity) {
		if (req.length >= req.capacity) {
			ret = -ENOSPC;
		} else {
			out.output.bytes[req.length] = 0;
			if (copy_to_user((void __user *)(unsigned long)req.buffer,
					 out.output.bytes, req.length + 1))
				ret = -EFAULT;
			else
				req.copied = req.length;
		}
	}
free_output:
	kvfree(out.output.bytes);
	kfree(out.path);
	kvfree(out.escaped);
	kvfree(out.line);
	mmput(mm);
put_task:
	nbdriver_query_put_process(task);
reply:
	req.error = ret;
	if (copy_to_user((void __user *)argument, &req, sizeof(req)))
		return -EFAULT;
	return ret;
}

static long metadata_mapping(unsigned long argument, bool vma_only)
{
	struct nbdriver_mapping_request req;
	struct task_struct *task;
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	unsigned long address;
	int ret;

	if (copy_from_user(&req, (void __user *)argument, sizeof(req)))
		return -EFAULT;
	address = untagged_addr((unsigned long)req.address);
	if (req.pid <= 0 || req.reserved || req.reserved2 || !address || address >= TASK_SIZE_64)
		return -EINVAL;
	req.physical = req.page_size = req.vma_start = req.vma_end = 0;
	req.flags = 0;
	task = nbdriver_query_get_process(req.pid, req.expected_start_ns);
	if (IS_ERR(task)) {
		ret = PTR_ERR(task);
		goto reply;
	}
	req.expected_start_ns = READ_ONCE(task->start_boottime);
	mm = get_task_mm(task);
	if (!mm) {
		ret = -ESRCH;
		goto put_task;
	}
	mmap_read_lock(mm);
	vma = find_vma(mm, address);
	if (!vma || address < vma->vm_start) {
		ret = -ENOENT;
	} else if (vma_only) {
		req.vma_start = vma->vm_start;
		req.vma_end = vma->vm_end;
		req.flags = (vma->vm_flags & (VM_READ | VM_WRITE) ? NBDRIVER_MAPPING_READ : 0) |
			(vma->vm_flags & VM_WRITE ? NBDRIVER_MAPPING_WRITE : 0) |
			(vma->vm_flags & VM_EXEC ? NBDRIVER_MAPPING_EXEC : 0) |
			(vma->vm_flags & VM_MAYSHARE ? NBDRIVER_MAPPING_SHARED : 0);
		ret = 0;
	} else {
		ret = nbdriver_mapping_locked(vma, address, &req);
	}
	mmap_read_unlock(mm);
	mmput(mm);
put_task:
	nbdriver_query_put_process(task);
reply:
	if (copy_to_user((void __user *)argument, &req, sizeof(req)))
		return -EFAULT;
	return ret;
}

static long metadata_fd_size(unsigned long argument)
{
	struct nbdriver_fd_size_request req;
	struct task_struct *task;
	struct file *file;
	struct kstat stat = {};
	int ret;

	if (copy_from_user(&req, (void __user *)argument, sizeof(req)))
		return -EFAULT;
	if (req.pid <= 0 || req.target_fd < 0 || req.reserved)
		return -EINVAL;
	req.size = 0;
	req.error = 0;
	if (!metadata_fget || !metadata_getattr) {
		ret = -EOPNOTSUPP;
		goto reply;
	}
	task = nbdriver_query_get_process(req.pid, req.expected_start_ns);
	if (IS_ERR(task)) {
		ret = PTR_ERR(task);
		goto reply;
	}
	req.expected_start_ns = READ_ONCE(task->start_boottime);
	file = metadata_get_file(task, req.target_fd);
	if (!file) {
		ret = -EBADF;
	} else {
		ret = metadata_stat(file, &stat);
		if (!ret && stat.size < 0)
			ret = -EOVERFLOW;
		if (!ret)
			req.size = stat.size;
		fput(file);
	}
	nbdriver_query_put_process(task);
reply:
	req.error = ret;
	if (copy_to_user((void __user *)argument, &req, sizeof(req)))
		return -EFAULT;
	return ret;
}

/* Validate that both callbacks belong to the pinned file's driver. */
static noinline bool NBDRIVER_NO_CFI
metadata_gpu_owner(struct file *file, unsigned long query, unsigned long ioctl_address)
{
	struct module *owner = file->f_op->owner;
	bool matches;

	/* __module_address() requires disabled preemption, including PREEMPT_RCU. */
	preempt_disable();
	matches = metadata_module_address(query) == owner &&
		metadata_module_address(ioctl_address) == owner;
	preempt_enable();
	return matches;
}

/* Caller holds its file reference through the eventual vendor call. Cache
 * replacement therefore cannot invalidate a callback already returned here. */
static int metadata_gpu_resolve(struct file *file, unsigned long *query)
{
	struct module *old = NULL;
	unsigned long ioctl_address, address;
	int ret = -ENOTTY;

	mutex_lock(&metadata_gpu_lock);
	if (metadata_gpu_fops == file->f_op &&
	    metadata_gpu_module == file->f_op->owner &&
	    metadata_gpu_ioctl == (unsigned long)file->f_op->unlocked_ioctl) {
		*query = metadata_gpu_query;
		ret = 0;
		goto unlock;
	}
	ioctl_address = nbdriver_ksym_lookup_optional("kgsl_ioctl");
	if (!ioctl_address || ioctl_address != (unsigned long)file->f_op->unlocked_ioctl)
		goto unlock;
	address = nbdriver_ksym_lookup_optional("kgsl_ioctl_gpuobj_info");
	ret = -EOPNOTSUPP;
	if (!address || !metadata_gpu_owner(file, address, ioctl_address) ||
	    !try_module_get(file->f_op->owner))
		goto unlock;
	old = metadata_gpu_module;
	metadata_gpu_fops = file->f_op;
	metadata_gpu_module = file->f_op->owner;
	metadata_gpu_ioctl = ioctl_address;
	metadata_gpu_query = address;
	*query = address;
	ret = 0;
unlock:
	mutex_unlock(&metadata_gpu_lock);
	module_put(old);
	return ret;
}

static noinline long NBDRIVER_NO_CFI
metadata_gpu_call(unsigned long address, void *private, struct nbdriver_gpu_object_info *info)
{
	return ((metadata_gpu_fn)address)(private, 0xc0300947U, info);
}

static long metadata_gpu(unsigned long argument)
{
	struct nbdriver_gpu_info_request req;
	struct task_struct *task;
	struct file *file;
	unsigned long query, ioctl_address;
	nbdriver_u32 code[7];
	unsigned int i;
	int ret;

	BUILD_BUG_ON(sizeof(req.info) != 48);
	if (copy_from_user(&req, (void __user *)argument, sizeof(req)))
		return -EFAULT;
	if (req.pid <= 0 || req.target_fd < 0 || !req.object_id || req.reserved)
		return -EINVAL;
	memset(&req.info, 0, sizeof(req.info));
	req.sample_ns = 0;
	req.result = -EOPNOTSUPP;
	if (!metadata_fget || !metadata_module_address) {
		ret = -EOPNOTSUPP;
		goto reply;
	}
	task = nbdriver_query_get_process(req.pid, req.expected_start_ns);
	if (IS_ERR(task)) {
		ret = PTR_ERR(task);
		goto reply;
	}
	req.expected_start_ns = READ_ONCE(task->start_boottime);
	file = metadata_get_file(task, req.target_fd);
	if (!file) {
		ret = -EBADF;
		goto put_task;
	}
	ret = -ENOTTY;
	if (!file->f_op || !file->private_data || !S_ISCHR(file_inode(file)->i_mode))
		goto put_file;
	ret = metadata_gpu_resolve(file, &query);
	if (ret)
		goto put_file;
	ioctl_address = (unsigned long)file->f_op->unlocked_ioctl;
	ret = -EOPNOTSUPP;
	if (!query || !metadata_gpu_owner(file, query, ioctl_address))
		goto put_file;
	/* File reference keeps vendor module text alive for this profile and call. */
	for (i = 0; i < ARRAY_SIZE(code); ++i)
		code[i] = READ_ONCE(((const u32 *)query)[i]);
	if (!nbdriver_kgsl_profile_matches(code))
		goto put_file;
	req.info.id = req.object_id;
	req.result = metadata_gpu_call(query, file->private_data, &req.info);
	req.sample_ns = ktime_get_ns();
	ret = 0;
	if (!req.result && req.info.id != req.object_id)
		ret = -EPROTO;
	/* The vendor return is separate from transport/validation errors. */
put_file:
	fput(file);
put_task:
	nbdriver_query_put_process(task);
reply:
	if (ret)
		req.result = ret;
	if (copy_to_user((void __user *)argument, &req, sizeof(req)))
		return -EFAULT;
	return ret;
}

static long metadata_random(unsigned long argument)
{
	struct nbdriver_random_request req;
	u8 bytes[NBDRIVER_RANDOM_MAX];
	int ret = 0;

	if (copy_from_user(&req, (void __user *)argument, sizeof(req)))
		return -EFAULT;
	if (req.reserved || !req.buffer || !req.size || req.size > sizeof(bytes))
		return -EINVAL;
	req.copied = 0;
	get_random_bytes(bytes, req.size);
	if (copy_to_user((void __user *)(unsigned long)req.buffer, bytes, req.size))
		ret = -EFAULT;
	else
		req.copied = req.size;
	memzero_explicit(bytes, sizeof(bytes));
	req.error = ret;
	if (copy_to_user((void __user *)argument, &req, sizeof(req)))
		return -EFAULT;
	return ret;
}

long nbdriver_metadata_ioctl(unsigned int command, unsigned long argument)
{
	if (!capable(CAP_SYS_PTRACE))
		return -EPERM;
	switch (command) {
	case NBDRIVER_IOC_MAPS: return metadata_maps(argument);
	case NBDRIVER_IOC_MAPPING: return metadata_mapping(argument, false);
	case NBDRIVER_IOC_VMA: return metadata_mapping(argument, true);
	case NBDRIVER_IOC_FD_SIZE: return metadata_fd_size(argument);
	case NBDRIVER_IOC_GPU_INFO: return metadata_gpu(argument);
	case NBDRIVER_IOC_RANDOM: return metadata_random(argument);
	default: return -ENOTTY;
	}
}
/* ===== END src/features/nbdriver_metadata.c ===== */

/* ===== BEGIN src/features/nbdriver_mapping.c ===== */
// SPDX-License-Identifier: GPL-2.0-only
/* Read-only page-table identity query, adapted from NBG-driver/query.c. */
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/pgtable.h>
#include <linux/spinlock.h>
#include <linux/version.h>
#include <asm/memory.h>
#include <asm/pgtable.h>
#include "nbdriver_abi.h"
#include "nbdriver_mapping.h"

static int mapping_leaf(struct vm_area_struct *vma, unsigned long address,
		pte_t entry, u64 base, u64 granule, struct nbdriver_mapping_request *req)
{
	unsigned long attr = pte_val(entry) & PTE_ATTRINDX_MASK;
	unsigned long pfn;

	if (!pte_valid(entry) || !(pte_val(entry) & PTE_USER))
		return -ENODATA;
	if (attr != PTE_ATTRINDX(MT_NORMAL) && attr != PTE_ATTRINDX(MT_NORMAL_NC)
#ifdef MT_NORMAL_TAGGED
	    && attr != PTE_ATTRINDX(MT_NORMAL_TAGGED)
#endif
	   )
		return -EOPNOTSUPP;
	pfn = (base + (address & (granule - 1))) >> PAGE_SHIFT;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
	if (!pfn_is_map_memory(pfn))
#else
	if (!pfn_valid(pfn))
#endif
		return -EOPNOTSUPP;
	req->physical = base + (address & (granule - 1));
	req->page_size = granule;
	req->vma_start = vma->vm_start;
	req->vma_end = vma->vm_end;
	/* Match ARM64's effective read permission for write-only VMAs.
	 * MAPS still reports the original VMA flags; neither query changes them. */
	req->flags = (vma->vm_flags & (VM_READ | VM_WRITE) ? NBDRIVER_MAPPING_READ : 0) |
		(vma->vm_flags & VM_WRITE ? NBDRIVER_MAPPING_WRITE : 0) |
		(vma->vm_flags & VM_EXEC ? NBDRIVER_MAPPING_EXEC : 0) |
		(vma->vm_flags & VM_MAYSHARE ? NBDRIVER_MAPPING_SHARED : 0);
	return 0;
}

int nbdriver_mapping_locked(struct vm_area_struct *vma, unsigned long address,
			    struct nbdriver_mapping_request *req)
{
	struct mm_struct *mm = vma->vm_mm;
	pgd_t *pgdp, pgd;
	p4d_t *p4dp, p4d;
	pud_t *pudp, pud;
	pmd_t *pmdp, pmd;
	pte_t *ptep, pte;
	spinlock_t *ptl;
	int attempt, ret;

	/* PFNMAP/IO can describe KGSL normal RAM; validate the actual leaf instead.
	 * hugetlb needs a distinct locking protocol and is explicitly unsupported. */
	if (vma->vm_flags & VM_HUGETLB)
		return -EOPNOTSUPP;
	pgdp = pgd_offset(mm, address);
	pgd = READ_ONCE(*pgdp);
	if (pgd_none(pgd) || pgd_bad(pgd))
		return -ENODATA;
	p4dp = p4d_offset(pgdp, address);
	p4d = READ_ONCE(*p4dp);
	if (p4d_none(p4d) || p4d_bad(p4d))
		return -ENODATA;
	pudp = pud_offset(p4dp, address);
	ptl = pud_lock(mm, pudp);
	pud = READ_ONCE(*pudp);
	if (pud_none(pud) || !pud_present(pud)) {
		spin_unlock(ptl);
		return -ENODATA;
	}
	if (pud_leaf(pud)) {
		ret = mapping_leaf(vma, address, __pte(pud_val(pud)),
			(u64)pud_pfn(pud) << PAGE_SHIFT, PUD_SIZE, req);
		spin_unlock(ptl);
		return ret;
	}
	if (pud_bad(pud)) {
		spin_unlock(ptl);
		return -ENODATA;
	}
	spin_unlock(ptl);
	for (attempt = 0; attempt < 3; ++attempt) {
		pmdp = pmd_offset(pudp, address);
		ptl = pmd_lock(mm, pmdp);
		pmd = READ_ONCE(*pmdp);
		if (pmd_none(pmd) || !pmd_present(pmd)) {
			spin_unlock(ptl);
			return -ENODATA;
		}
		if (pmd_leaf(pmd)) {
			ret = mapping_leaf(vma, address, __pte(pmd_val(pmd)),
				(u64)pmd_pfn(pmd) << PAGE_SHIFT, PMD_SIZE, req);
			spin_unlock(ptl);
			return ret;
		}
		if (pmd_bad(pmd)) {
			spin_unlock(ptl);
			return -ENODATA;
		}
		spin_unlock(ptl);
		ptep = pte_offset_map_lock(mm, pmdp, address, &ptl);
		if (!ptep)
			continue;
		pte = READ_ONCE(*ptep);
		ret = mapping_leaf(vma, address, pte,
			(u64)pte_pfn(pte) << PAGE_SHIFT, PAGE_SIZE, req);
		pte_unmap_unlock(ptep, ptl);
		return ret;
	}
	return -EAGAIN;
}
/* ===== END src/features/nbdriver_mapping.c ===== */

/* ===== BEGIN src/features/nbdriver_devpts.c ===== */
// SPDX-License-Identifier: GPL-2.0-only
/* Automatically discover the two exact argv[0] names. The immutable snapshot
 * holds PID + mm identities; neither PID reuse nor exec inherits a match. */
#include <linux/atomic.h>
#include <linux/capability.h>
#include <linux/dcache.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/kdev_t.h>
#include <linux/kprobes.h>
#include <linux/magic.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/rcupdate.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <asm/ptrace.h>

#include "nbdriver_abi.h"
#include "nbdriver_cfi.h"
#include "nbdriver_devpts.h"
#include "nbdriver_log.h"
#include "nbdriver_read.h"

#define NBDRIVER_DEVPTS_SLAVE_MAJOR 136U
#define NBDRIVER_DEVPTS_SCAN_DELAY msecs_to_jiffies(500)

static const char *const nbdriver_devpts_packages[] = {
	"com.tencent.tmgp.dfm", "com.tencent.rmcn",
};

struct nbdriver_devpts_target {
	struct nbdriver_devpts_target *next;
	struct pid *pid;
	struct mm_struct *mm;
	u32 package;
};

struct nbdriver_devpts_state {
	struct nbdriver_devpts_target *targets;
	u32 count;
};

static DEFINE_MUTEX(nbdriver_devpts_lock);
static struct nbdriver_devpts_state __rcu *nbdriver_devpts_state;
static struct kprobe nbdriver_devpts_permission_kp;
static struct kprobe nbdriver_devpts_getattr_kp;
static bool nbdriver_devpts_permission_active;
static bool nbdriver_devpts_getattr_active;
static bool nbdriver_devpts_stopping;
static u64 nbdriver_devpts_generation;
static int nbdriver_devpts_last_error;
static atomic64_t nbdriver_devpts_permission_hits = ATOMIC64_INIT(0);
static atomic64_t nbdriver_devpts_getattr_hits = ATOMIC64_INIT(0);

static int nbdriver_devpts_package_index(const char *name)
{
	u32 i;

	for (i = 0; i < ARRAY_SIZE(nbdriver_devpts_packages); ++i)
		if (!strcmp(name, nbdriver_devpts_packages[i]))
			return (int)i;
	return -ENOENT;
}

static bool nbdriver_devpts_target_current(const struct nbdriver_devpts_state *state)
{
	const struct nbdriver_devpts_target *target;

	if (!state)
		return false;
	for (target = state->targets; target; target = target->next)
		if (target->pid == task_tgid(current) &&
		    target->mm == READ_ONCE(current->mm))
			return true;
	return false;
}

static bool nbdriver_devpts_inode_matches(const struct inode *inode)
{
	return inode && inode->i_sb && S_ISCHR(inode->i_mode) &&
		inode->i_sb->s_magic == DEVPTS_SUPER_MAGIC &&
		imajor(inode) == NBDRIVER_DEVPTS_SLAVE_MAJOR &&
		iminor(inode) <= NBDRIVER_DEVPTS_MAX_INDEX;
}

static bool nbdriver_devpts_match_inode(const struct inode *inode)
{
	const struct nbdriver_devpts_state *state;
	bool match;

	if (!nbdriver_devpts_inode_matches(inode))
		return false;
	rcu_read_lock();
	state = rcu_dereference(nbdriver_devpts_state);
	match = nbdriver_devpts_target_current(state);
	rcu_read_unlock();
	return match;
}

static int NBDRIVER_NO_CFI nbdriver_devpts_return_enoent(struct pt_regs *regs)
{
	regs_set_return_value(regs, (unsigned long)-ENOENT);
	instruction_pointer_set(regs, procedure_link_pointer(regs));
	return 1;
}

static int nbdriver_devpts_permission_pre(struct kprobe *probe, struct pt_regs *regs)
{
	const struct inode *inode = (const struct inode *)(unsigned long)regs->regs[0];
	int mask = (int)regs->regs[1];

	(void)probe;
	if (!(mask & MAY_ACCESS) || !nbdriver_devpts_match_inode(inode))
		return 0;
	atomic64_inc(&nbdriver_devpts_permission_hits);
	return nbdriver_devpts_return_enoent(regs);
}

static int nbdriver_devpts_getattr_pre(struct kprobe *probe, struct pt_regs *regs)
{
	const struct path *path = (const struct path *)(unsigned long)regs->regs[0];

	(void)probe;
	if (!path || !path->dentry ||
	    !nbdriver_devpts_match_inode(d_backing_inode(path->dentry)))
		return 0;
	atomic64_inc(&nbdriver_devpts_getattr_hits);
	return nbdriver_devpts_return_enoent(regs);
}

static void nbdriver_devpts_drop_hooks_locked(void)
{
	if (nbdriver_devpts_getattr_active) {
		unregister_kprobe(&nbdriver_devpts_getattr_kp);
		nbdriver_devpts_getattr_active = false;
	}
	if (nbdriver_devpts_permission_active) {
		unregister_kprobe(&nbdriver_devpts_permission_kp);
		nbdriver_devpts_permission_active = false;
	}
}

static int nbdriver_devpts_install_hooks_locked(void)
{
	int ret;

	memset(&nbdriver_devpts_permission_kp, 0, sizeof(nbdriver_devpts_permission_kp));
	nbdriver_devpts_permission_kp.symbol_name = "security_inode_permission";
	nbdriver_devpts_permission_kp.pre_handler = nbdriver_devpts_permission_pre;
	ret = register_kprobe(&nbdriver_devpts_permission_kp);
	if (ret)
		return ret;
	nbdriver_devpts_permission_active = true;
	memset(&nbdriver_devpts_getattr_kp, 0, sizeof(nbdriver_devpts_getattr_kp));
	nbdriver_devpts_getattr_kp.symbol_name = "security_inode_getattr";
	nbdriver_devpts_getattr_kp.pre_handler = nbdriver_devpts_getattr_pre;
	ret = register_kprobe(&nbdriver_devpts_getattr_kp);
	if (ret) {
		nbdriver_devpts_drop_hooks_locked();
		return ret;
	}
	nbdriver_devpts_getattr_active = true;
	return 0;
}

static void nbdriver_devpts_free_state(struct nbdriver_devpts_state *state)
{
	struct nbdriver_devpts_target *target, *next;

	if (!state)
		return;
	for (target = state->targets; target; target = next) {
		next = target->next;
		put_pid(target->pid);
		mmdrop(target->mm);
		kfree(target);
	}
	kfree(state);
}

/* No target page faults, no userspace calls, and no sleeping in a probe. */
static int nbdriver_devpts_read_package(struct mm_struct *mm)
{
	struct nbdriver_read_request read = {};
	unsigned long start = READ_ONCE(mm->arg_start);
	unsigned long end = READ_ONCE(mm->arg_end);
	char name[32];

	if (!start || end <= start)
		return -ENOENT;
	read.address = start;
	read.size = min_t(unsigned long, end - start, sizeof(name));
	(void)nbdriver_read_mm(mm, &read, (u8 *)name, false);
	if (!memchr(name, '\0', read.copied))
		return -ENOENT;
	return nbdriver_devpts_package_index(name);
}

static int nbdriver_devpts_collect(struct nbdriver_devpts_state **result)
{
	struct nbdriver_devpts_state *state;
	struct task_struct **tasks, *task;
	size_t capacity = 32, count = 0, i;
	int ret = 0;

	/* Size from all existing userspace leaders: no fixed 512-candidate cap.
	 * Tasks born between these two passes are collected on the next refresh. */
	rcu_read_lock();
	for_each_process(task)
		if (!(READ_ONCE(task->flags) & PF_KTHREAD))
			++capacity;
	rcu_read_unlock();
	tasks = kvcalloc(capacity, sizeof(*tasks), GFP_KERNEL);
	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (!tasks || !state) {
		kvfree(tasks);
		kfree(state);
		return -ENOMEM;
	}
	rcu_read_lock();
	for_each_process(task) {
		if (READ_ONCE(task->flags) & PF_KTHREAD)
			continue;
		if (count == capacity)
			break;
		get_task_struct(task);
		tasks[count++] = task;
	}
	rcu_read_unlock();
	for (i = 0; i < count; ++i) {
		struct nbdriver_devpts_target *target;
		struct mm_struct *mm;
		int package;

		task = tasks[i];
		if (READ_ONCE(task->flags) & PF_EXITING)
			continue;
		mm = get_task_mm(task);
		if (!mm)
			continue;
		package = nbdriver_devpts_read_package(mm);
		if (package < 0 || READ_ONCE(task->mm) != mm ||
		    (READ_ONCE(task->flags) & PF_EXITING)) {
			mmput(mm);
			continue;
		}
		target = kzalloc(sizeof(*target), GFP_KERNEL);
		if (!target) {
			mmput(mm);
			ret = -ENOMEM;
			break;
		}
		rcu_read_lock();
		if (pid_alive(task))
			target->pid = get_pid(task_tgid(task));
		rcu_read_unlock();
		if (!target->pid) {
			mmput(mm);
			kfree(target);
			continue;
		}
		/* Keep only mm_count, allowing exit to reclaim mappings normally. */
		mmgrab(mm);
		target->mm = mm;
		target->package = (u32)package;
		mmput(mm);
		target->next = state->targets;
		state->targets = target;
		++state->count;
	}
	for (i = 0; i < count; ++i)
		put_task_struct(tasks[i]);
	kvfree(tasks);
	if (ret) {
		nbdriver_devpts_free_state(state);
		return ret;
	}
	*result = state;
	return 0;
}

static bool nbdriver_devpts_same_state(const struct nbdriver_devpts_state *a,
				     const struct nbdriver_devpts_state *b)
{
	const struct nbdriver_devpts_target *x, *y;

	if ((a ? a->count : 0) != (b ? b->count : 0))
		return false;
	if (!a || !a->count)
		return true;
	for (x = a->targets; x; x = x->next) {
		for (y = b->targets; y; y = y->next)
			if (x->pid == y->pid && x->mm == y->mm &&
			    x->package == y->package)
				break;
		if (!y)
			return false;
	}
	return true;
}

static void nbdriver_devpts_discover(struct work_struct *work);
static DECLARE_DELAYED_WORK(nbdriver_devpts_work, nbdriver_devpts_discover);

static void nbdriver_devpts_discover(struct work_struct *work)
{
	struct nbdriver_devpts_state *fresh = NULL, *old;
	u32 count;
	int ret;

	(void)work;
	if (READ_ONCE(nbdriver_devpts_stopping))
		return;
	ret = nbdriver_devpts_collect(&fresh);
	mutex_lock(&nbdriver_devpts_lock);
	nbdriver_devpts_last_error = ret;
	old = rcu_dereference_protected(nbdriver_devpts_state,
		lockdep_is_held(&nbdriver_devpts_lock));
	if (!ret && !nbdriver_devpts_same_state(old, fresh)) {
		count = fresh->count;
		rcu_assign_pointer(nbdriver_devpts_state, fresh);
		fresh = old;
		++nbdriver_devpts_generation;
		nbd_info("stage=devpts_auto targets=%u generation=%llu\n", count,
			 (unsigned long long)nbdriver_devpts_generation);
	}
	mutex_unlock(&nbdriver_devpts_lock);
	if (fresh) {
		synchronize_rcu();
		nbdriver_devpts_free_state(fresh);
	}
	if (ret)
		nbd_warn("stage=devpts_auto operation=scan error=%d\n", ret);
	if (!READ_ONCE(nbdriver_devpts_stopping))
		schedule_delayed_work(&nbdriver_devpts_work, NBDRIVER_DEVPTS_SCAN_DELAY);
}

int nbdriver_devpts_init(void)
{
	int ret;

	mutex_lock(&nbdriver_devpts_lock);
	ret = nbdriver_devpts_install_hooks_locked();
	mutex_unlock(&nbdriver_devpts_lock);
	if (ret) {
		nbd_err("stage=devpts_auto operation=install_hooks error=%d\n", ret);
		return ret;
	}
	WRITE_ONCE(nbdriver_devpts_stopping, false);
	schedule_delayed_work(&nbdriver_devpts_work, 0);
	nbd_info("stage=devpts_auto status=armed max_index=%u\n",
		 NBDRIVER_DEVPTS_MAX_INDEX);
	return 0;
}

void nbdriver_devpts_exit(void)
{
	struct nbdriver_devpts_state *old;

	WRITE_ONCE(nbdriver_devpts_stopping, true);
	cancel_delayed_work_sync(&nbdriver_devpts_work);
	mutex_lock(&nbdriver_devpts_lock);
	old = rcu_dereference_protected(nbdriver_devpts_state,
		lockdep_is_held(&nbdriver_devpts_lock));
	RCU_INIT_POINTER(nbdriver_devpts_state, NULL);
	nbdriver_devpts_drop_hooks_locked();
	mutex_unlock(&nbdriver_devpts_lock);
	synchronize_rcu();
	nbdriver_devpts_free_state(old);
}

u32 nbdriver_devpts_features(void)
{
	return NBDRIVER_FEATURE_DEVPTS;
}

long nbdriver_devpts_status_ioctl(unsigned long argument)
{
	struct nbdriver_devpts_status status = { .size = sizeof(status) };
	const struct nbdriver_devpts_state *state;
	const struct nbdriver_devpts_target *target;

	if (!capable(CAP_SYS_PTRACE))
		return -EPERM;
	mutex_lock(&nbdriver_devpts_lock);
	status.active = nbdriver_devpts_permission_active && nbdriver_devpts_getattr_active;
	status.max_index = NBDRIVER_DEVPTS_MAX_INDEX;
	status.generation = nbdriver_devpts_generation;
	status.last_error = nbdriver_devpts_last_error;
	state = rcu_dereference_protected(nbdriver_devpts_state,
		lockdep_is_held(&nbdriver_devpts_lock));
	for (target = state ? state->targets : NULL; target; target = target->next) {
		struct task_struct *task = get_pid_task(target->pid, PIDTYPE_TGID);
		pid_t pid = 0;

		if (task) {
			if (pid_alive(task) && !(READ_ONCE(task->flags) & PF_EXITING) &&
			    READ_ONCE(task->mm) == target->mm)
				pid = task_tgid_vnr(task);
			put_task_struct(task);
		}
		if (pid > 0) {
			++status.target_count;
			if (!status.pids[target->package])
				status.pids[target->package] = pid;
		}
	}
	status.permission_hits = atomic64_read(&nbdriver_devpts_permission_hits);
	status.getattr_hits = atomic64_read(&nbdriver_devpts_getattr_hits);
	mutex_unlock(&nbdriver_devpts_lock);
	return copy_to_user((void __user *)argument, &status, sizeof(status)) ? -EFAULT : 0;
}
/* ===== END src/features/nbdriver_devpts.c ===== */


