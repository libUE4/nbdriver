# nbdriver 驱动代码审查（2026-11-14）

本文绑定本轮自动化之前的历史快照；手动过滤接口已在后续版本移除，当前实现见[自动过滤记录](devpts-auto-20261114.md)。

该文档保留修复前的审查发现及证据。F01—F11 的当前处理状态见 [完整修复记录](audit-fixes-20261114.md)；下文“未解决”描述的是审查当时快照。

当前源码仍有需要修复的生命周期、权限和内核 API 使用问题，不能认定为“全系内核无死机、无报错、均能加载”。本轮完成源码审查、主机回归、6.6 严格编译及符号检查；没有在设备上触发崩溃、重刷或做并发卸载压测。

审查对象为当前工作区的三个合并实现文件、全部私有头、公共 ABI、两套 SDK、Kbuild/构建入口和 CI/测试。起始基线共 47 个文件的 SHA256 与副本保存在 `out/code-audit-20261114/baseline-sha256.json`、`snapshot/`。审查期间工作区另有更新，本轮继续检查并固定了 48 个文件的当前快照，见 `current-sha256.json`、`current-snapshot/`；两阶段差异保存在 `concurrent-changes.patch`。本次审查自身只新增报告和审查产物，未改驱动实现；F06 已由期间出现的工作区更新修复，其他建议尚未应用。

## 优先级总表

| 编号 | 优先级 | 问题 | 证据状态 |
| --- | --- | --- | --- |
| F01 | P1 | FD task_work 的最后一次模块引用释放后，仍执行模块内指令，存在并发卸载窗口 | 源码、内核生命周期和实际反汇编确认；未实机触发 |
| F02 | P1 | 旧 CFI 分支修改全局内核 slowpath，恢复失败仍继续退出 | 失败路径确定；旧 CFI 实机未测 |
| F03 | P1 | GPU 模块归属查询不满足 `__module_address()` 的禁抢占要求 | 对照本地 6.6 内核声明及 LOCKDEP 断言确认 |
| F04 | P1 | 写内存使用 READ 模式的 ptrace/LSM 权限检查 | 调用链及 SELinux 实现确认 |
| F05 | P2 | 终端过滤仅比较数字 PID，目标退出后可能误匹配复用 PID 的进程 | 对照 `free_pid()` 确认生命周期语义 |
| F06 | 已修复 | 审查起始时页表测试引用已删除文件，之后工作区更新修正 | 起始失败和当前通过均有日志 |
| F07 | P2 | argv[0] 已完整读到但后续参数页缺失时，PID 查询仍判失败 | 执行实际 `query_argv0()` 函数的主机探针复现 |
| F08 | P2 | PID 查询、身份校验混用命名空间内 PID 和全局 PID | 静态调用链确认，限定非初始 PID 命名空间 |
| F09 | P2 | THREAD_ENV 可把运行线程旧 TLS 当作有效结果，APGA 两半也非原子快照 | 对照 ARM64 TLS 保存代码确认；实机并发未测 |
| F10 | P2 | 部分兼容性/历史审计文档已与当前实现及验证版本不符 | 代码、构建记录和文档交叉确认 |
| F11 | P3 | 重复/无效代码、日志限流和连接错误报告仍需整理 | 静态检查 |

目前未解决项为 4 个 P1、5 个 P2 和 1 组 P3 写法/诊断问题，另有 F06 在检查期间已修复。P1 表示优先解决的系统稳定性、权限或全局状态风险；不是声称已实测发生 panic。P2 是确定的功能/验证问题或有明确条件的行为缺陷。

## F01：task_work 回调与卸载之间缺少完整的生命周期闭合

位置：`src/nbdriver_platform.c:94`、`:111`、`:112`、`:234`。

`nbdriver_schedule_fd()` 为 task_work 增加模块引用；回调最后执行 `module_put(THIS_MODULE)`。问题出现在非 root 拒绝、FD 分配/用户输出失败，或者新 FD 被其他线程立即关闭且没有其他模块引用时：该次 put 可能释放最后一个使用引用，但回调尚未返回到内核 task_work 调用者。

