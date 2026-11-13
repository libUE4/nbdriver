# NBG-driver 进程、SO、线程、寄存器及上下文接口

2026-11-13，按当前工作区源码静态核对，未新增实机验证。表中函数是内核实现，用户态应通过已认证的 socket ioctl 协议调用。

| 获取内容 | 内核函数 | 命令/入口 | 返回数据与限制 |
|---|---|---|---|
| 按完整 argv[0] 查目标 PID | [find_pid_by_cmdline_ranked](/root/workspace/NBG-driver/src/core/process.c:235) | `OP_FIND_PID` | 当前 OP_FIND_PID 实际调用此函数，精确匹配 argv[0]；同名候选依次按线程数、RSS、PID 排序。候选扫描有固定上限，argv 读取不主动补页。 |
| 其他内核进程查找辅助 | [find_pid_by_cmdline](/root/workspace/NBG-driver/src/core/process.c:179) | `内核内部` | 按 argv[0] 或其 basename 匹配，返回首个目标；另有 find_pid_by_name 短名/长名兼容查找，不能与当前 OP_FIND_PID 调度混为一谈。 |
| 按 comm 查单个目标 PID | [nbg_query_pid](/root/workspace/NBG-driver/src/core/query.c:75) | `OP_GET_PID` | 匹配 task->comm，返回首个进程组长 PID；与完整 argv[0] 查找语义不同。 |
| 枚举同名目标进程 | [nbg_query_pids](/root/workspace/NBG-driver/src/core/query.c:104) | `OP_GET_PIDS` | comm 匹配；返回 out_pids/out_count/out_total，内核数组最多 64 项。 |
| 目标 SO 基址（原接口） | [get_module_base](/root/workspace/NBG-driver/src/core/process.c:45) | `OP_MODULE_BASE` | 完整 basename 或受限数字版本后缀匹配，只接受 vm_pgoff==0 的文件 VMA。 |
| 目标 SO 基址和跨度 | [nbg_query_module_base](/root/workspace/NBG-driver/src/core/query.c:233) | `OP_MODULE_BASE_EX` | 文件名子串匹配，返回首个匹配 VMA 起点与最后一个匹配 VMA 末端的跨度。 |
| 过滤大 VMA 后查 SO | [nbg_query_module_base2](/root/workspace/NBG-driver/src/core/query.c:252) | `OP_MODULE_BASE2_EX` | max_vma_size 非零时过滤超过限制的单个映射。 |
| 目标 SO 完整路径 | [nbg_query_module_path](/root/workspace/NBG-driver/src/core/query.c:271) | `OP_MODULE_PATH_EX` | 返回首个匹配 VMA 的完整 d_path。 |
| 目标进程映射列表 | [ro_maps_ioctl](/root/workspace/NBG-driver/src/core/readonly.c:254) | `NBG_RO_MAPS` | 输出 maps 格式文本、长度、进程启动时间；最大容量 4 MiB。 |
| 目标任务 TLS | [nbg_query_tls](/root/workspace/NBG-driver/src/core/query.c:296) | `OP_GET_TLS` | 读取指定 PID/TID 保存的 TPIDR_EL0。 |
| 按名称选择目标线程 | [nbg_query_thread_tid](/root/workspace/NBG-driver/src/core/query.c:361) | `OP_GET_THREAD_TID` | 匹配目标线程组内非组长线程的 comm；优先 on_cpu，再累计执行时间等条件。 |
| 枚举全部同名线程 | [nbg_query_thread_tids](/root/workspace/NBG-driver/src/core/query.c:415) | `OP_GET_THREAD_TIDS` | 返回 TID、on_cpu/running 标志、exec_ns、总数；排除组长，最多保存 64 条。 |
| 目标线程栈 | [nbg_query_thread_stack](/root/workspace/NBG-driver/src/core/query.c:486) | `OP_GET_THREAD_STACK` | 查询 Bionic stack_and_tls:<tid> 命名 VMA，返回上下界和 TLS，受配置与映射命名约束。 |
| 只读查线程及身份 | [ro_thread_ioctl](/root/workspace/NBG-driver/src/core/readonly.c:297) | `NBG_RO_THREAD：FIND / IDENTITY` | FIND 按线程名找 TID，含组长；IDENTITY 校验 PID/TID 所属关系，返回 comm、进程及线程启动时间。 |
| 目标线程系统调用位置 | [ro_thread_ioctl](/root/workspace/NBG-driver/src/core/readonly.c:297) | `NBG_RO_THREAD：SYSCALL` | 调用 task_current_syscall，返回 syscall_number、PC、SP 和身份；不导出完整通用寄存器，采样可能暂不可用。 |
| 目标线程 TLS/PAC 环境 | [comm_ioctl_thread_env](/root/workspace/NBG-driver/src/comm/socket_ioctl.c:1103) | `OP_THREAD_ENV` | 输入 PID+TID 并校验归属；返回 tls、APGAKey lo/hi 和 valid 位。读取的是内核保存状态，不包含通用寄存器。 |
| 目标线程浮点控制状态 | [ro_thread_fpstate_ioctl](/root/workspace/NBG-driver/src/core/readonly.c:135) | `NBG_RO_THREAD_FPSTATE` | 通过 task_call_func 采样 TLS、FPSR、FPCR、启动时间、采样时间；不是 Q0–Q31 数据。目标为 current 或 on_cpu 时可返回 EAGAIN。 |
| 一次性通用寄存器采样 | [hwbp_capture_control](/root/workspace/NBG-driver/src/hwbp/hwbp_capture.c:378) | `OP_HWBP_CAPTURE` | ARM/READ/CANCEL；只允许验证为 BR X8 的指令处。返回 X0–X30、SP、PSTATE、PC、分支目标、命中 TID/时间；v2 可附带部分指针内容和栈字节，没有 Q 寄存器数组。 |
| 断点/观察点事件 | [watch_read_kernel](/root/workspace/NBG-driver/src/hwbp/hwbp_logger.c:340) | `OP_WATCH_READ` | 读取 WATCH_EVENT：watch_addr、PC、PID/TID、类型和次数；不是完整寄存器快照。 |
| 目标线程 ioctl 上下文会话 | [comm_ioctl_target_thread_session](/root/workspace/NBG-driver/src/comm/socket_ioctl.c:1073) | `OP_TARGET_THREAD_IOCTL_SESSION` | BEGIN/FINISH/ABORT；用户提供寄存器/栈镜像，驱动在目标 task_work 中临时设置现场执行指定 ioctl 并恢复。这是执行会话，不是读取目标已有完整上下文。 |
| 地址对应物理映射 | [nbg_query_mapping](/root/workspace/NBG-driver/src/core/query.c:707) | `OP_QUERY_MAPPING` | 返回物理地址、页大小和映射标志。 |
| 地址区间与访问状态 | [nbg_query_access](/root/workspace/NBG-driver/src/core/query.c:783) | `OP_QUERY_ACCESS` | 返回 VMA 上下界、权限、驻留状态。 |
| 目标线程信号配置 | [nbg_query_signal](/root/workspace/NBG-driver/src/core/query.c:807) | `OP_QUERY_SIGNAL` | 返回信号 handler/flags/restorer、掩码、备用信号栈和 TGID。 |
| 使用目标 PAC 密钥计算 | [nbg_query_pacga](/root/workspace/NBG-driver/src/core/query.c:884) | `OP_EXECUTE_PACGA` | 返回 PACGA 运算结果及 supported 状态。 |
| 目标 FD 元数据 | [ro_fd_ioctl](/root/workspace/NBG-driver/src/core/readonly.c:341) | `NBG_RO_FD` | 返回文件大小、设备、inode、mode、uid/gid、时间等，并带进程启动时间。 |
| 目标映射的底层文件内容 | [ro_file_ioctl](/root/workspace/NBG-driver/src/core/readonly.c:401) | `NBG_RO_FILE` | 读取已有 VMA 的 backing file；与读取运行时目标内存语义不同。 |
| 读取内部绑定目标 PID | [nbg_target_bound_pid](/root/workspace/NBG-driver/src/core/target.c:396) | `仅内核内部` | 返回内部目标绑定 PID；相关 nbg_target_bind_pid、nbg_target_task_is_game 和自动发现用于目标生命周期管理。 |

