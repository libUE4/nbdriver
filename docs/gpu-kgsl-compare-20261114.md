# 四份驱动的进程与 GPU/KGSL 源码对比

日期：2026-11-14。对象是当前工作区的 enkernel、LinyuKernel/LingYuKernel、lsdriver、NBG-driver。

本次状态：源码分析完成；没有为本次比较编译、加载或切换这四份模块，也没有完成跨设备功能验证。以下兼容性判断是实现依据上的相对评价。此前 nbdriver 的设备测试不能证明这四份源码的兼容性。

已保存 24 个重点源文件的副本与 SHA256：`../out/gpu-compare-20261114/manifest.json`；可重复的静态检查命令、输出与退出状态在同目录 `source-checks.json`。未修改四份驱动源码。

## 结论

| 比较范围 | 当前源码结论 |
| --- | --- |
| 完整包名定位进程、SO、maps、线程等常规查询 | NBG-driver 覆盖较全面，尤其具有完整 argv[0] 路径；enkernel 的线程、模块等查询也较丰富 |
| GPU 对象元数据与 syncsource 查询 | enkernel 的适配与校验设计最完整；NBG-driver 也有真实实现，但指令位置指纹限制明显 |
| KGSL 进程节点处理 | NBG-driver 覆盖新建与已有节点，覆盖面较广，但恢复有缺口；enkernel 有不同的自动拦截路线；lsdriver 实现了新建节点拦截；当前 LinyuKernel 只有登记 PID 的接口 |
| Linux 5.x/6.x 通用适配基础 | NBG-driver 有较系统的版本兼容层；enkernel、lsdriver 也有版本分支；当前 LinyuKernel 的进程 VMA 遍历直接依赖新接口 |
| 已证明跨厂商、跨内核无故障 | 本次四者都没有这项证据，不能给出这样的结论 |

## 1. enkernel

### 进程查询

`mem.c:36` / `:67` 按 comm 查一个或多个进程；模块基址、大小、路径以及线程查找、枚举、栈查询也有实现。comm 受 TASK_COMM_LEN 限制，不能直接承担任意完整 Android 包名的精确匹配。

`mem.c:102` 的模块查询使用文件名子串匹配并汇总映射范围，存在相似名称串入的可能；与 NBG-driver 的文件起始映射、较精确名称匹配策略不同。TLS 等保存状态读取也不能一概视为正在运行线程的实时寄存器快照。

### GPU/KGSL 查询

入口是 `kgsl.c:1419` 的 `kgsl_query()`，提供验证目标 FD、syncsource peek、GPUOBJ_INFO。基本思路是持有目标 KGSL file 引用，再读取其对应的厂商数据。

- `kgsl.c:1333` 在任务、文件表相应锁下取得并持有 file，减少目标退出、关闭 FD 与查询之间的生命周期问题。
- `kgsl.c:1381` 调用厂商原生只读 INFO handler，让厂商自己的实现解释 memdesc，避免在本模块复制一套固定私有结构布局。限制为固定查询操作和内核缓冲区。
- `kgsl.c:865` 分别解析 syncsource CREATE、DESTROY，要求解析出的 process_priv、IDR、锁偏移一致，并核对字段到调用参数的数据流；未知形态返回不支持。
- 对 ARM64 PLT、CFI 跳板、部分 BTI/PAC 和 SCS 形态有专门处理，适配范围比固定几个指令位置更有扩展空间。
- `kgsl.c:1067` 起校验活体 IDR；通过 XArray 分配标记判断空闲，并要求两轮结果一致。能区分“已分配但对象指针为 NULL”和空闲 ID。查询结果仍然只是当时观察，不能预留下一次分配结果。

局限：依赖厂商私有符号、handler 调用约定以及可识别的机器码形态；并非公共 UAPI 保证。源码明确假设 KGSL 在缓存生命周期内常驻，没有看到 GPU 模块卸载、重载后的缓存失效机制。接口持有者被视为已授权，并不额外执行一次目标 ptrace 凭据检查。

### KGSL 节点处理

`kgsl.c:1636`、`:1664`、`:1704` 拦截通用 kobject/debugfs 创建函数，按 root 调用者及父节点名等条件处理 proc、pagetables 节点。主要作用于随后发生的创建，不主动遍历删除已有节点。

需要关注：通用 hook 的判断没有逐次验证完整 KGSL 祖先路径或调用来源；仅检测系统存在 KGSL，不能保证每次遇到的同名 proc/pagetables 都属于 KGSL。缓存顶层 kobject 与数字 TGID 的环表也没有看到对应删除。因此不能因为 hook 符号通用就认定其行为跨厂商最安全。

## 2. NBG-driver

### 进程查询

`src/core/process.c:235` 的 `find_pid_by_cmdline_ranked()` 支持完整 argv[0] 匹配，并按线程数、RSS 等选择候选；另有 comm、同名进程、SO 起始映射与路径、maps、线程等查询。`src/core/readonly.c` 还提供独立只读查询路径和部分上下文采样。

限制同样存在：旧 argv[0] 扫描最多 512 个候选任务、读取缓冲区 512 字节，依赖相应页已驻留；截断与读取失败影响定位。不能把“存在完整包名接口”理解为任何负载下都不会漏查，也不能把独立只读路径的访问检查套用到所有旧接口。

### GPU/KGSL 查询

