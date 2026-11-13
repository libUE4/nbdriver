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
#include <linux/uaccess.h>

#include <asm/cacheflush.h>
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

static noinline struct mm_struct *NBDRIVER_NO_CFI
nbdriver_access_mm(struct task_struct *task)
{
	return nbdriver_mm_access(task, PTRACE_MODE_READ_FSCREDS);
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

static int nbdriver_read_target(struct nbdriver_read_request *request, u8 *bytes)
{
	struct pid *pid;
	struct task_struct *task;
	struct mm_struct *mm;
	unsigned long address = (unsigned long)request->address;
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

	mmap_read_lock(mm);
	while (request->copied < request->size) {
		struct vm_area_struct *vma;
		struct page *page;
		unsigned long *zero_pfn = READ_ONCE(nbdriver_zero_pfn);
		unsigned long at = address + request->copied;
		size_t length;
		bool fault_attempted = false;

retry_vma:
		vma = find_vma(mm, at);
		if (!vma || at < vma->vm_start) {
			ret = -EFAULT;
			break;
		}
		if (!(vma->vm_flags & VM_READ)) {
			ret = -EACCES;
			break;
		}
		if (vma->vm_flags & (VM_IO | VM_PFNMAP | VM_MIXEDMAP | VM_HUGETLB)) {
			ret = -EOPNOTSUPP;
			break;
		}
		length = min_t(size_t, request->size - request->copied,
				   PAGE_SIZE - offset_in_page(at));
		length = min_t(size_t, length, vma->vm_end - at);
		ret = nbdriver_walk_get_page(vma, at, false, zero_pfn != NULL,
					     zero_pfn ? READ_ONCE(*zero_pfn) : 0, &page);
		if (ret == -ENODATA && !fault_attempted) {
			fault_attempted = true;
			ret = nbdriver_fault_in_page(mm, at);
			if (!ret)
				goto retry_vma;
		}
		if (ret)
			break;
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
		nbdriver_walk_put_page(page);
		request->copied += length;
	}
	mmap_read_unlock(mm);
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
	unsigned long address = (unsigned long)request->address;
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
	bytes = kmalloc(request.size, GFP_KERNEL);
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
	kfree(bytes);
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
	bytes = kmalloc(request.size, GFP_KERNEL);
	if (!bytes)
		return -ENOMEM;
	not_copied = copy_from_user(bytes, source, request.size);
	if (not_copied) {
		kfree(bytes);
		return -EFAULT;
	}

	ret = nbdriver_write_target(&request, bytes);
	request.error = ret;
	request.failed_address = ret ? request.address + request.copied : 0;
	kfree(bytes);
	if (copy_to_user((void __user *)argument, &request, sizeof(request)))
		return -EFAULT;
	return ret;
}
