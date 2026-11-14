/* ===== BEGIN src/core/nbdriver_main.c ===== */
// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/module.h>

#include "nbdriver_abi.h"
#include "nbdriver_comm.h"
#include "nbdriver_entry.h"
#include "nbdriver_ksym.h"
#include "nbdriver_log.h"
#include "nbdriver_cfi.h"
#include "nbdriver_read.h"
#include "nbdriver_query.h"
#include "nbdriver_metadata.h"
#include "nbdriver_devpts.h"
#include "nbdriver_runtime.h"

static int __init nbdriver_init(void)
{
	int ret;

	ret = nbdriver_comm_init();
	if (ret) {
		nbd_err("stage=module_init dependency=comm error=%d\n", ret);
		return ret;
	}

	ret = nbdriver_ksym_init();
	if (ret) {
		nbd_err("stage=module_init dependency=ksym error=%d\n", ret);
		goto err_comm;
	}

	ret = nbdriver_cfi_init();
	if (ret) {
		nbd_err("stage=module_init dependency=cfi error=%d\n", ret);
		goto err_ksym;
	}

	ret = nbdriver_read_init();
	if (ret) {
		nbd_err("stage=module_init dependency=read error=%d\n", ret);
		goto err_cfi;
	}

	ret = nbdriver_query_init();
	if (ret) {
		nbd_err("stage=module_init dependency=query error=%d\n", ret);
		goto err_read;
	}

	ret = nbdriver_metadata_init();
	if (ret)
		goto err_query;

	ret = nbdriver_devpts_init();
	if (ret)
		goto err_metadata;

	ret = nbdriver_entry_init();
	if (ret) {
		nbd_err("stage=module_init dependency=entry error=%d\n", ret);
		goto err_devpts;
	}

	ret = nbdriver_runtime_init();
	if (ret)
		goto err_entry;

	nbd_info("stage=module_init status=loaded abi=%u entry=%s\n",
		 NBDRIVER_ABI_VERSION, nbdriver_entry_target());
	return 0;

err_devpts:
	nbdriver_devpts_exit();
err_entry:
	nbdriver_entry_exit();
err_metadata:
	nbdriver_metadata_exit();
err_query:
	nbdriver_query_exit();
err_read:
	nbdriver_read_exit();
err_cfi:
	nbdriver_cfi_exit();
err_ksym:
	nbdriver_ksym_exit();
err_comm:
	nbdriver_comm_exit();
	return ret;
}

static void __exit nbdriver_exit(void)
{
	nbdriver_runtime_exit();
	nbdriver_entry_exit();
	nbdriver_devpts_exit();
	nbdriver_metadata_exit();
	nbdriver_query_exit();
	nbdriver_read_exit();
	nbdriver_cfi_exit();
	nbdriver_ksym_exit();
	nbdriver_comm_exit();
	nbd_info("stage=module_exit status=unloaded\n");
}

module_init(nbdriver_init);
module_exit(nbdriver_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("nbdriver ioctl communication");
MODULE_VERSION("0.1.0");
/* ===== END src/core/nbdriver_main.c ===== */

/* ===== BEGIN src/core/nbdriver_comm.c ===== */
// SPDX-License-Identifier: GPL-2.0-only
#include <linux/anon_inodes.h>
#include <linux/errno.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/uaccess.h>

#include "nbdriver_abi.h"
#include "nbdriver_comm.h"
#include "nbdriver_log.h"
#include "nbdriver_read.h"
#include "nbdriver_query.h"
#include "nbdriver_metadata.h"
#include "nbdriver_devpts.h"

/*
 * 这一层只依赖全系 EXPORT_SYMBOL(_GPL) 的接口：
 * anon_inode_getfile / get_unused_fd_flags / put_unused_fd / fd_install。
 * DDK 六个 KMI 的 Module.symvers 已确认全部导出，可直接静态链接。
 */

static long nbdriver_ioctl(struct file *file, unsigned int command,
			   unsigned long argument)
{
	(void)file;
	if (nbdriver_runtime_quiesced() &&
	    command != NBDRIVER_IOC_GET_ABI &&
	    command != NBDRIVER_IOC_RUNTIME_STATUS &&
	    command != NBDRIVER_IOC_RUNTIME_CONTROL)
		return -ESHUTDOWN;

	switch (command) {
	case NBDRIVER_IOC_MAPS:
	case NBDRIVER_IOC_MAPPING:
	case NBDRIVER_IOC_VMA:
	case NBDRIVER_IOC_FD_SIZE:
	case NBDRIVER_IOC_GPU_SYNC:
	case NBDRIVER_IOC_GPU_VISIBILITY:
	case NBDRIVER_IOC_GPU_INFO:
	case NBDRIVER_IOC_RANDOM:
		return nbdriver_metadata_ioctl(command, argument);
	case NBDRIVER_IOC_DEVPTS_STATUS:
		return nbdriver_devpts_status_ioctl(argument);
	case NBDRIVER_IOC_FIND_PROCESS:
	case NBDRIVER_IOC_MODULE_BASE:
	case NBDRIVER_IOC_PROCESS_ID:
	case NBDRIVER_IOC_THREAD_ID:
	case NBDRIVER_IOC_THREAD_SIGNAL:
	case NBDRIVER_IOC_THREAD_ENV:
	case NBDRIVER_IOC_THREAD_FP:
		return nbdriver_query_ioctl(command, argument);
	case NBDRIVER_IOC_READV:
		return nbdriver_readv_ioctl(argument);
	case NBDRIVER_IOC_READ_PROCESS:
		return nbdriver_read_ioctl(argument);
	case NBDRIVER_IOC_WRITE_PROCESS:
		return nbdriver_write_ioctl(argument);
	case NBDRIVER_IOC_GET_ABI: {
		struct nbdriver_abi_info info = {
			.version = NBDRIVER_ABI_VERSION,
			.size = sizeof(info),
			.features = NBDRIVER_FEATURE_READ_PROCESS |
				NBDRIVER_FEATURE_WRITE_PROCESS |
				NBDRIVER_FEATURE_READV | nbdriver_query_features() |
				nbdriver_metadata_features() |
				nbdriver_devpts_features() |
				nbdriver_runtime_features() |
				(nbdriver_read_fault_in_ready() ?
				 NBDRIVER_FEATURE_READ_FAULT_IN : 0),
			.reserved = 0,
		};

		if (!argument) {
			nbd_warn("stage=ioctl command=get_abi reason=null_argument error=%d\n",
				 -EFAULT);
			return -EFAULT;
		}
		if (copy_to_user((void __user *)argument, &info, sizeof(info))) {
			nbd_warn("stage=ioctl command=get_abi reason=copy_to_user_failed error=%d\n",
				 -EFAULT);
			return -EFAULT;
		}
		return 0;
	}
	case NBDRIVER_IOC_RUNTIME_STATUS:
		return nbdriver_runtime_ioctl(argument);
	case NBDRIVER_IOC_RUNTIME_CONTROL:
		return nbdriver_runtime_control_ioctl(argument);
	default:
		nbd_warn("stage=ioctl command=0x%08x reason=unknown_command error=%d\n",
			 command, -ENOTTY);
		return -ENOTTY;
	}
}

static const struct file_operations nbdriver_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = nbdriver_ioctl,
	.compat_ioctl = nbdriver_ioctl,
};