`src/comm/socket_ioctl.c:239` 的 GPUOBJ_INFO 取得目标 file 引用，确认 KGSL 身份后调用厂商 INFO，输出对象信息与采样时间。`comm_ioctl_kgsl_syncsource_snapshot()`（`:405`）取得 process_private 引用，在解析出的锁保护下扫描有界 IDR，输出占用位图、数量和 first_free。

优点：真实实现了两种查询；syncsource 的整个位图比仅返回一个候选空闲 ID 更丰富；持锁期间的位图具有同一临界区的一致性，前提是私有布局和锁识别正确。

兼容性瓶颈：

- GPUOBJ_INFO 固定核对 handler 的第 0、4、6 条指令；syncsource 固定检查 DESTROY 的第 5、7、11 条指令。重编译、插入前导指令或寄存器分配变化都可能返回 EPROTO。
- GPU FD 身份比较是 unlocked_ioctl 与解析符号的直接地址相等，没有看到 enkernel 那样的多种 CFI 入口识别。
- syncsource 缺少 CREATE、DESTROY 的独立交叉验证；缓存私有函数地址的有效期也需要结合 GPU 模块生命周期处理。
- 扫描用 `idr_find() != NULL` 判断占用，不能覆盖 IDR 允许的 NULL 占位情况。这是通用数据结构语义上的限制，尚未证明目标 KGSL 必然会出现该情况。

Linux 官方文档说明 IDR 可以先关联 NULL 来预留一个 ID：[ID Allocation](https://docs.kernel.org/core-api/idr.html)。因此 NULL 查询结果不能普遍等同于未分配。

### KGSL 节点处理

`src/hide/hide_kgsl.c:159` 使用 kprobe 拦截 KGSL sysfs/debugfs 初始化，以及受 KGSL 祖先条件约束的 sysfs group 创建；`:52` 处理已有 sysfs 节点，已有 debugfs 节点走路径过滤。相比仅拦截未来创建，这条路径的覆盖面更广。

明确缺口：`:206` 的恢复只解除 debugfs 路径过滤，没有重建被实际移除的 sysfs 节点。hook 部分安装成功也可能返回成功，并不证明全部节点路径都已覆盖。伪造初始化失败对不同厂商调用者的影响，需要核对实际错误处理与清理路径。

### 版本适配

`include/nbg_compat.h` 有 mmap、VMA iterator、临时映射和部分页表辅助接口的版本分支；`src/core/gki_compat.c` 有旧 GKI 导出裁剪的兼容处理。

`.github/workflows/build-kernel.yml` 声明了从 android12-5.10 到 android17-6.18 的八项构建矩阵。本次只核实到配置存在；没有取得全部构建成功记录，更没有取得这些环境的 KGSL 运行通过记录。矩阵中的字符串也不能证明任一具体手机的厂商 GPU 私有接口受支持。

## 3. lsdriver

进程相关代码以给定 PID 后查询 task/mm、枚举 VMA/模块、读取部分线程状态为主。`virtual_memory_enum.h` 有新旧 VMA 遍历分支和 Android 匿名映射名称差异处理。

KGSL 主要实现位于 `hide_kgsl.h:127` 起：保存最多 8 个 TGID，以 inline hook 拦截 KGSL sysfs/debugfs 初始化和 sysfs group 创建。表满返回 ENOSPC；首个目标安装 hook，最后一个目标移除时卸载，生命周期比只有登记表的接口完整。

没有找到独立 GPUOBJ_INFO、syncsource 查询路径。文件开头列出的大量 GPU 路径在注释块内，不能计为已实现覆盖。该路线也没有主动处理已创建的 KGSL 节点。

兼容性成本包括厂商符号、inline hook 的指令重定位与保护机制，以及只存数字 TGID 的绑定。Makefile 统一指定 ARMv8.6-A 及多个扩展，也额外限制 CPU 覆盖；仅根据 Linux 版本匹配不足以判断是否可以安全执行整个模块。

## 4. LinyuKernel

这里仅评价当前工作区这份源码。`src/process.c:106` 按 comm 找 PID，支持模块/BSS 查询、进程列表及基本信息。模块遍历直接使用 `struct vma_iterator` / `for_each_vma`，没有在这条路径看到旧内核链表遍历适配；对 5.x 不能直接作出兼容承诺。

`src/hide.c:393` 的 `linyu_hide_kgsl()` 校验并登记 PID。全目录交叉引用显示，hidden_kgsl_pids/count 只有声明及该函数中的写入、去重访问，没有消费它们的过滤回调。因此该接口返回成功，只能说明登记逻辑执行，不能说明 GPU/KGSL 节点已经处理。表满时还会返回成功。

没有找到 GPU 对象、syncsource 数据查询实现。构建开启 KBUILD_MODPOST_WARN=1，也意味着需要另外检查最终模块的未解析导入，不能只看 make 退出成功。

## 给 nbdriver 的取舍

进程接口可以参考 NBG-driver 的功能覆盖，但保留本驱动已经实现的身份、边界和生命周期检查。只读 KGSL 数据查询可参考 enkernel 的目标 file 引用、受限原生 INFO、双向布局校验和明确的不支持返回。

优先补足模块卸载/重载时缓存失效、目标代际核验、能力报告及具体厂商驱动的运行验证。GPU 对象元数据查询、CPU 进程内存读取和 GPU 虚拟地址翻译是三个不同能力；本次没有从这些查询接口确认一套通用 GPU 页表/IOMMU 地址翻译实现。

节点处理单独评估范围、初始化失败语义与恢复完整性。当前没有足够证据将其中任何一套直接标为“全机型兼容”。
