# nbdriver 审查项修复（2026-11-14）

本文绑定本轮自动化之前的历史快照；手动过滤接口已在后续版本移除，当前实现见[自动过滤记录](devpts-auto-20261114.md)。

本轮针对 [审查报告](driver-code-review-20261114.md) 的 F01—F11 修改驱动、SDK、测试与验证文档，保留三个 C 实现文件的合并结构。F06 在本轮之前已经修复，本轮保留并回归。修改先在独立副本执行，原件和 SHA256 位于 `out/audit-fixes-20261114/before/`、`before-sha256.json`。

## 改动与依据

| 审查项 | 当前实现 |
| --- | --- |
| F01：task_work 最后引用后的模块指令 | ARM64 具有准确回调原型的入口调用 C worker，恢复调用者 SP/LR 后直接跳到内核 `module_put`。释放最后引用后返回内核调用者，不返回模块代码；不依赖编译器尾调用优化。入口显式保存 PAC/BTI 语义，保留 CFI 类型元数据。 |
| F02：全局 CFI 修改及失败恢复 | 删除全部内核 slowpath 文本改写及相应恢复状态。动态调用保留准确原型及局部调用点处理；旧工具链所需的本地 abort handler 遇错仍 fail closed。 |
| F03：GPU 所有者查询锁契约 | `__module_address` 查询、比较时禁止抢占，结束后恢复；vendor handler 在该短临界区外运行。 |
| F04：写请求的 LSM 模式 | 读使用 READ_FSCREDS，写使用 ATTACH_FSCREDS；保留能力检查、VMA/PTE/COW 约束和拒绝错误。 |
| F05：PID 复用 | devpts 比较 TGID 的 PID 对象身份。ENABLE 经目标权限检查并持有对象引用。QUERY 在目标退出或对调用者不可见后报告 pid=0、enabled=0、max_index=0，保留命中计数，直到 DISABLE/重新配置清除。 |
| F06：合并后的 walker 测试 | 保留已修复的实际源码段提取，回归页大小/页表分支，不再引用被删除的实现文件。 |
| F07：argv[0] 完整前缀 | 已读前缀包含 NUL 即可接受完整 argv[0]；无 NUL 才处理后续读错误或超长。 |
| F08：PID 命名空间 | 输入查询、进程/线程输出、身份校验统一为调用者可见 PID/TID；枚举不计入不可见进程，devpts 匹配不依赖数字编号。 |
| F09：线程环境 | current 在禁抢占范围读取实时 TPIDR_EL0；其他线程经 task_call_func 固定调度状态，运行中返回 EAGAIN，后端缺失返回 EOPNOTSUPP。TLS/APGA 的 valid 表示采样有效，允许数值为零。设备测试对合法 EAGAIN 有界重试。 |
| F10：记录版本错配 | 当前兼容性文档按 reboot/task_work/匿名 FD 实现重写。历史记录保留原版本对象数和覆盖范围，不把旧 8 KMI 结果挪到本轮三个文件上。 |
| F11：诊断及冗余 | 错误/警告日志限流；缺符号按文本在锁内判重。删除无效语句，修正 FD 发布注释。两套 SDK 自动校验 ABI，错误时关 FD；内核回传安装 errno，SDK 保留 EMFILE/EPERM 等真实错误。ABI 结构、命令号和版本 5 保持兼容，仅增加握手 pending 常量。 |

## 本轮验证

命令、原始 stdout/stderr、退出状态逐项记录在 `out/audit-fixes-20261114/*.json` 及同名日志。源码/模块版本和最终文件差异以 `validation-summary.json`、`changes.json`、`changes.patch` 为准。

- 本地 android15-6.6 ARM64、KCFI、PAC、PREEMPT：W=1、严格 modpost、声明与 2048 字节栈帧检查通过，无编译警告。缺少 vmlinux，仅跳过 BTF 生成。
- 最终模块 73 个静态导入全部在本次 6.6 symvers 中；不把此检查等同设备 CRC/签名或动态私有 ABI 检查。
- 强制定义旧 CFI 条件宏的额外 6.6 构建通过，用于检查删除补丁后的条件代码；它不是 5.10/5.15 的真实旧 CFI 构建或运行证明。
- 单头 C11/C++17、旧 SDK、ABI 一致性、walker、读取权限、批量读、mapping、metadata、进程线程排序及 SDK 参数检查通过。
- 新主机探针运行实际生产函数体，覆盖完整 argv[0] 前缀/后续页错误、命名空间身份与引用释放、PID 数字复用与退出状态、TLS 实时/保存态/零值/运行中拒绝、读写授权及拒绝传播。GPU adapter 断言归属查询期间确实禁抢占。
- `tools/check_callback_tail.py` 检查最终 ELF 的真实回调指令，CI 每个 KMI 都检查这一点。另以 Unicorn 执行真实新旧回调字节，在模拟 module_put 时解除模块映射：旧版随后取指失败，新版恢复原 SP/FP/LR 后正常返回内核调用者。该测试模拟 worker/module_put，只验证控制流，不验证真实调度或 CFI 运行时。