int nbdriver_comm_install_fd(int __user *output_fd)
{
	struct file *file;
	int fd;
	int written;

	if (!output_fd) {
		nbd_warn("stage=install_fd reason=null_output error=%d\n", -EFAULT);
		return -EFAULT;
	}

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		nbd_err("stage=install_fd operation=get_unused_fd_flags error=%d\n", fd);
		return fd;
	}

	file = anon_inode_getfile("[nbdriver]", &nbdriver_fops, NULL,
				  O_RDWR | O_CLOEXEC);
	if (IS_ERR(file)) {
		int error = PTR_ERR(file);

		nbd_err("stage=install_fd operation=anon_inode_getfile error=%d\n",
			error);
		put_unused_fd(fd);
		return error;
	}

	/*
	 * Copy first so a bad user pointer can roll back the reserved descriptor.
	 * The integer is valid only after this syscall returns; it must not be
	 * used as an early-publish signal to another thread.
	 */
	if (copy_to_user(output_fd, &fd, sizeof(fd))) {
		nbd_err("stage=install_fd operation=copy_to_user error=%d\n", -EFAULT);
		fput(file);
		put_unused_fd(fd);
		return -EFAULT;
	}

	fd_install(fd, file);

	written = fd;
	nbd_info("stage=install_fd status=ok fd=%d\n", written);
	return 0;
}

int nbdriver_comm_init(void)
{
	return 0;
}

void nbdriver_comm_exit(void)
{
}
/* ===== END src/core/nbdriver_comm.c ===== */

/* ===== BEGIN src/core/nbdriver_read.c ===== */
// SPDX-License-Identifier: GPL-2.0-only
#include <linux/capability.h>
#include <linux/err.h>
#include <linux/mm.h>
#include <linux/pid.h>
#include <linux/ptrace.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/highmem.h>
#include <linux/vmalloc.h>
#include <linux/uaccess.h>

#include <asm/cacheflush.h>
#include <asm/sysreg.h>
#include <asm/cpufeature.h>
#include <asm/memory.h>

#include "nbdriver_abi.h"
#include "nbdriver_cfi.h"
#include "nbdriver_ksym.h"
#include "nbdriver_log.h"
#include "nbdriver_read.h"
#include "nbdriver_walk.h"

static __typeof__(&mm_access) nbdriver_mm_access;
typedef __typeof__(&fixup_user_fault) nbdriver_fixup_user_fault_fn_t;
static nbdriver_fixup_user_fault_fn_t nbdriver_fixup_user_fault;
static unsigned long *nbdriver_zero_pfn;
static __typeof__(&copy_to_user_page) nbdriver_copy_to_user_page;

/* The write operation needs PROCESS__PTRACE authorization in SELinux;
 * CAP_SYS_PTRACE is an additional gate, not a replacement for this mode. */
static noinline struct mm_struct *NBDRIVER_NO_CFI
nbdriver_access_mm_mode(struct task_struct *task, bool write)
{
	return nbdriver_mm_access(task, write ? PTRACE_MODE_ATTACH_FSCREDS :
				 PTRACE_MODE_READ_FSCREDS);
}

struct mm_struct *nbdriver_access_mm(struct task_struct *task)
{
	return nbdriver_access_mm_mode(task, false);
}

/* fixup_user_fault may drop mmap_lock; callers must look up the VMA again. */
static noinline int NBDRIVER_NO_CFI
nbdriver_fault_in_page(struct mm_struct *mm, unsigned long address)
{
	nbdriver_fixup_user_fault_fn_t fn =
		READ_ONCE(nbdriver_fixup_user_fault);
	unsigned int flags = 0;
	bool unlocked = false;

	if (!fn)
		return -ENOSYS;
	if (mm != current->mm)
		flags |= FAULT_FLAG_REMOTE;
	return fn(mm, address, flags, &unlocked);
}

static noinline void NBDRIVER_NO_CFI
nbdriver_copy_executable(struct vm_area_struct *vma, struct page *page,
			 unsigned long address, void *destination,
			 const void *source, unsigned long length)
{
	nbdriver_copy_to_user_page(vma, page, address, destination, source, length);
}

int nbdriver_read_init(void)
{
	nbdriver_mm_access = (__typeof__(nbdriver_mm_access))
		nbdriver_ksym_lookup("mm_access");
	nbdriver_fixup_user_fault =
		(nbdriver_fixup_user_fault_fn_t)
		nbdriver_ksym_lookup("fixup_user_fault");
	nbdriver_zero_pfn =
		(unsigned long *)nbdriver_ksym_lookup("zero_pfn");
	nbd_info("stage=read_init fault_in=%s zero_page=%s\n",
		nbdriver_fixup_user_fault ? "ready" : "unavailable",
		nbdriver_zero_pfn ? "ready" : "unavailable");
	nbdriver_copy_to_user_page = (__typeof__(nbdriver_copy_to_user_page))
		nbdriver_ksym_lookup("copy_to_user_page");
	nbd_info("stage=read_init executable_write=%s\n",
		nbdriver_copy_to_user_page ? "ready" : "unavailable");
	if (!nbdriver_mm_access) {
		nbd_err("stage=read_init symbol=mm_access error=%d\n", -ENOENT);
		return -ENOENT;
	}
	return 0;
}

bool nbdriver_read_fault_in_ready(void)
{
	return READ_ONCE(nbdriver_fixup_user_fault) != NULL &&
		READ_ONCE(nbdriver_zero_pfn) != NULL;
}

void nbdriver_read_exit(void)
{
	nbdriver_zero_pfn = NULL;
	nbdriver_fixup_user_fault = NULL;
	nbdriver_copy_to_user_page = NULL;
	nbdriver_mm_access = NULL;
}