本轮实际编译的 `nbdriver_platform.o` 在 `module_put` 调用后仍有 `ldr`、`ldp`、`autiasp`、`ret`，不是已经跳出模块的尾调用。内核编译参数也包含 `-fno-optimize-sibling-calls`。如果回调在这里被抢占，另一个 CPU 卸载模块，退出路径只注销入口探针，没有等待这些已从 task_work 队列取出、正在执行的回调返回，存在返回已释放模块代码的窗口。

普通成功连接时匿名文件的 `.owner` 引用通常能继续保活模块，所以只做“打开、关闭、然后卸载”的顺序测试不充分。`unregister_kprobe()` 同步的是探针处理程序，不是后续独立运行的 task_work。`task_work_run()` 调用回调时也没有替模块提供贯穿回调的额外模块引用或 RCU 读侧保护。

修复建议：让退出路径明确排空正在执行的异步回调，或使最后一个引用的释放发生在已返回模块外的执行路径。仅加一个在 `module_put()` 前减为零的计数、或者依赖编译器尾调用，不能证明窗口关闭。回归应覆盖非 root 握手、EMFILE/ENOMEM/EFAULT 与并发普通卸载；在可恢复的测试内核上使用 KASAN/抢占配置验证。

证据：`out/code-audit-20261114/evidence-00.stdout` 中 `nbdriver_fd_work_fn`；`kernel-source-evidence.json` 中 task_work 和模块卸载实现。这里只确认竞态条件，没有设备 panic 记录。

## F02：旧 CFI 分支会留下无法可靠恢复的全局补丁

位置：`src/nbdriver_platform.c:388`、`:426`、`:439`、`:457`。

在 `NBDRIVER_LEGACY_CFI` 分支中，初始化把 `__cfi_slowpath` 等函数开头改为 BTI/RET，影响整个内核及其他模块的检查行为，并非只修复 nbdriver 的一个调用点。

有两条明确的错误恢复缺口：

1. 初始化补丁调用失败后会尝试恢复；即使恢复也失败，仍返回初始化错误。`nbdriver_cfi_count` 此时尚未设置，主初始化直接走 `err_ksym`，没有后续恢复记录或重试路径。
2. 卸载恢复失败只打印日志，随后仍把 count 清零、函数指针清空并继续卸载。补丁可能仍留在全局内核文本内，恢复所需状态却消失。

此外，此路径没有对其他文本修改者建立补丁归属协议；卸载时直接写回保存的指令，无法证明那时的文本仍属于本模块的那次修改。

修复建议：优先取消全局 CFI slowpath 改写，保持动态函数原型准确，并使用已有的局部调用点处理。需要保留文本修改时，必须有可审计的归属检查和失败恢复设计；不能以“报一条错然后继续卸载”作为恢复成功。

本次 6.6 构建不进入旧 CFI 分支，不能验证 5.10/5.15 上这段补丁或恢复代码。旧版本的“编译通过”也不等于该路径运行通过。

## F03：GPU 所有者验证使用了不符合接口约束的锁

位置：`src/nbdriver_features.c:350`，尤其是 `:357`—`:360`。

`metadata_gpu_owner()` 在普通 `rcu_read_lock()` 下调用动态解析的 `__module_address()`。本地 6.6 的 `kernel/module/main.c:3339` 明确要求调用者禁抢占或持有模块互斥锁；`kernel/module/internal.h:120` 的断言检查 `rcu_read_lock_sched_held()` 或模块锁。

在 PREEMPT_RCU 内核上，普通 RCU 读锁不禁止抢占，ioctl 所持有的进程 exec 读锁也不满足该要求。地址落入模块区域、启用 LOCKDEP 且 debug_locks 有效时，可以走到 WARN。若目标配置 `panic_on_warn=1`，报警可升级为 panic。本轮普通 DDK 构建没有启用这一调试组合，主机 GPU stub 也不检查禁抢占语义，因此已有通过记录覆盖不到它。

