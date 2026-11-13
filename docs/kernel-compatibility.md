# ARM64 Android 内核兼容策略

当前实现使用匿名 FD + ioctl：SDK 通过带 magic 的 reboot 系统调用请求连接，`__arm64_sys_reboot` 的 kprobe 只排入 task_work；FD 分配及用户回写在正常任务上下文执行。没有 `/dev/nbdriver` 节点，也没有 syscall table 槽位改写。

## 当前依赖与失败行为

- 静态模块导入必须由目标 `Module.symvers` 提供；配置、CRC、页大小、编译器和签名策略必须匹配目标。
- 初始化通过短期 kprobe 取得 kallsyms resolver，再解析 task_work_add、mm_access 和 ptrace_may_access。它们不全部属于稳定 KMI；符号不存在或探针被禁止时，模块明确返回初始化错误。
- 内存补页、FP/线程环境、FD 元数据和 KGSL 是有条件的能力；检查 GET_ABI，并处理请求返回的 EOPNOTSUPP、EAGAIN 等错误。
- 不改写全局内核 CFI slowpath。动态调用使用目标头文件的准确原型及局部调用点处理，回调保留 CFI 类型元数据。
- FD task_work 的 ARM64 回调恢复原始栈帧/LR 后跳转到内核 module_put，释放最后引用后没有模块内返回指令。CI 使用 `tools/check_callback_tail.py` 检查实际 ELF，包括 LTO 工件。
- PID/TID 输入输出均按调用者 PID 命名空间解释；devpts 自动发现两个完整包名，用 PID 对象和 mm 身份匹配；退出重启可重新绑定，PID 复用/exec 不继承旧状态。加载阶段必须安装两个 security_inode_* 探针，失败则回滚并返回初始化错误。

## 验证范围

[2026-11-14 修复记录](audit-fixes-20261114.md) 与对应产物 JSON 绑定当前修复的源码哈希。旧 misc 模块、旧匿名 FD 模块或历史 8 KMI 构建的结果，只适用于它们各自的版本。

本地可用的完整 DDK 是 android15-6.6。本轮本地构建/模拟测试不能替代其他 KMI 的构建和设备运行结果。仓库 CI 保留 android12-5.10、android13-5.10、android13-5.15、android14-5.15、android14-6.1、android15-6.6、android16-6.12、android17-6.18 八个代表性目标，先执行主机检查，再分别构建与校验符号和回调尾转移。

一个代表性分支构建通过，不能证明所有 5.x/6.x 厂商固件接受同一 `.ko`；5.4 等不在该矩阵内的分支也不能按矩阵结果报告支持。

## 目标适配流程

1. 记录设备身份、Android/KMI 分支、架构、配置、内核签名策略及正确的 Module.symvers。
2. 用匹配的目标构建目录编译，运行严格 modpost、静态导入检查及 ELF 回调检查。
3. 保存源码/模块哈希、编译器、vermagic 和日志。静态符号检查不校验设备签名，也看不到 kallsyms 动态调用的实际 ABI。
4. 在可恢复测试设备验证加载、ABI、权限拒绝、读写/查询、错误回收及普通卸载。并发卸载、旧 CFI、LOCKDEP/KASAN 等分别记录实际测试范围。
5. 按明确测试过的设备及固件交付，保持原模块与恢复方法；不修改 boot、签名强制策略或设备保护配置来掩盖不匹配。

历史来源与设备结果见 [远程编译记录](remote-build.md)、[旧设备记录](device-6.6.md) 和 [历史函数审计](audit-functions-and-loading-20261112.md)。
