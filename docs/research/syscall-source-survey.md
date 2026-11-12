# 历史源码调查归档

以下为本轮改动前已存在的调查原文，保留引用、哈希和未完成项。其源码为空、入口尚未决定等状态描述是历史状态，不能代表当前工程。原文件 SHA-256：`3af5cf0bb83fa53a43b067ff1b2e50bcfee4e6f488252e1c4c67eefc4d3a3bce`。本轮没有重新下载核验其中六个上游源码哈希。

---

# ARM64 内核通信入口兼容性调查

## 范围与证据

本轮针对 enkernel CI 声明支持的 Android common ARM64 KMI 基线检查系统调用分发源码：5.10、5.15、6.1、6.6、6.12、6.18。检查对象是各 `kernel/common` 分支的 `arch/arm64/kernel/syscall.c`，不是对所有厂商内核或所有补丁版本的穷举。

分支 URL 使用可变分支名。为固定本轮实际读取的文件内容，记录解码后源码 SHA-256：

| KMI 分支 | 源码 SHA-256 |
|---|---|
| [android12-5.10](https://android.googlesource.com/kernel/common/+/refs/heads/android12-5.10/arch/arm64/kernel/syscall.c) | `cc21a34e3d5e4719031446c5d7808df361adde2d8ba3e059dca3c93d2330d2c4` |
| [android13-5.15](https://android.googlesource.com/kernel/common/+/refs/heads/android13-5.15/arch/arm64/kernel/syscall.c) | `d4a37f1885ff7d5f868de4f0dc96704fff5193694c4413dfb5481d259343ab0a` |
| [android14-6.1](https://android.googlesource.com/kernel/common/+/refs/heads/android14-6.1/arch/arm64/kernel/syscall.c) | `c494fa9aca7af7db680a1adb8ed61067091d3543930d875209389437c6a2a715` |
| [android15-6.6](https://android.googlesource.com/kernel/common/+/refs/heads/android15-6.6/arch/arm64/kernel/syscall.c) | `7910df847f4641949f90d7e677f9dedf431dd053277989459e41fb133efc110a` |
| [android16-6.12](https://android.googlesource.com/kernel/common/+/refs/heads/android16-6.12/arch/arm64/kernel/syscall.c) | `8b157a6e36d59e8b6f2a259d574d431fd27cf75d351d68beff0d96c398504ecc` |
| [android17-6.18](https://android.googlesource.com/kernel/common/+/refs/heads/android17-6.18/arch/arm64/kernel/syscall.c) | `e075c9c2a417b4803a2548958f5a239e5ffeab590d90f57a1db5bdf6ccdf1c0f` |

这些哈希是本轮取回的源码文件校验值，不是 Git commit ID。enkernel 仓库的 `.github/workflows/build-kernel.yml` 本身列出了同一组 Android KMI 基线，可据此确定本轮比较范围。

## 已观察到的差异

| 分支 | ARM64 syscall 调用号 | 兼容 ABI 分支 | 调用栈随机化相关调用 |
|---|---|---|---|
| android12-5.10 | `regs->regs[8]`，分发到 `sys_call_table` | `regs->regs[7]`，分发到 `compat_sys_call_table` | 本文件中未观察到 `add_random_kstack_offset` / `choose_random_kstack_offset` |
| android13-5.15 | 相同 | 相同 | 使用 `add_random_kstack_offset()` 和 `choose_random_kstack_offset(get_random_int() & 0x1FF)` |
| android14-6.1 | 相同 | 相同 | 使用 `get_random_u16()`；相对 5.15 的函数名/表达式已不同 |
| android15-6.6 | 相同 | 相同 | 使用 `get_random_u16()` |
| android16-6.12 | 相同 | 表名为 `__NR_compat32_syscalls`，表仍为 `compat_sys_call_table` | 使用 `get_random_u16()` |
| android17-6.18 | 相同 | 与 6.12 相同 | 使用 `get_random_u16()` |

在这六个文件中，核心路径仍是 `do_el0_svc()` → `el0_svc_common()` → `invoke_syscall()` → `__invoke_syscall()`；`invoke_syscall` 使用系统调用号和传入表执行分发，并设置返回值。6.12 的兼容 syscall 数量宏表明 ABI 相关表述有变化，但不能仅从这一处推断 32 位兼容 ABI 的配置状态或厂商设备支持情况。

### 可得结论

1. 版本号不是调用约定或实现细节的充分证据。同一个 Android KMI 版本可能带有上游未包含的回移；例如本轮 6.1 Android common 文件观察到的是 `get_random_u16()`。
2. ARM64 syscall 入口代码是内核内部实现，不是给外部模块调用的通信 API。对系统调用表或其空槽位的依赖会耦合内部符号、编译选项、CFI/LTO、厂商回移和内核配置；本轮源码阅读没有证明任何目标设备可安全地这样做。
3. “找到空 syscall 槽并替换”不是稳定 ABI；对 `reboot` 探针的依赖也须单独证明符号存在、可探测、调用包装与寄存器布局吻合。上述源码对比不构成该证明。

## 匿名 FD API 的初步检查

对上游 `include/linux/anon_inodes.h` 的 5.10、5.15、6.1、6.6、6.12、6.18 源文件进行了定向检查：六个版本都声明 `anon_inode_getfile`。在 6.1、6.6、6.12、6.18 中还观察到 `anon_inode_getfile_secure`；6.12、6.18 另有 `anon_inode_getfile_fmode`、`anon_inode_create_getfile` 和 `anon_inode_create_getfd` 声明。

该项基于上游 Linux tag，不等同于 Android common GKI 或目标厂商内核验证。即使内核树存在某个声明，模块仍需检查符号导出、模块版本信息和实际配置。没有在本轮对六个 Android 分支逐一确认这些导出属性。

同一批 Android common 分支的头文件还显示：

- `include/linux/file.h` 中 `get_unused_fd_flags(unsigned flags)`、`put_unused_fd(unsigned int fd)`、`fd_install(unsigned int fd, struct file *file)` 声明在六个分支保持一致。
- `include/linux/task_work.h` 中 `task_work_add(task, callback, mode)` 签名保持一致。枚举从 5.10/5.15 的 `TWA_RESUME`、`TWA_SIGNAL`，扩展到 6.1/6.6 的 `TWA_SIGNAL_NO_IPI`，再到 6.12/6.18 的 `TWA_NMI_CURRENT`；当前实现只依赖 `TWA_RESUME`。
- `include/linux/kprobes.h` 中 `register_kprobe(struct kprobe *)` 与 `unregister_kprobe(struct kprobe *)` 声明保持一致，但该声明不能证明配置启用或 GKI KMI 导出。
- `include/linux/miscdevice.h` 中 `misc_register`/`misc_deregister` 声明在六个分支保持一致。

## 通信层决策建议

复用 enkernel 的**分层思路**：稳定 ABI、功能请求分发、会话/文件生命周期分开。不要直接复制其扫描/改写 syscall table 和 reboot kprobe 获取 FD 的机制。这些属于依赖内核内部实现的安装入口，不是 ioctl 数据 ABI 的必需部分。

不改写 syscall table、不挂 reboot kprobe。候选接入方式（设备节点、自定义 socket family、匿名 FD、用户态共享页 + 原子标志位）尚未选定；共享页方案的初步风险是 `get_user_pages_remote` 的导出状态与固定用户 VA 约定。

## 尚未完成的核查

- 六个 Android common 分支的 `anon_inode_getfile`、`get_unused_fd_flags`、`fd_install` 和 kprobe API 的声明与导出状态。
- `reboot` 实现中 `SYSCALL_DEFINE*` 生成的 wrapper 在每个构建配置中的实际符号名；宏展开文本不能证明最终 ELF 符号可用。
- enkernel SDK 的各候选 syscall 编号与目标架构/内核表的对应关系。
- 每个 KMI 的真实 DDK 编译、模块加载和用户态 ioctl 往返。

当前 `nbdriver/src/` 仍为空；本轮没有添加通信代码，也没有宣称完成运行兼容验证。
