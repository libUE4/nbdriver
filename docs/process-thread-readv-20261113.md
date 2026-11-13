# 进程、线程环境及批量读迁移记录

状态：源码实现与本地检查通过；完整 DDK 模块链接及实机验证待完成。
本轮范围为迁移计划的阶段 1、3，以及阶段 2 中的批量读。未修改 NBG-Draw-dfm。

**已实现**

- 精确完整 argv[0] 查 PID，保留线程数、RSS、PID 的候选排序。
- 精确 SO 文件名/数字版本后缀、vm_pgoff==0 的模块起始映射查询。
- start_boottime 进程启动身份查询，与现有读写 expected_start_ns 一致。
- 非组长线程按 comm 查找，保留 on_cpu、累计执行时间、TID 的选择顺序。
- PID/TID 归属和可选进程/线程启动身份校验；读取 TLS/APGA 及各自 valid 位。
- 非运行目标线程的保存状态采样：TLS、FPSR/FPCR、身份和采样时间。task_call_func 不可用则不发布相应能力；运行中或 current 目标返回 EAGAIN。
- 批量读：最多 1024 项，每项最多 512 KiB，总有效数据最多 16 MiB；每块最多 64 KiB。整批引用同一个 task/mm，单项错误不阻止后续项；输出 copied/error/failed_address 和成功项计数。

ABI 仍为 5，原有读写结构和命令保持不变。新增命令为 0x10–0x16，GET_ABI 新增能力位；C/C++ 独立单头同步维护。
所有新增查询沿用 CAP_SYS_PTRACE 及 ptrace/LSM 访问判断。元数据查询持有 exec_update_lock，批量读通过既有 mm_access 取得访问权限和地址空间引用。

本次未新增驻留页读命令。驻留页读指不触发补页，只读取当前存在可读物理映射的页，主要避免扫描等待补页。批量读沿用现有普通读的补页行为。PID 查找内部对 argv 页采用不补页读取，以保持旧查找语义。

**真实验证**

- 主机：原单头 C11/C++17 多文件链接及模拟行为、旧读写 smoke 构建、共享 ABI 大小/偏移、单头 ABI 对比、页表遍历测试通过。
- 新 SDK：PID/SO/身份/线程环境/FP/批量入口的 C/C++ 参数封装和错误返回测试通过。
- 选择规则：直接运行内核所用的 PID/线程排序和 SO 精确匹配 helper，覆盖错误库名前缀/后缀。
- 批量调度：执行 src/nbdriver_core.c（read 段） 中实际批量函数体，替换 task/mm、copy 和页读取依赖，覆盖 512 KiB 分块、部分失败、混合成功/失败、身份不符、容量限制及引用回收。该测试不等价于设备页表测试。
- 内核：以本地 /opt/ddk/src/android15-6.6 的 GKI 配置准备独立构建目录，Clang 18 编译并链接 nbdriver.o 通过。新增外部符号全部出现在此源码的 KMI 列表中。
- 新设备探针已编译为 Android API24 ARM64 ELF；仅生成，尚未执行。探针检查自身唯一 argv[0]、libc.so 的 ELF 头、命名线程、保存的 FPSR 标志以及 512 KiB 混合批量读取。

完整命令、实际输出、退出状态、构建前后版本、校验和与补丁位于 out/process-thread-readv-20261113/。
其中 checks.json 汇总主机检查；kernel-objects.log 为内核对象构建日志；symbol-delta.json 为源码 KMI 列表对比。
本地没有匹配 DDK Module.symvers/CRC，因此没有把 nbdriver.o 或任何未完成 modpost 的文件宣称为可加载模块；其他 KMI 和目标设备均待验证。

**运行设备探针**

将本次源码在匹配目标设备的完整 DDK 中构建为模块并加载后，以有权限的用户运行：

```sh
/data/local/tmp/device_query_readv_smoke
```

探针可执行文件在 out/process-thread-readv-20261113/device_query_readv_smoke。
它只检查自身创建的测试进程/线程和自身缓冲区，不修改其他进程。
没有设备时不可将探针编译成功写成实机通过。

**来源与边界**

选择与查询逻辑参考用户工作区 NBG-driver（当前提交 bfd65b01995b844062e486af6ea20591d1ed7f58）的 src/core/process.c、src/core/query.c、src/core/readonly.c、src/comm/socket_ioctl.c；原文件哈希见本轮 source-reference.sha256。
保留其算法含义，未迁入 socket 通信、断点或业务 hook。
相较旧实现，超出 512 个进程候选时返回 E2BIG；未找到和权限/身份错误均明确返回，不假装完整扫描成功。
FP 控制状态不包含 Q0–Q31；线程环境与批量数据不是全寄存器或目标内存的原子快照。
原有 tests/nbdriver_smoke.c 用户修改保持字节不变。

**恢复**

本轮原件位于 out/process-thread-readv-20261113/before/；最终源码副本保存在 work/。
changes.patch、manifest.json、before.sha256、after.sha256 记录修改范围和文件版本。
restore.py 可按 manifest 恢复已有文件并移除本轮新增文件，且会先核对当前文件哈希；已在额外恢复副本验证。
