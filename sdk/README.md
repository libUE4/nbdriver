# 用户态 SDK

按 LinyuKernel SDK 的方式，协议结构、命令号和 static inline 调用实现直接写在
`sdk/include/nbdriver.h` 中。复制这一个文件到项目即可，支持 C/C++，无须生成器、
类封装、额外源文件或其他项目头文件。

```c
#include "nbdriver.h"

int main(void)
{
    int fd = nbdriver_open();
    int value = 123;
    struct nbdriver_abi_info info;
    if (fd < 0)
        return 1;
    if (nbdriver_get_abi(fd, &info) < 0 ||
        info.version != NBDRIVER_ABI_VERSION || info.size != sizeof(info)) {
        nbdriver_close(fd);
        return 2;
    }
    if (!(info.features & NBDRIVER_FEATURE_READ_PROCESS)) {
        nbdriver_close(fd);
        return 3;
    }
    /* 示例读取本进程，实际使用时传入目标 pid、地址和接收缓冲区。 */
    int output = 0;
    int result = nbdriver_read_process_memory(fd, getpid(),
                    (nbdriver_u64)(uintptr_t)&value, &output, sizeof(output));
    nbdriver_close(fd);
    return result == 0 && output == value ? 0 : 4;
}
```

接口：

- `nbdriver_open()`：通过 reboot 入口取得匿名 FD，并校验 ABI；校验失败自动关闭 FD。
- `nbdriver_close(fd)`：关闭连接；负数 FD 不做处理。
- `nbdriver_get_abi(fd, &info)`：查询版本、结构大小和功能位。
- `nbdriver_read_process_memory(fd, pid, address, buffer, size)`：读取内存。
- `nbdriver_write_process_memory(fd, pid, address, buffer, size)`：写入内存。
- `nbdriver_ioctl(fd, command, argument)`：直接发起协议调用。

读写接口成功返回 `0`，失败返回 `-1` 并设置 `errno`，单次最多 64 KiB。
失败时可能已经完成部分读写，缓冲区不会自动清零，也不会自动重试。
需要精确的复制长度、失败地址或 PID 身份校验时，先将请求结构清零，使用
`nbdriver_read_process(fd, &request)` / `nbdriver_write_process(fd, &request)`，
检查 `copied`、`error`、`failed_address` 和 `expected_start_ns`。

连接失败保留驱动回写的 EPERM/EMFILE/ENOMEM 等负 errno；未收到回写时保留可见的 syscall 错误，非法 magic 的 EINVAL 映射为 ENODEV。旧模块没有错误回写时无法区分其内部失败。ABI 不匹配返回 EPROTO。

协议与内核 `include/nbdriver_abi.h` 保持一致。修改协议后运行：

```sh
python3 tests/test_single_header.py
```

测试复制单头到独立目录，检查 C11/C++17 编译、多文件链接、模拟 FD 安装、
查询和读写、参数校验、关闭行为，并比较单头与内核 ABI。模拟测试不执行真实 reboot。
运行示例需要目标设备已经加载匹配的 nbdriver，编译通过不代表实机识别通过。

原有 `nbdriver/client.h` + `sdk/src/client.c` 接口仍可使用；同一个源文件选择一种
接入方式，避免同时包含它和 `nbdriver.h`。


**进程、线程查询和批量读扩展（ABI 5 的新增命令）**

| 单头接口 | 作用 | GET_ABI 能力位 |
|---|---|---|
| nbdriver_find_process(fd, name) | 精确匹配完整 argv[0]，返回排序后的目标 PID | FIND_PROCESS |
| nbdriver_get_module_base(fd, pid, name, &base) | 文件起始 VMA 的 SO 基址 | MODULE_BASE |
| nbdriver_get_process_start_time(fd, pid, &start_ns) | start_boottime，单位 ns | PROCESS_ID |
| nbdriver_get_thread_id(fd, pid, comm) | 非组长同名线程择优，返回 TID | THREAD_ID |
| nbdriver_get_thread_environment(fd, pid, tid, &out) | TLS、APGA 和有效位 | THREAD_ENV |
| nbdriver_get_thread_fpstate(fd, pid, tid, &out) | 保存的 TLS/FPSR/FPCR 与身份、采样时间 | THREAD_FP |
| nbdriver_readv_process(fd, &request) | 带预期身份和汇总信息的批量读 | READV |
| nbdriver_read_process_vector(fd, pid, entries, count, start_ns) | 批量读便利接口 | READV |

