# 进程、SO、线程、寄存器和上下文函数清单

基于当前工作区源码静态核对，2026-11-13。未运行设备验证；接口存在不代表全部路径已经实机通过。

进程名称匹配需区分 task->comm 与完整包名/cmdline；CPU 现场还需区分主动查询、断点命中快照和内部远程调用保存。

**enkernel**

| 功能 | 函数及源码 | 内核实现或请求入口 | 实际结果与限制 |
|---|---|---|---|
| 查单个目标进程 | [get_process_id](/root/workspace/enkernel/sdk/include/dfm/driver.h:32) | `mem_get_pid` | 按 task->comm 匹配进程组长，返回首个匹配 PID；不是解析完整包名或 cmdline。 |
| 枚举同名进程 | [get_process_ids](/root/workspace/enkernel/sdk/include/dfm/driver.h:38) | `mem_get_pids` | 返回 PID 数组、实际返回数量和截断状态；内核数组最多 64 个。旧模块回退单 PID 时标记 truncated。 |
| 目标 SO 基址及跨度 | [get_module_base](/root/workspace/enkernel/sdk/include/dfm/driver.h:40) | `mem_get_module_base` | 文件名子串匹配，返回首个匹配 VMA 起点和最后匹配 VMA 末端之间的跨度。 |
| 过滤超大 VMA 后查 SO | [get_module_base2](/root/workspace/enkernel/sdk/include/dfm/driver.h:44) | `mem_get_module_base2` | max_vma_size 过滤单个 VMA，0 表示不限制。 |
| 目标 SO 完整路径 | [get_module_path](/root/workspace/enkernel/sdk/include/dfm/driver.h:49) | `mem_get_module_path` | 返回首个匹配映射的 d_path，可用于区分相同 SO 文件名的安装路径。 |
| 按名称查目标线程 | [get_thread_tid](/root/workspace/enkernel/sdk/include/dfm/driver.h:51) | `mem_get_thread_tid` | 在目标线程组的非组长线程中匹配 comm；优先 on_cpu，再按运行时间等排序。 |
| 枚举同名目标线程 | [get_thread_tids](/root/workspace/enkernel/sdk/include/dfm/driver.h:55) | `mem_get_thread_tids` | 返回 TID、调度状态标志和累计执行时间；不包括组长，内核最多 64 条，并报告截断。 |
| 线程栈和 TLS 区间 | [get_thread_stack](/root/workspace/enkernel/sdk/include/dfm/driver.h:58) | `mem_get_thread_stack` | 查询 Bionic stack_and_tls:<tid> 命名 VMA 的上下界与 TLS，依赖对应内核配置和命名。 |
| 指定任务 TLS | [get_tls](/root/workspace/enkernel/sdk/include/dfm/driver.h:50) | `mem_get_tls` | 传入 PID/TID，读取任务保存的 TPIDR_EL0 值；不是完整线程寄存器快照。 |
| 硬件断点资源 | [hwb_info](/root/workspace/enkernel/sdk/include/dfm/driver.h:75)、[hwb_slots](/root/workspace/enkernel/sdk/include/dfm/driver.h:76) | `dfm_hwb_info / dfm_hwb_slots` | 查询槽位能力、目标线程 perf 断点和调试寄存器；调试寄存器数组须检查 REGS_VALID 标志。 |
| 断点命中时的寄存器与现场 | [hwb_set](/root/workspace/enkernel/sdk/include/dfm/driver.h:77)、[hwb_read](/root/workspace/enkernel/sdk/include/dfm/driver.h:78) | `dfm_hwb_set / dfm_hwb_read / dfm_hwb_record_hit` | 先配置断点，再读取命中记录；含 X0–X30、PC/LR/SP/PSTATE、Q0–Q31、FPSR/FPCR、ESR、命中地址与次数。 |
| 虚拟地址映射 | [query_mapping](/root/workspace/enkernel/sdk/include/dfm/driver.h:111) | `mem_query_mapping` | 查询物理地址、页大小及映射标志。 |
| 地址访问状态 | [query_access](/root/workspace/enkernel/sdk/include/dfm/driver.h:112) | `mem_query_access` | 查询 VMA 起止、R/W/X/共享权限，以及驻留/未驻留等状态。 |
| 线程信号现场配置 | [query_signal](/root/workspace/enkernel/sdk/include/dfm/driver.h:121) | `mem_query_signal` | 查询指定 TID 的信号处理函数、flags/restorer、掩码、备用信号栈和 TGID；不是完整 CPU 上下文。 |
| 使用目标任务 PAC 上下文计算 | [execute_pacga](/root/workspace/enkernel/sdk/include/dfm/driver.h:64) | `mem_execute_pacga` | 用目标任务 APGAKey 执行 PACGA 并返回结果，不直接返回 PAC 密钥。 |
| 目标 PID/FD 的 GPU 查询 | [query_kgsl](/root/workspace/enkernel/sdk/include/dfm/driver.h:73) | `kgsl_query` | 按操作查询目标已有 KGSL FD/GPU 对象等数据，不是 CPU 线程寄存器接口。 |
| 内部内存读取上下文 | [mem_read_ctx_init_pid](/root/workspace/enkernel/mem.c:1374)、[mem_read_ctx_init_current](/root/workspace/enkernel/mem.c:1399)、[mem_read_ctx_read](/root/workspace/enkernel/mem.c:1421)、[mem_read_ctx_release](/root/workspace/enkernel/mem.c:1440) | `同左，仅内核内部` | 管理 mm_struct 引用和内存读取生命周期；名称中的 ctx 不代表寄存器上下文。 |