## 实机与交付

在当前连接的 PKR110 / OP60EBL1 设备实测，运行内核 `6.6.30-酷安姜北尘`，构建模块 vermagic 为 `6.6.127-4k SMP preempt mod_unload modversions aarch64`。本设备实际接受普通 insmod，未使用强制加载/卸载或修改 boot；该自定义内核的结果不能代表官方固件的版本/签名检查。

| 实测 | 结果与证据 |
| --- | --- |
| 原模块识别、保存 | 原 srcversion `841C76821C4607BFB96D9FD`，从设备保存为 `device-before.ko`，SHA256 `4d047525ccf0a7f271f54d82f90134c520711214be9957b3371f70ec8d7a9796`。 |
| 原版行为 | 108 项读写、查询/批量、metadata（含真实 KGSL 对照）和原 devpts 测试通过。新连接探针复现 EMFILE 被报成 ENODEV，退出 1 为预期的缺陷复现，见 `device-before-connection-v2.*`。 |
| 新版核心读写 | 108 通过、0 失败、0 跳过；含远程写入/回读、非驻留补页、COW 拒绝、可执行页及并发 madvise，见 `device-after-legacy.*`。 |
| 新版查询与扩展 | argv[0] PID、SO/ELF、线程 TLS/FP、身份过期、混合失败批量读、512 KiB 内容，以及 maps/mapping/FD/random 全部通过，见 `device-after-query.*`、`device-after-metadata.*`。 |
| KGSL | 实际分配 GPU 对象，以 native GPUOBJ_INFO 对照字段及错误，`--require-gpu` 下 0 失败、0 跳过。 |
| devpts | 仅目标命中、非目标正常、目标退出后状态归零且保留计数、停用/重启用和非法输入通过，见 `device-after-devpts.*`。原版目标退出后仍 enabled=1，新版为 enabled=0。 |
| 连接失败回收 | 16 次无效输出指针无 FD 泄漏、RLIMIT_NOFILE 耗尽返回 EMFILE、非 root 返回 EPERM，全部通过，见 `device-after-connection.*`。 |
| 普通卸载及恢复 | 持有 FD 时 rmmod 被拒绝，关闭后引用为 0 可正常卸载；恢复原模块并跑查询成功，随后卸载原模块、重载新版再跑查询成功。见 `device-reference-*`、`device-unload-*`、`device-restored-*`、`device-final-*`。 |
| 最终设备状态 | 新 srcversion `07760E06D3F76941C392E7E`，模块引用 0，boot ID 与基线相同。测试时间段的 dmesg 未发现 BUG/WARNING/Oops/panic/CFI failure；错误输入产生预期 errno 日志，日志限流生效。见 `device-kernel-review.txt`。 |

最终模块：`out/audit-fixes-20261114/nbdriver-android15-6.6-fixed.ko`；SHA256 `84e9d8a2b0253a75139230492f3186e4319d3e1ea89a801340c63bfdb77eda40`。手机当前保留该修复版。测试程序源码为 `tests/nbdriver_smoke.c`、`device_query_readv_smoke.c`、`device_metadata_smoke.c`、`devpts_filter_smoke.c` 和新增的 `device_connection_smoke.c`。

设备恢复使用已验证的原模块：先关闭全部驱动 FD，普通 `rmmod nbdriver`，再 `insmod /data/local/tmp/nbdriver-dfm-writeonly-6.6.ko`；若卸载返回 busy，先释放调用者持有的 FD，不强制卸载。本地也保留相同原模块的 `device-before.ko`。

## 恢复方法

本地恢复脚本为 `out/audit-fixes-20261114/restore.py`，先检查所有待恢复文件仍是本轮交付哈希，再恢复原文件并移除本轮新增文件，避免覆盖后续修改。执行：

```sh
python3 out/audit-fixes-20261114/restore.py --target /root/workspace/nbdriver
```

恢复仅针对 `changes.json` 列出的源码/文档文件，不切换手机模块，也不删除本轮编译工件和日志。恢复操作在额外副本中验证，结果见 `restore-validation.json`。

## 尚未覆盖的验证

本轮不具备其他完整 KMI 构建环境，远程 8 KMI CI 未运行；其他品牌/固件、5.4 等分支未取得本轮验证结果。没有运行 KASAN/KCSAN/LOCKDEP 调试内核、并发卸载故障注入或旧 CFI 实机测试。命名空间/PID 复用及部分 argv[0] 边界由实际函数的主机 adapter 验证，尚非设备上构造所有边界。

因此，审查中列出的实现缺陷已逐项处理，但不能把有限测试表述为“所有 5.x/6.x、所有厂商均可加载且绝不会死机”。512 个 PID 候选、驻留可写普通页写入、KGSL 适配范围及非原子内存快照等既定能力边界仍按 SDK 文档执行。