修复建议：在两次短小的模块归属查询及比较范围内满足禁抢占契约，随后恢复上下文；不要在该区域调用会睡眠的 vendor handler。文件引用继续负责保活实际文件驱动。

此处确定的是 API 锁契约不符和条件性 WARN 风险，没有据此推断所有 6.6 内核都会 UAF 或死机。

## F04：写内存复用了读权限检查

位置：`src/nbdriver_core.c:285`、`:288`、`:500`。

`nbdriver_write_target()` 调用 `nbdriver_access_mm()`；后者固定传入 `PTRACE_MODE_READ_FSCREDS`。因此写入目标时也只请求了读模式的 ptrace/LSM 检查。

虽然入口另外要求 `CAP_SYS_PTRACE`，SELinux 仍区分这两种模式：本地 `security/selinux/hooks.c:2094` 对 READ 检查 `FILE__READ`，其他模式检查 `PROCESS__PTRACE`。能力位不能代替这里正确的 LSM 操作类型。标准 `process_vm_access` 使用的是 ATTACH 模式。

修复建议：给 mm 访问封装明确区分读写授权；写操作使用适当的 ATTACH 模式，并明确选择 FSCREDS/REALCREDS 的接口语义。保持现有 VMA/PTE 写权限检查、进程身份校验及引用回收。应使用“可读但拒绝 ptrace”的 SELinux 策略验证写请求被拒绝。

本项是权限语义错误，不是声称任意普通应用能无条件获得驱动写权限。

## F05：终端过滤的数字 PID 可能被复用

位置：`src/nbdriver_features.c:639`—`:644`、`:827`—`:843`。

当前状态确实持有 `struct pid *`，但匹配仍是 `pid_nr(old_pid) == task_tgid_nr(current)`。注释所写的“持有 pid 引用，号码就不会被复用”不成立：本地 `kernel/pid.c:129` 的 `free_pid()` 会先从命名空间 idr 删除数字 PID，再经 RCU 释放其引用。额外引用保护的是旧对象的内存，不是永久保留其号码。

目标退出并被回收后，另一个进程可以获得同一数字 PID；旧过滤状态仍是 enabled，便可能误过滤新进程的 `/dev/pts` access/stat。

修复建议：使用持有的 PID 对象身份与当前 TGID 对象比较，而不是比较数字；同时明确目标退出后 QUERY 的状态语义。回归需要覆盖目标退出、PID 重用以及重新 ENABLE，不能只验证目标存活期间的命中计数。

## F06：合并后遗漏页表测试适配——检查期间已修复

位置：`tests/test_walk.py:182`、`:289`；`.github/workflows/kernel-matrix.yml:41`。

审查起始版本的测试生成 C 程序仍 `#include "nbdriver_walk.c"`，却没有生成相应临时文件，而原文件已经合入 `nbdriver_core.c` 并删除。当时原样执行 `python3 tests/test_walk.py` 实际退出 1，错误是：

```text
fatal error: nbdriver_walk.c: No such file or directory
```

该起始版本会使 CI 的 ABI job 在这一步停止。这是上轮合并遗漏的测试引用；当时其他四个 Python 测试通过不能代表完整 CI 通过。

在本轮先用审查副本验证路径适配后，工作区随后也出现了相同的真实 walker 分段提取修正，并新增读取权限测试及 CI 依赖。已重新固定当前快照，原样运行修正后的 walker 通过，4K/16K/64K 三组 synthetic 分支测试通过。此项已经关闭，不能再当作当前仍失败的问题；新增 CI 改动不等于远程全矩阵已经运行成功。

原失败日志：`out/code-audit-20261114/host-00.stderr`；副本验证：`walk-adapted-result.json`、`walk-adapted.stdout`；当前快照通过日志：`current-host-00.stdout`、`current-host-results.json`。synthetic 测试不验证真实页表锁和 MM 并发。