static int nbdriver_read_mm_inner(struct mm_struct *mm, struct nbdriver_read_request *request,
		     u8 *bytes, bool fault_in)
{
	/* Android malloc pointers can carry a top-byte tag. Keep the ABI address
	 * for diagnostics, and walk VMAs/page tables using the untagged address. */
	unsigned long address = untagged_addr((unsigned long)request->address);
	int ret = 0;

	while (request->copied < request->size) {
		struct vm_area_struct *vma;
		struct page *page;
		unsigned long *zero_pfn = READ_ONCE(nbdriver_zero_pfn);
		unsigned long at = address + request->copied;
		size_t length;
		bool fault_attempted = false;
		bool mapped_ram, uncached = false;

retry_vma:
		vma = find_vma(mm, at);
		if (!vma || at < vma->vm_start) {
			ret = -EFAULT;
			break;
		}
		/* arm64 vm_get_page_prot(VM_WRITE) is readable (PAGE_READONLY
		 * for private mappings, PAGE_SHARED for shared mappings). Keep
		 * PROT_NONE/execute-only rejection stays in place. Special VMAs
		 * are accepted only after validating and pinning normal RAM. */
		if (!(vma->vm_flags & (VM_READ | VM_WRITE))) {
			ret = -EACCES;
			break;
		}
		if (vma->vm_flags & VM_HUGETLB) {
			ret = -EOPNOTSUPP;
			break;
		}
		mapped_ram = vma->vm_flags & (VM_IO | VM_PFNMAP | VM_MIXEDMAP);
		length = min_t(size_t, request->size - request->copied,
				   PAGE_SIZE - offset_in_page(at));
		length = min_t(size_t, length, vma->vm_end - at);
		if (mapped_ram)
			ret = nbdriver_walk_get_ram_page(vma, at, &page, &uncached);
		else
			ret = nbdriver_walk_get_page(vma, at, false, zero_pfn != NULL,
					     zero_pfn ? READ_ONCE(*zero_pfn) : 0, &page);
		/* A GPU/device VMA must never fault new pages in on this path. */
		if (ret == -ENODATA && !mapped_ram && fault_in && !fault_attempted) {
			fault_attempted = true;
			ret = nbdriver_fault_in_page(mm, at);
			if (!ret)
				goto retry_vma;
		}
		if (ret)
			break;
		if (uncached) {
			/* Match the observed Normal-NC leaf; a cached direct-map alias
			 * can return stale GPU memory. Never ioremap arbitrary MMIO. */
			void *mapped = vmap(&page, 1, VM_MAP,
					    pgprot_writecombine(PAGE_KERNEL));

			if (!mapped) {
				nbdriver_walk_put_page(page);
				ret = -ENOMEM;
				break;
			}
			memcpy(bytes + request->copied,
			       (u8 *)mapped + offset_in_page(at), length);
			vunmap(mapped);
		} else {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
		{
			void *mapped = kmap_local_page(page);

			memcpy(bytes + request->copied,
			       (u8 *)mapped + offset_in_page(at), length);
			kunmap_local(mapped);
		}
#else
		{
			void *mapped = kmap_atomic(page);

			memcpy(bytes + request->copied,
			       (u8 *)mapped + offset_in_page(at), length);
			kunmap_atomic(mapped);
		}
#endif
		}
		nbdriver_walk_put_page(page);
		request->copied += length;
	}
	return ret;
}

int nbdriver_read_mm_locked(struct mm_struct *mm,
		struct nbdriver_read_request *request, u8 *bytes)
{
	return nbdriver_read_mm_inner(mm, request, bytes, false);
}

int nbdriver_read_mm(struct mm_struct *mm, struct nbdriver_read_request *request,
		     u8 *bytes, bool fault_in)
{
	int ret;

	mmap_read_lock(mm);
	ret = nbdriver_read_mm_inner(mm, request, bytes, fault_in);
	mmap_read_unlock(mm);
	return ret;
}

static int nbdriver_read_target(struct nbdriver_read_request *request, u8 *bytes)
{
	struct pid *pid;
	struct task_struct *task;
	struct mm_struct *mm;
	u64 start_ns;
	int ret = 0;

	pid = find_get_pid(request->pid);
	if (!pid)
		return -ESRCH;
	task = get_pid_task(pid, PIDTYPE_TGID);
	put_pid(pid);
	if (!task)
		return -ESRCH;
	start_ns = READ_ONCE(task->start_boottime);
	if (request->expected_start_ns && request->expected_start_ns != start_ns) {
		ret = -ESTALE;
		goto put_task;
	}
	request->expected_start_ns = start_ns;
	mm = nbdriver_access_mm(task);
	if (IS_ERR(mm)) {
		ret = PTR_ERR(mm);
		goto put_task;
	}
	if (!mm) {
		ret = -ESRCH;
		goto put_task;
	}

	ret = nbdriver_read_mm(mm, request, bytes, true);
	mmput(mm);
put_task:
	put_task_struct(task);
	return ret;
}

static int nbdriver_write_target(struct nbdriver_write_request *request,
				 const u8 *bytes)
{
	struct pid *pid;
	struct task_struct *task;
	struct mm_struct *mm;
	unsigned long address = untagged_addr((unsigned long)request->address);
	u64 start_ns;
	int ret = 0;

	pid = find_get_pid(request->pid);
	if (!pid)
		return -ESRCH;
	task = get_pid_task(pid, PIDTYPE_TGID);
	put_pid(pid);
	if (!task)
		return -ESRCH;
	start_ns = READ_ONCE(task->start_boottime);
	if (request->expected_start_ns && request->expected_start_ns != start_ns) {
		ret = -ESTALE;
		goto put_task;
	}
	request->expected_start_ns = start_ns;
	mm = nbdriver_access_mm_mode(task, true);
	if (IS_ERR(mm)) {
		ret = PTR_ERR(mm);
		goto put_task;
	}
	if (!mm) {
		ret = -ESRCH;
		goto put_task;
	}

	mmap_read_lock(mm);
	while (request->copied < request->size) {
		struct vm_area_struct *vma;
		struct page *page;
		unsigned long at = address + request->copied;
		size_t length;
		void *mapped;

		vma = find_vma(mm, at);
		if (!vma || at < vma->vm_start) {
			ret = -EFAULT;
			break;
		}
		if (!(vma->vm_flags & VM_WRITE)) {
			ret = -EACCES;
			break;
		}
		if ((vma->vm_flags & VM_EXEC) && !nbdriver_copy_to_user_page) {
			ret = -ENOSYS;
			break;
		}
		if (vma->vm_flags & (VM_IO | VM_PFNMAP | VM_MIXEDMAP | VM_HUGETLB)) {
			ret = -EOPNOTSUPP;
			break;
		}
		length = min_t(size_t, request->size - request->copied,
				   PAGE_SIZE - offset_in_page(at));
		length = min_t(size_t, length, vma->vm_end - at);
		ret = nbdriver_walk_get_page(vma, at, true, false, 0, &page);
		if (ret)
			break;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
		mapped = kmap_local_page(page);
#else
		mapped = kmap_atomic(page);
#endif
		if (vma->vm_flags & VM_EXEC)
			nbdriver_copy_executable(vma, page, at,
				(u8 *)mapped + offset_in_page(at),
				bytes + request->copied, length);
		else
			memcpy((u8 *)mapped + offset_in_page(at),
			       bytes + request->copied, length);
		flush_dcache_page(page);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
		kunmap_local(mapped);
#else
		kunmap_atomic(mapped);
#endif
		set_page_dirty_lock(page);
		nbdriver_walk_put_page(page);
		request->copied += length;
	}
	mmap_read_unlock(mm);
	mmput(mm);
put_task:
	put_task_struct(task);
	return ret;
}

