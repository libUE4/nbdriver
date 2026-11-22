/* ===== BEGIN src/features/nbdriver_metadata.c ===== */
// SPDX-License-Identifier: GPL-2.0-only
/*
 * 当前职责：maps/VMA/mapping、FD/KGSL/random、devpts 与 proc 可见性。
 * 文件中的 BEGIN/END 标签保留来源边界，不是现有目录。
 * 元数据、自动 devpts 与进程/GPU 过滤有不同权限和生命周期，不能相互代替。
 * 可选 KGSL/proc 能力按后端发现或注册结果发布，feature 不保证所有厂商布局。
 * devpts 随模块初始化自动绑定双包；状态 ioctl 只读，旧手动命令 0x1c 已退役。
 */
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
#include <linux/magic.h>
#include <linux/mutex.h>
#include <linux/namei.h>
#include <linux/preempt.h>
#include <linux/pid.h>
#include <linux/proc_fs.h>
#include <linux/random.h>
#include <linux/rcupdate.h>
#include <linux/srcu.h>
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
static __typeof__(&dentry_open) metadata_open;
static __typeof__(&vfs_llseek) metadata_llseek;

/* Proc visibility follows NBG-driver globally: hide registered identities
 * from proc enumeration/path access without a game/devpts viewer binding. */
static bool nbdriver_proc_path_hidden(const struct dentry *dentry);
static bool nbdriver_proc_name_hidden(const char *name, int length);
static int nbdriver_proc_visibility_init(void);
static void nbdriver_proc_visibility_exit(void);
static int nbdriver_pid_table_visibility_init(void);
static void nbdriver_pid_table_visibility_exit(void);
static u32 nbdriver_pid_table_visibility_features(void);
static int nbdriver_module_trace_init(void);
static void nbdriver_module_trace_exit(void);
static u32 nbdriver_module_trace_features(void);
static int nbdriver_kallsyms_init(void);
static void nbdriver_kallsyms_exit(void);
static u32 nbdriver_kallsyms_features(void);
static int nbdriver_sysfs_module_init(void);
static void nbdriver_sysfs_module_exit(void);
static u32 nbdriver_sysfs_module_features(void);
static int nbdriver_pidfd_visibility_init(void);
static void nbdriver_pidfd_visibility_exit(void);
static u32 nbdriver_pidfd_visibility_features(void);
static int nbdriver_printk_visibility_init(void);
static void nbdriver_printk_visibility_exit(void);
static u32 nbdriver_printk_visibility_features(void);
static u32 nbdriver_proc_visibility_features(void);
int nbdriver_metadata_init(void)
{
	metadata_fget = (__typeof__(metadata_fget))nbdriver_ksym_lookup_optional("fget_task");
	metadata_getattr = (__typeof__(metadata_getattr))nbdriver_ksym_lookup_optional("vfs_getattr");
	metadata_open = (__typeof__(metadata_open))nbdriver_ksym_lookup_optional("dentry_open");
	metadata_llseek = (__typeof__(metadata_llseek))nbdriver_ksym_lookup_optional("vfs_llseek");
	nbdriver_kgsl_init();
	/* Optional backends must not stop the base driver from initializing. */
	return 0;
}

void nbdriver_metadata_exit(void)
{
	nbdriver_kgsl_exit();
	metadata_getattr = NULL;
	metadata_open = NULL;
	metadata_llseek = NULL;
	metadata_fget = NULL;
}