## F07：argv[0] 的后续页缺失会造成不必要的 PID 查找失败

位置：`src/nbdriver_core.c:912`—`:926`。

`query_argv0()` 一次读取到 `arg_end` 或容量上限，然后先检查整体 ret，最后才检查 NUL。因此 argv[0] 本身已在成功读取的前缀内完整结束，但后面的其他参数所在页未驻留/不可读时，函数仍直接返回失败，外层跳过该进程。

本轮探针执行的是当前源码中的实际函数体：模拟 argv[0] 位于页尾、已读前缀为 `target\0`，后续读取返回 ENODATA。实际输出：

```text
query_argv0 result=-61 prefix=target nul_in_copied=yes
```

修复建议：只要已读前缀包含终止符，就可接受该完整 argv[0]；前缀没有 NUL 时才按读错误/超长处理。继续保持不为进程名查找主动补页的既定语义。

证据：`out/code-audit-20261114/argv0-partial-probe.c` 和 `.json`。该探针模拟内存读依赖，没有宣称已在手机上构造此页布局。

## F08：非初始 PID 命名空间下编号语义不一致

位置：`src/nbdriver_core.c:867`、`:877`、`:983`、`:1120`。

`find_get_pid()` 按调用者命名空间找输入 PID/TID，但 `task->tgid`、`task_tgid_nr()`、`task_pid_nr()` 返回/比较全局编号。因此命名空间中的 PID 123 即使被正确找到，也可能在 `task->tgid != pid` 时被拒绝；find_process 返回的全局 PID 又不能直接作为同一命名空间内的下一次查询输入。

终端过滤对象比较可同时解决其一部分编号问题，但查询和输出仍需单独统一。

修复建议：统一采用调用者可见的编号及对应 namespace helper，或者明确限制为初始 PID 命名空间并在入口拒绝其他情况。常规 Android 初始 PID 命名空间不直接暴露此问题；容器环境需要专门回归。

## F09：线程环境接口没有保证返回实时/一致 TLS 状态

位置：`src/nbdriver_core.c:1135`—`:1159`。

THREAD_ENV 获取 exec 读锁后直接读 `task_user_tls(task)`，未使用 FP 接口那样的调度状态固定，也未拒绝 current/on_cpu。ARM64 用户态可以修改 TPIDR_EL0，而 `arch/arm64/kernel/process.c:422` 的代码在上下文切换等保存点才把它更新进任务结构。因此返回成功且 valid 有值，不证明这是查询时的硬件 TLS。

APGA 的 lo/hi 也分别读取，没有保证与目标并发密钥重置之间的一致性。另外，用非零值表示 TLS/APGA valid，会把合法的零 TLS 和“不可获得”混为一谈。

修复建议：明确这是保存态还是一致采样接口；若要一致采样，固定目标调度状态，对运行中目标明确返回可重试错误，对 current 采用正确的本地采样路径。valid 表示采样/功能是否有效，而不是数据是否非零。

THREAD_FP 已有 current/on_cpu 拒绝路径，此项不把它与 THREAD_ENV 混为同一实现。

## F10：验证材料与当前实现存在版本错配

`docs/kernel-compatibility.md` 仍把当前通信写成 misc `/dev/nbdriver`，并称不 hook reboot、不依赖 kallsyms/kprobe；当前实际实现正相反。若按该文档诊断加载问题，会排查错入口。

`docs/audit-functions-and-loading-20261112.md:22` 被更新为“三个合并对象均参与 8 KMI 构建”，但该文档自身绑定的是 11 月 12 日旧提交；本轮合并的 `merge-validation.json` 仅记录了 6.6 构建，且该旧文档其他段落还写只有 GET_ABI、features=0。不能把旧提交的 8 KMI 成功记录挪作当前三个实现文件的全矩阵证据。这处历史记录的改写也是上轮整理中的错误。