**LinyuKernel / LingYuKernel**

| 功能 | 函数及源码 | 内核实现或请求入口 | 实际结果与限制 |
|---|---|---|---|
| 查单个目标进程 | [lingyu_find_process](/root/workspace/LinyuKernel/LingYuKernel/sdk/lingyu_kernel_sdk.h:263) | `linyu_process_find → linyu_find_process_by_name` | LINYU_FIND_PROCESS；精确匹配 task->comm，返回首个 PID。 |
| 给定 PID/TID 获取任务对象 | [linyu_get_target_task](/root/workspace/LinyuKernel/LingYuKernel/src/process.c:6) | `同左，仅内核内部` | 通过 PIDTYPE_PID 取得带引用的 task_struct；不是按线程名查询。 |
| 枚举进程 | [linyu_process_list](/root/workspace/LinyuKernel/LingYuKernel/src/process.c:198) | `同左` | LINYU_LIST_PROCESSES；返回进程组长 PID 列表，当前上限 4096；SDK 没有专用包装函数。 |
| 查询进程存活 | [linyu_process_is_alive](/root/workspace/LinyuKernel/LingYuKernel/src/process.c:184)、[linyu_is_pid_alive](/root/workspace/LinyuKernel/LingYuKernel/src/process.c:123) | `同左` | LINYU_IS_PROCESS_ALIVE；返回 alive。 |
| 查询进程信息 | [linyu_process_get_info](/root/workspace/LinyuKernel/LingYuKernel/src/process.c:238) | `同左` | LINYU_GET_PROCESS_INFO；返回 comm、map_count、total_vm。total_vm 是页计数，不是字节数。 |
| 目标 SO 基址 | [lingyu_get_module_base](/root/workspace/LinyuKernel/LingYuKernel/sdk/lingyu_kernel_sdk.h:279)、[linyu_get_module_base](/root/workspace/LinyuKernel/LingYuKernel/src/process.c:20) | `linyu_process_get_module_base` | LINYU_GET_MODULE_BASE；精确匹配映射文件 basename，返回首个映射起点。当前 ioctl 实现固定 size=0。 |
| 目标 SO 后接 BSS 区域 | [linyu_get_module_base_bss](/root/workspace/LinyuKernel/LingYuKernel/src/process.c:57)、[linyu_process_get_module_base_bss](/root/workspace/LinyuKernel/LingYuKernel/src/process.c:168) | `同左` | LINYU_GET_MODULE_BASE_BSS；按最后一个同名文件 VMA 后紧邻的匿名 VMA 识别 BSS，并非解析 ELF 符号。 |
| 绑定指定目标进程的 FD | [linyu_bind_proc](/root/workspace/LinyuKernel/LingYuKernel/src/entry.c:259) | `同左` | LINYU_BIND_PROC；校验目标 PID 并返回专用匿名 FD，属于目标绑定接口。 |
| 虚拟地址转物理地址 | [linyu_translate_process_vaddr](/root/workspace/LinyuKernel/LingYuKernel/src/memory.c:129)、[linyu_memory_vaddr_translate](/root/workspace/LinyuKernel/LingYuKernel/src/memory.c:501)、[linyu_memory_at_s1e0r](/root/workspace/LinyuKernel/LingYuKernel/src/memory.c:533) | `同左` | LINYU_VADDR_TRANSLATE / LINYU_AT_S1E0R；当前后者也调用软件地址转换函数，不应按函数名认定执行了 AT 指令。 |
| 查询页信息和 PTE | [linyu_memory_get_page_info](/root/workspace/LinyuKernel/LingYuKernel/src/memory.c:550)、[linyu_memory_get_pte_mapping](/root/workspace/LinyuKernel/LingYuKernel/src/memory.c:569)、[linyu_memory_page_table_walk](/root/workspace/LinyuKernel/LingYuKernel/src/memory.c:620) | `同左` | GET_PAGE_INFO 当前 flags 固定 0；PTE_MAPPING 返回读取的 PTE；PAGE_TABLE_WALK 当前两个输出字段均填物理地址，并非真实 PGD/PTE 值。 |
| 硬件断点信息与局部现场采样 | [linyu_hwbp_info](/root/workspace/LinyuKernel/LingYuKernel/src/hwbp.c:344)、[linyu_sample_hbp_handler](/root/workspace/LinyuKernel/LingYuKernel/src/hwbp.c:262) | `同左` | LINYU_HWBP_INFO；采样函数能写 PC/SP/PSTATE/X0–X7，但全项目仅发现其声明和定义，未找到注册或调用点，不能据此认定采样链已接通。 |
| 远程调用时保存现场 | [linyu_rc_tw_callback](/root/workspace/LinyuKernel/LingYuKernel/src/remote_call.c:199) | `linyu_remote_call_ioctl / linyu_remote_call_batch_ioctl 的内部回调` | 在目标任务执行时保存 task_pt_regs 和 FPSIMD 状态，用于远程调用流程；不是用户态 get_context 接口。 |
| 远程调用后恢复现场 | [linyu_rc_try_restore_context](/root/workspace/LinyuKernel/LingYuKernel/src/remote_call.c:327) | `同左` | 内部恢复 pt_regs/FPSIMD，safe_signal.c 中存在调用；不是任意读取或写回目标上下文的 SDK 接口。 |
| 系统调用参数与返回记录 | [linyu_syscall_trace_ctl](/root/workspace/LinyuKernel/LingYuKernel/src/syscall_trace.c:225)、[linyu_syscall_trace_list](/root/workspace/LinyuKernel/LingYuKernel/src/syscall_trace.c:282) | `entry_common / ret_common 采集` | LINYU_SYSCALL_TRACE_CTL/LIST；当前注册 openat/close/read/write，记录 TID、编号、参数、返回值与耗时。 |