long nbdriver_read_ioctl(unsigned long argument)
{
	struct nbdriver_read_request request;
	void __user *destination;
	u8 *bytes;
	u32 available;
	unsigned long not_copied;
	int ret;

	if (!capable(CAP_SYS_PTRACE))
		return -EPERM;
	if (copy_from_user(&request, (void __user *)argument, sizeof(request)))
		return -EFAULT;
	if (request.pid <= 0 || !request.size || request.size > NBDRIVER_READ_MAX ||
	    !request.buffer || request.copied || request.error || request.failed_address)
		return -EINVAL;
	if (!request.address ||
	    request.size > ULONG_MAX - (unsigned long)request.address)
		return -EINVAL;
	destination = u64_to_user_ptr(request.buffer);
	if (!access_ok(destination, request.size))
		return -EFAULT;
	bytes = kvmalloc(request.size, GFP_KERNEL);
	if (!bytes)
		return -ENOMEM;

	ret = nbdriver_read_target(&request, bytes);
	available = request.copied;
	if (available) {
		not_copied = copy_to_user(destination, bytes, available);
		if (not_copied) {
			request.copied = available - not_copied;
			ret = -EFAULT;
		}
	}
	request.error = ret;
	request.failed_address = ret ? request.address + request.copied : 0;
	kvfree(bytes);
	if (copy_to_user((void __user *)argument, &request, sizeof(request)))
		return -EFAULT;
	return ret;
}

long nbdriver_write_ioctl(unsigned long argument)
{
	struct nbdriver_write_request request;
	void __user *source;
	u8 *bytes;
	unsigned long not_copied;
	int ret;

	if (!capable(CAP_SYS_PTRACE))
		return -EPERM;
	if (copy_from_user(&request, (void __user *)argument, sizeof(request)))
		return -EFAULT;
	if (request.pid <= 0 || !request.size || request.size > NBDRIVER_WRITE_MAX ||
	    !request.buffer || request.copied || request.error || request.failed_address)
		return -EINVAL;
	if (!request.address ||
	    request.size > ULONG_MAX - (unsigned long)request.address)
		return -EINVAL;
	source = u64_to_user_ptr(request.buffer);
	if (!access_ok(source, request.size))
		return -EFAULT;
	bytes = kvmalloc(request.size, GFP_KERNEL);
	if (!bytes)
		return -ENOMEM;
	not_copied = copy_from_user(bytes, source, request.size);
	if (not_copied) {
		kvfree(bytes);
		return -EFAULT;
	}

	ret = nbdriver_write_target(&request, bytes);
	request.error = ret;
	request.failed_address = ret ? request.address + request.copied : 0;
	kvfree(bytes);
	if (copy_to_user((void __user *)argument, &request, sizeof(request)))
		return -EFAULT;
	return ret;
}

static int nbdriver_readv_entry_valid(const struct nbdriver_readv_entry *entry)
{
	if (entry->reserved || !entry->size || entry->size > NBDRIVER_READV_ITEM_MAX ||
	    !entry->address || entry->size > ULONG_MAX - entry->address ||
	    !entry->buffer || entry->size > ULONG_MAX - entry->buffer)
		return -EINVAL;
	if (!access_ok(u64_to_user_ptr(entry->buffer), entry->size))
		return -EFAULT;
	return 0;
}

long nbdriver_readv_ioctl(unsigned long argument)
{
	struct nbdriver_readv_request req;
	struct nbdriver_readv_entry *entries;
	struct task_struct *task = NULL;
	struct mm_struct *mm = NULL;
	struct pid *id;
	u64 total = 0;
	u8 *bytes = NULL;
	size_t array_size;
	u32 i;
	int ret = 0;

	if (!capable(CAP_SYS_PTRACE))
		return -EPERM;
	if (copy_from_user(&req, (void __user *)argument, sizeof(req)))
		return -EFAULT;
	if (req.pid <= 0 || !req.entries || !req.count || req.count > NBDRIVER_READV_MAX)
		return -EINVAL;
	array_size = req.count * sizeof(*entries);
	entries = kvmalloc(array_size, GFP_KERNEL);
	if (!entries)
		return -ENOMEM;
	if (copy_from_user(entries, u64_to_user_ptr(req.entries), array_size)) {
		ret = -EFAULT;
		goto free_entries;
	}
	req.completed = 0;
	req.error = 0;
	for (i = 0; i < req.count; ++i) {
		entries[i].copied = 0;
		entries[i].error = nbdriver_readv_entry_valid(&entries[i]);
		entries[i].failed_address = entries[i].error ? entries[i].address : 0;
		if (!entries[i].error)
			total += entries[i].size;
	}
	if (total > NBDRIVER_READV_TOTAL_MAX) {
		ret = -E2BIG;
		goto report;
	}
	bytes = kvmalloc(NBDRIVER_READ_MAX, GFP_KERNEL);
	if (!bytes) {
		ret = -ENOMEM;
		goto report;
	}
	id = find_get_pid(req.pid);
	if (id) {
		task = get_pid_task(id, PIDTYPE_TGID);
		put_pid(id);
	}
	if (!task) {
		ret = -ESRCH;
		goto report;
	}
	if (req.expected_start_ns &&
	    req.expected_start_ns != READ_ONCE(task->start_boottime)) {
		ret = -ESTALE;
		goto report;
	}
	req.expected_start_ns = READ_ONCE(task->start_boottime);
	mm = nbdriver_access_mm(task);
	if (IS_ERR_OR_NULL(mm)) {
		ret = mm ? PTR_ERR(mm) : -ESRCH;
		mm = NULL;
		goto report;
	}
	/* One task/mm reference for the entire batch: no numeric-PID re-resolution. */
	for (i = 0; i < req.count; ++i) {
		struct nbdriver_readv_entry *entry = &entries[i];

		if (entry->error)
			continue;
		while (entry->copied < entry->size) {
			struct nbdriver_read_request part = {};
			unsigned long not_copied;
			int error;

			part.address = entry->address + entry->copied;
			part.size = min_t(u32, entry->size - entry->copied, NBDRIVER_READ_MAX);
			error = nbdriver_read_mm(mm, &part, bytes, true);
			not_copied = copy_to_user(
				u64_to_user_ptr(entry->buffer + entry->copied), bytes, part.copied);
			entry->copied += part.copied - not_copied;
			if (not_copied)
				error = -EFAULT;
			if (error) {
				entry->error = error;
				entry->failed_address = entry->address + entry->copied;
				break;
			}
		}
		if (!entry->error)
			++req.completed;
	}
 report:
	if (ret) {
		req.error = ret;
		for (i = 0; i < req.count; ++i) {
			entries[i].error = ret;
			entries[i].failed_address = entries[i].address + entries[i].copied;
		}
	}
	if (copy_to_user(u64_to_user_ptr(req.entries), entries, array_size))
		ret = -EFAULT;
	if (copy_to_user((void __user *)argument, &req, sizeof(req)))
		ret = -EFAULT;
	if (mm)
		mmput(mm);
	if (task)
		put_task_struct(task);
	kvfree(bytes);
 free_entries:
	kvfree(entries);
	/* Successful batch dispatch returns 0; callers inspect each entry.error. */
	return ret;
}
/* ===== END src/core/nbdriver_read.c ===== */