修复建议：历史记录固定其源码/工件版本；单独维护当前状态页，列出当前哈希对应的构建、签名、目标设备与运行结果。补跑当前版本矩阵后才更新覆盖结论。

## F11：写法和诊断方面的低优先级问题

- `src/nbdriver_features.c:863` 的 if/else 两边完全相同；`:875` 的 `req.enabled = req.enabled` 是无效语句。它们不构成当前死机证据，但应清掉以便检查真正的错误分支。
- `src/nbdriver_core.c:226` 注释同时写“先装 fd 再回报”和“先 copy 再 install”；实际是后者。应明确调用者只在系统调用返回后使用 FD；不能把输出整数视为给其他线程的提前发布协议。
- 非 root 握手、未知 ioctl 等路径逐次 printk，没有限流；反复错误请求可能形成日志/性能压力。`nbdriver_log_missing_once()` 也按字符串指针而非文本判重，去重语义不稳。
- 单头 `nbdriver_open()` 在未获 FD 时统一设置 ENODEV，隐藏了 EPERM、EMFILE、ENOMEM 等真实原因；内核安装失败只靠 dmesg 报告，批量部署排障较差。它目前也不自动进行 ABI 握手，使用者仍应查询 ABI/功能位。
- 公共 ABI 在单头中有一份镜像；当前一致性测试通过，后续必须继续约束两份定义的漂移。旧式 SDK 与单头同一翻译单元混用会有重复声明问题，现有 SDK 文档已要求选择一种接法。

## 加载兼容性与既定功能限制

以下与“能否刷入/能否连接”直接有关，但不都属于新发现的缺陷：

| 条件 | 当前代码的实际行为/限制 |
| --- | --- |
| 目标架构 | 使用 ARM64 pt_regs、页表、缓存和线程布局；不是任意架构的通用模块 |
| KMI/签名/页大小 | 必须使用目标允许的配置、CRC/符号表和签名策略；一次 6.6 构建不覆盖其他 5.x/6.x 分支，也不保证不同厂商固件接受同一 ko |
| kprobe/符号可见性 | kprobe 注册、kallsyms resolver、reboot 目标、task_work_add、mm_access、ptrace_may_access 缺少或不可探测会使模块初始化失败；不是单纯“导出符号都齐全”就能保证加载 |
| 动态私有调用 | `nm -u` 和 modpost 看不到通过 kallsyms 获得的这些地址，也不能校验所有目标函数/私有结构的运行时 ABI；不能据 `UNRESOLVED: none` 声称动态路径已验证 |
| 检查脚本 | `tools/check_undefined.py` 检查导入符号名，读取 symvers 时没有保存 CRC；它不是目标设备 CRC、签名和运行时符号探测检查器 |
| root/能力/策略 | 取得 FD 检查 euid root；后续操作还需 CAP_SYS_PTRACE 和相关 LSM 授权，seccomp/策略也可能阻止 reboot 入口；已加载不等于应用一定连接成功 |
| 可选后端 | task_call_func 缺少时 FP 返回 EOPNOTSUPP；fget_task/vfs_getattr 缺少时 FD 大小不可用；缺少补页或可执行页缓存接口时相关请求受限 |
| GPU | 仅 KGSL 路径，匹配文件回调、模块归属及三条 opcode 特征；它不等于完整厂商 ABI 验证，不覆盖 Mali/所有 Adreno 驱动构建。GET_ABI 的 GPU_INFO 只检查前置符号，不代表每次 vendor 调用都能成功 |
| 终端过滤 | 功能位无条件发布，但 ENABLE 才真正注册两个 security_* 探针；目标符号不可探测时 ENABLE 仍会失败。默认关闭，非加载时必需后端 |
| PID 查找 | 512 个候选上限在 SDK 中已有说明，超过就返回 E2BIG；这是已公开的功能限制，不能说已完整扫描任意规模系统 |
| SO 基址 | 只接受文件起始映射 vm_pgoff==0 和指定 basename/版本后缀；不是所有 APK 内嵌库布局都能按此语义取得基址 |
| 写内存 | 驻留且可写的普通页路径；不为未驻留写入主动补页、不处理 COW 写故障，原 smoke 明确检查了非驻留写入不补页。不能与普通用户态写或 process_vm_writev 的功能覆盖等同 |
| write-only VMA 读取 | 期间新增的 VM_READ/VM_WRITE 分支支持 ARM64 已驻留 write-only 普通映射读取，未修改 VMA 权限。冷页仍经无 WRITE 标志的 fixup_user_fault；本地内核此分支要求 VM_READ，因此不能外推为冷 write-only 页也可补页读取 |
| 内存快照 | 批量读保持同一个 task/mm 引用，但不停止目标线程；数据不具备跨项原子快照语义 |

