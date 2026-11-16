/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NBDRIVER_READ_H
#define NBDRIVER_READ_H

#include <linux/cred.h>
#include <linux/sched.h>
#include <linux/types.h>

struct task_struct;
struct mm_struct;
struct nbdriver_read_request;

/* 连接 worker 允许 root / Android system。此 helper 只检查 euid，
 * 不是 SF 可执行文件认证；读取分支与查询分支使用该例外。
 * 写入、devpts 状态和 SF_GROUPS 各有独立门禁，不能据此一并放行。 */
static inline bool nbdriver_sf_read_caller(void)
{
	return uid_eq(current_euid(), GLOBAL_ROOT_UID) ||
		uid_eq(current_euid(), KUIDT_INIT(1000));
}

/* 返回 mm_users 引用、NULL 或 ERR_PTR，调用方负责 mmput。
 * root/system 读取走 get_task_mm，其余路径使用对应 ptrace/LSM 模式。 */
struct mm_struct *nbdriver_access_mm(struct task_struct *task);
/* Caller owns an mm_users reference; false never faults missing pages in. */
int nbdriver_read_mm(struct mm_struct *mm, struct nbdriver_read_request *request,
		     u8 *bytes, bool fault_in);
/* For legacy userspace VMA names: caller holds mmap_lock, never faults pages. */
int nbdriver_read_mm_locked(struct mm_struct *mm,
		struct nbdriver_read_request *request, u8 *bytes);

int nbdriver_read_init(void);
void nbdriver_read_exit(void);
long nbdriver_read_ioctl(unsigned long argument);
long nbdriver_write_ioctl(unsigned long argument);
long nbdriver_readv_ioctl(unsigned long argument);
bool nbdriver_read_fault_in_ready(void);

#endif /* NBDRIVER_READ_H */