/* ===== BEGIN src/core/nbdriver_query.c ===== */
// SPDX-License-Identifier: GPL-2.0-only
/* Process/module selection and read-only thread queries adapted from the
 * workspace NBG-driver process.c, query.c and readonly.c. Preserve its
 * selection semantics while using nbdriver's ABI and access checks. */
#include <linux/capability.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/path.h>
#include <linux/pid.h>
#include <linux/ptrace.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/smp.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <asm/sysreg.h>

#include "nbdriver_abi.h"
#include "nbdriver_cfi.h"
#include "nbdriver_ksym.h"
#include "nbdriver_query.h"
#include "nbdriver_query_rules.h"
#include "nbdriver_read.h"

#define NBDRIVER_FIND_MAX_TASKS 512U
static __typeof__(&ptrace_may_access) query_access;
static __typeof__(&task_call_func) query_task_call;
static __typeof__(&__lock_task_sighand) query_lock_sighand;

int nbdriver_query_init(void)
{
	query_access = (__typeof__(query_access))
		nbdriver_ksym_lookup("ptrace_may_access");
	query_task_call = (__typeof__(query_task_call))
		nbdriver_ksym_lookup("task_call_func");
	query_lock_sighand = (__typeof__(query_lock_sighand))
		nbdriver_ksym_lookup_optional("__lock_task_sighand");
	return query_access ? 0 : -ENOENT;
}

void nbdriver_query_exit(void)
{
	query_lock_sighand = NULL;
	query_task_call = NULL;
	query_access = NULL;
}

u32 nbdriver_query_features(void)
{
	return NBDRIVER_FEATURE_FIND_PROCESS | NBDRIVER_FEATURE_MODULE_BASE |
		NBDRIVER_FEATURE_PROCESS_ID | NBDRIVER_FEATURE_THREAD_ID |
		(query_lock_sighand ? NBDRIVER_FEATURE_THREAD_SIGNAL : 0) |
		(query_task_call ? NBDRIVER_FEATURE_THREAD_ENV |
		 NBDRIVER_FEATURE_THREAD_FP : 0);
}

static noinline bool NBDRIVER_NO_CFI query_may_access(struct task_struct *task)
{
	return query_access(task, PTRACE_MODE_READ_FSCREDS);
}

/* Hold exec_update_lock through permission checking and the metadata sample. */
static int query_lock_task(struct task_struct *task)
{
	int ret = down_read_killable(&task->signal->exec_update_lock);

	if (ret)
		return ret;
	if (!pid_alive(task) || (READ_ONCE(task->flags) & PF_EXITING))
		ret = -ESRCH;
	else if (!query_may_access(task))
		ret = -EACCES;
	if (ret)
		up_read(&task->signal->exec_update_lock);
	return ret;
}

static struct task_struct *query_task(pid_t pid, pid_t tid,
				    u64 expected, u64 expected_thread)
{
	struct task_struct *task;
	struct pid *id;
	int ret;

	if (pid <= 0 || tid <= 0)
		return ERR_PTR(-EINVAL);
	id = find_get_pid(tid);
	if (!id)
		return ERR_PTR(-ESRCH);
	task = get_pid_task(id, PIDTYPE_PID);
	put_pid(id);
	if (!task)
		return ERR_PTR(-ESRCH);
	ret = query_lock_task(task);
	if (ret)
		goto put_task;
	if (task_tgid_vnr(task) != pid)
		ret = -ESRCH;
	else if ((expected && expected != READ_ONCE(task->group_leader->start_boottime)) ||
		 (expected_thread && expected_thread != READ_ONCE(task->start_boottime)))
		ret = -ESTALE;
	if (!ret)
		return task;
	up_read(&task->signal->exec_update_lock);
put_task:
	put_task_struct(task);
	return ERR_PTR(ret);
}

static void query_put_task(struct task_struct *task)
{
	up_read(&task->signal->exec_update_lock);
	put_task_struct(task);
}

struct task_struct *nbdriver_query_get_process(pid_t pid, u64 expected_start_ns)
{
	return query_task(pid, pid, expected_start_ns, 0);
}

void nbdriver_query_put_process(struct task_struct *task)
{
	query_put_task(task);
}

static bool query_name_valid(const char *name, size_t capacity)
{
	return name[0] && memchr(name, '\0', capacity);
}

/* Read argv[0] from the pinned mm without faulting target pages in. */
static int query_argv0(struct mm_struct *mm, char *name, size_t capacity)
{
	struct nbdriver_read_request read = {};
	unsigned long start = READ_ONCE(mm->arg_start);
	unsigned long end = READ_ONCE(mm->arg_end);
	int ret;

	if (end <= start || !start)
		return -ENOENT;
	read.address = start;
	read.size = min_t(unsigned long, end - start, capacity);
	ret = nbdriver_read_mm(mm, &read, (u8 *)name, false);
	/* A later argv page may be absent even though argv[0] already ended. */
	if (memchr(name, '\0', read.copied))
		return 0;
	return ret ? ret : -ENAMETOOLONG;
}