**已有用户态封装的位置**

NBG-driver 当前这些源码下未发现独立的通用 SDK；消费方 NBG-Draw-dfm 的 kernel_rw.cpp 已封装下列接口，不能把消费方类成员误写成内核函数：

| 用户态类成员 | 实际命令 | 作用 |
|---|---|---|
| [GetPid](/root/workspace/NBG-Draw-dfm/jni/src/driver/kernel_rw.cpp:968) | `OP_FIND_PID` | 完整 argv[0] 目标进程查询 |
| [GetModuleBase](/root/workspace/NBG-Draw-dfm/jni/src/driver/kernel_rw.cpp:981) | `OP_MODULE_BASE` | SO 基址 |
| [getThreadId](/root/workspace/NBG-Draw-dfm/jni/src/driver/kernel_rw.cpp:1011) | `OP_GET_THREAD_TID` | 按名称查询线程 |
| [getProcessStartTime](/root/workspace/NBG-Draw-dfm/jni/src/driver/kernel_rw.cpp:993) | `NBG_RO_THREAD/IDENTITY` | 进程启动时间 |
| [readThreadEnvironment](/root/workspace/NBG-Draw-dfm/jni/src/driver/kernel_rw.cpp:657) | `OP_THREAD_ENV` | TLS/APGA 环境 |
| [ReadThreadFloatingState](/root/workspace/NBG-Draw-dfm/jni/src/driver/kernel_rw.cpp:676) | `NBG_RO_THREAD_FPSTATE` | TLS/FPSR/FPCR 等状态 |
| [getProcessMaps](/root/workspace/NBG-Draw-dfm/jni/src/driver/kernel_rw.cpp:1049) | `NBG_RO_MAPS` | 映射文本 |
| [getFileDescriptorSize](/root/workspace/NBG-Draw-dfm/jni/src/driver/kernel_rw.cpp:1071) | `NBG_RO_FD` | 目标 FD 大小 |
| [queryMapping](/root/workspace/NBG-Draw-dfm/jni/src/driver/kernel_rw.cpp:1026) | `OP_QUERY_MAPPING` | 物理映射 |
| [targetThreadIoctlSession](/root/workspace/NBG-Draw-dfm/jni/src/driver/kernel_rw.cpp:756) | `OP_TARGET_THREAD_IOCTL_SESSION` | 目标线程执行会话 |
| [watchRead](/root/workspace/NBG-Draw-dfm/jni/src/driver/kernel_rw.cpp:956) | `OP_WATCH_READ` | 断点事件 |
| [queryHwbpInfo](/root/workspace/NBG-Draw-dfm/jni/src/driver/kernel_rw.cpp:570) | `OP_HWBP_INFO` | BRP/WRP 数量 |
| [queryHwbpCaps](/root/workspace/NBG-Draw-dfm/jni/src/driver/kernel_rw.cpp:579) | `OP_HWBP_CAPS` | 调试功能/可用槽位能力 |

**边界说明**

- FPSTATE 名称不代表完整浮点寄存器文件：当前只读 ABI 没有 Q0–Q31 字段。
- WATCH_READ 返回事件摘要，不能替代 HWBP_CAPTURE 的 X 寄存器采样。
- HWBP_CAPTURE 是限定 BR X8 指令的触发式采样，没有发现任意时刻导出全套 CPU/浮点现场的通用 get_context 命令。
- 只读接口还执行目标身份、CAP_SYS_PTRACE 与 ptrace/LSM 访问检查，且部分查询依赖动态解析的内核函数；命令存在不代表所有设备上都可用。
- OP_HWBP_INFO / OP_HWBP_CAPS 的处理逻辑位于 socket_ioctl.c 的分发 case 内，不是独立同名内核函数。