本轮未把上述明确返回 EOPNOTSUPP/E2BIG/ENODATA 的路径描述为“必然导致死机”。API 契约不匹配、生命周期竞态与正常的不支持返回需要分开处理。

## 本轮实际验证

| 检查 | 结果 |
| --- | --- |
| 单头 C11/C++17、ABI 对比 | 通过 |
| readv 实际调度函数 synthetic 测试 | 通过 |
| metadata 实际分发函数 synthetic 测试 | 通过 |
| mapping synthetic 测试 | 通过 |
| ABI、PID/线程排序、devpts/query/metadata SDK 检查 | 通过 |
| 起始版本 `tests/test_walk.py` | 失败，缺少已经合并的 nbdriver_walk.c |
| 当前快照 `tests/test_walk.py` | 已通过，期间工作区更新补上了真实源码段提取 |
| 新增读取权限测试、更新后 readv/metadata/mapping、设备读取程序语法 | 通过；对期间变更重新回归 |
| 审查副本中适配合并路径后的 walker | 通过，PAGE_SHIFT 12/14/16；仅模拟分支测试 |
| argv[0] 完整前缀、后续页失败探针 | 复现 F07 的漏匹配行为 |
| 当前源码 ARM64 android15-6.6 编译 | 严格 modpost、W=1、声明/栈帧检查通过；无编译警告；因没有 vmlinux 跳过 BTF |
| 6.6 静态导入符号检查 | 68 项导入，UNRESOLVED: none |
| 其他当前源码 KMI / 厂商模块加载 | 本轮未构建/未上机验证 |
| KASAN/KCSAN/LOCKDEP/并发卸载/旧 CFI 恢复故障注入 | 本轮未运行 |

`host-results.json` 保留起始版本 15 条实际主机命令，14 条退出 0，一条原样 walker 测试退出 1；`current-host-results.json` 记录更新后快照的 6 条回归命令。初始与当前快照分别构建，完整 stdout/stderr 和命令在 `kernel-build-*`、`undefined-*` 及带 `current-` 前缀的对应文件中。没有把安装工具、编译完成或模拟测试通过写成实机无死机。

两阶段验证模块的 vermagic 与 SHA256 见审查产物 `evidence-01.stdout`、`current-modinfo.stdout` 和 `audit-summary.json`。它只是审查构建产物，不是已获全部设备验证的刷入包。

## 建议修复顺序

1. 闭合 task_work/模块卸载生命周期；去掉全局 CFI slowpath 改写及其错误恢复缺口。
2. 修正 GPU 模块查询的上下文契约、写内存的 LSM 操作模式。
3. 修复 PID 对象匹配、argv[0] 部分读取判断；保留已修复的页表测试路径回归。
4. 统一命名空间编号、线程保存态/采样语义及无效/重复代码。
5. 修正版本记录，再构建当前哈希的目标 KMI 矩阵；在可恢复测试设备上验证卸载竞争、锁检查及失败路径。

本次审查没有覆盖写入驱动实现，无需执行审查代码恢复；期间其他工作区更新保留在当前快照中，未回退。两阶段副本和探针均独立保存在 `out/code-audit-20261114/`。后续修复应针对 `current-sha256.json` 对应的源码，不使用其他历史模块的测试结果替代。