**lsdriver**

| 功能 | 函数及源码 | 内核实现或请求入口 | 实际结果与限制 |
|---|---|---|---|
| 给定 PID/TID 获取任务对象 | [get_task_by_pid](/root/workspace/lsdriver/export_fun.h:350) | `仅内核内部` | 返回带引用的 task_struct；不是按名称找 PID。 |
| 获取进程地址空间 | [get_mm_by_pid](/root/workspace/lsdriver/export_fun.h:361) | `仅内核内部` | 取得 mm_struct，用于 VMA 和内存读写。 |
| 枚举目标 SO/分段/可扫描区域 | [virtual_memory_enum](/root/workspace/lsdriver/virtual_memory_enum.h:126) | `request_op_vmem_info` | 输入 TGID，输出 modules/segment_info/regions；包含文件模块、BSS 和 RWX 伪模块。文件模块受当前 /data/ 等过滤逻辑约束。 |
| 按目标线程名查 TLS | [get_tpidr_el0_by_name](/root/workspace/lsdriver/arm64_env.h:18) | `经 get_env_params 间接使用` | 输入 TGID 和线程名，遍历线程并读取 TPIDR_EL0；不返回匹配线程的 TID。 |
| 读取目标 PAC 密钥 | [get_pacga_key](/root/workspace/lsdriver/arm64_env.h:42) | `经 get_env_params 间接使用` | 返回目标任务保存的 APGAKey lo/hi，依赖 CONFIG_ARM64_PTR_AUTH。 |
| 获取目标环境参数 | [get_env_params](/root/workspace/lsdriver/arm64_env.h:77) | `request_op_env_get_params` | 返回 TLS、PAC lo/hi 和各自状态；整体成功可以只代表其中一项成功，须分别检查 tls_status/pacga_status。 |
| 硬件断点采集入口 | [set_process_hwbp](/root/workspace/lsdriver/break_point.h:619) | `request_op_hwbp_set` | 安装采集回调，按目标 TGID 和线程作用域在命中时采集。 |
| PTE/影子页表/单步采集入口 | [set_process_ptebp](/root/workspace/lsdriver/break_point.h:641)、[set_process_dptdbg](/root/workspace/lsdriver/break_point.h:659)、[set_process_stepbp](/root/workspace/lsdriver/break_point.h:677) | `request_op_ptebp_set / dptdbg_set / stepbp_set` | 共享断点记录和采样回调。当前 PTE 入口实际包含 arm64_ptedbg-20260822-000439.h。 |
| 读取/选择性更新命中现场 | [sample_hbp_handler](/root/workspace/lsdriver/break_point.h:210)、[bp_record_read_all](/root/workspace/lsdriver/break_point.h:195) | `断点内部回调，数据位于共享 bp_info.points[].records[]` | PC/LR/SP/PSTATE、X0–X29、orig_x0/syscallno、Q0–Q31、FPSR/FPCR；LR 即 X30。按掩码支持记录或写回寄存器。 |
| 系统调用事件现场 | [syscall_monitor_install](/root/workspace/lsdriver/arm64_syscall_monitor.h:589)、[syscall_monitor_do_el0_svc_hook_work](/root/workspace/lsdriver/arm64_syscall_monitor.h:557) | `request_op_syscall_monitor_set` | 在 syscall 入口分析寄存器参数并写内核日志；无独立的用户态 get_context 返回接口。 |
| 目标计时器读取监控 | [cntvct_monitor_install](/root/workspace/lsdriver/arm64_cntvct_monitor.h:172) | `request_op_cntvct_monitor_set` | 跟踪目标读取 CNTVCT_EL0 的现场/PC 范围，属于事件监控而非通用寄存器查询。 |

**当前未发现的独立查询接口**

- LinyuKernel：未发现按线程名返回 TID、枚举目标线程、查询线程栈、读取 TLS 或导出完整线程 CPU 上下文的专用 ioctl/SDK 函数；不要将远程调用内部保存直接算作 get_context 能力。
- lsdriver：当前共享 request_op 中未发现按名称查目标 PID、列出目标线程或按线程名返回 TID 的通用请求；线程名查找用于 TLS 获取。
- enkernel：完整通用/浮点寄存器记录来自断点命中采集，未发现不依赖命中、任意时刻导出整套线程 CPU 寄存器的 get_context 接口。

LinyuKernel 除 lingyu_find_process、lingyu_get_module_base 等现成包装外，其余已接入的 ioctl 可以通过 lingyu_kernel_ioctl(fd, 命令, &请求) 调用。lsdriver 表中的内核函数不能直接在用户态 include 后调用，需要通过共享请求协议。
