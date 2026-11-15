# 厂商内核导出符号差异

2026-11-15 首次扫描服务器当时现存的全部 10 份驱动上报，随后又读取了新增的 5 份 6.12 上报，合计 15 份。四份新版压缩包来自 Android 12 / 5.10，内核明确报告 `init_user_ns`、`get_task_cred` 和 `__put_cred` 为 Unknown symbol，加载阶段返回 errno=2。模块已下载并通过摘要检查，因此这次 errno=2 不表示本地文件不存在。其余六份旧刷入器日志涉及 5.15、6.1、6.6，因旧工具仅接受模块名 NBG 而拒绝 nbdriver，尚未执行内核加载；这些日志没有提供新的缺失符号证据。

新增的五份 6.12.23 报告不是 Unknown symbol：驱动初始化已开始执行，`syscall18_probe` 注册返回 -ENOENT，导致 entry 初始化退出。源码把探针目标固定为 `__arm64_sys_lookup_dcookie`。Android 16 / 6.12 的[官方 syscall 表](https://android.googlesource.com/kernel/common/+/refs/tags/android16-6.12-2025-06_r38/scripts/syscall.tbl)已将 18 号槽映射到 sys_ni_syscall。探针失败日志本身不能证明目标函数是缺失、不可定位还是受探针限制；因此新的失败日志记录每个候选目标和实际错误。

此次修正：

- UDP 初始化仅需目标 UID，改用内核 `task_uid()` 在 RCU 保护下复制值，不持有凭据指针，不再引入 `get_task_cred` / `__put_cred` 静态依赖。
- 系统合成进程授权仍要求初始用户命名空间及四个 UID 均为 AID_SYSTEM；初始命名空间由无父节点、level=0 的内核不变量识别，避免导入 `init_user_ns`。子命名空间和任何 UID 不匹配继续拒绝。
- syscall18 引导在旧目标不存在时尝试 `__arm64_sys_ni_syscall`，两个处理函数均接受 pt_regs 指针。预处理先验证 syscallno=18 和双 magic，其他未实现系统调用不能触发 FD 安装；非 ENOENT 的探针限制不被后备路径忽略。SDK 仍使用原来的 syscall18 协议，无需更改 ABI。
- 构建导入检查把三个已在厂商设备确认缺失的符号列入禁止静态导入清单，即使 DDK 的 Module.symvers 声称导出也不能通过。
- DDK 审计额外记录凭据与用户命名空间相关头文件、实现和符号。生产代码测试覆盖 UID 值快照、资源释放、初始命名空间限制、四个 UID 检查和原有引用计数。入口测试执行真实的选择与预处理代码，覆盖旧/新路径、其他系统调用隔离、magic、权限错误、缺失目标及清理。

完整证据、原始包和哈希保存在工作区 `out/vendor-symbol-audit-20261115/`。此修正不改变用户层 ABI。DDK 编译与导入检查不能替代厂商设备加载测试，也不能据此保证所有内核均可加载。旧 NBG 刷入器的名称校验和握手协议需单独更新，不能靠给 nbdriver 改文件名解决。
