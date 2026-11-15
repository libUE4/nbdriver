# 通信入口比较与选择

## 对比范围

比较对象是取得控制 FD 的入口及后续 ioctl 传输，不比较各项目的内存、触控等业务功能，也不把某个内核版本编译成功等同于全系运行兼容。

| 项目 | 获取 FD | 回调中的处理 | 额外内核依赖 | 主要问题 |
|---|---|---|---|---|
| enkernel | 扫描并改写 `sys_call_table` 的未实现槽位；另注册 reboot kprobe 作为 fallback | reboot kprobe 仅分配 `GFP_ATOMIC` work 并排入 `task_work`；FD 分配和用户拷贝延迟到 task_work | kallsyms resolver、sys_call_table、stop_machine/fixmap 写入路径、kprobe、task_work | 双路径依赖较多非稳定内核内部接口；syscall table 不是模块 ABI。其 kprobe fallback 的调用上下文处理比 LinyuKernel 当前实现稳妥 |
| LinyuKernel | reboot kprobe | 在 kprobe pre-handler 直接分配 FD、创建 anon inode、安装 FD 并改写 PC | kprobe、anon inode/FD helper、reboot wrapper 布局 | 对 `__arm64_sys_reboot` 探针时 `regs->regs[0]` 是 wrapper 收到的寄存器结构指针；当前 handler 却直接拿它作为 magic。并且 pre-handler 内执行可能睡眠的文件/FD 分配 |
| nbdriver 当前选择 | 动态空槽入口与独立 reboot kprobe；SDK 扫描候选后回退 | 两个入口均延迟到 task_work 安装 FD | 动态表/NI 解析、stop_machine、nofault、vmap、RCU Tasks；kprobe、task_work、anon FD | 表路径依赖不满足时可单独使用 reboot；表引用通过 QUIESCE 恢复并释放；仍需目标设备验证 |

证据位置：enkernel `main.c` 约 136–229 行和 `syscall.c` 约 187–260 行；LinyuKernel `LingYuKernel/src/entry.c` 约 94–127 行；当前实现位于 `src/nbdriver_platform.c（entry 段）` 和 `src/nbdriver_core.c（comm 段）`。

Linux Kprobes 文档说明 pre-handler 是由断点 trap 路径调用；Linux locking 文档把 `copy_to_user()`、`kmalloc(GFP_KERNEL)` 等列为可能睡眠操作，并禁止在不可睡眠上下文执行。因此 LinyuKernel 直接在 probe callback 内创建 FD 的路径需要单独处理上下文风险。[Kprobes](https://docs.kernel.org/trace/kprobes.html)、[Locking](https://docs.kernel.org/kernel-hacking/locking.html)

## 决策

2026-11-15 根据用户要求采用 enkernel 的双路径和候选扫描思路。槽位只接受解析到的未实现处理器及其 CFI 跳板，普通系统调用不覆盖；选槽范围与 SDK 一致。表路径使用动态解析的 nofault 读写、单页别名和 stop_machine，不照搬 init_mm/fixmap 静态依赖。两种入口均使用现有 task_work 及经机器码验证的 module_put 尾转移。表引用通过 QUIESCE 的恢复及 RCU Tasks 同步释放，失败保留引用并返回错误。详见 [兼容策略](kernel-compatibility.md)。

这不是稳定公共接口的保证。anon inode、FD helper、kprobe 与 task_work 符号是否允许外部模块使用，取决于各目标 DDK/KMI。六个 DDK 的构建结果及后续实机调用记录见 `docs/remote-build.md` 和 `docs/validation/`。

此前 misc 字符设备接口的文件、已构建 6.6 模块和当前设备验证结果已保存于 `/root/workspace/.aicode/nbdriver/miscdev-before-anonfd-20261112` 及 `out/device-6.6-20261112T090714Z/`。替换必须重新做目标设备加载和恢复验证。