能力位名称均带 `NBDRIVER_FEATURE_` 前缀。新功能沿用现有 CAP_SYS_PTRACE 和
ptrace/LSM 权限检查；查询不能替代加载驱动或授予调用权限。

PID/TID 输入输出采用调用者 PID 命名空间编号。PID 查询保留线程数、RSS、PID 的优先级排序；超过 512 个候选返回 E2BIG，
不静默输出截断后的“最佳”候选。目标 argv 页未驻留时不会为查找主动补页。
SO 匹配 basename/数字版本后缀，要求 vm_pgoff==0，不使用文件名子串匹配。
线程按 on_cpu、累计执行时间、TID 排序，排除进程组长。

线程环境中的 `expected_start_ns` / `expected_thread_start_ns`：输入非零可校验身份，
成功后返回实际值；便利函数默认查询当前身份，需要传入预期身份时使用原始 ioctl。
THREAD_ENV 查询 current 时读取实时 TPIDR_EL0，并在禁抢占区间读取密钥；其他目标先使用 task_call_func 固定调度状态；运行中则在释放调度锁后向目标 CPU 发送 IPI，并核对 current 身份后采样。迁移/CPU 离线最多重试三次，仍不稳定返回 EAGAIN；APGA 软件与硬件密钥不一致时也返回 EAGAIN。TLS/APGA 的 valid 表示采样/硬件能力有效，零值也可有效。缺少 task_call_func 时不发布远程 THREAD_ENV/THREAD_FP 能力，远程请求返回 EOPNOTSUPP。
FP 不导出 Q0–Q31，仍拒绝 current/on_cpu 的保存态采样。

```c
struct nbdriver_readv_entry entries[2];
struct nbdriver_readv_request batch;
nbdriver_u64 start_ns;
memset(entries, 0, sizeof(entries));
memset(&batch, 0, sizeof(batch));
/* fd、pid、address_a/address_b、buffer_a/buffer_b、size_a/b 已由调用方提供。 */
if (nbdriver_get_process_start_time(fd, pid, &start_ns) == 0) {
    entries[0].address = address_a;
    entries[0].buffer = (nbdriver_u64)(uintptr_t)buffer_a;
    entries[0].size = size_a;
    entries[1].address = address_b;
    entries[1].buffer = (nbdriver_u64)(uintptr_t)buffer_b;
    entries[1].size = size_b;
    batch.pid = pid;
    batch.count = 2;
    batch.entries = (nbdriver_u64)(uintptr_t)entries;
    batch.expected_start_ns = start_ns;
    if (nbdriver_readv_process(fd, &batch) == 0) {
        /* 逐项检查 entries[i].error/copied；batch.completed 是完整成功的项数。 */
    }
}
```

每批最多 1024 项，每项最多 512 KiB，有效项总大小最多 16 MiB。
内核复用现有普通读取路径，以最多 64 KiB 为一块处理，并在整批持有同一个 task/mm 引用。
这保持原有补页行为，不新增驻留页读命令；批量读取不承诺目标数据的原子快照。
逐项失败不阻止后续项：批次成功派发时 ioctl 返回 0，错误写入各 entry.error（负 errno）。
全局失败返回 -1/errno，并填写 batch.error 及各项错误；用户态结果地址本身无效时无法保证回写。
批次和条目均须先清零，reserved 必须为 0；失败保留可读前缀，不自动清零或重试。

测试及设备探针：

```sh
python3 tests/test_single_header.py
python3 tests/test_readv.py
# tests/device_query_readv_smoke.c 可仅用 nbdriver.h 编译为 Android 可执行文件。
```