static long query_find_process(unsigned long argument)
{
	struct nbdriver_pid_request req;
	struct task_struct **candidates, *task;
	char *argv0;
	unsigned int count = 0, i;
	int best_threads = -1, ret = -ESRCH;
	long best_rss = -1;
	bool truncated = false;

	if (copy_from_user(&req, (void __user *)argument, sizeof(req)))
		return -EFAULT;
	if (req.reserved || !query_name_valid(req.name, sizeof(req.name)))
		return -EINVAL;
	req.pid = 0;
	req.start_ns = 0;
	candidates = kvmalloc_array(NBDRIVER_FIND_MAX_TASKS, sizeof(*candidates), GFP_KERNEL);
	argv0 = kmalloc(NBDRIVER_QUERY_NAME_MAX, GFP_KERNEL);
	if (!candidates || !argv0) {
		ret = -ENOMEM;
		goto free_buffers;
	}
	rcu_read_lock();
	for_each_process(task) {
		if ((task->flags & PF_KTHREAD) || !task_tgid_vnr(task))
			continue;
		if (count == NBDRIVER_FIND_MAX_TASKS) {
			truncated = true;
			break;
		}
		get_task_struct(task);
		candidates[count++] = task;
	}
	rcu_read_unlock();
	if (truncated) {
		ret = -E2BIG; /* Do not claim a globally ranked result from a truncated scan. */
		goto put_candidates;
	}
	for (i = 0; i < count; ++i) {
		struct mm_struct *mm;
		int threads;
		long rss;
		pid_t pid;

		task = candidates[i];
		if (query_lock_task(task))
			continue;
		mm = get_task_mm(task);
		if (!mm)
			goto unlock;
		if (!query_argv0(mm, argv0, NBDRIVER_QUERY_NAME_MAX) &&
		    !strcmp(argv0, req.name)) {
			threads = get_nr_threads(task);
			rss = get_mm_rss(mm);
			pid = task_tgid_vnr(task);
			if (nbdriver_pid_better(threads, rss, pid, best_threads, best_rss, req.pid)) {
				req.pid = pid;
				req.start_ns = READ_ONCE(task->start_boottime);
				best_threads = threads;
				best_rss = rss;
				ret = 0;
			}
		}
		mmput(mm);
 unlock:
		up_read(&task->signal->exec_update_lock);
	}
 put_candidates:
	for (i = 0; i < count; ++i)
		put_task_struct(candidates[i]);
 free_buffers:
	kvfree(candidates);
	kfree(argv0);
	if (copy_to_user((void __user *)argument, &req, sizeof(req)))
		return -EFAULT;
	return ret;
}

static long query_module(unsigned long argument)
{
	struct nbdriver_module_request req;
	struct task_struct *task;
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	char *path;
	int ret = -ENOENT;

	if (copy_from_user(&req, (void __user *)argument, sizeof(req)))
		return -EFAULT;
	if (req.reserved || !query_name_valid(req.name, sizeof(req.name)))
		return -EINVAL;
	req.base = 0;
	task = query_task(req.pid, req.pid, req.expected_start_ns, 0);
	if (IS_ERR(task))
		return PTR_ERR(task);
	req.expected_start_ns = READ_ONCE(task->start_boottime);
	mm = get_task_mm(task);
	if (!mm) {
		ret = -ESRCH;
		goto put_task;
	}
	path = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!path) {
		ret = -ENOMEM;
		goto put_mm;
	}
	mmap_read_lock(mm);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
	{
		VMA_ITERATOR(iterator, mm, 0);
		for_each_vma(iterator, vma) {
#else
		for (vma = mm->mmap; vma; vma = vma->vm_next) {
#endif
			char *name;

			if (!vma->vm_file || vma->vm_pgoff)
				continue;
			name = d_path(&vma->vm_file->f_path, path, PATH_MAX);
			if (IS_ERR(name)) {
				ret = PTR_ERR(name);
				break;
			}
			if (nbdriver_module_name_matches(name, req.name)) {
				req.base = vma->vm_start;
				ret = 0;
				break;
			}
		}
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
	}
#endif
	mmap_read_unlock(mm);
	kfree(path);
 put_mm:
	mmput(mm);
 put_task:
	query_put_task(task);
	if (copy_to_user((void __user *)argument, &req, sizeof(req)))
		return -EFAULT;
	return ret;
}

static long query_identity(unsigned long argument)
{
	struct nbdriver_process_identity req;
	struct task_struct *task;

	if (copy_from_user(&req, (void __user *)argument, sizeof(req)))
		return -EFAULT;
	if (req.reserved)
		return -EINVAL;
	task = query_task(req.pid, req.pid, req.start_ns, 0);
	if (IS_ERR(task))
		return PTR_ERR(task);
	req.start_ns = READ_ONCE(task->start_boottime);
	query_put_task(task);
	return copy_to_user((void __user *)argument, &req, sizeof(req)) ? -EFAULT : 0;
}

static long query_thread(unsigned long argument)
{
	struct nbdriver_thread_request req;
	struct task_struct *leader, *thread;
	u64 best_exec = 0;
	bool best_cpu = false;

	if (copy_from_user(&req, (void __user *)argument, sizeof(req)))
		return -EFAULT;
	if (!query_name_valid(req.comm, sizeof(req.comm)))
		return -EINVAL;
	leader = query_task(req.pid, req.pid, req.expected_start_ns, 0);
	if (IS_ERR(leader))
		return PTR_ERR(leader);
	req.tid = 0;
	req.thread_start_ns = 0;
	req.expected_start_ns = READ_ONCE(leader->start_boottime);
	rcu_read_lock();
	for_each_thread(leader, thread) {
		bool on_cpu = false;
		u64 exec_ns;
		pid_t tid;

		if (thread == leader || !pid_alive(thread) ||
		    (READ_ONCE(thread->flags) & PF_EXITING) ||
		    strncmp(thread->comm, req.comm, TASK_COMM_LEN))
			continue;
#ifdef CONFIG_SMP
		on_cpu = READ_ONCE(thread->on_cpu) != 0;
#endif
		exec_ns = READ_ONCE(thread->se.sum_exec_runtime);
		tid = task_pid_vnr(thread);
		if (nbdriver_thread_better(on_cpu, exec_ns, tid, best_cpu, best_exec, req.tid)) {
			req.tid = tid;
			req.thread_start_ns = READ_ONCE(thread->start_boottime);
			best_cpu = on_cpu;
			best_exec = exec_ns;
		}
	}
	rcu_read_unlock();
	query_put_task(leader);
	if (copy_to_user((void __user *)argument, &req, sizeof(req)))
		return -EFAULT;
	return req.tid ? 0 : -ESRCH;
}

