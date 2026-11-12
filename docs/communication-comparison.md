# 通信入口比较与选择

## 对比范围

比较对象是取得控制 FD 的入口及后续 ioctl 传输，不比较各项目的内存、触控等业务功能，也不把某个内核版本编译成功等同于全系运行兼容。

| 项目 | 获取 FD | 回调中的处理 | 额外内核依赖 | 主要问题 |
|---|---|---|---|---|
| enkernel | 扫描并改写 `sys_call_table` 的未实现槽位；另注册 reboot kprobe 作为 fallback | reboot kprobe 仅分配 `GFP_ATOMIC` work 并排入 `task_work`；FD 分配和用户拷贝延迟到 task_work | kallsyms resolver、sys_call_table、stop_machine/fixmap 写入路径、kprobe、task_work | 双路径依赖较多非稳定内核内部接口；syscall table 不是模块 ABI。其 kprobe fallback 的调用上下文处理比 LinyuKernel 当前实现稳妥 |
| LinyuKernel | reboot kprobe | 在 kprobe pre-handler 直接分配 FD、创建 anon inode、安装 FD 并改写 PC | kprobe、anon inode/FD helper、reboot wrapper 布局 | 对 `__arm64_sys_reboot` 探针时 `regs->regs[0]` 是 wrapper 收到的寄存器结构指针；当前 handler 却直接拿它作为 magic。并且 pre-handler 内执行可能睡眠的文件/FD 分配 |
| nbdriver 当前选择 | reboot kprobe 作为握手入口，SDK 仍以 ioctl 操作 anon FD | 只在 pre-handler 解码 magic 并排入 `task_work`；任务回调才分配 FD 和回写用户内存 | kprobe、task_work、anon inode/FD helpers | 避开 syscall table 改写，但仍依赖内核探针、wrapper ABI 与具体 KMI 导出；必须按目标 DDK 编译和实机测试 |

证据位置：enkernel `main.c` 约 136–229 行和 `syscall.c` 约 187–260 行；LinyuKernel `LingYuKernel/src/entry.c` 约 94–127 行；当前实现位于 `src/nbdriver_entry_reboot.c` 和 `src/nbdriver_comm.c`。

Linux Kprobes 文档说明 pre-handler 是由断点 trap 路径调用；Linux locking 文档把 `copy_to_user()`、`kmalloc(GFP_KERNEL)` 等列为可能睡眠操作，并禁止在不可睡眠上下文执行。因此 LinyuKernel 直接在 probe callback 内创建 FD 的路径需要单独处理上下文风险。[Kprobes](https://docs.kernel.org/trace/kprobes.html)、[Locking](https://docs.kernel.org/kernel-hacking/locking.html)

## 决策

在这两个现成实现中，**enkernel 的 task_work fallback 是更可靠的代码参考**：它正确地区分 ARM64 syscall wrapper 收到的 `pt_regs *`，且不在 kprobe pre-handler 中执行 FD 分配和用户拷贝。其主路径通过改写 syscall table，跨 GKI/KMI 兼容性反而更差。因此 nbdriver 只采用 task_work 延迟安装这一部分，不移植 syscall table 搜索或改写。

这不是稳定公共接口的保证。anon inode、FD helper、kprobe 与 task_work 符号是否允许外部模块使用，取决于各目标 DDK/KMI。六个 DDK 的构建结果及后续实机调用记录见 `docs/remote-build.md` 和 `docs/validation/`。

此前 misc 字符设备接口的文件、已构建 6.6 模块和当前设备验证结果已保存于 `/root/workspace/.aicode/nbdriver/miscdev-before-anonfd-20261112` 及 `out/device-6.6-20261112T090714Z/`。替换必须重新做目标设备加载和恢复验证。