设备探针会创建有唯一 argv[0] 的自身测试进程和一个命名线程，验证 SO ELF 头、
进程身份、线程状态及 512 KiB 混合成功/失败批量读取；它不会修改其他进程。

**maps、映射与元数据扩展**

**GPU/KGSL 查询与节点可撤销过滤**

```c
struct nbdriver_gpu_info_request info;
nbdriver_read_gpu_object_info(fd, pid, kgsl_fd, object_id, expected_start_ns, &info);
/* info.result 是 KGSL 原生 GPUOBJ_INFO 返回值。 */

struct nbdriver_gpu_sync_request sync;
nbdriver_get_gpu_syncsources(fd, pid, kgsl_fd, 4096, expected_start_ns, &sync);
/* occupied 位图和 first_free 只是同一把 KGSL 锁下的观察结果，不是预留。 */

nbdriver_hide_gpu_process(fd, pid);
nbdriver_show_gpu_process(fd, pid);
```

GPU 查询由驱动根据 PID、目标进程中的 KGSL FD 和对象 ID 完成，用户层不需要提供
KGSL 私有结构偏移、内核地址或函数地址。syncsource 查询返回有界 IDR 位图；如果
当前 KGSL 编译形态无法通过布局交叉校验，调用失败并保留 `fail_stage`，不会猜测
私有结构。

隐藏接口只保存 PID 对应的内核 `struct pid` 和 `start_boottime`，过滤 KGSL 的
`proc/<pid>`、`pagetables/<pid>` 目录枚举及访问；它不删除厂商节点、不伪造 KGSL
初始化失败。调用 `nbdriver_show_gpu_process()` 或驱动卸载后，过滤立即撤销。
进程退出或 PID 重用时，后台清理会丢弃旧绑定。功能位分别是
`NBDRIVER_FEATURE_GPU_SYNC` 与 `NBDRIVER_FEATURE_GPU_VISIBILITY`；缺少对应内核
符号或 kprobe 入口时不会发布能力位。

这些接口仍受现有 `CAP_SYS_PTRACE`、进程身份和 LSM 检查约束。GPU 查询不等同于
GPU 虚拟地址翻译，也不保证所有厂商 KGSL 私有编译形态都支持。

| 单头接口 | 返回 |
|---|---|
| nbdriver_process_maps(fd, &req) | 原始 maps 请求，保留 required length、copied、error、身份 |
| nbdriver_get_process_maps(fd, pid, buffer, capacity, &length, start_ns) | 0 成功，-1/errno；NULL/0 可查询长度 |
| nbdriver_query_mapping(fd, pid, address, start_ns, &out) | 物理地址、页粒度、VMA 边界、权限 |
| nbdriver_get_file_descriptor_size(fd, pid, target_fd, start_ns, &size) | 目标 FD 的 VFS 大小 |
| nbdriver_read_gpu_object_info(fd, pid, target_fd, object_id, start_ns, &out) | 0 表示传输完成，另查 out.result / sample_ns |
| nbdriver_read_random(fd, buffer, size) | 返回 1–64 的字节数，或 -1/errno |

maps capacity 包含末尾 NUL、length 不包含；最多 4 MiB。ENOSPC 时返回完整所需长度，
不复制部分文本。请求先清零；启动身份 start_ns=0 可查询当前身份，非零用于防 PID 重用。
映射查询不补页，物理地址只是瞬时查询结果；缺页 ENODATA、hugetlb/设备内存 EOPNOTSUPP。

GPU 沿用旧驱动已知 KGSL 实现签名，并验证 FD/模块归属；Mali、缺 KGSL 或未匹配的实现
返回明确错误。GPU_INFO 能力位表示符号前提存在，请求时仍需校验具体实现。
不得忽略 out.result 的负错误值。FD_SIZE 缺后台时不声明能力。

[完整行为、测试与限制](../docs/maps-metadata-20261113.md)。
测试程序：tests/device_metadata_smoke.c；--require-gpu 将 GPU 缺项计为失败。

**自动终端过滤**