/* Saved state is sampled under scheduling locks only when off-CPU. Running
 * targets are sampled on their own CPU, with identity checked in the IPI. */
static int query_environment_sample(struct task_struct *task, void *output)
{
	struct nbdriver_thread_environment *req = output;

#ifdef CONFIG_SMP
	if (task != current && READ_ONCE(task->on_cpu))
		return -EAGAIN;
#endif
	if (!pid_alive(task) || (READ_ONCE(task->flags) & PF_EXITING))
		return -ESRCH;
	req->tls = task == current ? read_sysreg(tpidr_el0) :
		READ_ONCE(*task_user_tls(task));
	req->valid = NBDRIVER_THREAD_TLS_VALID; /* A zero TLS is still a sample. */
#ifdef CONFIG_ARM64_PTR_AUTH
	if (system_supports_generic_auth()) {
		req->apga_lo = READ_ONCE(task->thread.keys_user.apga.lo);
		req->apga_hi = READ_ONCE(task->thread.keys_user.apga.hi);
		/* Reject a sample taken midway through PR_PAC_RESET_KEYS, before
		 * the newly generated software key has been fully installed. */
		if (task == current &&
		    (req->apga_lo != read_sysreg_s(SYS_APGAKEYLO_EL1) ||
		     req->apga_hi != read_sysreg_s(SYS_APGAKEYHI_EL1))) {
			req->valid = 0;
			return -EAGAIN;
		}
		req->valid |= NBDRIVER_THREAD_APGA_VALID;
	}
#endif
	return 0;
}

struct query_environment_ipi_sample {
	struct task_struct *task;
	struct nbdriver_thread_environment *req;
	int result;
};

static void query_environment_ipi(void *argument)
{
	struct query_environment_ipi_sample *sample = argument;

	/* Migration or a context switch makes this a retry, never a sample
	 * of whichever unrelated task happens to occupy this CPU. */
	sample->result = current == sample->task ?
		query_environment_sample(current, sample->req) : -EAGAIN;
}

static noinline int NBDRIVER_NO_CFI
query_environment_call(struct task_struct *task,
		       struct nbdriver_thread_environment *req)
{
	unsigned int attempt;
	int ret;

	if (task == current) {
		preempt_disable();
		ret = query_environment_sample(task, req);
		preempt_enable();
		return ret;
	}
	if (!query_task_call)
		return -EOPNOTSUPP;
	for (attempt = 0; attempt < 3; ++attempt) {
		struct query_environment_ipi_sample sample = {task, req, -EAGAIN};

		ret = query_task_call(task, query_environment_sample, req);
		if (ret != -EAGAIN)
			return ret;
		/* task_call_func has released all scheduling locks before this
		 * synchronous IPI. Holding rq/pi locks here could deadlock. */
		ret = smp_call_function_single(task_cpu(task), query_environment_ipi,
					       &sample, 1);
		if (!ret && sample.result != -EAGAIN)
			return sample.result;
		if (ret && ret != -ENXIO)
			return ret;
	}
	req->valid = 0;
	return -EAGAIN;
}

static long query_environment(unsigned long argument)
{
	struct nbdriver_thread_environment req;
	struct task_struct *task;
	int ret;

	if (copy_from_user(&req, (void __user *)argument, sizeof(req)))
		return -EFAULT;
	if (req.reserved)
		return -EINVAL;
	task = query_task(req.pid, req.tid, req.expected_start_ns,
			  req.expected_thread_start_ns);
	if (IS_ERR(task))
		return PTR_ERR(task);
	req.valid = 0;
	req.tls = req.apga_lo = req.apga_hi = 0;
	req.expected_start_ns = READ_ONCE(task->group_leader->start_boottime);
	req.expected_thread_start_ns = READ_ONCE(task->start_boottime);
	ret = query_environment_call(task, &req);
	query_put_task(task);
	if (copy_to_user((void __user *)argument, &req, sizeof(req)))
		return -EFAULT;
	return ret;
}

/* task_call_func holds the target's scheduling locks. No lazy live-FP sample. */
static int query_fp_sample(struct task_struct *task, void *output)
{
	struct nbdriver_thread_fpstate *req = output;

	if (task == current)
		return -EAGAIN;
#ifdef CONFIG_SMP
	if (READ_ONCE(task->on_cpu))
		return -EAGAIN;
#endif
	if (!pid_alive(task) || (READ_ONCE(task->flags) & PF_EXITING))
		return -ESRCH;
	req->tls = READ_ONCE(*task_user_tls(task));
	req->fpsr = READ_ONCE(task->thread.uw.fpsimd_state.fpsr);
	req->fpcr = READ_ONCE(task->thread.uw.fpsimd_state.fpcr);
	req->sample_ns = ktime_get_ns();
	req->valid = NBDRIVER_THREAD_FP_VALID;
	return 0;
}

static noinline int NBDRIVER_NO_CFI query_fp_call(struct task_struct *task,
					       struct nbdriver_thread_fpstate *req)
{
	return query_task_call(task, query_fp_sample, req);
}

static long query_fpstate(unsigned long argument)
{
	struct nbdriver_thread_fpstate req;
	struct task_struct *task;
	int ret;

	if (copy_from_user(&req, (void __user *)argument, sizeof(req)))
		return -EFAULT;
	if (req.reserved)
		return -EINVAL;
	if (!query_task_call)
		return -EOPNOTSUPP;
	task = query_task(req.pid, req.tid, req.expected_start_ns,
			  req.expected_thread_start_ns);
	if (IS_ERR(task))
		return PTR_ERR(task);
	req.valid = 0;
	req.tls = req.sample_ns = 0;
	req.fpsr = req.fpcr = 0;
	req.expected_start_ns = READ_ONCE(task->group_leader->start_boottime);
	req.expected_thread_start_ns = READ_ONCE(task->start_boottime);
	ret = query_fp_call(task, &req);
	query_put_task(task);
	if (ret)
		return ret;
	return copy_to_user((void __user *)argument, &req, sizeof(req)) ? -EFAULT : 0;
}

/* Dynamic helper retains the kernel's RCU/exit-safe sighand lock protocol. */
static noinline struct sighand_struct *NBDRIVER_NO_CFI
query_signal_lock(struct task_struct *task, unsigned long *flags)
{
	return query_lock_sighand(task, flags);
}

