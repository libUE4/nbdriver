# 双包自动终端过滤（2026-11-14）

模块加载即安装探针并启动自动发现，无需客户端连接或开启请求。按完整 `argv[0]` 精确匹配 `com.tencent.tmgp.dfm` 与 `com.tencent.rmcn`；两包、同名多实例可以同时生效。每轮发现完成后间隔 500 ms 再扫描，启动/重启后自动更新绑定，系统繁忙时调度可能延后。

过滤范围固定为 devpts 字符设备主设备号 136、minor 0..31。目标 access/stat 探测返回 ENOENT，普通 open/read/write 保持内核语义。冒号子进程、相似前缀和其他包不会因为名字相近被纳入；没有 UID 扩展或 fork 继承规则。

## 删除内容与接口

已移除手动设置 PID、范围、开启/关闭的内核分支、请求结构、SDK 函数和测试调用；原命令 0x1c 退役，返回 ENOTTY，不复用编号。旧操作示例和当前文档中的手动流程已清理，历史审计以各自快照为准，原件在本轮 before/initial-before 保存。

仅保留只读诊断 `nbdriver_get_devpts_status()`：命令 `NBDRIVER_IOC_DEVPTS_STATUS`，编号 0x1f，_IOR，56 字节。其他 ABI 5 接口不变。

| 字段 | 语义 |
| --- | --- |
| active | 两个探针已安装；没有目标运行时也保持 1，等待自动发现。 |
| max_index | 固定 31。 |
| target_count | 调用者 PID 命名空间内可见且身份仍有效的绑定数。 |
| pids[0] / pids[1] | DFM / RMCN 的一个代表实例，未运行或不可见为 0。 |
| permission_hits / getattr_hits | 本次模块生命周期的累计命中数，重载归零。 |
| generation | 绑定集合变化时递增；普通状态查询不改变它。 |
| last_error | 最近扫描的错误；成功为 0，分配失败保留旧有效快照并继续重试。 |

## 生命周期

扫描只在 workqueue 任务上下文运行，读取目标 argv 页面不触发补页；探针不读用户内存、不睡眠。两轮任务列表枚举用于按当前规模分配临时引用表，没有原进程查询 API 的固定 512 候选上限；枚举期间新生进程由下一轮收集。

匹配快照不可变，由 RCU 发布。条目保存 PID 对象与 mm 身份：PID 数字复用不会命中旧对象，exec 换 mm 后立即不匹配。mm 只增加 mm_count，不保留 mm_users，允许退出进程正常回收映射。退出/替换后等待 RCU 再释放条目；只读查询也排除退出、exec 和命名空间不可见的旧条目。

模块退出先置停止标志，再 cancel_delayed_work_sync，随后清除快照、注销两个探针、等待 RCU 并释放全部引用。加载时第二个探针注册失败会回滚第一个并返回初始化错误，不留下半启用状态。

## 本轮实际验证

全部命令、原始输出及退出状态保存在 `out/devpts-auto-20261114/`。`initial-before/` 绑定任务开始快照；期间已有信号/VMA、线程环境 IPI 修复经三方合并保留，`before/` 与最终恢复基线对应合并前的工作区，避免回滚时抹掉其他修改。

- 生产代码主机 adapter 测试通过：精确双包、多实例、完整 argv 前缀、PID 对象复用、exec/mm 变化、退出/命名空间、只影响 access/stat、分配失败保留旧绑定、探针失败回滚和停止后不再排队，且引用归零。它不代替真实调度器/RCU 压测。
- 单头 C11/C++17、旧 SDK、ABI 镜像、只读状态参数/错误传播、walker、读取权限、readv、metadata/mapping、信号查询及原审查回归通过。
- ARM64 android15-6.6：W=1、严格 modpost、声明和 2048 字节栈帧检查通过，无编译警告；缺少 vmlinux，BTF 跳过。最终 79 个静态导入均可解析；真实 ELF 回调尾转移检查通过。

设备为 PKR110 / OP60EBL1，运行内核 `6.6.30-酷安姜北尘`。使用普通 insmod/rmmod，未修改 boot 或强制加载/卸载。

| 实测 | 结果 |
| --- | --- |
| 模块加载前已有两个自建目标 | 以两个完整包名作为 argv[0] 的自建 fixture 在重载前均可正常 access/stat；父进程此时不持有驱动 FD。 |
| 自动生效 | 加载后无需配置 ioctl，两个 fixture 同时 access/stat=ENOENT，普通 open 成功；target_count=3 包含当时真实 DFM。 |
| 排除误匹配 | `com.tencent.tmgp.dfm:helper` fixture 与普通父进程保持正常。 |
| 重启与 exec | RMCN fixture 退出后新 PID 自动绑定；DFM fixture exec 成其他名字/mm 后立即恢复可见。 |
| 删除手动入口 | 直接发送退役的 0x1c 请求返回 ENOTTY。 |
| 原有功能 | 108 项核心读写全通过、0 失败、0 跳过；PID/SO/线程/TLS/FP/批量、maps/mapping/FD/random、真实 KGSL 对照及连接错误回收均通过。 |
| 卸载与恢复 | 周期扫描运行时正常卸载成功；恢复已备份的精确原模块 AB221D20972DC1D2E4F5CD0 成功，再卸载并重载自动版成功。 |
| 最终设备 | srcversion=20A070376ADA13E19024FCA，引用数 0，active=1，真实 DFM PID=19528；RMCN 正式应用未运行。boot ID 未变，测试窗口无 BUG/WARNING/Oops/panic/CFI failure。 |

双包行为测试使用自建进程验证内核的 argv[0] 匹配和实际系统调用结果，未向正式游戏注入代码；不能把它写成两个正式应用均在运行。最终真实 DFM 另以 pidof/cmdline 与状态 PID 对照。原始证据见 `device-auto.*`、`device-*-status.*`、`device-real-package-identity.*`、`device-final-state.*` 和 `device-kernel-review.txt`。

## 产物与恢复

模块：`out/devpts-auto-20261114/nbdriver-auto-android15-6.6.ko`，SHA256 `145b0b7bc5c4b658d75cbcb778b9bf28e2835544855031e35f8703a7d2df0706`。最终文件哈希、补丁和验证范围见 `after-sha256.json`、`changes.patch`、`validation-summary.json`。

本地恢复：`python3 out/devpts-auto-20261114/restore.py --target /root/workspace/nbdriver`。脚本先校验所有交付哈希再恢复，已在额外副本验证；不覆盖后续修改，不改变设备模块。设备恢复先关闭驱动 FD，再普通 rmmod，然后 insmod `/data/local/tmp/nbdriver-auto-20261114/before.ko`；原模块同时保存在本地 `device-before.ko`，SHA256 `da2c2c46cf716384ef29359c37307edc341340d8b32fae5da51fe69b9980d134`。

未进行其他完整 KMI/品牌固件、5.10/5.15 旧 CFI、KASAN/KCSAN/LOCKDEP 或并发卸载故障注入验证，不以单台 6.6 结果声明全系兼容。