模块加载后启动自动发现，按完整 argv[0] 精确匹配 `com.tencent.tmgp.dfm`、
`com.tencent.rmcn`，两者及同名多实例可同时生效。每轮完成后间隔 500 ms 再发现；
进程启动/重启无需客户端连接。正常调度下异步生效，系统繁忙时发现可能延后。
只改变目标访问 `/dev/pts/0..31` 的 access/stat 结果为 ENOENT；普通 open/read/write
语义保持不变。按 PID 对象和 mm 身份匹配，PID 复用、exec 不继承旧绑定。

```c
struct nbdriver_devpts_status state;
nbdriver_get_devpts_status(fd, &state);  /* 只读，不触发绑定 */
```

`active` 表示两个探针已安装；`target_count` 为调用者可见的有效绑定实例数。
`pids[0]`/`pids[1]` 分别给出 DFM/RMCN 的一个代表实例，未运行时为 0。
`generation` 在绑定集合变化时递增，`last_error` 为最近扫描错误，两个 hits 是本次
模块生命周期内的累计命中计数。无人运行时仍 active=1，等待后续进程。

手动设置 PID、开关和范围的接口已经删除，原命令 0x1c 不再处理；只读状态命令为
0x1f（56 字节、_IOR）。其他 ABI 5 命令和结构不变。卸载自动停止扫描、撤销探针并
释放 PID/mm 引用。详见[实现与验证](../docs/devpts-auto-20261114.md)。

**2026-11-14 接口修正**

写内存使用 ATTACH_FSCREDS 的 ptrace/LSM 模式，读取使用 READ_FSCREDS；调用者能力不代替这些检查。自动过滤使用完整包名、PID 对象及 mm 身份；退出目标在状态查询中被排除，并由下一轮扫描回收，不会误匹配复用 PID 的进程。

通用 ABI 版本保持为 5；自动过滤替换了原开发期手动接口。INSTALL_PENDING 仅用于连接输出哨兵。两套 SDK 都自动执行 ABI 握手。


## 只读信号与 VMA 查询（ABI 5 可选扩展）

先检查 `NBDRIVER_FEATURE_THREAD_SIGNAL`（bit 16）和 `NBDRIVER_FEATURE_VMA`（bit 17）。
旧命令号、结构布局及 ABI 版本不变。`THREAD_SIGNAL` 为 ioctl `0x1d`，80 字节；
`VMA` 为 ioctl `0x1e`，复用 64 字节 `nbdriver_mapping_request`。

`nbdriver_get_thread_signal(fd, pid, tid, signo, process_start, thread_start, &out)`
读取原生 ARM64 信号 1..64 的 handler、用户可见 flags、restorer、action_mask、
该线程 blocked_mask、sample_ns。两个 start 参数非零时严格校验；输出含实际开始时间。
持有原有 exec/access 身份检查，并由内核 `__lock_task_sighand` 在 RCU/退出安全协议下
锁定 sighand；在锁内复制动作和屏蔽掩码，锁外拷贝用户结果。符号缺失时不公布功能位，
返回 EOPNOTSUPP。无 setter、信号投递、目标寄存器或内存写入。

`nbdriver_query_vma(fd, pid, address, process_start, &out)` 只检查 VMA 范围与有效 R/W/X/
共享标志，不遍历页表或补页；`physical`/`page_size` 始终为零。已存在的冷页、PROT_NONE
映射也可查询；无映射返回 ENOENT。原 MAPPING 仍要求可读取的普通 RAM 叶项。
保留位非零、信号越界、错误身份和权限不足均明确失败。

UDP 下行采集新增 `nbdriver_udp_capture_start/read/status/stop`，支持标准 `poll()`，能力位为 `NBDRIVER_FEATURE_UDP_CAPTURE`。START 返回的 generation 必须传给 READ；READ 返回记录数，0 表示暂时没有数据，-1 使用 errno。单头与旧 client.c SDK 均支持。使用前检查能力位；旧模块会缺少该位或返回 ENOTTY。

数据是原始下行 IPv4 UDP 载荷，不负责解密。UID 筛选、GRO/截断、队列溢出及 FD 生命周期详见 [UDP 接口说明](../docs/udp-capture-20261115.md)。
