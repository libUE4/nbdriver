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
	metadata_module_address = NULL;
	metadata_getattr = NULL;
	metadata_fget = NULL;
}

u32 nbdriver_metadata_features(void)
{
	u32 features = NBDRIVER_FEATURE_MAPS | NBDRIVER_FEATURE_MAPPING |
		NBDRIVER_FEATURE_RANDOM;

	if (metadata_fget && metadata_getattr)
		features |= NBDRIVER_FEATURE_FD_SIZE;
	/* Do not cache vendor text addresses: the KGSL module can load later.
	 * A request pins its file first, then validates ownership and profile. */
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

static long metadata_mapping(unsigned long argument)
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
	ret = !vma || address < vma->vm_start ? -ENOENT :
		nbdriver_mapping_locked(vma, address, &req);
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

	rcu_read_lock();
	matches = metadata_module_address(query) == owner &&
		metadata_module_address(ioctl_address) == owner;
	rcu_read_unlock();
	return matches;
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
	ioctl_address = nbdriver_ksym_lookup_optional("kgsl_ioctl");
	if (!ioctl_address || (void *)file->f_op->unlocked_ioctl != (void *)ioctl_address)
		goto put_file;
	query = nbdriver_ksym_lookup_optional("kgsl_ioctl_gpuobj_info");
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
	case NBDRIVER_IOC_MAPPING: return metadata_mapping(argument);
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
/* PID-scoped /dev/pts probe filter adapted from NBG-driver/devpts_filter.c.
 * Hooks are installed only for an explicit ENABLE request and are removed by
 * DISABLE/module exit. No package auto-discovery or hidden-process state is
 * carried into nbdriver. */
#include <linux/atomic.h>
#include <linux/capability.h>
#include <linux/dcache.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kdev_t.h>
#include <linux/kprobes.h>
#include <linux/magic.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <asm/ptrace.h>

#include "nbdriver_abi.h"
#include "nbdriver_cfi.h"
#include "nbdriver_devpts.h"
#include "nbdriver_log.h"

#define NBDRIVER_DEVPTS_SLAVE_MAJOR 136U

struct nbdriver_devpts_state {
	struct pid *target_pid;
	u32 max_index;
};

static DEFINE_MUTEX(nbdriver_devpts_lock);
static struct nbdriver_devpts_state __rcu *nbdriver_devpts_state;
static struct kprobe nbdriver_devpts_permission_kp;
static struct kprobe nbdriver_devpts_getattr_kp;
static bool nbdriver_devpts_permission_active;
static bool nbdriver_devpts_getattr_active;
static atomic64_t nbdriver_devpts_permission_hits = ATOMIC64_INIT(0);
static atomic64_t nbdriver_devpts_getattr_hits = ATOMIC64_INIT(0);

static bool nbdriver_devpts_target_current(const struct nbdriver_devpts_state *state)
{
	/* target_pid is held until after synchronize_rcu(), so pid_nr cannot be
	 * recycled while a hook is comparing the current tgid. */
	return state && state->target_pid &&
		pid_nr(state->target_pid) == task_tgid_nr(current);
}

static bool nbdriver_devpts_inode_matches(const struct inode *inode,
						 const struct nbdriver_devpts_state *state)
{
	if (!inode || !inode->i_sb || !S_ISCHR(inode->i_mode) ||
		inode->i_sb->s_magic != DEVPTS_SUPER_MAGIC ||
		imajor(inode) != NBDRIVER_DEVPTS_SLAVE_MAJOR)
		return false;
	return iminor(inode) <= state->max_index;
}

static const struct nbdriver_devpts_state *nbdriver_devpts_match_inode(
						const struct inode *inode)
{
	const struct nbdriver_devpts_state *state;

	rcu_read_lock();
	state = rcu_dereference(nbdriver_devpts_state);
	if (!nbdriver_devpts_target_current(state) ||
		!nbdriver_devpts_inode_matches(inode, state))
		state = NULL;
	/* The returned pointer is consumed only while this RCU read section is
	 * active by the callers below. */
	return state;
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
	const struct nbdriver_devpts_state *state;
	int result = 0;

	(void)probe;
	if (!(mask & MAY_ACCESS))
		return 0;
	state = nbdriver_devpts_match_inode(inode);
	if (state) {
		atomic64_inc(&nbdriver_devpts_permission_hits);
		result = nbdriver_devpts_return_enoent(regs);
	}
	rcu_read_unlock();
	return result;
}

static int nbdriver_devpts_getattr_pre(struct kprobe *probe, struct pt_regs *regs)
{
	const struct path *path = (const struct path *)(unsigned long)regs->regs[0];
	const struct inode *inode;
	const struct nbdriver_devpts_state *state;
	int result = 0;

	(void)probe;
	if (!path || !path->dentry)
		return 0;
	inode = d_backing_inode(path->dentry);
	state = nbdriver_devpts_match_inode(inode);
	if (state) {
		atomic64_inc(&nbdriver_devpts_getattr_hits);
		result = nbdriver_devpts_return_enoent(regs);
	}
	rcu_read_unlock();
	return result;
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

	if (nbdriver_devpts_permission_active && nbdriver_devpts_getattr_active)
		return 0;
	nbdriver_devpts_drop_hooks_locked();
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
	if (!state)
		return;
	if (state->target_pid)
		put_pid(state->target_pid);
	kfree(state);
}

static void nbdriver_devpts_fill_locked(struct nbdriver_devpts_filter_request *req)
{
	const struct nbdriver_devpts_state *state = rcu_dereference_protected(
		nbdriver_devpts_state, lockdep_is_held(&nbdriver_devpts_lock));

	req->abi_version = NBDRIVER_DEVPTS_FILTER_ABI_VERSION;
	req->enabled = state && nbdriver_devpts_permission_active &&
		nbdriver_devpts_getattr_active;
	req->pid = state && state->target_pid ? pid_nr(state->target_pid) : 0;
	req->max_index = state ? state->max_index : 0;
	req->permission_hits = atomic64_read(&nbdriver_devpts_permission_hits);
	req->getattr_hits = atomic64_read(&nbdriver_devpts_getattr_hits);
}

int nbdriver_devpts_init(void)
{
	return 0;
}

void nbdriver_devpts_exit(void)
{
	struct nbdriver_devpts_state *old;

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

long nbdriver_devpts_ioctl(unsigned long argument)
{
	struct nbdriver_devpts_filter_request req;
	struct nbdriver_devpts_state *new_state = NULL, *old_state;
	struct pid *pid = NULL;
	int ret = 0;

	if (!capable(CAP_SYS_PTRACE))
		return -EPERM;
	if (copy_from_user(&req, (void __user *)argument, sizeof(req)))
		return -EFAULT;
	if (req.abi_version != NBDRIVER_DEVPTS_FILTER_ABI_VERSION)
		return -EPROTO;
	if (req.enabled > NBDRIVER_DEVPTS_FILTER_QUERY)
		return -EINVAL;
	if (req.enabled == NBDRIVER_DEVPTS_FILTER_QUERY) {
		mutex_lock(&nbdriver_devpts_lock);
		nbdriver_devpts_fill_locked(&req);
		mutex_unlock(&nbdriver_devpts_lock);
		goto copy_reply;
	}
	if (req.enabled == NBDRIVER_DEVPTS_FILTER_ENABLE) {
		if (req.pid <= 0 || req.max_index > NBDRIVER_DEVPTS_FILTER_MAX_INDEX)
			return -EINVAL;
		pid = find_get_pid(req.pid);
	{
		struct task_struct *task = pid ? get_pid_task(pid, PIDTYPE_TGID) : NULL;

		if (!task) {
			put_pid(pid);
			return -ESRCH;
		}
		put_task_struct(task);
	}
		new_state = kzalloc(sizeof(*new_state), GFP_KERNEL);
		if (!new_state) {
			put_pid(pid);
			return -ENOMEM;
		}
		new_state->target_pid = pid;
		new_state->max_index = req.max_index;
	}

	mutex_lock(&nbdriver_devpts_lock);
	old_state = rcu_dereference_protected(nbdriver_devpts_state,
		lockdep_is_held(&nbdriver_devpts_lock));
	if (req.enabled == NBDRIVER_DEVPTS_FILTER_ENABLE) {
		ret = nbdriver_devpts_install_hooks_locked();
		if (!ret) {
			rcu_assign_pointer(nbdriver_devpts_state, new_state);
			new_state = NULL;
			atomic64_set(&nbdriver_devpts_permission_hits, 0);
			atomic64_set(&nbdriver_devpts_getattr_hits, 0);
		}
	} else {
		RCU_INIT_POINTER(nbdriver_devpts_state, NULL);
		nbdriver_devpts_drop_hooks_locked();
		atomic64_set(&nbdriver_devpts_permission_hits, 0);
		atomic64_set(&nbdriver_devpts_getattr_hits, 0);
	}
	if (ret)
		nbdriver_devpts_fill_locked(&req);
	else
		nbdriver_devpts_fill_locked(&req);
	mutex_unlock(&nbdriver_devpts_lock);
	if (!ret && old_state) {
		synchronize_rcu();
		nbdriver_devpts_free_state(old_state);
	}
	nbdriver_devpts_free_state(new_state);

copy_reply:
	req.enabled = req.enabled;
	if (copy_to_user((void __user *)argument, &req, sizeof(req)))
		return -EFAULT;
	return ret;
}
/* ===== END src/features/nbdriver_devpts.c ===== */

