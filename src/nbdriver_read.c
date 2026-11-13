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

#include <asm/memory.h>

#include "nbdriver_abi.h"
#include "nbdriver_cfi.h"
#include "nbdriver_ksym.h"
#include "nbdriver_log.h"
#include "nbdriver_read.h"
#include "nbdriver_walk.h"

static __typeof__(&mm_access) nbdriver_mm_access;
typedef void (*nbdriver_icache_flush_fn_t)(unsigned long,
						    unsigned long);
static nbdriver_icache_flush_fn_t nbdriver_icache_flush;

static noinline struct mm_struct *NBDRIVER_NO_CFI
nbdriver_access_mm(struct task_struct *task)
{
	return nbdriver_mm_access(task, PTRACE_MODE_READ_FSCREDS);
}

static noinline void NBDRIVER_NO_CFI
nbdriver_flush_icache(unsigned long start, unsigned long end)
{
	nbdriver_icache_flush_fn_t fn = READ_ONCE(nbdriver_icache_flush);

	if (fn)
		fn(start, end);
}

int nbdriver_read_init(void)
{
	nbdriver_mm_access = (__typeof__(nbdriver_mm_access))
		nbdriver_ksym_lookup("mm_access");
	nbdriver_icache_flush =
		(nbdriver_icache_flush_fn_t)
		nbdriver_ksym_lookup("caches_clean_inval_pou");
	if (!nbdriver_icache_flush)
		nbdriver_icache_flush =
			(nbdriver_icache_flush_fn_t)
			nbdriver_ksym_lookup("flush_icache_range");
	if (!nbdriver_icache_flush)
		nbdriver_icache_flush =
			(nbdriver_icache_flush_fn_t)
			nbdriver_ksym_lookup("__flush_icache_range");
	if (!nbdriver_mm_access) {
		nbd_err("stage=read_init symbol=mm_access error=%d\n", -ENOENT);
		return -ENOENT;
	}
	return 0;
}

void nbdriver_read_exit(void)
{
	nbdriver_icache_flush = NULL;
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
		unsigned long at = address + request->copied;
		size_t length;

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
		ret = nbdriver_walk_get_page(vma, at, &page);
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
		if ((vma->vm_flags & VM_EXEC) && !nbdriver_icache_flush) {
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
		ret = nbdriver_walk_get_page(vma, at, &page);
		if (ret)
			break;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
		mapped = kmap_local_page(page);
#else
		mapped = kmap_atomic(page);
#endif
		memcpy((u8 *)mapped + offset_in_page(at),
		       bytes + request->copied, length);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
		kunmap_local(mapped);
#else
		kunmap_atomic(mapped);
#endif
		flush_dcache_page(page);
		if (vma->vm_flags & VM_EXEC)
			nbdriver_flush_icache(at, at + length);
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