u32 nbdriver_metadata_features(void)
{
	u32 features = NBDRIVER_FEATURE_MAPS | NBDRIVER_FEATURE_MAPPING | NBDRIVER_FEATURE_VMA |
		NBDRIVER_FEATURE_RANDOM;

	if (metadata_fget && metadata_getattr)
		features |= NBDRIVER_FEATURE_FD_SIZE;
	features |= nbdriver_kgsl_features();
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

/* Reopen the same regular inode with an independent open-file description.
 * Seeking fget_task()'s file directly would change the game's shared f_pos.
 * The authorized target's credentials also work for an unlinked cache inode. */
static noinline int NBDRIVER_NO_CFI
metadata_seek_copy(struct file *file, loff_t offset, unsigned int whence,
		   loff_t *result)
{
	struct file *copy;

	copy = metadata_open(&file->f_path, O_RDONLY | O_LARGEFILE, file->f_cred);
	if (IS_ERR(copy))
		return PTR_ERR(copy);
	*result = metadata_llseek(copy, offset, whence);
	fput(copy);
	return 0;
}

static long metadata_fd_seek(unsigned long argument)
{
	struct nbdriver_fd_seek_request req;
	struct task_struct *task;
	struct file *file;
	int ret = 0;

	if (copy_from_user(&req, (void __user *)argument, sizeof(req)))
		return -EFAULT;
	if (req.pid <= 0 || req.target_fd < 0 || req.reserved ||
	    (req.whence != SEEK_SET && req.whence != SEEK_END &&
	     req.whence != SEEK_DATA && req.whence != SEEK_HOLE))
		return -EINVAL;
	req.result = 0;
	req.error = 0;
	if (!metadata_fget || !metadata_open || !metadata_llseek) {
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
		if (!S_ISREG(file_inode(file)->i_mode))
			ret = -EOPNOTSUPP;
		else
			ret = metadata_seek_copy(file, req.offset, req.whence, &req.result);
		fput(file);
	}
	nbdriver_query_put_process(task);
reply:
	req.error = ret;
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
	if (!nbdriver_sf_read_caller() && !capable(CAP_SYS_PTRACE))
		return -EPERM;
	switch (command) {
	case NBDRIVER_IOC_MAPS: return metadata_maps(argument);
	case NBDRIVER_IOC_MAPPING: return metadata_mapping(argument, false);
	case NBDRIVER_IOC_VMA: return metadata_mapping(argument, true);
	case NBDRIVER_IOC_FD_SIZE: return metadata_fd_size(argument);
	case NBDRIVER_IOC_FD_SEEK: return metadata_fd_seek(argument);
	case NBDRIVER_IOC_GPU_INFO: return nbdriver_kgsl_info_ioctl(argument);
	case NBDRIVER_IOC_GPU_SYNC: return nbdriver_kgsl_sync_ioctl(argument);
	case NBDRIVER_IOC_GPU_VISIBILITY: return nbdriver_kgsl_visibility_ioctl(argument);
	case NBDRIVER_IOC_RANDOM: return metadata_random(argument);
	default: return -ENOTTY;
	}
}
/* ===== END src/features/nbdriver_metadata.c ===== */

/* ===== BEGIN src/features/nbdriver_kgsl.c ===== */
// SPDX-License-Identifier: GPL-2.0-only
/* Query decoding adapted from workspace enkernel/kgsl.c (module GPL).
 * Target validation, ABI, owner pinning and bounded snapshot are nbdriver's.
 * Never import foreign control entries or node creation overrides. */
#include <linux/fdtable.h>
#include <linux/idr.h>
#include <linux/kallsyms.h>
#include <linux/limits.h>
#include <linux/sched/signal.h>
#include <linux/spinlock.h>
#include <linux/xarray.h>

static int (*kgsl_read_fn)(void *, const void *, size_t);
static int (*kgsl_size_fn)(unsigned long, unsigned long *, unsigned long *);
static struct module *(*kgsl_module_fn)(unsigned long);
/* DDK exports do not guarantee a vendor export. Keep this optional so an
 * unexported XArray helper cannot prevent the entire module from loading. */
static __typeof__(&xas_find_marked) kgsl_xas_find_marked_fn;
static DEFINE_MUTEX(kgsl_lock);
static struct module *kgsl_owner;
static const struct file_operations *kgsl_fops;
static unsigned long kgsl_ioctl_key;
static bool kgsl_pinned;

static noinline int NBDRIVER_NO_CFI
kgsl_nofault(void *to, const void *from, size_t size)
{
    return kgsl_read_fn ? kgsl_read_fn(to, from, size) : -EOPNOTSUPP;
}
static noinline int NBDRIVER_NO_CFI
kgsl_symbol_size(unsigned long address, unsigned long *size, unsigned long *offset)
{
    return kgsl_size_fn ? kgsl_size_fn(address, size, offset) : 0;
}
/* Called under kgsl_lock with a verified file and owner reference. */
static noinline bool NBDRIVER_NO_CFI kgsl_text_owned(unsigned long address)
{
    struct module *owner;
    if (!address || !kgsl_module_fn)
        return false;
    preempt_disable();
    owner = kgsl_module_fn(address);
    preempt_enable();
    return owner == kgsl_owner;
}
#define KGSL_LAYOUT_SCAN_INSNS        256U
#define KGSL_LAYOUT_MAX_ADDS           48U
#define KGSL_LAYOUT_MAX_CANDIDATES     16U
#define KGSL_LAYOUT_PROC_SCAN_INSNS    24U
#define KGSL_LAYOUT_FIELD_USE_WINDOW    3U
#define KGSL_LAYOUT_CALL_WINDOW         12U
#define KGSL_LAYOUT_PAIR_DISTANCE      24U
#define KGSL_LAYOUT_MAX_FIELD_OFFSET 0x4000U
#define KGSL_LAYOUT_STUB_MAX_BYTES      16U
#define KGSL_LAYOUT_STUB_MAX_HOPS        3U
#define KGSL_SYNCSOURCE_STABLE_TRIES     3U
#define KGSL_SYNCSOURCE_XAS_RETRIES      8U

enum kgsl_peek_fail_stage {
    KGSL_PEEK_FAIL_NONE = 0,
    KGSL_PEEK_FAIL_FILE_IDENTITY = 1,
    KGSL_PEEK_FAIL_NOT_KGSL_FILE = 2,
    KGSL_PEEK_FAIL_RUNTIME_SYMBOLS = 3,
    KGSL_PEEK_FAIL_CREATE_LAYOUT = 4,
    KGSL_PEEK_FAIL_DESTROY_LAYOUT = 5,
    KGSL_PEEK_FAIL_LAYOUT_MISMATCH = 6,
    KGSL_PEEK_FAIL_LAYOUT_OFFSETS = 7,
    KGSL_PEEK_FAIL_PROCESS_PRIV = 8,
    KGSL_PEEK_FAIL_LIVE_IDR = 9,
    KGSL_PEEK_FAIL_LOWEST_FREE = 10,
};

struct kgsl_layout {
    unsigned int process_priv_offset;
    unsigned int syncsource_idr_offset;
    unsigned int syncsource_lock_offset;
};

struct kgsl_layout_candidate {
    struct kgsl_layout layout;
    unsigned int process_reg;
};

struct kgsl_add_imm {
    unsigned int index;
    unsigned int rd;
    unsigned int rn;
    unsigned int imm;
};

struct kgsl_runtime_symbols {
    unsigned long create;
    unsigned long destroy;
    unsigned long fops;
    unsigned long ioctl;
    unsigned long ioctl_cfi;
    unsigned long idr_alloc;
    unsigned long idr_alloc_cfi;
    unsigned long idr_find;
    unsigned long idr_find_cfi;
    unsigned long raw_spin_lock;
    unsigned long raw_spin_lock_cfi;
};

struct kgsl_call_targets {
    unsigned long direct;
    unsigned long cfi_jt;
};

static s64 kgsl_sign_extend(u64 value, unsigned int sign_bit)
{
    u64 sign = 1ULL << sign_bit;

    return (s64)((value ^ sign) - sign);
}

static bool kgsl_a64_bl(u32 insn, unsigned long pc,
                        unsigned long *target)
{
    s64 offset;

    if ((insn & 0xFC000000U) != 0x94000000U)
        return false;
    offset = kgsl_sign_extend(insn & 0x03FFFFFFU, 25U) * 4;
    *target = pc + offset;
    return true;
}

static bool kgsl_a64_unconditional_b(u32 insn)
{
    return (insn & 0xFC000000U) == 0x14000000U;
}

static unsigned long kgsl_resolve_plt_target(unsigned long plt)
{
    u32 code[4];

    if (kgsl_nofault(code, (const void *)plt, sizeof(code)))
        return 0;

    /* Current arm64 module PLT: ADRP X16; ADD X16, X16, #lo12; BR X16. */
    if ((code[0] & 0x9F00001FU) == 0x90000010U &&
        (code[1] & 0xFFC003FFU) == 0x91000210U &&
        code[2] == 0xD61F0200U) {
        u64 immlo = (code[0] >> 29) & 0x3U;
        u64 immhi = (code[0] >> 5) & 0x7FFFFU;
        s64 page_offset =
            kgsl_sign_extend((immhi << 2) | immlo, 20U) * 4096;
        unsigned long page = (unsigned long)
            ((s64)(plt & ~0xFFFUL) + page_offset);
        unsigned long lo12 = (code[1] >> 10) & 0xFFFU;

        return page + lo12;
    }

    /* Older arm64 module PLT: MOVN/MOVK/MOVK X16; BR X16. */
    if ((code[0] & 0xFFE0001FU) == 0x92800010U &&
        (code[1] & 0xFF80001FU) == 0xF2800010U &&
        (code[2] & 0xFF80001FU) == 0xF2800010U &&
        ((code[1] >> 21) & 0x3U) == 1U &&
        ((code[2] >> 21) & 0x3U) == 2U &&
        code[3] == 0xD61F0200U) {
        u64 value = ~((u64)(code[0] >> 5) & 0xFFFFULL);
        u64 imm16 = ((u64)(code[1] >> 5) & 0xFFFFULL) << 16;
        u64 imm32 = ((u64)(code[2] >> 5) & 0xFFFFULL) << 32;

        value = (value & ~0x00000000FFFF0000ULL) | imm16;
        value = (value & ~0x0000FFFF00000000ULL) | imm32;
        return (unsigned long)value;
    }

    return 0;
}

static bool kgsl_a64_hint(u32 insn)
{
    return (insn & 0xFFFFF01FU) == 0xD503201FU;
}

static bool kgsl_a64_b_imm(u32 insn, unsigned long pc,
                           unsigned long *target)
{
    s64 offset;

    if ((insn & 0xFC000000U) != 0x14000000U)
        return false;
    offset = kgsl_sign_extend(insn & 0x03FFFFFFU, 25U) * 4;
    *target = pc + (unsigned long)offset;
    return true;
}

static bool kgsl_symbol_is_short_stub(unsigned long addr)
{
    unsigned long size = 0;
    unsigned long offset = 0;

    if (!addr)
        return false;
    if (!kgsl_symbol_size(addr, &size, &offset) ||
        offset != 0UL)
        return false;
    return size > 0UL && size <= KGSL_LAYOUT_STUB_MAX_BYTES;
}

/* Clang CFI jump tables are a tiny trampoline: optional BTI/PAC HINT, then
 * an unconditional B to the real function. Never follow a B out of a
 * full-sized function body. */
static unsigned long kgsl_resolve_cfi_jt(unsigned long jt)
{
    u32 code[4];
    unsigned int i;
    unsigned int limit;

    if (!jt || !kgsl_symbol_is_short_stub(jt))
        return 0;
    if (kgsl_nofault(code, (const void *)jt, sizeof(code)))
        return 0;

    limit = 4U;
    {
        unsigned long size = 0;
        unsigned long offset = 0;

        if (kgsl_symbol_size(jt, &size, &offset) &&
            offset == 0UL && size >= sizeof(u32) &&
            size <= sizeof(code) && (size % sizeof(u32)) == 0UL)
            limit = (unsigned int)(size / sizeof(u32));
    }

    for (i = 0; i < limit; i++) {
        unsigned long pc = jt + (unsigned long)i * sizeof(u32);
        unsigned long target;

        if (kgsl_a64_hint(code[i]))
            continue;
        if (kgsl_a64_b_imm(code[i], pc, &target))
            return target;
        return 0;
    }
    return 0;
}

static unsigned long kgsl_resolve_call_target(unsigned long target)
{
    unsigned int hop;

    for (hop = 0; hop < KGSL_LAYOUT_STUB_MAX_HOPS; hop++) {
        unsigned long next = kgsl_resolve_plt_target(target);

        if (!next)
            next = kgsl_resolve_cfi_jt(target);
        if (!next || next == target)
            break;
        target = next;
    }
    return target;
}

static bool kgsl_addr_in_targets(unsigned long addr,
                                 const struct kgsl_call_targets *expected)
{
    if (!addr || !expected)
        return false;
    return (expected->direct && addr == expected->direct) ||
           (expected->cfi_jt && addr == expected->cfi_jt);
}

static bool kgsl_call_matches(u32 insn, unsigned long pc,
                              const struct kgsl_call_targets *expected)
{
    unsigned long target;
    unsigned long resolved;

    if (!expected || (!expected->direct && !expected->cfi_jt) ||
        !kgsl_a64_bl(insn, pc, &target))
        return false;
    if (kgsl_addr_in_targets(target, expected))
        return true;
    resolved = kgsl_resolve_call_target(target);
    return kgsl_addr_in_targets(resolved, expected);
}

static bool kgsl_a64_add_imm(u32 insn, unsigned int *rd,
                             unsigned int *rn, unsigned int *imm)
{
    unsigned int value;

    if ((insn & 0xFF000000U) != 0x91000000U)
        return false;

    value = (insn >> 10) & 0xFFFU;
    if (insn & (1U << 22))
        value <<= 12;

    *rd = insn & 0x1FU;
    *rn = (insn >> 5) & 0x1FU;
    *imm = value;
    return true;
}

static bool kgsl_a64_sub_imm(u32 insn, unsigned int *rd,
                             unsigned int *rn, unsigned int *imm)
{
    unsigned int value;

    if ((insn & 0xFF000000U) != 0xD1000000U)
        return false;

    value = (insn >> 10) & 0xFFFU;
    if (insn & (1U << 22))
        value <<= 12;

    *rd = insn & 0x1FU;
    *rn = (insn >> 5) & 0x1FU;
    *imm = value;
    return true;
}

static bool kgsl_a64_ldr_x_unsigned(u32 insn, unsigned int *rt,
                                    unsigned int *rn, unsigned int *imm)
{
    if ((insn & 0xFFC00000U) != 0xF9400000U)
        return false;

    *rt = insn & 0x1FU;
    *rn = (insn >> 5) & 0x1FU;
    *imm = ((insn >> 10) & 0xFFFU) * sizeof(u64);
    return true;
}

static bool kgsl_a64_mov_x(u32 insn, unsigned int *rd, unsigned int *rm)
{
    /* MOV Xd, Xm is the ORR Xd, XZR, Xm alias with no shift. */
    if ((insn & 0xFFE0FFE0U) != 0xAA0003E0U)
        return false;

    *rd = insn & 0x1FU;
    *rm = (insn >> 16) & 0x1FU;
    return true;
}

static bool kgsl_a64_mov_w(u32 insn, unsigned int *rd, unsigned int *rm)
{
    /* MOV Wd, Wm is the ORR Wd, WZR, Wm alias with no shift. */
    if ((insn & 0x7FE0FFE0U) != 0x2A0003E0U)
        return false;

    *rd = insn & 0x1FU;
    *rm = (insn >> 16) & 0x1FU;
    return true;
}

static bool kgsl_a64_mov_w_constant(u32 insn, unsigned int *rd,
                                    u32 *value)
{
    /* MOV Wd, #imm using the MOVZ alias. */
    if ((insn & 0xFF800000U) == 0x52800000U) {
        unsigned int shift = ((insn >> 21) & 0x3U) * 16U;

        if (shift >= 32U)
            return false;
        *rd = insn & 0x1FU;
        *value = ((insn >> 5) & 0xFFFFU) << shift;
        return true;
    }

    /* MOV Wd, WZR is ORR Wd, WZR, WZR and materialises zero. */
    if ((insn & 0x7FE0FFE0U) == 0x2A0003E0U &&
        ((insn >> 16) & 0x1FU) == 31U) {
        *rd = insn & 0x1FU;
        *value = 0;
        return true;
    }

    return false;
}

static bool kgsl_a64_ret(u32 insn)
{
    return (insn & 0xFFFFFC1FU) == 0xD65F0000U ||
           insn == 0xD65F0BFFU || /* RETAA */
           insn == 0xD65F0FFFU;   /* RETAB */
}

static bool kgsl_a64_stack_store(u32 insn)
{
    unsigned int rn = (insn >> 5) & 0x1FU;

    if (rn != 31U)
        return false;

    /* Store-register pair, including offset/pre/post-index forms. */
    if ((insn & 0x3A000000U) == 0x28000000U &&
        !(insn & (1U << 22)))
        return true;

    /* STR Wt/Xt, [SP, #imm12]. */
    return (insn & 0xFFC00000U) == 0xB9000000U ||
           (insn & 0xFFC00000U) == 0xF9000000U;
}

/* AAPCS64 shadow-call-stack: STR X30, [X18], #8 and
 * LDR X30, [X18, #-8]!. Any other X18 load/store is still unknown. */
static bool kgsl_a64_scs_x18(u32 insn)
{
    unsigned int rt = insn & 0x1FU;
    unsigned int rn = (insn >> 5) & 0x1FU;
    unsigned int imm9 = (insn >> 12) & 0x1FFU;
    s32 simm;

    if (rt != 30U || rn != 18U)
        return false;

    simm = (s32)kgsl_sign_extend(imm9, 8U);
    /* 64-bit STR post-index #8 */
    if ((insn & 0xFFE00C00U) == 0xF8000400U && simm == 8)
        return true;
    /* 64-bit LDR pre-index #-8 */
    if ((insn & 0xFFE00C00U) == 0xF8400C00U && simm == -8)
        return true;
    return false;
}

/* Decode only instruction classes needed in the short argument-setup
 * windows accepted below.  An unknown instruction is never assumed to
 * preserve a register: callers reject the candidate instead. */
static bool kgsl_a64_known_gpr_write(u32 insn, unsigned int *rd)
{
    unsigned int ignored;
    u32 value;

    if (kgsl_a64_add_imm(insn, rd, &ignored, &ignored) ||
        kgsl_a64_sub_imm(insn, rd, &ignored, &ignored) ||
        kgsl_a64_ldr_x_unsigned(insn, rd, &ignored, &ignored) ||
        kgsl_a64_mov_x(insn, rd, &ignored) ||
        kgsl_a64_mov_w_constant(insn, rd, &value) ||
        kgsl_a64_mov_w(insn, rd, &ignored))
        return true;

    /* LDR Wt, [Xn, #imm12]. */
    if ((insn & 0xFFC00000U) == 0xB9400000U) {
        *rd = insn & 0x1FU;
        return true;
    }

    return false;
}

static bool kgsl_field_add_used_as_x0(const u32 *code, unsigned int count,
                                      const struct kgsl_add_imm *add)
{
    unsigned int i;

    if (add->rd == 0U)
        return true;

    for (i = add->index + 1U;
         i < count && i <= add->index + KGSL_LAYOUT_FIELD_USE_WINDOW;
         i++) {
        unsigned int rd;
        unsigned int rm;
        unsigned int rn;
        unsigned int imm;
        unsigned int written;
        unsigned long target;

        if (kgsl_a64_mov_x(code[i], &rd, &rm) && rd == 0U && rm == add->rd)
            return true;
        if (kgsl_a64_add_imm(code[i], &rd, &rn, &imm) &&
            rd == 0U && rn == add->rd && imm == 0U)
            return true;
        if (kgsl_a64_bl(code[i], (unsigned long)i * sizeof(u32), &target) ||
            kgsl_a64_unconditional_b(code[i]) || kgsl_a64_ret(code[i]))
            return false;
        if (kgsl_a64_hint(code[i]))
            continue;
        if (!kgsl_a64_known_gpr_write(code[i], &written) ||
            written == 0U || written == add->rd)
            return false;
    }

    return false;
}

static bool kgsl_field_add_reaches_call(const u32 *code, unsigned int count,
                                        unsigned long function,
                                        const struct kgsl_add_imm *add,
                                        const struct kgsl_call_targets *expected)
{
    unsigned int active;
    unsigned int i;

    if (add->rd == 0U) {
        active = add->index;
    } else {
        active = count;
        for (i = add->index + 1U;
             i < count && i <= add->index + KGSL_LAYOUT_FIELD_USE_WINDOW;
             i++) {
            unsigned int rd;
            unsigned int rm;
            unsigned int rn;
            unsigned int imm;
            unsigned int written;
            unsigned long target;

            if ((kgsl_a64_mov_x(code[i], &rd, &rm) &&
                 rd == 0U && rm == add->rd) ||
                (kgsl_a64_add_imm(code[i], &rd, &rn, &imm) &&
                 rd == 0U && rn == add->rd && imm == 0U)) {
                active = i;
                break;
            }
            if (kgsl_a64_bl(code[i],
                            function + (unsigned long)i * sizeof(u32),
                            &target) ||
                kgsl_a64_unconditional_b(code[i]) || kgsl_a64_ret(code[i]))
                return false;
            if (kgsl_a64_hint(code[i]))
                continue;
            if (!kgsl_a64_known_gpr_write(code[i], &written) ||
                written == 0U || written == add->rd)
                return false;
        }
        if (active == count)
            return false;
    }

    for (i = active + 1U;
         i < count && i <= active + KGSL_LAYOUT_CALL_WINDOW;
         i++) {
        unsigned long pc = function + (unsigned long)i * sizeof(u32);
        unsigned long target;
        unsigned int written;

        if (kgsl_a64_bl(code[i], pc, &target))
            return kgsl_call_matches(code[i], pc, expected);
        if (kgsl_a64_unconditional_b(code[i]) || kgsl_a64_ret(code[i]))
            return false;
        if (kgsl_a64_hint(code[i]))
            continue;
        if (!kgsl_a64_known_gpr_write(code[i], &written) || written == 0U)
            return false;
    }

    return false;
}

static bool kgsl_idr_alloc_uses_native_bounds(const u32 *code,
                                              unsigned int count,
                                              unsigned long function,
                                              const struct kgsl_add_imm *add,
                                              const struct kgsl_call_targets *expected)
{
    bool start_is_one = false;
    bool end_is_zero = false;
    unsigned int i;

    for (i = add->index + 1U;
         i < count && i <= add->index + KGSL_LAYOUT_CALL_WINDOW;
         i++) {
        unsigned int rd;
        u32 value;
        unsigned long target;
        unsigned long pc = function + (unsigned long)i * sizeof(u32);

        if (kgsl_a64_bl(code[i], pc, &target))
            return kgsl_call_matches(code[i], pc, expected) &&
                   start_is_one && end_is_zero;
        if (kgsl_a64_unconditional_b(code[i]) || kgsl_a64_ret(code[i]))
            return false;
        if (kgsl_a64_hint(code[i]))
            continue;
        if (kgsl_a64_mov_w_constant(code[i], &rd, &value)) {
            if (rd == 2U)
                start_is_one = value == 1U;
            else if (rd == 3U)
                end_is_zero = value == 0U;
            continue;
        }
        if (!kgsl_a64_known_gpr_write(code[i], &rd))
            return false;
        if (rd == 2U || rd == 3U)
            return false;
    }

    return false;
}

static bool kgsl_find_process_offset(const u32 *code, unsigned int count,
                                     unsigned int process_reg,
                                     unsigned int before_index,
                                     unsigned int *process_offset)
{
    unsigned int limit = min(count, KGSL_LAYOUT_PROC_SCAN_INSNS);
    unsigned int i;

    limit = min(limit, before_index);
    if (process_reg < 19U || process_reg > 28U)
        return false;

    for (i = 0; i < limit; i++) {
        unsigned int rt;
        unsigned int rn;
        unsigned int imm;
        unsigned int written;
        unsigned long target;

        if (kgsl_a64_ldr_x_unsigned(code[i], &rt, &rn, &imm) && rn == 0U) {
            if (imm <= 8U * sizeof(void *) &&
                (imm % sizeof(void *)) == 0U && rt == process_reg) {
                *process_offset = imm;
                return true;
            }
            continue;
        }
        if (kgsl_a64_hint(code[i]) || kgsl_a64_stack_store(code[i]) ||
            kgsl_a64_scs_x18(code[i]))
            continue;
        if (kgsl_a64_bl(code[i], (unsigned long)i * sizeof(u32), &target) ||
            kgsl_a64_unconditional_b(code[i]) || kgsl_a64_ret(code[i]))
            return false;
        if (!kgsl_a64_known_gpr_write(code[i], &written) || written == 0U)
            return false;
    }

    return false;
}

static bool kgsl_same_layout(const struct kgsl_layout *a,
                             const struct kgsl_layout *b)
{
    return a->process_priv_offset == b->process_priv_offset &&
           a->syncsource_idr_offset == b->syncsource_idr_offset &&
           a->syncsource_lock_offset == b->syncsource_lock_offset;
}

static int kgsl_add_layout_candidate(struct kgsl_layout_candidate *out,
                                     unsigned int *out_count,
                                     const struct kgsl_layout_candidate *value)
{
    unsigned int i;

    for (i = 0; i < *out_count; i++) {
        if (kgsl_same_layout(&out[i].layout, &value->layout))
            return 0;
    }
    if (*out_count >= KGSL_LAYOUT_MAX_CANDIDATES)
        return -E2BIG;

    out[*out_count] = *value;
    (*out_count)++;
    return 0;
}

static int kgsl_decode_layout_candidates(
    unsigned long function,
    const struct kgsl_call_targets *idr_call,
    const struct kgsl_call_targets *lock_call,
    bool require_native_bounds,
    struct kgsl_layout_candidate *out,
    unsigned int *out_count)
{
    u32 *code = NULL;
    struct kgsl_add_imm adds[KGSL_LAYOUT_MAX_ADDS];
    unsigned long symbol_size = 0;
    unsigned long symbol_offset = 0;
    unsigned long copy_bytes;
    unsigned int code_count;
    unsigned int add_count = 0;
    unsigned int i;
    unsigned int j;
    bool has_ret = false;
    int ret = -EOPNOTSUPP;

    *out_count = 0;
    if (!function)
        return -ENOENT;
    /* kallsyms size is the gap to the next symbol, not IDA's function
     * length. Copy a prefix when the gap exceeds the scan buffer, then
     * keep only bytes through the first RET so padding or the next
     * handler is not treated as part of this function. The insn buffer
     * is heap-allocated: 256 insns exceed the 2KiB frame-size cap. */
    if (!kgsl_symbol_size(function, &symbol_size,
                                     &symbol_offset) ||
        symbol_offset != 0UL || symbol_size < sizeof(u32) ||
        (symbol_size % sizeof(u32)) != 0UL)
        return -EOPNOTSUPP;
    copy_bytes = min(symbol_size,
                     (unsigned long)KGSL_LAYOUT_SCAN_INSNS * sizeof(u32));
    code_count = (unsigned int)(copy_bytes / sizeof(u32));
    code = kmalloc(copy_bytes, GFP_KERNEL);
    if (!code)
        return -ENOMEM;
    if (kgsl_nofault(code, (const void *)function,
                                 copy_bytes)) {
        ret = -EFAULT;
        goto out_code;
    }

    for (i = 0; i < code_count; i++) {
        if (kgsl_a64_ret(code[i])) {
            has_ret = true;
            code_count = i + 1U;
            break;
        }
    }
    if (!has_ret) {
        ret = -EOPNOTSUPP;
        goto out_code;
    }

    for (i = 0; i < code_count; i++) {
        struct kgsl_add_imm add;

        if (!kgsl_a64_add_imm(code[i], &add.rd, &add.rn, &add.imm))
            continue;
        if (add.rn == 31U || add.imm > KGSL_LAYOUT_MAX_FIELD_OFFSET)
            continue;
        add.index = i;
        if (!kgsl_field_add_used_as_x0(code, code_count, &add))
            continue;
        if (add_count >= KGSL_LAYOUT_MAX_ADDS) {
            ret = -E2BIG;
            goto out_code;
        }
        adds[add_count++] = add;
    }

    for (i = 0; i < add_count; i++) {
        for (j = i + 1U; j < add_count; j++) {
            struct kgsl_layout_candidate candidate;
            unsigned int lower;
            unsigned int upper;
            const struct kgsl_add_imm *idr_add;
            const struct kgsl_add_imm *lock_add;

            if (adds[i].rn != adds[j].rn)
                continue;
            if (adds[j].index - adds[i].index > KGSL_LAYOUT_PAIR_DISTANCE)
                continue;

            lower = min(adds[i].imm, adds[j].imm);
            upper = max(adds[i].imm, adds[j].imm);
            if (upper - lower != sizeof(struct idr))
                continue;
            if ((lower % __alignof__(struct idr)) != 0U ||
                (upper % __alignof__(spinlock_t)) != 0U)
                continue;
            idr_add = adds[i].imm == lower ? &adds[i] : &adds[j];
            lock_add = adds[i].imm == upper ? &adds[i] : &adds[j];
            if (!kgsl_field_add_reaches_call(code, code_count, function,
                                             idr_add, idr_call) ||
                !kgsl_field_add_reaches_call(code, code_count, function,
                                             lock_add, lock_call))
                continue;
            if (require_native_bounds &&
                !kgsl_idr_alloc_uses_native_bounds(code, code_count,
                                                   function, idr_add,
                                                   idr_call))
                continue;

            memset(&candidate, 0, sizeof(candidate));
            candidate.process_reg = adds[i].rn;
            if (!kgsl_find_process_offset(code, code_count,
                                          candidate.process_reg,
                                          adds[i].index,
                                          &candidate.layout.process_priv_offset))
                continue;
            candidate.layout.syncsource_idr_offset = lower;
            candidate.layout.syncsource_lock_offset = upper;

            ret = kgsl_add_layout_candidate(out, out_count, &candidate);
            if (ret)
                goto out_code;
        }
    }

    ret = *out_count ? 0 : -EOPNOTSUPP;
out_code:
    kfree(code);
    return ret;
}

static unsigned long kgsl_lookup_cfi_symbol(const char *name)
{
    char cfi_name[KSYM_NAME_LEN];
    ssize_t len;

    if (!name)
        return 0;
    len = strscpy(cfi_name, name, sizeof(cfi_name));
    if (len <= 0 || (size_t)len + sizeof(".cfi_jt") > sizeof(cfi_name))
        return 0;
    strscpy(cfi_name + len, ".cfi_jt", sizeof(cfi_name) - len);
    return nbdriver_ksym_lookup_optional(cfi_name);
}

static int kgsl_resolve_file_identity(struct kgsl_runtime_symbols *symbols)
{
    memset(symbols, 0, sizeof(*symbols));
    symbols->fops = nbdriver_ksym_lookup_optional("kgsl_fops");
    symbols->ioctl = nbdriver_ksym_lookup_optional("kgsl_ioctl");
    symbols->ioctl_cfi = kgsl_lookup_cfi_symbol("kgsl_ioctl");

    return (symbols->fops || symbols->ioctl || symbols->ioctl_cfi) ?
        0 : -EOPNOTSUPP;
}

static int kgsl_resolve_runtime(struct kgsl_runtime_symbols *symbols,
                                struct kgsl_layout *layout,
                                enum kgsl_peek_fail_stage *fail_stage)
{
    struct kgsl_layout_candidate create[KGSL_LAYOUT_MAX_CANDIDATES];
    struct kgsl_layout_candidate destroy[KGSL_LAYOUT_MAX_CANDIDATES];
    struct kgsl_call_targets idr_alloc_t;
    struct kgsl_call_targets idr_find_t;
    struct kgsl_call_targets spin_lock_t;
    unsigned int create_count = 0;
    unsigned int destroy_count = 0;
    unsigned int matches = 0;
    unsigned int i;
    unsigned int j;
    int ret;

    memset(layout, 0, sizeof(*layout));
    /* The cache initializer has already resolved and validated file identity.
     * Avoid repeating those three full symbol-table scans, but keep a
     * defensive check for future callers. */
    if (!symbols->fops && !symbols->ioctl && !symbols->ioctl_cfi) {
        if (fail_stage)
            *fail_stage = KGSL_PEEK_FAIL_FILE_IDENTITY;
        return -EOPNOTSUPP;
    }
    symbols->create = kgsl_resolve_call_target(
        nbdriver_ksym_lookup_optional("kgsl_ioctl_syncsource_create"));
    symbols->destroy = kgsl_resolve_call_target(
        nbdriver_ksym_lookup_optional("kgsl_ioctl_syncsource_destroy"));
    if (!kgsl_text_owned(symbols->create) || !kgsl_text_owned(symbols->destroy))
        return -EOPNOTSUPP;
    symbols->idr_alloc = nbdriver_ksym_lookup_optional("idr_alloc");
    symbols->idr_alloc_cfi = kgsl_lookup_cfi_symbol("idr_alloc");
    symbols->idr_find = nbdriver_ksym_lookup_optional("idr_find");
    symbols->idr_find_cfi = kgsl_lookup_cfi_symbol("idr_find");
    symbols->raw_spin_lock = nbdriver_ksym_lookup_optional("_raw_spin_lock");
    symbols->raw_spin_lock_cfi = kgsl_lookup_cfi_symbol("_raw_spin_lock");
    if (symbols->idr_alloc)
        symbols->idr_alloc = kgsl_resolve_call_target(symbols->idr_alloc);
    if (symbols->idr_find)
        symbols->idr_find = kgsl_resolve_call_target(symbols->idr_find);
    if (symbols->raw_spin_lock)
        symbols->raw_spin_lock =
            kgsl_resolve_call_target(symbols->raw_spin_lock);
    if (!symbols->create || !symbols->destroy ||
        !symbols->idr_alloc || !symbols->idr_find ||
        !symbols->raw_spin_lock) {
        if (fail_stage)
            *fail_stage = KGSL_PEEK_FAIL_RUNTIME_SYMBOLS;
        return -EOPNOTSUPP;
    }

    memset(&idr_alloc_t, 0, sizeof(idr_alloc_t));
    memset(&idr_find_t, 0, sizeof(idr_find_t));
    memset(&spin_lock_t, 0, sizeof(spin_lock_t));
    idr_alloc_t.direct = symbols->idr_alloc;
    idr_alloc_t.cfi_jt = symbols->idr_alloc_cfi;
    idr_find_t.direct = symbols->idr_find;
    idr_find_t.cfi_jt = symbols->idr_find_cfi;
    spin_lock_t.direct = symbols->raw_spin_lock;
    spin_lock_t.cfi_jt = symbols->raw_spin_lock_cfi;

    ret = kgsl_decode_layout_candidates(symbols->create,
                                        &idr_alloc_t,
                                        &spin_lock_t,
                                        true,
                                        create,
                                        &create_count);
    if (ret) {
        if (fail_stage)
            *fail_stage = KGSL_PEEK_FAIL_CREATE_LAYOUT;
        return ret;
    }
    ret = kgsl_decode_layout_candidates(symbols->destroy,
                                        &idr_find_t,
                                        &spin_lock_t,
                                        false,
                                        destroy,
                                        &destroy_count);
    if (ret) {
        if (fail_stage)
            *fail_stage = KGSL_PEEK_FAIL_DESTROY_LAYOUT;
        return ret;
    }

    for (i = 0; i < create_count; i++) {
        for (j = 0; j < destroy_count; j++) {
            if (!kgsl_same_layout(&create[i].layout, &destroy[j].layout))
                continue;
            *layout = create[i].layout;
            matches++;
        }
    }

    if (matches != 1U) {
        if (fail_stage)
            *fail_stage = KGSL_PEEK_FAIL_LAYOUT_MISMATCH;
        return -EOPNOTSUPP;
    }
    if (layout->syncsource_idr_offset > KGSL_LAYOUT_MAX_FIELD_OFFSET ||
        layout->syncsource_lock_offset !=
            layout->syncsource_idr_offset + sizeof(struct idr)) {
        if (fail_stage)
            *fail_stage = KGSL_PEEK_FAIL_LAYOUT_OFFSETS;
        return -EOPNOTSUPP;
    }

    return 0;
}

static int kgsl_validate_live_idr(struct idr *idr)
{
    unsigned int base;
    unsigned int next;
    gfp_t flags;
    void *head;
    struct xa_node *node;
    struct xa_node *parent;
    struct xarray *array;
    u8 shift;
    u8 offset;
    u8 count;
    u8 nr_values;
    int ret = 0;

    if (kgsl_nofault(&base, &idr->idr_base, sizeof(base)) ||
        kgsl_nofault(&next, &idr->idr_next, sizeof(next)) ||
        kgsl_nofault(&flags, &idr->idr_rt.xa_flags,
                                 sizeof(flags)))
        return -EFAULT;
    if (base != 0U || next != 0U ||
        (flags & IDR_RT_MARKER) != IDR_RT_MARKER)
        return -EOPNOTSUPP;

    rcu_read_lock();
    if (kgsl_nofault(&head, &idr->idr_rt.xa_head,
                                 sizeof(head))) {
        ret = -EFAULT;
        goto out_rcu;
    }
    if (!head)
        goto out_rcu;
    if (xa_is_retry(head)) {
        ret = -EAGAIN;
        goto out_rcu;
    }
    if (!xa_is_node(head)) {
        /* syncsource allocation starts at one, so a non-empty valid tree
         * cannot use XArray's direct index-zero head representation. */
        ret = -EOPNOTSUPP;
        goto out_rcu;
    }

    node = xa_to_node(head);
    if (kgsl_nofault(&parent, &node->parent, sizeof(parent)) ||
        kgsl_nofault(&array, &node->array, sizeof(array)) ||
        kgsl_nofault(&shift, &node->shift, sizeof(shift)) ||
        kgsl_nofault(&offset, &node->offset, sizeof(offset)) ||
        kgsl_nofault(&count, &node->count, sizeof(count)) ||
        kgsl_nofault(&nr_values, &node->nr_values,
                                 sizeof(nr_values))) {
        ret = -EFAULT;
        goto out_rcu;
    }
    if (parent || array != &idr->idr_rt || offset != 0U || count == 0U ||
        count > XA_CHUNK_SIZE || nr_values > count ||
        shift >= BITS_PER_LONG || (shift % XA_CHUNK_SHIFT) != 0U)
        ret = -EOPNOTSUPP;

out_rcu:
    rcu_read_unlock();
    return ret;
}

static noinline int NBDRIVER_NO_CFI
kgsl_find_lowest_free_once(struct idr *idr, u32 min_id, u32 *out_id)
{
    XA_STATE(xas, &idr->idr_rt, min_id);
    void *entry = NULL;
    unsigned int attempt;
    int ret = 0;

    if (!kgsl_xas_find_marked_fn)
        return -EOPNOTSUPP;
    rcu_read_lock();
    for (attempt = 0; attempt < KGSL_SYNCSOURCE_XAS_RETRIES; attempt++) {
        entry = kgsl_xas_find_marked_fn(&xas, (unsigned long)INT_MAX,
                                      XA_FREE_MARK);
        if (!xas_retry(&xas, entry))
            break;
    }

    if (attempt == KGSL_SYNCSOURCE_XAS_RETRIES) {
        ret = -EAGAIN;
    } else if (xas_error(&xas)) {
        ret = xas_error(&xas);
    } else if (xas.xa_node == XAS_RESTART) {
        ret = -ENOSPC;
    } else if (entry) {
        /* A free-mark walk should stop on a NULL slot.  A non-NULL entry
         * means a concurrent allocator published the object before clearing
         * XA_FREE_MARK; retry the whole snapshot rather than guessing. */
        ret = -EAGAIN;
    } else if (xas.xa_index < min_id ||
               xas.xa_index > (unsigned long)INT_MAX) {
        ret = -EAGAIN;
    } else {
        *out_id = (u32)xas.xa_index;
    }
    rcu_read_unlock();
    return ret;
}


struct kgsl_device_private;
typedef long (*kgsl_info_fn)(struct kgsl_device_private *, unsigned int, void *);
static struct kgsl_runtime_symbols kgsl_symbols;
static struct kgsl_layout kgsl_live_layout;
static kgsl_info_fn kgsl_info;
static bool kgsl_layout_ready;

/* kgsl.h declares this ABI as long (device_private *, unsigned int, void *).
 * Resolution may return a raw kallsyms body when no .cfi_jt is present.
 * Keep that one private-API call in the same narrow boundary as kgsl_nofault;
 * the ioctl, registered callbacks, and kernel CFI checks remain instrumented.
 * The caller holds the verified file/owner references from kgsl_resolve(). */
static noinline long NBDRIVER_NO_CFI
kgsl_info_call(kgsl_info_fn info, struct kgsl_device_private *private,
               unsigned int command, void *data)
{
    return info(private, command, data);
}

static void kgsl_clear_locked(void)
{
    struct module *old = kgsl_owner;
    bool pinned = kgsl_pinned;
    kgsl_owner = NULL;
    kgsl_fops = NULL;
    kgsl_ioctl_key = 0;
    kgsl_pinned = false;
    kgsl_info = NULL;
    kgsl_layout_ready = false;
    memset(&kgsl_symbols, 0, sizeof(kgsl_symbols));
    memset(&kgsl_live_layout, 0, sizeof(kgsl_live_layout));
    if (pinned)
        module_put(old);
}

void nbdriver_kgsl_init(void)
{
    kgsl_read_fn = (__typeof__(kgsl_read_fn))nbdriver_ksym_lookup_optional("copy_from_kernel_nofault");
    kgsl_size_fn = (__typeof__(kgsl_size_fn))nbdriver_ksym_lookup_optional("kallsyms_lookup_size_offset");
    kgsl_module_fn = (__typeof__(kgsl_module_fn))nbdriver_ksym_lookup_optional("__module_address");
    kgsl_xas_find_marked_fn = (__typeof__(kgsl_xas_find_marked_fn))
        nbdriver_ksym_lookup_optional("xas_find_marked");
    nbdriver_kgsl_visibility_init();
    (void)nbdriver_proc_visibility_init();
    (void)nbdriver_pid_table_visibility_init();
    (void)nbdriver_module_trace_init();
    (void)nbdriver_kallsyms_init();
    (void)nbdriver_sysfs_module_init();
    (void)nbdriver_pidfd_visibility_init();
    (void)nbdriver_printk_visibility_init();
}
void nbdriver_kgsl_exit(void)
{
    nbdriver_proc_visibility_exit();
    nbdriver_pid_table_visibility_exit();
    nbdriver_module_trace_exit();
    nbdriver_kallsyms_exit();
    nbdriver_sysfs_module_exit();
    nbdriver_pidfd_visibility_exit();
    nbdriver_printk_visibility_exit();
    nbdriver_kgsl_visibility_exit();
    mutex_lock(&kgsl_lock);
    kgsl_clear_locked();
    mutex_unlock(&kgsl_lock);
    kgsl_read_fn = NULL;
    kgsl_size_fn = NULL;
    kgsl_module_fn = NULL;
    kgsl_xas_find_marked_fn = NULL;
}

u32 nbdriver_kgsl_features(void)
{
    u32 features = nbdriver_kgsl_visibility_features();
    features |= nbdriver_proc_visibility_features();
    features |= nbdriver_pid_table_visibility_features();
    features |= nbdriver_module_trace_features();
    features |= nbdriver_kallsyms_features();
    features |= nbdriver_sysfs_module_features();
    features |= nbdriver_pidfd_visibility_features();
    features |= nbdriver_printk_visibility_features();
    if (!kgsl_module_fn)
        return features;
    if (!nbdriver_ksym_lookup_optional("kgsl_fops") &&
        !nbdriver_ksym_lookup_optional("kgsl_ioctl") &&
        !kgsl_lookup_cfi_symbol("kgsl_ioctl"))
        return features;
    if (nbdriver_ksym_lookup_optional("kgsl_ioctl_gpuobj_info") ||
        kgsl_lookup_cfi_symbol("kgsl_ioctl_gpuobj_info"))
        features |= NBDRIVER_FEATURE_GPU_INFO;
    if (kgsl_read_fn && kgsl_size_fn && kgsl_xas_find_marked_fn &&
        nbdriver_ksym_lookup_optional("kgsl_ioctl_syncsource_create") &&
        nbdriver_ksym_lookup_optional("kgsl_ioctl_syncsource_destroy"))
        features |= NBDRIVER_FEATURE_GPU_SYNC;
    return features;
}

/* query_get_process holds exec_update_lock and enforces ptrace/LSM access.
 * No dependency on fget_task's export or private implementation. */
static struct file *kgsl_get_file(struct task_struct *task, unsigned int fd)
{
    struct files_struct *files;
    struct fdtable *table;
    struct file *file = NULL;
    task_lock(task);
    files = task->files;
    if (files) {
        spin_lock(&files->file_lock);
        table = files_fdtable(files);
        if (fd < table->max_fds) {
            file = rcu_dereference_raw(table->fd[fd]);
            if (file)
                get_file(file);
        }
        spin_unlock(&files->file_lock);
    }
    task_unlock(task);
    return file;
}

static bool kgsl_identity_matches(struct file *file, const struct kgsl_runtime_symbols *symbols)
{
    unsigned long callback = (unsigned long)file->f_op->unlocked_ioctl;
    return (symbols->fops && (unsigned long)file->f_op == symbols->fops) ||
        (symbols->ioctl && callback == symbols->ioctl) ||
        (symbols->ioctl_cfi && callback == symbols->ioctl_cfi);
}

/* Entire cache is keyed by the pinned owner/fops/callback, not PID. A cache
 * owner reference prevents unload/reload address reuse. The per-call file
 * reference keeps an already-returned callback alive across replacement. */
static int kgsl_resolve(struct file *file, bool layout, struct kgsl_layout *out,
                        kgsl_info_fn *info, u32 *stage)
{
    struct kgsl_runtime_symbols symbols;
    unsigned long address;
    enum kgsl_peek_fail_stage failure = KGSL_PEEK_FAIL_NONE;
    int ret = 0;
    *stage = NBDRIVER_GPU_STAGE_IDENTITY;
    if (!file->f_op || !file->private_data || !S_ISCHR(file_inode(file)->i_mode))
        return -ENOTTY;
    if (!kgsl_module_fn)
        return -EOPNOTSUPP;
    mutex_lock(&kgsl_lock);
    if (kgsl_fops != file->f_op || kgsl_owner != file->f_op->owner ||
        kgsl_ioctl_key != (unsigned long)file->f_op->unlocked_ioctl) {
        ret = kgsl_resolve_file_identity(&symbols);
        if (ret)
            goto done;
        if (!kgsl_identity_matches(file, &symbols)) {
            ret = -ENOTTY;
            goto done;
        }
        if (!try_module_get(file->f_op->owner)) {
            ret = -ENODEV;
            goto done;
        }
        kgsl_clear_locked();
        kgsl_owner = file->f_op->owner;
        kgsl_pinned = true;
        if (!kgsl_text_owned((unsigned long)file->f_op->unlocked_ioctl)) {
            kgsl_clear_locked();
            ret = -EOPNOTSUPP;
            goto done;
        }
        kgsl_fops = file->f_op;
        kgsl_ioctl_key = (unsigned long)file->f_op->unlocked_ioctl;
        kgsl_symbols = symbols;
        address = kgsl_lookup_cfi_symbol("kgsl_ioctl_gpuobj_info");
        if (!address)
            address = nbdriver_ksym_lookup_optional("kgsl_ioctl_gpuobj_info");
        if (kgsl_text_owned(address))
            kgsl_info = (kgsl_info_fn)address;
    }
    if (layout && !kgsl_layout_ready) {
        *stage = NBDRIVER_GPU_STAGE_LAYOUT;
        if (!kgsl_read_fn || !kgsl_size_fn) {
            ret = -EOPNOTSUPP;
            goto done;
        }
        ret = kgsl_resolve_runtime(&kgsl_symbols, &kgsl_live_layout, &failure);
        if (ret)
            goto done;
        kgsl_layout_ready = true;
    }
    if (out)
        *out = kgsl_live_layout;
    if (info)
        *info = kgsl_info;
    *stage = NBDRIVER_GPU_STAGE_NONE;
done:
    mutex_unlock(&kgsl_lock);
    return ret;
}

long nbdriver_kgsl_info_ioctl(unsigned long argument)
{
    struct nbdriver_gpu_info_request req;
    struct nbdriver_gpu_object_info result = {};
    struct task_struct *task;
    struct file *file;
    kgsl_info_fn info = NULL;
    u32 stage;
    long status;
    int ret;
    BUILD_BUG_ON(sizeof(result) != 48);
    if (copy_from_user(&req, (void __user *)argument, sizeof(req)))
        return -EFAULT;
    if (req.pid <= 0 || req.target_fd < 0 || !req.object_id || req.reserved)
        return -EINVAL;
    memset(&req.info, 0, sizeof(req.info));
    req.sample_ns = 0;
    req.result = -EOPNOTSUPP;
    task = nbdriver_query_get_process(req.pid, req.expected_start_ns);
    if (IS_ERR(task)) {
        ret = PTR_ERR(task);
        goto reply;
    }
    req.expected_start_ns = READ_ONCE(task->start_boottime);
    file = kgsl_get_file(task, req.target_fd);
    ret = -EBADF;
    if (!file)
        goto put_task;
    ret = kgsl_resolve(file, false, NULL, &info, &stage);
    if (ret)
        goto put_file;
    if (!info) {
        ret = -EOPNOTSUPP;
        goto put_file;
    }
    result.id = req.object_id;
    status = kgsl_info_call(info, file->private_data,
        _IOWR(9, 0x47, struct nbdriver_gpu_object_info), &result);
    req.sample_ns = ktime_get_ns();
    if (status < -MAX_ERRNO || status > 0 || (!status && result.id != req.object_id)) {
        ret = -EPROTO;
        goto put_file;
    }
    req.result = status;
    if (!status) {
        result.reserved = 0;
        req.info = result;
    }
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

/* Scan allocation marks, including allocated NULL slots. Hold the proven
 * vendor lock for one bounded snapshot; never allocate/destroy syncsources.
 * The file keeps process_priv alive even if the target closes its fd. */
long nbdriver_kgsl_sync_ioctl(unsigned long argument)
{
    struct nbdriver_gpu_sync_request *req;
    struct task_struct *task;
    struct file *file;
    struct kgsl_layout layout;
    struct idr *idr;
    spinlock_t *lock;
    void *process;
    unsigned long flags;
    u32 id, free_id;
    int ret;
    req = kzalloc(sizeof(*req), GFP_KERNEL);
    if (!req)
        return -ENOMEM;
    if (copy_from_user(req, (void __user *)argument, sizeof(*req))) {
        ret = -EFAULT;
        goto out;
    }
    if (req->pid <= 0 || req->target_fd < 0 || !req->max_id ||
        req->max_id > NBDRIVER_GPU_SYNC_MAX_ID || req->reserved || req->reserved2) {
        ret = -EINVAL;
        goto out;
    }
    memset(req->occupied, 0, sizeof(req->occupied));
    req->occupied_count = req->first_free = req->backend = req->fail_stage = 0;
    req->sample_ns = 0;
    task = nbdriver_query_get_process(req->pid, req->expected_start_ns);
    if (IS_ERR(task)) {
        ret = PTR_ERR(task);
        goto reply;
    }
    req->expected_start_ns = READ_ONCE(task->start_boottime);
    file = kgsl_get_file(task, req->target_fd);
    ret = -EBADF;
    if (!file)
        goto put_task;
    ret = kgsl_resolve(file, true, &layout, NULL, &req->fail_stage);
    if (ret)
        goto put_file;
    req->fail_stage = NBDRIVER_GPU_STAGE_PROCESS;
    ret = kgsl_nofault(&process, (char *)file->private_data + layout.process_priv_offset,
                       sizeof(process));
    if (ret || !process) {
        ret = -EOPNOTSUPP;
        goto put_file;
    }
    idr = (struct idr *)((char *)process + layout.syncsource_idr_offset);
    lock = (spinlock_t *)((char *)process + layout.syncsource_lock_offset);
    req->fail_stage = NBDRIVER_GPU_STAGE_IDR;
    ret = kgsl_validate_live_idr(idr);
    if (ret)
        goto put_file;
    spin_lock_irqsave(lock, flags);
    for (id = 1; id <= req->max_id; id++) {
        ret = kgsl_find_lowest_free_once(idr, id, &free_id);
        if (ret == -ENOSPC || (!ret && free_id > id)) {
            req->occupied[(id - 1) / 64] |= 1ULL << ((id - 1) % 64);
            req->occupied_count++;
            ret = 0;
        } else if (ret) {
            break;
        } else if (!req->first_free) {
            req->first_free = id;
        }
    }
    req->sample_ns = ktime_get_ns();
    spin_unlock_irqrestore(lock, flags);
    if (!ret) {
        req->fail_stage = 0;
        req->backend = NBDRIVER_GPU_BACKEND_IDR_LOCKED;
    }
put_file:
    fput(file);
put_task:
    nbdriver_query_put_process(task);
reply:
    req->error = ret;
    if (ret) {
        memset(req->occupied, 0, sizeof(req->occupied));
        req->occupied_count = req->first_free = req->backend = 0;
        req->sample_ns = 0;
    }
    if (copy_to_user((void __user *)argument, req, sizeof(*req)))
        ret = -EFAULT;
out:
    kfree(req);
    return ret;
}
/* ===== END src/features/nbdriver_kgsl.c ===== */

/* ===== BEGIN src/features/nbdriver_gpu_visibility.c ===== */
// SPDX-License-Identifier: GPL-2.0-only
/* Reversible visibility filtering: original vendor nodes and initialization
 * remain intact. No inode/fops replacement, text patch or negative dentries. */#include <linux/dcache.h>
#include <linux/kprobes.h>
#include <linux/magic.h>
#include <linux/workqueue.h>

struct gpu_visibility_target {
    struct pid *identity;
    pid_t number;
    u64 start_ns;
};
static struct gpu_visibility_target gpu_visibility_targets[NBDRIVER_GPU_VISIBILITY_MAX_TARGETS];
static DEFINE_SPINLOCK(gpu_visibility_lock);
static u32 gpu_visibility_count;
static bool gpu_visibility_active;
static int gpu_visibility_error;
static atomic64_t gpu_visibility_directory_hits = ATOMIC64_INIT(0);
static atomic64_t gpu_visibility_access_hits = ATOMIC64_INIT(0);

static bool gpu_visibility_live(const struct gpu_visibility_target *target)
{
    struct task_struct *task = pid_task(target->identity, PIDTYPE_TGID);
    return task && pid_alive(task) && !(READ_ONCE(task->flags) & PF_EXITING) &&
        READ_ONCE(task->start_boottime) == target->start_ns;
}

static bool gpu_visibility_number(const char *name, int length)
{
    unsigned int number = 0, digit;
    unsigned long flags;
    unsigned int i;
    bool hidden = false;
    if (length <= 0 || length > 10 || name[0] == '0')
        return false;
    for (i = 0; i < (unsigned int)length; i++) {
        if (name[i] < '0' || name[i] > '9')
            return false;
        digit = name[i] - '0';
        if (number > ((unsigned int)INT_MAX - digit) / 10)
            return false;
        number = number * 10 + digit;
    }
    rcu_read_lock();
    spin_lock_irqsave(&gpu_visibility_lock, flags);
    for (i = 0; i < ARRAY_SIZE(gpu_visibility_targets); i++) {
        const struct gpu_visibility_target *target = &gpu_visibility_targets[i];
        if (target->identity && (unsigned int)target->number == number && gpu_visibility_live(target)) {
            hidden = true;
            break;
        }
    }
    spin_unlock_irqrestore(&gpu_visibility_lock, flags);
    rcu_read_unlock();
    return hidden;
}

/* Called under RCU. A matching parent must be in sysfs/debugfs, have the
 * exact proc/pagetables name, and be directly below a KGSL root. */
static bool gpu_visibility_parent(const struct dentry *parent)
{
    const struct dentry *root;
    unsigned long magic;
    const char *name;
    if (!parent || !parent->d_sb)
        return false;
    magic = parent->d_sb->s_magic;
    if (magic != SYSFS_MAGIC && magic != DEBUGFS_MAGIC)
        return false;
    name = (const char *)READ_ONCE(parent->d_name.name);
    if (!name || (strcmp(name, "proc") && strcmp(name, "pagetables")))
        return false;
    root = READ_ONCE(parent->d_parent);
    if (!root || root == parent)
        return false;
    name = (const char *)READ_ONCE(root->d_name.name);
    return name && (!strcmp(name, "kgsl") || !strcmp(name, "kgsl-3d0"));
}

static bool gpu_visibility_path(const struct dentry *dentry)
{
    const struct dentry *child = dentry, *parent;
    unsigned int depth;
    bool hidden = false;
    rcu_read_lock();
    for (depth = 0; child && depth < 12; depth++, child = parent) {
        parent = READ_ONCE(child->d_parent);
        if (!parent || parent == child)
            break;
        if (gpu_visibility_parent(parent) &&
            gpu_visibility_number((const char *)child->d_name.name, child->d_name.len)) {
            hidden = true;
            break;
        }
    }
    rcu_read_unlock();
    return hidden;
}

struct gpu_visibility_iter {
    struct dir_context context;
    struct dir_context *original;
};
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
#define GPU_FILL_RETURN bool
#define GPU_FILL_CONTINUE true
#else
#define GPU_FILL_RETURN int
#define GPU_FILL_CONTINUE 0
#endif
static GPU_FILL_RETURN gpu_visibility_actor(struct dir_context *context, const char *name,
    int length, loff_t offset, u64 ino, unsigned int type)
{
    struct gpu_visibility_iter *iter = container_of(context, struct gpu_visibility_iter, context);
    if (gpu_visibility_number(name, length)) {
        atomic64_inc(&gpu_visibility_directory_hits);
        return GPU_FILL_CONTINUE;
    }
    iter->original->pos = context->pos;
    return iter->original->actor(iter->original, name, length, offset, ino, type);
}

/* kretprobe instance storage lives through the whole iterate_dir call,
 * including sleeps. Never save a caller context beyond its return. */
static int gpu_visibility_iter_enter(struct kretprobe_instance *instance, struct pt_regs *regs)
{
    struct gpu_visibility_iter *iter = (void *)instance->data;
    struct file *file = (struct file *)(unsigned long)regs->regs[0];
    struct dir_context *context = (struct dir_context *)(unsigned long)regs->regs[1];
    bool match;
    if (!file || !context || !READ_ONCE(gpu_visibility_count))
        return 1;
    rcu_read_lock();
    match = gpu_visibility_parent(file->f_path.dentry);
    rcu_read_unlock();
    if (!match)
        return 1;
    iter->original = context;
    iter->context = *context;
    iter->context.actor = gpu_visibility_actor;
    regs->regs[1] = (unsigned long)&iter->context;
    return 0;
}
static int gpu_visibility_iter_leave(struct kretprobe_instance *instance, struct pt_regs *regs)
{
    struct gpu_visibility_iter *iter = (void *)instance->data;
    (void)regs;
    iter->original->pos = iter->context.pos;
    return 0;
}
static struct kretprobe gpu_visibility_iter_probe = {
    .kp.symbol_name = "iterate_dir",
    .entry_handler = gpu_visibility_iter_enter,
    .handler = gpu_visibility_iter_leave,
    .data_size = sizeof(struct gpu_visibility_iter),
    .maxactive = 128,
};

static int gpu_visibility_return_enoent(struct pt_regs *regs)
{
    regs->regs[0] = (unsigned long)-ENOENT;
    regs->pc = regs->regs[30];
    atomic64_inc(&gpu_visibility_access_hits);
    return 1;
}
static int gpu_visibility_open_pre(struct kprobe *probe, struct pt_regs *regs)
{
    const struct file *file = (const struct file *)(unsigned long)regs->regs[0];
    (void)probe;
    if (file && READ_ONCE(gpu_visibility_count) &&
        gpu_visibility_path(file->f_path.dentry))
        return gpu_visibility_return_enoent(regs);
    return 0;
}
static int gpu_visibility_getattr_pre(struct kprobe *probe, struct pt_regs *regs)
{
    const struct path *path = (const struct path *)(unsigned long)regs->regs[0];
    (void)probe;
    if (path && READ_ONCE(gpu_visibility_count) &&
        gpu_visibility_path(path->dentry))
        return gpu_visibility_return_enoent(regs);
    return 0;
}
static struct kprobe gpu_visibility_open_probe = {
    .symbol_name = "security_file_open", .pre_handler = gpu_visibility_open_pre,
};
static struct kprobe gpu_visibility_getattr_probe = {
    .symbol_name = "security_inode_getattr", .pre_handler = gpu_visibility_getattr_pre,
};

static void gpu_visibility_reap(struct work_struct *work);
static DECLARE_DELAYED_WORK(gpu_visibility_work, gpu_visibility_reap);
static void gpu_visibility_reap(struct work_struct *work)
{
    struct pid *drop[NBDRIVER_GPU_VISIBILITY_MAX_TARGETS];
    unsigned int i, count = 0;
    unsigned long flags;
    (void)work;
    rcu_read_lock();
    spin_lock_irqsave(&gpu_visibility_lock, flags);
    for (i = 0; i < ARRAY_SIZE(gpu_visibility_targets); i++) {
        struct gpu_visibility_target *target = &gpu_visibility_targets[i];
        if (target->identity && !gpu_visibility_live(target)) {
            drop[count++] = target->identity;
            memset(target, 0, sizeof(*target));
            gpu_visibility_count--;
        }
    }
    spin_unlock_irqrestore(&gpu_visibility_lock, flags);
    rcu_read_unlock();
    for (i = 0; i < count; i++)
        put_pid(drop[i]);
    if (READ_ONCE(gpu_visibility_active))
        schedule_delayed_work(&gpu_visibility_work, HZ);
}
void nbdriver_kgsl_visibility_init(void)
{
    int ret;
    ret = register_kretprobe(&gpu_visibility_iter_probe);
    if (ret)
        goto failed;
    ret = register_kprobe(&gpu_visibility_open_probe);
    if (ret)
        goto drop_iter;
    ret = register_kprobe(&gpu_visibility_getattr_probe);
    if (ret)
        goto drop_open;
    WRITE_ONCE(gpu_visibility_active, true);
    gpu_visibility_error = 0;
    schedule_delayed_work(&gpu_visibility_work, HZ);
    return;
drop_open:
    unregister_kprobe(&gpu_visibility_open_probe);
drop_iter:
    unregister_kretprobe(&gpu_visibility_iter_probe);
failed:
    gpu_visibility_error = ret;
}
void nbdriver_kgsl_visibility_exit(void)
{
    unsigned int i;
    if (READ_ONCE(gpu_visibility_active)) {
        WRITE_ONCE(gpu_visibility_active, false);
        cancel_delayed_work_sync(&gpu_visibility_work);
        unregister_kprobe(&gpu_visibility_getattr_probe);
        unregister_kprobe(&gpu_visibility_open_probe);
        unregister_kretprobe(&gpu_visibility_iter_probe);
    }
    for (i = 0; i < ARRAY_SIZE(gpu_visibility_targets); i++) {
        put_pid(gpu_visibility_targets[i].identity);
        memset(&gpu_visibility_targets[i], 0, sizeof(gpu_visibility_targets[i]));
    }
    gpu_visibility_count = 0;
}
u32 nbdriver_kgsl_visibility_features(void)
{
    return READ_ONCE(gpu_visibility_active) ? NBDRIVER_FEATURE_GPU_VISIBILITY : 0;
}
long nbdriver_kgsl_visibility_ioctl(unsigned long argument)
{
    struct nbdriver_gpu_visibility_request req;
    struct task_struct *task = NULL;
    struct pid *identity = NULL, *drop = NULL;
    unsigned long flags;
    unsigned int i, empty = ARRAY_SIZE(gpu_visibility_targets);
    int ret = 0;
    if (copy_from_user(&req, (void __user *)argument, sizeof(req)))
        return -EFAULT;
    if (req.operation > NBDRIVER_GPU_VISIBILITY_SHOW || req.pid < 0 ||
        (!req.pid && req.operation != NBDRIVER_GPU_VISIBILITY_QUERY))
        return -EINVAL;
    req.hidden = 0;
    if (req.pid) {
        task = nbdriver_query_get_process(req.pid, req.expected_start_ns);
        if (IS_ERR(task)) {
            ret = PTR_ERR(task);
            task = NULL;
            goto reply;
        }
        req.expected_start_ns = READ_ONCE(task->start_boottime);
        identity = get_pid(task_tgid(task));
    }
    if (req.operation == NBDRIVER_GPU_VISIBILITY_HIDE && !READ_ONCE(gpu_visibility_active)) {
        ret = gpu_visibility_error ? gpu_visibility_error : -EOPNOTSUPP;
        goto reply;
    }
    spin_lock_irqsave(&gpu_visibility_lock, flags);
    for (i = 0; i < ARRAY_SIZE(gpu_visibility_targets); i++) {
        struct gpu_visibility_target *target = &gpu_visibility_targets[i];
        if (!target->identity && empty == ARRAY_SIZE(gpu_visibility_targets))
            empty = i;
        if (!identity || target->identity != identity)
            continue;
        req.hidden = 1;
        if (req.operation == NBDRIVER_GPU_VISIBILITY_SHOW) {
            drop = target->identity;
            memset(target, 0, sizeof(*target));
            gpu_visibility_count--;
            req.hidden = 0;
        }
        break;
    }
    if (identity && req.operation == NBDRIVER_GPU_VISIBILITY_HIDE && !req.hidden) {
        if (empty == ARRAY_SIZE(gpu_visibility_targets)) {
            ret = -ENOSPC;
        } else {
            gpu_visibility_targets[empty].identity = identity;
            gpu_visibility_targets[empty].start_ns = req.expected_start_ns;
            gpu_visibility_targets[empty].number = pid_nr(identity);
            identity = NULL;
            gpu_visibility_count++;
            req.hidden = 1;
        }
    }
    spin_unlock_irqrestore(&gpu_visibility_lock, flags);
reply:
    put_pid(identity);
    put_pid(drop);
    if (task)
        nbdriver_query_put_process(task);
    req.active = READ_ONCE(gpu_visibility_active);
    req.target_count = READ_ONCE(gpu_visibility_count);
    req.directory_hits = atomic64_read(&gpu_visibility_directory_hits);
    req.access_hits = atomic64_read(&gpu_visibility_access_hits);
    req.missed = READ_ONCE(gpu_visibility_iter_probe.nmissed) +
        READ_ONCE(gpu_visibility_iter_probe.kp.nmissed) +
        READ_ONCE(gpu_visibility_open_probe.nmissed) +
        READ_ONCE(gpu_visibility_getattr_probe.nmissed);
    req.error = ret;
    if (copy_to_user((void __user *)argument, &req, sizeof(req)))
        return -EFAULT;
    return ret;
}
/* ===== END src/features/nbdriver_gpu_visibility.c ===== */

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

/* ===== BEGIN src/features/nbdriver_proc_visibility.c ===== */
/* kern_path uses the VFS namespace on newer KMI branches. MODULE_INFO takes
 * a string on all supported branches, unlike the changing IMPORT_NS macro. */
MODULE_INFO(import_ns, "VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver");
/* This follows NBG-driver's proc strategy: replace only proc_root's lookup
 * and iterate_shared operations, then invalidate cached PID dentries through
 * pid_revalidate. The hide table itself is the global KGSL identity table. */
static struct inode_operations nbdriver_proc_iop;
static struct file_operations nbdriver_proc_fop;
static const struct inode_operations *nbdriver_proc_orig_iop;
static const struct file_operations *nbdriver_proc_orig_fop;
static struct inode *nbdriver_proc_root_inode;
/* Own the kern_path reference for as long as the inode callbacks exist. */
static struct path nbdriver_proc_root_path;
static struct dentry *(*nbdriver_proc_orig_lookup)(struct inode *, struct dentry *, unsigned int);
static int (*nbdriver_proc_orig_readdir)(struct file *, struct dir_context *);
DEFINE_STATIC_SRCU(nbdriver_proc_srcu);
static DEFINE_MUTEX(nbdriver_proc_lock);
static bool nbdriver_proc_hooked;

static bool nbdriver_proc_root_dentry(const struct dentry *dentry)
{
    return dentry && dentry->d_sb &&
        dentry->d_sb->s_magic == PROC_SUPER_MAGIC &&
        READ_ONCE(dentry->d_parent) == dentry;
}

static bool nbdriver_proc_name_hidden(const char *name, int length)
{
    return gpu_visibility_number(name, length);
}

static bool nbdriver_proc_path_hidden(const struct dentry *dentry)
{
    const struct dentry *child = dentry;
    const struct dentry *parent;
    unsigned int depth;

    if (!READ_ONCE(gpu_visibility_count))
        return false;
    rcu_read_lock();
    for (depth = 0; child && depth < 16; depth++, child = parent) {
        parent = READ_ONCE(child->d_parent);
        if (!parent || parent == child)
            break;
        if (nbdriver_proc_root_dentry(parent) &&
            nbdriver_proc_name_hidden((const char *)child->d_name.name,
                                      child->d_name.len)) {
            rcu_read_unlock();
            return true;
        }
    }
    rcu_read_unlock();
    return false;
}

struct nbdriver_proc_filter_ctx {
    struct dir_context context;
    struct dir_context *original;
};

static GPU_FILL_RETURN nbdriver_proc_actor(struct dir_context *context,
    const char *name, int length, loff_t offset, u64 ino, unsigned int type)
{
    struct nbdriver_proc_filter_ctx *filter =
        container_of(context, struct nbdriver_proc_filter_ctx, context);
    if (nbdriver_proc_name_hidden(name, length))
        return GPU_FILL_CONTINUE;
    filter->original->pos = context->pos;
    return filter->original->actor(filter->original, name, length,
                                   offset, ino, type);
}

static struct dentry *nbdriver_proc_lookup(struct inode *dir,
    struct dentry *dentry, unsigned int flags)
{
    struct dentry *ret;
    int idx = srcu_read_lock(&nbdriver_proc_srcu);
    if (nbdriver_proc_name_hidden(dentry->d_name.name, dentry->d_name.len))
        ret = ERR_PTR(-ENOENT);
    else if (nbdriver_proc_orig_lookup)
        ret = nbdriver_proc_orig_lookup(dir, dentry, flags);
    else
        ret = ERR_PTR(-ENOENT);
    srcu_read_unlock(&nbdriver_proc_srcu, idx);
    return ret;
}

static int nbdriver_proc_readdir(struct file *file, struct dir_context *context)
{
    struct nbdriver_proc_filter_ctx filter = {
        .context = { .actor = nbdriver_proc_actor, .pos = context->pos },
        .original = context,
    };
    int ret;
    int idx = srcu_read_lock(&nbdriver_proc_srcu);
    ret = nbdriver_proc_orig_readdir ?
        nbdriver_proc_orig_readdir(file, &filter.context) : -ENOENT;
    context->pos = filter.context.pos;
    srcu_read_unlock(&nbdriver_proc_srcu, idx);
    return ret;
}

struct nbdriver_proc_revalidate_data { struct dentry *dentry; };
static struct kretprobe nbdriver_proc_revalidate_probe;
static bool nbdriver_proc_revalidate_hooked;

static int nbdriver_proc_revalidate_entry(struct kretprobe_instance *instance,
    struct pt_regs *regs)
{
    struct nbdriver_proc_revalidate_data *data = (void *)instance->data;
    struct dentry *dentry = (struct dentry *)(unsigned long)regs->regs[0];
    if (!READ_ONCE(gpu_visibility_count) || !dentry ||
        !nbdriver_proc_path_hidden(dentry))
        return 1;
    data->dentry = dentry;
    return 0;
}

static int nbdriver_proc_revalidate_ret(struct kretprobe_instance *instance,
    struct pt_regs *regs)
{
    struct nbdriver_proc_revalidate_data *data = (void *)instance->data;
    if (data->dentry && regs_return_value(regs) != 0)
        regs_set_return_value(regs, 0);
    return 0;
}

static int nbdriver_proc_visibility_init(void)
{
    struct path proc_path;
    int ret;

    mutex_lock(&nbdriver_proc_lock);
    if (nbdriver_proc_hooked) {
        mutex_unlock(&nbdriver_proc_lock);
        return 0;
    }
    /* Use exported APIs directly. Their kallsyms bodies are not valid
     * indirect-call targets on legacy CFI kernels. */
    ret = kern_path("/proc", LOOKUP_DIRECTORY, &proc_path);
    if (ret) {
        mutex_unlock(&nbdriver_proc_lock);
        return ret;
    }
    nbdriver_proc_root_inode = d_inode(proc_path.dentry);
    if (!nbdriver_proc_root_inode || !nbdriver_proc_root_inode->i_op ||
        !nbdriver_proc_root_inode->i_fop) {
        path_put(&proc_path);
        mutex_unlock(&nbdriver_proc_lock);
        return -ENOENT;
    }
    inode_lock(nbdriver_proc_root_inode);
    nbdriver_proc_orig_iop = nbdriver_proc_root_inode->i_op;
    nbdriver_proc_orig_fop = nbdriver_proc_root_inode->i_fop;
    nbdriver_proc_orig_lookup = nbdriver_proc_orig_iop->lookup;
    nbdriver_proc_orig_readdir = nbdriver_proc_orig_fop->iterate_shared;
    memcpy(&nbdriver_proc_iop, nbdriver_proc_orig_iop, sizeof(nbdriver_proc_iop));
    memcpy(&nbdriver_proc_fop, nbdriver_proc_orig_fop, sizeof(nbdriver_proc_fop));
    nbdriver_proc_iop.lookup = nbdriver_proc_lookup;
    nbdriver_proc_fop.iterate_shared = nbdriver_proc_readdir;
    nbdriver_proc_fop.owner = THIS_MODULE;
    WRITE_ONCE(nbdriver_proc_root_inode->i_op, &nbdriver_proc_iop);
    WRITE_ONCE(nbdriver_proc_root_inode->i_fop, &nbdriver_proc_fop);
    inode_unlock(nbdriver_proc_root_inode);
    nbdriver_proc_root_path = proc_path; /* Transfer the successful lookup reference. */
    nbdriver_proc_hooked = true;

    memset(&nbdriver_proc_revalidate_probe, 0,
           sizeof(nbdriver_proc_revalidate_probe));
    nbdriver_proc_revalidate_probe.entry_handler =
        nbdriver_proc_revalidate_entry;
    nbdriver_proc_revalidate_probe.handler = nbdriver_proc_revalidate_ret;
    nbdriver_proc_revalidate_probe.data_size =
        sizeof(struct nbdriver_proc_revalidate_data);
    nbdriver_proc_revalidate_probe.maxactive = max(num_possible_cpus() * 4, 64U);
    nbdriver_proc_revalidate_probe.kp.symbol_name = "pid_revalidate";
    ret = register_kretprobe(&nbdriver_proc_revalidate_probe);
    if (ret) {
        memset(&nbdriver_proc_revalidate_probe, 0,
               sizeof(nbdriver_proc_revalidate_probe));
        nbdriver_proc_revalidate_hooked = false;
    } else {
        nbdriver_proc_revalidate_hooked = true;
    }
    mutex_unlock(&nbdriver_proc_lock);
    return 0;
}

static void nbdriver_proc_visibility_exit(void)
{
    mutex_lock(&nbdriver_proc_lock);
    if (!nbdriver_proc_hooked) {
        mutex_unlock(&nbdriver_proc_lock);
        return;
    }
    inode_lock(nbdriver_proc_root_inode);
    WRITE_ONCE(nbdriver_proc_root_inode->i_op, nbdriver_proc_orig_iop);
    WRITE_ONCE(nbdriver_proc_root_inode->i_fop, nbdriver_proc_orig_fop);
    inode_unlock(nbdriver_proc_root_inode);
    synchronize_srcu(&nbdriver_proc_srcu);
    if (nbdriver_proc_revalidate_hooked) {
        unregister_kretprobe(&nbdriver_proc_revalidate_probe);
        nbdriver_proc_revalidate_hooked = false;
    }
    nbdriver_proc_root_inode = NULL;
    nbdriver_proc_orig_iop = NULL;
    nbdriver_proc_orig_fop = NULL;
    nbdriver_proc_orig_lookup = NULL;
    nbdriver_proc_orig_readdir = NULL;
    path_put(&nbdriver_proc_root_path);
    memset(&nbdriver_proc_root_path, 0, sizeof(nbdriver_proc_root_path));
    nbdriver_proc_hooked = false;
    mutex_unlock(&nbdriver_proc_lock);
}

static u32 nbdriver_proc_visibility_features(void)
{
    return READ_ONCE(nbdriver_proc_hooked) ? NBDRIVER_FEATURE_PROC_VISIBILITY : 0;
}
/* ===== END src/features/nbdriver_proc_visibility.c ===== */
/* ===== BEGIN src/features/nbdriver_pid_table_visibility.c ===== */
#include "nbdriver_filelock.h"
#include <linux/kprobes.h>
#include <linux/seq_file.h>
/* The /proc hook above hides the directory entry, but the pid stays named in
 * world-readable tables that are not proc_root children:
 *
 *   /sys/fs/cgroup/cgroup.procs   - every task on the device, mode 0755
 *   /sys/fs/cgroup/cgroup.threads - same list, thread granularity
 *   /proc/locks                   - flock/POSIX owner pid, mode 0444
 *
 * "Listed in a table but absent from /proc" is therefore a fingerprint any
 * unprivileged app can compute. These probes suppress the row instead of
 * relocating the process: staying put means no private cgroup directory ever
 * appears, so there is nothing left to enumerate either.
 *
 * Both cgroup.procs and cgroup.threads are served by cgroup_procs_show, so a
 * single probe covers both files. seq_read_iter treats a positive return from
 * ->show() as "skip this record" and simply advances to the next one. */
static struct kprobe nbdriver_cgroup_procs_probe;
static struct kprobe nbdriver_locks_show_probe;
static bool nbdriver_pid_table_cgroup_hooked;
static bool nbdriver_pid_table_locks_hooked;
static bool nbdriver_pid_table_hooked;
static atomic64_t nbdriver_pid_table_hits = ATOMIC64_INIT(0);

/* A hidden target is matched by namespace-visible pid number, the same
 * predicate the /proc entry hook already uses. */
static bool nbdriver_pid_table_hidden(pid_t number)
{
    struct gpu_visibility_target *target;
    unsigned long flags;
    unsigned int i;
    bool hidden = false;

    if (number <= 1 || !READ_ONCE(gpu_visibility_count))
        return false;
    rcu_read_lock();
    spin_lock_irqsave(&gpu_visibility_lock, flags);
    for (i = 0; i < ARRAY_SIZE(gpu_visibility_targets); i++) {
        target = &gpu_visibility_targets[i];
        if (target->identity && target->number == number &&
            gpu_visibility_live(target)) {
            hidden = true;
            break;
        }
    }
    spin_unlock_irqrestore(&gpu_visibility_lock, flags);
    rcu_read_unlock();
    return hidden;
}

/* Skip the probed function and make it report "nothing written". The caller
 * then emits no text for this record. */
static int NBDRIVER_NO_CFI nbdriver_pid_table_skip(struct pt_regs *regs, long result)
{
    regs_set_return_value(regs, (unsigned long)result);
    instruction_pointer_set(regs, procedure_link_pointer(regs));
    atomic64_inc(&nbdriver_pid_table_hits);
    return 1;
}

/* int cgroup_procs_show(struct seq_file *s, void *v) */
static int NBDRIVER_NO_CFI nbdriver_cgroup_procs_pre(struct kprobe *probe, struct pt_regs *regs)
{
    const struct task_struct *task = (const struct task_struct *)(unsigned long)regs->regs[1];

    (void)probe;
    if (!task || !READ_ONCE(gpu_visibility_count))
        return 0;
    if (!nbdriver_pid_table_hidden(task_pid_vnr((struct task_struct *)task)))
        return 0;
    /* 1 is "record skipped": no pid text ever reaches the reader. */
    return nbdriver_pid_table_skip(regs, 1);
}

/* int locks_show(struct seq_file *f, void *v): v is fl_link/flc_link.
 * Resolve its container using the exact headers for the target kernel.
 * The owner pid is recorded in the initial namespace. */
static int NBDRIVER_NO_CFI nbdriver_locks_show_pre(struct kprobe *probe, struct pt_regs *regs)
{
    const void *lock = (const void *)(unsigned long)regs->regs[1];
    pid_t owner;

    (void)probe;
    if (!lock || !READ_ONCE(gpu_visibility_count))
        return 0;
    owner = nbdriver_filelock_owner(lock);
    if (!nbdriver_pid_table_hidden(owner))
        return 0;
    return nbdriver_pid_table_skip(regs, 0);
}

static int nbdriver_pid_table_visibility_init(void)
{
    int error;

    if (READ_ONCE(nbdriver_pid_table_hooked))
        return 0;
    /* cgroup.procs is mandatory: without it the fingerprint stays. */
    if (!nbdriver_ksym_lookup_optional("cgroup_procs_show"))
        return -ENOENT;
    nbdriver_cgroup_procs_probe.symbol_name = "cgroup_procs_show";
    nbdriver_cgroup_procs_probe.pre_handler = nbdriver_cgroup_procs_pre;
    error = register_kprobe(&nbdriver_cgroup_procs_probe);
    if (error) {
        nbdriver_cgroup_procs_probe.symbol_name = NULL;
        return error;
    }
    nbdriver_pid_table_cgroup_hooked = true;
    /* /proc/locks is best effort: a build can omit the symbol. Losing it must
     * not disable the cgroup half, which is the world-readable leak. */
    if (nbdriver_ksym_lookup_optional("locks_show")) {
        nbdriver_locks_show_probe.symbol_name = "locks_show";
        nbdriver_locks_show_probe.pre_handler = nbdriver_locks_show_pre;
        if (register_kprobe(&nbdriver_locks_show_probe) == 0)
            nbdriver_pid_table_locks_hooked = true;
        else
            nbdriver_locks_show_probe.symbol_name = NULL;
    }
    WRITE_ONCE(nbdriver_pid_table_hooked, true);
    return 0;
}

static void nbdriver_pid_table_visibility_exit(void)
{
    if (!READ_ONCE(nbdriver_pid_table_hooked))
        return;
    if (nbdriver_pid_table_locks_hooked) {
        unregister_kprobe(&nbdriver_locks_show_probe);
        nbdriver_pid_table_locks_hooked = false;
    }
    if (nbdriver_pid_table_cgroup_hooked) {
        unregister_kprobe(&nbdriver_cgroup_procs_probe);
        nbdriver_pid_table_cgroup_hooked = false;
    }
    WRITE_ONCE(nbdriver_pid_table_hooked, false);
}

static u32 nbdriver_pid_table_visibility_features(void)
{
    return READ_ONCE(nbdriver_pid_table_hooked) ? NBDRIVER_FEATURE_PID_TABLE_VISIBILITY : 0;
}
/* ===== END src/features/nbdriver_pid_table_visibility.c ===== */
/* ===== BEGIN src/features/nbdriver_module_trace_visibility.c ===== */
#include <linux/kprobes.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/string.h>
/* Hiding the process is pointless if the driver that hides it is itself
 * advertised. Hiding /proc/<pid> and the pid tables left three kernel
 * interfaces that still name this module to every app on the device:
 *
 *   /proc/modules         - "nbdriver <size> <refs> Live 0x0 (OE)"
 *   /sys/module/nbdriver  - 0755 directory; srcversion/build-id/refcnt readable
 *   /proc/kallsyms        - ~800 "nbdriver_*" symbols with the [nbdriver] tag;
 *                           kptr_restrict only zeroes addresses, not names
 *
 * These probes suppress the module's own name from the first two. kallsyms is
 * deliberately not filtered here: rewriting its output would also hide the
 * symbols this driver resolves at runtime through its own ksym layer.
 *
 * m_show() is the /proc/modules record printer, but plain "m_show" is not
 * unique: fs/namespace.c defines an unrelated m_show() for /proc/mounts.
 * Registering a kprobe by that name would attach to whichever symbol the
 * kernel's kallsyms happens to resolve first, and mistaking it would break
 * mountinfo output. Instead the modules_op seq_operations is located, its
 * .show member is read, and the probe is registered on that exact address.
 *
 * seq_operations is { start, stop, next, show }, so show is the fourth
 * pointer. m_show() returns 1 for our own module, which seq_read_iter treats
 * as "skip this record"; every other module still prints normally. */
static struct kprobe nbdriver_modules_probe;
static bool nbdriver_module_trace_hooked;
static atomic64_t nbdriver_module_trace_hits = ATOMIC64_INIT(0);

/* Members of struct seq_operations, in declaration order. */
#define NBDRIVER_SEQ_SHOW_INDEX 3

/* Reads modules_op.show. Returns 0 when the symbol is unavailable, in which
 * case /proc/modules is left untouched rather than risking a probe on the
 * wrong m_show.
 *
 * No address-validity guard here: on arm64 virt_addr_valid() only accepts
 * linear-map addresses, and modules_op lives in the kernel image's .rodata,
 * which is not part of the linear map. Such a guard always fails and would
 * silently disable this feature. The symbol comes from kallsyms, so a non-zero
 * result is already a kernel text/data address. */
static unsigned long nbdriver_modules_show_address(void)
{
    const unsigned long ops = nbdriver_ksym_lookup_optional("modules_op");

    if (!ops)
        return 0;
    return ((const unsigned long *)ops)[NBDRIVER_SEQ_SHOW_INDEX];
}

/* The module's own name, resolved once so the comparison never depends on the
 * probed record layout beyond the struct module pointer in arg 2. */
static const char *nbdriver_module_trace_name(void)
{
    return KBUILD_MODNAME;
}

/* m_show(struct seq_file *m, void *p) receives the list_head, not the
 * struct module: the body does list_entry(p, struct module, list). That is
 * why the comparison has to go through container_of() instead of casting the
 * argument to struct module* directly - doing the latter reads a bogus name
 * and never matches.
 *
 * modules_op is `static const struct seq_operations` in .rodata, so its .show
 * member has to be read out at init to register the probe by address; the
 * plain symbol name "m_show" is ambiguous (fs/namespace.c has another one). */
static int NBDRIVER_NO_CFI nbdriver_modules_show_pre(struct kprobe *probe, struct pt_regs *regs)
{
    const struct list_head *node = (const struct list_head *)(unsigned long)regs->regs[1];
    const struct module *mod;

    (void)probe;
    if (!node || node == LIST_POISON1 || node == LIST_POISON2)
        return 0;
    mod = container_of(node, struct module, list);
    if (strcmp(mod->name, nbdriver_module_trace_name()) != 0)
        return 0;
    /* 1 makes seq_read_iter skip this record without printing it. */
    regs_set_return_value(regs, 1);
    instruction_pointer_set(regs, procedure_link_pointer(regs));
    atomic64_inc(&nbdriver_module_trace_hits);
    return 1;
}

static int nbdriver_module_trace_init(void)
{
    unsigned long show;

    if (READ_ONCE(nbdriver_module_trace_hooked))
        return 0;
    show = nbdriver_modules_show_address();
    if (!show)
        return -ENOENT;
    nbd_info("stage=module_trace symbol=modules_op show=%px\n", (void *)show);
    nbdriver_modules_probe.addr = (kprobe_opcode_t *)show;
    nbdriver_modules_probe.pre_handler = nbdriver_modules_show_pre;
    if (register_kprobe(&nbdriver_modules_probe) != 0) {
        nbdriver_modules_probe.addr = NULL;
        return -EIO;
    }
    WRITE_ONCE(nbdriver_module_trace_hooked, true);
    return 0;
}

static void nbdriver_module_trace_exit(void)
{
    if (!READ_ONCE(nbdriver_module_trace_hooked))
        return;
    unregister_kprobe(&nbdriver_modules_probe);
    WRITE_ONCE(nbdriver_module_trace_hooked, false);
}

static u32 nbdriver_module_trace_features(void)
{
    return READ_ONCE(nbdriver_module_trace_hooked) ? NBDRIVER_FEATURE_MODULE_TRACE_VISIBILITY : 0;
}
/* ===== END src/features/nbdriver_module_trace_visibility.c ===== */
/* ===== BEGIN src/features/nbdriver_kallsyms_visibility.c ===== */
#include <linux/kallsyms.h>
#include <linux/kprobes.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/string.h>
/* /proc/kallsyms lists every symbol with a "[module]" tag. kptr_restrict only
 * zeroes the address column; the name and the owning module stay readable, so
 * an unprivileged app still sees the ~800 nbdriver_* symbols and learns exactly
 * what the driver does (proc visibility, gpu visibility, udp capture, ...).
 *
 * s_show() prints one symbol per call and its second argument is the
 * struct kallsym_iter for that row, which carries module_name. Returning 1
 * from ->show() makes seq_read_iter skip the record, so rows whose module_name
 * matches this module are dropped while every core-kernel symbol still shows.
 *
 * As with modules_op, s_show is not a unique symbol name, so the probe is
 * registered on the address read from kallsyms_op.show instead of by name. */
static struct kprobe nbdriver_kallsyms_probe;
static bool nbdriver_kallsyms_hooked;
static atomic64_t nbdriver_kallsyms_hits = ATOMIC64_INIT(0);

#define NBDRIVER_KSYM_MODULE_NAME_LEN MODULE_NAME_LEN
/* Layout of struct kallsym_iter in kernel/kallsyms.c (private to that file):
 *   long long pos, pos_mod_end, pos_ftrace_mod_end, pos_bpf_end;
 *   unsigned long value;
 *   unsigned int nameoff;
 *   char type;
 *   char name[KSYM_NAME_LEN];
 *   char module_name[MODULE_NAME_LEN];
 *
 * The struct itself is not visible to modules, so the prefix is mirrored here.
 *
 * The offset must be computed as offsetof(...) + KSYM_NAME_LEN, NOT
 * sizeof(prefix) + KSYM_NAME_LEN: sizeof() pads the struct to its 8-byte
 * alignment (45 -> 48), while in the real struct name[] starts immediately
 * after type at byte 45. Using sizeof() shifted the read by 3 bytes, so the
 * module name never matched and the filter silently did nothing. */
struct nbdriver_kallsym_iter_prefix {
    long long pos;
    long long pos_mod_end;
    long long pos_ftrace_mod_end;
    long long pos_bpf_end;
    unsigned long value;
    unsigned int nameoff;
    char type;
    char name[KSYM_NAME_LEN];
};

#define NBDRIVER_KSYM_ITER_MODULE_OFFSET \
    (offsetof(struct nbdriver_kallsym_iter_prefix, name) + KSYM_NAME_LEN)

static int NBDRIVER_NO_CFI nbdriver_kallsyms_show_pre(struct kprobe *probe, struct pt_regs *regs)
{
    const char *module_name;

    (void)probe;
    if (!regs->regs[1])
        return 0;
    module_name = (const char *)regs->regs[1] + NBDRIVER_KSYM_ITER_MODULE_OFFSET;
    if (module_name[0] == '\0')
        return 0;
    if (strncmp(module_name, nbdriver_module_trace_name(), NBDRIVER_KSYM_MODULE_NAME_LEN) != 0)
        return 0;
    /* Skip this symbol row; core kernel symbols are unaffected. */
    regs_set_return_value(regs, 1);
    instruction_pointer_set(regs, procedure_link_pointer(regs));
    atomic64_inc(&nbdriver_kallsyms_hits);
    return 1;
}

static int nbdriver_kallsyms_init(void)
{
    const unsigned long ops = nbdriver_ksym_lookup_optional("kallsyms_op");
    unsigned long show;

    if (READ_ONCE(nbdriver_kallsyms_hooked))
        return 0;
    if (!ops)
        return -ENOENT;
    show = ((const unsigned long *)ops)[NBDRIVER_SEQ_SHOW_INDEX];
    if (!show)
        return -ENOENT;
    nbdriver_kallsyms_probe.addr = (kprobe_opcode_t *)show;
    nbdriver_kallsyms_probe.pre_handler = nbdriver_kallsyms_show_pre;
    if (register_kprobe(&nbdriver_kallsyms_probe) != 0) {
        nbdriver_kallsyms_probe.addr = NULL;
        return -EIO;
    }
    WRITE_ONCE(nbdriver_kallsyms_hooked, true);
    return 0;
}

static void nbdriver_kallsyms_exit(void)
{
    if (!READ_ONCE(nbdriver_kallsyms_hooked))
        return;
    unregister_kprobe(&nbdriver_kallsyms_probe);
    WRITE_ONCE(nbdriver_kallsyms_hooked, false);
}

static u32 nbdriver_kallsyms_features(void)
{
    return READ_ONCE(nbdriver_kallsyms_hooked) ? NBDRIVER_FEATURE_KALLSYMS_VISIBILITY : 0;
}
/* ===== END src/features/nbdriver_kallsyms_visibility.c ===== */
/* ===== BEGIN src/features/nbdriver_sysfs_module_visibility.c ===== */
#include <linux/dcache.h>
#include <linux/fs.h>
#include <linux/kprobes.h>
#include <linux/magic.h>
#include <linux/string.h>
/* Hiding the module from /proc/modules and /proc/kallsyms is not enough:
 * the kernel also exposes a sysfs directory per loaded module, and
 * /sys/module/nbdriver is world-readable:
 *
 *   /sys/module/nbdriver/srcversion   - binary identity of this build
 *   /sys/module/nbdriver/notes/.note.gnu.build-id - linkable fingerprint
 *   /sys/module/nbdriver/refcnt, coresize, initstate
 *
 * Any app can `ls /sys/module` and see the directory name, which is enough to
 * identify a third-party driver even when every pid is hidden.
 *
 * /sys/module is kernfs, so its listing is produced by
 * kernfs_fop_readdir() (fs/kernfs/dir.c), which emits names through the
 * dir_context actor. Wrapping that actor lets the single entry be dropped while
 * every other module directory still appears. */
static struct kretprobe nbdriver_sysfs_module_probe;
static bool nbdriver_sysfs_module_hooked;
static atomic64_t nbdriver_sysfs_module_hits = ATOMIC64_INIT(0);

struct nbdriver_kernfs_iter {
    struct dir_context context;
    struct dir_context *original;
};

/* True when the directory being read is exactly /sys/module. Both the sb magic
 * and the dentry name are checked so an identically named directory elsewhere
 * in sysfs is not affected. */
static bool nbdriver_sysfs_module_dir(const struct dentry *dentry)
{
    const char *name;
    const struct dentry *parent;

    if (!dentry || !dentry->d_sb || dentry->d_sb->s_magic != SYSFS_MAGIC)
        return false;
    name = (const char *)READ_ONCE(dentry->d_name.name);
    if (!name || strcmp(name, "module"))
        return false;
    /* Must be a direct child of the sysfs root. */
    parent = READ_ONCE(dentry->d_parent);
    if (!parent)
        return false;
    if (parent != dentry) {
        const struct dentry *grand = READ_ONCE(parent->d_parent);
        if (grand && grand == parent)
            return true;
    }
    return false;
}

static GPU_FILL_RETURN nbdriver_kernfs_actor(struct dir_context *context, const char *name,
    int length, loff_t offset, u64 ino, unsigned int type)
{
    struct nbdriver_kernfs_iter *iter =
        container_of(context, struct nbdriver_kernfs_iter, context);

    if (length > 0 && (size_t)length == strlen(nbdriver_module_trace_name()) &&
        !memcmp(name, nbdriver_module_trace_name(), (size_t)length)) {
        atomic64_inc(&nbdriver_sysfs_module_hits);
        return GPU_FILL_CONTINUE;
    }
    iter->original->pos = context->pos;
    return iter->original->actor(iter->original, name, length, offset, ino, type);
}

static int nbdriver_sysfs_module_enter(struct kretprobe_instance *instance,
                                       struct pt_regs *regs)
{
    struct nbdriver_kernfs_iter *iter = (void *)instance->data;
    struct file *file = (struct file *)(unsigned long)regs->regs[0];
    struct dir_context *context = (struct dir_context *)(unsigned long)regs->regs[1];

    if (!file || !context || !READ_ONCE(nbdriver_sysfs_module_hooked))
        return 1;
    if (!nbdriver_sysfs_module_dir(file->f_path.dentry))
        return 1;
    iter->original = context;
    iter->context = *context;
    iter->context.actor = nbdriver_kernfs_actor;
    regs->regs[1] = (unsigned long)&iter->context;
    return 0;
}

static int nbdriver_sysfs_module_leave(struct kretprobe_instance *instance,
                                       struct pt_regs *regs)
{
    struct nbdriver_kernfs_iter *iter = (void *)instance->data;

    (void)regs;
    iter->original->pos = iter->context.pos;
    return 0;
}

static int nbdriver_sysfs_module_init(void)
{
    if (READ_ONCE(nbdriver_sysfs_module_hooked))
        return 0;
    memset(&nbdriver_sysfs_module_probe, 0, sizeof(nbdriver_sysfs_module_probe));
    nbdriver_sysfs_module_probe.kp.symbol_name = "kernfs_fop_readdir";
    nbdriver_sysfs_module_probe.entry_handler = nbdriver_sysfs_module_enter;
    nbdriver_sysfs_module_probe.handler = nbdriver_sysfs_module_leave;
    nbdriver_sysfs_module_probe.data_size = sizeof(struct nbdriver_kernfs_iter);
    nbdriver_sysfs_module_probe.maxactive = 64;
    if (register_kretprobe(&nbdriver_sysfs_module_probe) != 0)
        return -EIO;
    WRITE_ONCE(nbdriver_sysfs_module_hooked, true);
    return 0;
}

static void nbdriver_sysfs_module_exit(void)
{
    if (!READ_ONCE(nbdriver_sysfs_module_hooked))
        return;
    unregister_kretprobe(&nbdriver_sysfs_module_probe);
    WRITE_ONCE(nbdriver_sysfs_module_hooked, false);
}

static u32 nbdriver_sysfs_module_features(void)
{
    return READ_ONCE(nbdriver_sysfs_module_hooked) ? NBDRIVER_FEATURE_SYSFS_MODULE_VISIBILITY : 0;
}
/* ===== END src/features/nbdriver_sysfs_module_visibility.c ===== */
/* ===== BEGIN src/features/nbdriver_pidfd_visibility.c ===== */
#include <linux/kprobes.h>
#include <linux/pid.h>
#include <linux/sched.h>
/* Hiding /proc/<pid> and the world-readable pid tables still leaves one
 * syscall that names a process without ever touching procfs:
 *
 *   pidfd_open(pid, 0)
 *
 * It resolves the pid through find_get_pid() against the kernel pid hash, so a
 * hidden target still yields a usable pidfd while a non-existent pid yields
 * clean errors. An unprivileged app can therefore binary-probe every pid and
 * learn that a hidden process exists - the exact signal this driver is meant
 * to remove. kill(pid, 0) reveals the same thing through the signal path.
 *
 * Both are made to report ESRCH for hidden targets, which is what the kernel
 * returns for a pid that does not exist, so nothing distinguishes the two
 * cases from the caller's point of view. The probe returns before the real
 * handler runs, so no pidfd and no permission check leaks timing either. */
static struct kprobe nbdriver_pidfd_probe;
static struct kprobe nbdriver_kill_probe;
static struct kprobe nbdriver_tgkill_probe;
static struct kprobe nbdriver_tkill_probe;
static bool nbdriver_pidfd_hooked;
static bool nbdriver_kill_hooked;
static bool nbdriver_tgkill_hooked;
static bool nbdriver_tkill_hooked;
static atomic64_t nbdriver_pidfd_hits = ATOMIC64_INIT(0);

/* Proves from userspace that the pidfd probe really fires: the first
 * interception emits one kernel log line under a distinct tag ("nbpf: ", which
 * the printk filter does not match), and the counter is also reported to the
 * caller. Without an observable counter a registered-but-unfired probe is
 * indistinguishable from a working one - that is exactly how the arm64 pt_regs
 * indirection bug stayed hidden earlier. */
static void nbdriver_pidfd_note_hit(void)
{
    if (atomic64_inc_return(&nbdriver_pidfd_hits) == 1)
        pr_info("nbpf: pidfd/kill visibility active\n");
}

/* Shared with the /proc and pid-table hooks: same target table, same liveness
 * check, so all of them agree on exactly which pids are hidden. */
static bool nbdriver_pidfd_hidden(long pid)
{
    struct gpu_visibility_target *target;
    unsigned long flags;
    unsigned int i;
    bool hidden = false;

    if (pid <= 1 || pid > INT_MAX || !READ_ONCE(gpu_visibility_count))
        return false;
    rcu_read_lock();
    spin_lock_irqsave(&gpu_visibility_lock, flags);
    for (i = 0; i < ARRAY_SIZE(gpu_visibility_targets); i++) {
        target = &gpu_visibility_targets[i];
        if (target->identity && target->number == (pid_t)pid &&
            gpu_visibility_live(target)) {
            hidden = true;
            break;
        }
    }
    spin_unlock_irqrestore(&gpu_visibility_lock, flags);
    rcu_read_unlock();
    return hidden;
}

/* On arm64 a syscall implementation is __arm64_sys_<name>(const struct pt_regs *),
 * so the argument this kprobe sees in x0 is a pointer to the pt_regs of the
 * syscall, not the syscall's first argument. The real pid therefore lives at
 * outer_regs->regs[0] and has to be dereferenced once. Reading regs->regs[0]
 * directly yields the pt_regs pointer itself, which is never a hidden pid -
 * that is why the probe registered successfully but never fired. */
static inline long nbdriver_syscall_arg0(struct pt_regs *regs)
{
    const struct pt_regs *inner = (const struct pt_regs *)(unsigned long)regs->regs[0];

    if (!inner)
        return 0;
    return (long)inner->regs[0];
}

static int NBDRIVER_NO_CFI nbdriver_pidfd_open_pre(struct kprobe *probe, struct pt_regs *regs)
{
    (void)probe;
    if (!nbdriver_pidfd_hidden(nbdriver_syscall_arg0(regs)))
        return 0;
    /* ESRCH is exactly what a missing pid produces. */
    regs_set_return_value(regs, (unsigned long)-ESRCH);
    instruction_pointer_set(regs, procedure_link_pointer(regs));
    nbdriver_pidfd_note_hit();
    return 1;
}

/* kill(pid_t pid, int sig): the existing signal path answers "does this pid
 * exist" through EPERM vs ESRCH. Report ESRCH so it matches the pidfd result. */
static int NBDRIVER_NO_CFI nbdriver_kill_pre(struct kprobe *probe, struct pt_regs *regs)
{
    (void)probe;
    if (!nbdriver_pidfd_hidden(nbdriver_syscall_arg0(regs)))
        return 0;
    regs_set_return_value(regs, (unsigned long)-ESRCH);
    instruction_pointer_set(regs, procedure_link_pointer(regs));
    nbdriver_pidfd_note_hit();
    return 1;
}

/* tgkill(tgid, pid, sig) and tkill(pid, sig) are separate entry points that a
 * detector can call instead of kill(); both expose the same existence signal.
 * tgkill carries (tgid, tid) so either slot may name the hidden process. */
static int NBDRIVER_NO_CFI nbdriver_tgkill_pre(struct kprobe *probe, struct pt_regs *regs)
{
    const struct pt_regs *inner;
    long a0, a1;

    (void)probe;
    inner = (const struct pt_regs *)(unsigned long)regs->regs[0];
    if (!inner)
        return 0;
    a0 = (long)inner->regs[0];
    a1 = (long)inner->regs[1];
    if (!nbdriver_pidfd_hidden(a0) && !nbdriver_pidfd_hidden(a1))
        return 0;
    regs_set_return_value(regs, (unsigned long)-ESRCH);
    instruction_pointer_set(regs, procedure_link_pointer(regs));
    nbdriver_pidfd_note_hit();
    return 1;
}

static int nbdriver_pidfd_visibility_init(void)
{
    if (READ_ONCE(nbdriver_pidfd_hooked))
        return 0;
    /* On arm64 the generated syscall body is __arm64_sys_<name>; the __do_/
     * __se_ forms exist on other architectures. Try arm64 first, then the
     * generic spellings, so this keeps working if the symbol layout changes. */
    if (nbdriver_ksym_lookup_optional("__arm64_sys_pidfd_open"))
        nbdriver_pidfd_probe.symbol_name = "__arm64_sys_pidfd_open";
    else if (nbdriver_ksym_lookup_optional("__do_sys_pidfd_open"))
        nbdriver_pidfd_probe.symbol_name = "__do_sys_pidfd_open";
    else if (nbdriver_ksym_lookup_optional("__se_sys_pidfd_open"))
        nbdriver_pidfd_probe.symbol_name = "__se_sys_pidfd_open";
    else
        return -ENOENT;
    nbdriver_pidfd_probe.pre_handler = nbdriver_pidfd_open_pre;
    if (register_kprobe(&nbdriver_pidfd_probe) != 0) {
        nbdriver_pidfd_probe.symbol_name = NULL;
        return -EIO;
    }
    nbdriver_pidfd_hooked = true;
    /* kill() is the other pid-existence oracle; optional so a missing symbol
     * never disables the pidfd half. */
    if (nbdriver_ksym_lookup_optional("__arm64_sys_kill"))
        nbdriver_kill_probe.symbol_name = "__arm64_sys_kill";
    else if (nbdriver_ksym_lookup_optional("__do_sys_kill"))
        nbdriver_kill_probe.symbol_name = "__do_sys_kill";
    else if (nbdriver_ksym_lookup_optional("__se_sys_kill"))
        nbdriver_kill_probe.symbol_name = "__se_sys_kill";
    if (nbdriver_kill_probe.symbol_name) {
        nbdriver_kill_probe.pre_handler = nbdriver_kill_pre;
        if (register_kprobe(&nbdriver_kill_probe) == 0)
            nbdriver_kill_hooked = true;
        else
            nbdriver_kill_probe.symbol_name = NULL;
    }
    /* tgkill uses the same handler: tgid and tid are both checked. */
    if (nbdriver_ksym_lookup_optional("__arm64_sys_tgkill"))
        nbdriver_tgkill_probe.symbol_name = "__arm64_sys_tgkill";
    if (nbdriver_tgkill_probe.symbol_name) {
        nbdriver_tgkill_probe.pre_handler = nbdriver_tgkill_pre;
        if (register_kprobe(&nbdriver_tgkill_probe) == 0)
            nbdriver_tgkill_hooked = true;
        else
            nbdriver_tgkill_probe.symbol_name = NULL;
    }
    if (nbdriver_ksym_lookup_optional("__arm64_sys_tkill"))
        nbdriver_tkill_probe.symbol_name = "__arm64_sys_tkill";
    if (nbdriver_tkill_probe.symbol_name) {
        nbdriver_tkill_probe.pre_handler = nbdriver_kill_pre;
        if (register_kprobe(&nbdriver_tkill_probe) == 0)
            nbdriver_tkill_hooked = true;
        else
            nbdriver_tkill_probe.symbol_name = NULL;
    }
    return 0;
}

static void nbdriver_pidfd_visibility_exit(void)
{
    if (nbdriver_tkill_hooked) {
        unregister_kprobe(&nbdriver_tkill_probe);
        nbdriver_tkill_hooked = false;
    }
    if (nbdriver_tgkill_hooked) {
        unregister_kprobe(&nbdriver_tgkill_probe);
        nbdriver_tgkill_hooked = false;
    }
    if (nbdriver_kill_hooked) {
        unregister_kprobe(&nbdriver_kill_probe);
        nbdriver_kill_hooked = false;
    }
    if (nbdriver_pidfd_hooked) {
        unregister_kprobe(&nbdriver_pidfd_probe);
        nbdriver_pidfd_hooked = false;
    }
}

static u32 nbdriver_pidfd_visibility_features(void)
{
    return READ_ONCE(nbdriver_pidfd_hooked) ? NBDRIVER_FEATURE_PIDFD_VISIBILITY : 0;
}
/* ===== END src/features/nbdriver_pidfd_visibility.c ===== */
/* ===== BEGIN src/features/nbdriver_printk_visibility.c ===== */
#include <linux/kprobes.h>
#include <linux/printk.h>
#include <linux/string.h>
/* dmesg is the last place the module still announces itself, and it can only
 * be closed globally: setting dmesg_restrict=1 also blocks the kernel log for
 * every other consumer, and this build's own licence check reads it, so a
 * global switch breaks authentication.
 *
 * Instead the message is dropped at the single point where it enters the ring
 * buffer: vprintk_store(). Every pr_* macro funnels through it, so returning
 * early there means the record is never stored and never appears in dmesg,
 * /proc/kmsg or klogctl(), regardless of who reads them.
 *
 * Identification does not need the formatted text: this module's logging
 * macros prepend a literal "nbdriver: " to the format string, so the fmt
 * argument itself starts with the marker. Comparing that prefix is exact, is
 * unaffected by rate limiting or by %p hashing, and never matches another
 * subsystem's output. */
static struct kprobe nbdriver_printk_probe;
static bool nbdriver_printk_hooked;
static atomic64_t nbdriver_printk_hits = ATOMIC64_INIT(0);

#define NBDRIVER_PRINTK_MARKER "nbdriver: "
#define NBDRIVER_PRINTK_MARKER_LEN (sizeof(NBDRIVER_PRINTK_MARKER) - 1)

/* vprintk_store(int facility, int level, const struct dev_printk_info *dev_info,
 *               const char *fmt, va_list args)
 * fmt is the 4th argument. Returning 0 reports "nothing stored", which is the
 * same value the function returns when printk recursion is detected. */
static int NBDRIVER_NO_CFI nbdriver_printk_store_pre(struct kprobe *probe, struct pt_regs *regs)
{
    const char *fmt = (const char *)(unsigned long)regs->regs[3];

    (void)probe;
    if (!fmt)
        return 0;
    if (strncmp(fmt, NBDRIVER_PRINTK_MARKER, NBDRIVER_PRINTK_MARKER_LEN) != 0)
        return 0;
    regs_set_return_value(regs, 0);
    instruction_pointer_set(regs, procedure_link_pointer(regs));
    atomic64_inc(&nbdriver_printk_hits);
    return 1;
}

static int nbdriver_printk_visibility_init(void)
{
    if (READ_ONCE(nbdriver_printk_hooked))
        return 0;
    if (!nbdriver_ksym_lookup_optional("vprintk_store"))
        return -ENOENT;
    nbdriver_printk_probe.symbol_name = "vprintk_store";
    nbdriver_printk_probe.pre_handler = nbdriver_printk_store_pre;
    if (register_kprobe(&nbdriver_printk_probe) != 0) {
        nbdriver_printk_probe.symbol_name = NULL;
        return -EIO;
    }
    WRITE_ONCE(nbdriver_printk_hooked, true);
    return 0;
}

static void nbdriver_printk_visibility_exit(void)
{
    if (!READ_ONCE(nbdriver_printk_hooked))
        return;
    unregister_kprobe(&nbdriver_printk_probe);
    WRITE_ONCE(nbdriver_printk_hooked, false);
}

static u32 nbdriver_printk_visibility_features(void)
{
    return READ_ONCE(nbdriver_printk_hooked) ? NBDRIVER_FEATURE_PRINTK_VISIBILITY : 0;
}
/* ===== END src/features/nbdriver_printk_visibility.c ===== */