static long query_signal(unsigned long argument)
{
	struct nbdriver_thread_signal req;
	struct task_struct *task;
	struct sighand_struct *sighand;
	struct sigaction *action;
	unsigned long irq_flags;
	int ret = 0;

	BUILD_BUG_ON(sizeof(sigset_t) != sizeof(u64));
	if (copy_from_user(&req, (void __user *)argument, sizeof(req)))
		return -EFAULT;
	req.handler = req.flags = req.restorer = req.action_mask = 0;
	req.blocked_mask = req.sample_ns = 0;
	if (req.reserved || !req.signo || req.signo > _NSIG) {
		ret = -EINVAL;
		goto reply;
	}
	if (!query_lock_sighand) {
		ret = -EOPNOTSUPP;
		goto reply;
	}
	task = query_task(req.pid, req.tid, req.expected_start_ns,
			  req.expected_thread_start_ns);
	if (IS_ERR(task)) {
		ret = PTR_ERR(task);
		goto reply;
	}
	sighand = query_signal_lock(task, &irq_flags);
	if (!sighand) {
		ret = -ESRCH;
		goto put_task;
	}
	action = &sighand->action[req.signo - 1].sa;
	req.expected_start_ns = READ_ONCE(task->group_leader->start_boottime);
	req.expected_thread_start_ns = READ_ONCE(task->start_boottime);
	req.handler = (unsigned long)action->sa_handler;
	req.flags = action->sa_flags & UAPI_SA_FLAGS;
#ifdef __ARCH_HAS_SA_RESTORER
	req.restorer = (unsigned long)action->sa_restorer;
#endif
	memcpy(&req.action_mask, &action->sa_mask, sizeof(req.action_mask));
	memcpy(&req.blocked_mask, &task->blocked, sizeof(req.blocked_mask));
	req.sample_ns = ktime_get_ns();
	spin_unlock_irqrestore(&sighand->siglock, irq_flags);
put_task:
	query_put_task(task);
reply:
	/* User copies are outside the IRQ-disabled region. All errors clear data. */
	return copy_to_user((void __user *)argument, &req, sizeof(req)) ? -EFAULT : ret;
}

long nbdriver_query_ioctl(unsigned int command, unsigned long argument)
{
	if (!capable(CAP_SYS_PTRACE))
		return -EPERM;
	switch (command) {
	case NBDRIVER_IOC_FIND_PROCESS: return query_find_process(argument);
	case NBDRIVER_IOC_MODULE_BASE: return query_module(argument);
	case NBDRIVER_IOC_PROCESS_ID: return query_identity(argument);
	case NBDRIVER_IOC_THREAD_ID: return query_thread(argument);
	case NBDRIVER_IOC_THREAD_ENV: return query_environment(argument);
	case NBDRIVER_IOC_THREAD_SIGNAL: return query_signal(argument);
	case NBDRIVER_IOC_THREAD_FP: return query_fpstate(argument);
	default: return -ENOTTY;
	}
}
/* ===== END src/core/nbdriver_query.c ===== */

/* ===== BEGIN src/core/nbdriver_walk.c ===== */
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
			     bool write, bool allow_zero_page, unsigned long zero_pfn,
			     struct page **result, bool mapped_ram, bool *uncached)
{
	struct page *page, *head;
	unsigned long attr = pte_val(entry) & PTE_ATTRINDX_MASK;
	bool zero_page = !write && allow_zero_page && !block && pfn == zero_pfn;

	if (!pte_valid(entry))
		return -ENODATA;
	if (!(pte_val(entry) & PTE_USER))
		return -EACCES;
	if (write && !pte_write(entry))
		return -EACCES;
	if (attr != PTE_ATTRINDX(MT_NORMAL) &&
	    !(mapped_ram && !write && attr == PTE_ATTRINDX(MT_NORMAL_NC))
#ifdef MT_NORMAL_TAGGED
	    && attr != PTE_ATTRINDX(MT_NORMAL_TAGGED)
#endif
	    )
		return -EOPNOTSUPP;
	if (nbdriver_pte_devmap(entry))
		return -EOPNOTSUPP;
	if (!block && pte_special(entry) && !zero_page && !(mapped_ram && !write))
		return -EOPNOTSUPP;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
	if (!pfn_is_map_memory(pfn))
		return -EFAULT;
#else
	if (!pfn_valid(pfn))
		return -EFAULT;
#endif

	page = pfn_to_page(pfn);
	head = compound_head(page);
	if ((PageReserved(head) && !zero_page) || PageSlab(head))
		return -EOPNOTSUPP;
	/* The leaf lock keeps the mapping's page reference alive during get_page. */
	if (!nbdriver_page_ref_get(page))
		return -EAGAIN;
	if (uncached)
		*uncached = attr == PTE_ATTRINDX(MT_NORMAL_NC);
	*result = page;
	return 0;
}

static int nbdriver_walk_get_page_inner(struct vm_area_struct *vma, unsigned long address,
			   bool write, bool allow_zero_page, unsigned long zero_pfn,
			   struct page **result, bool mapped_ram, bool *uncached)
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
		ret = nbdriver_leaf_get(__pte(pud_val(pud)), pfn, true, write,
						allow_zero_page, zero_pfn, result, mapped_ram, uncached);
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
			ret = nbdriver_leaf_get(__pte(pmd_val(pmd)), pfn, true, write,
						allow_zero_page, zero_pfn, result, mapped_ram, uncached);
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
			ret = nbdriver_leaf_get(pte, pte_pfn(pte), false, write,
					allow_zero_page, zero_pfn, result, mapped_ram, uncached);
		pte_unmap_unlock(ptep, ptl);
		return ret;
	}
	return -EAGAIN;
}
int nbdriver_walk_get_page(struct vm_area_struct *vma, unsigned long address,
			   bool write, bool allow_zero_page, unsigned long zero_pfn,
			   struct page **result)
{
	return nbdriver_walk_get_page_inner(vma, address, write, allow_zero_page,
					   zero_pfn, result, false, NULL);
}

/* Caller has already checked VMA access. Only resident, normal CPU RAM with
 * a live non-reserved/non-slab page is accepted, pinned under the leaf lock. */
int nbdriver_walk_get_ram_page(struct vm_area_struct *vma, unsigned long address,
			       struct page **result, bool *uncached)
{
	*uncached = false;
	return nbdriver_walk_get_page_inner(vma, address, false, false, 0,
					   result, true, uncached);
}
/* ===== END src/core/nbdriver_walk.c ===== */
