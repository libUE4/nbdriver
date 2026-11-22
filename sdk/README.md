# 用户态 SDK

更新：2026-11-16，协议为 ABI 5。完整命令/结构/能力位见[当前实现清单](../docs/current-state.md)。

## 两种接入方式

| 方式 | 需要的文件 | 接口范围 |
|---|---|---|
| 推荐：单头 | 复制 `sdk/include/nbdriver.h` | C/C++ static inline；连接、查询、读写/READV、元数据、GPU、devpts、runtime、UDP |
| 编译型 SDK | `sdk/include/nbdriver_abi.h`、`sdk/include/nbdriver/client.h`、链接 `sdk/src/client.c` | 连接、ABI/runtime、原始读写、模块加载原语、UDP；不是单头的完整接口镜像 |

同一个翻译单元选择一种接入方式，不同时包含两套同名声明/定义。
公共头文件统一放在 `sdk/include/`，内核使用其中的共享协议头 `nbdriver_abi.h`。
单头包含协议镜像，不需要内核头、生成器、类封装或另一个源文件。
编译型 SDK 没有 `nbdriver_close()`、查询/READV/元数据等全部便利包装；关闭使用系统 `close(fd)`，
缺少包装的命令可按共享 ABI 调用 ioctl。`nbdriver_load_module_path` 只是 finit_module 原语，
不含下载、SHA 校验、预检、刷入失败上报或卸载。

## 最小示例：读取自己的缓冲区

```c
#include "nbdriver.h"

int main(void)
{
    int source = 123, output = 0;
    struct nbdriver_abi_info abi = {0};
    struct nbdriver_runtime_status runtime = {0};
    int fd = nbdriver_open();
    if (fd < 0)
        return 1;
    if (nbdriver_get_abi(fd, &abi) < 0 ||
        abi.version != NBDRIVER_ABI_VERSION || abi.size != sizeof(abi) ||
        !(abi.features & NBDRIVER_FEATURE_READ_PROCESS) ||
        !(abi.features & NBDRIVER_FEATURE_RUNTIME_STATUS) ||
        nbdriver_get_runtime_status(fd, &runtime) < 0 ||
        runtime.version != NBDRIVER_ABI_VERSION || runtime.size != sizeof(runtime) ||
        !(runtime.flags & NBDRIVER_RUNTIME_READY)) {
        nbdriver_close(fd);
        return 2;
    }
    int result = nbdriver_read_process_memory(fd, getpid(),
        (nbdriver_u64)(uintptr_t)&source, &output, sizeof(output));
    nbdriver_close(fd);
    return result == 0 && output == source ? 0 : 3;
}
```

示例需要原生 Android ARM64、允许连接的身份及已加载的匹配模块。主机编译通过不等于可在主机运行引导。
迁移前后的历史专项测试通过 mock syscall/ioctl 验证包装，不调用真实候选系统调用；这些专项源码现已移除。

## 连接与返回值

`nbdriver_open` 扫描 150 个有序候选系统调用，再尝试 ARM64 reboot(142)。
FD 从用户输出参数获取；只有 GET_ABI 版本/大小校验通过才返回。FD 0 合法，失败为 -1/errno。
它不验证每项能力或 runtime READY，调用方按需另查。

输出哨兵未变表示候选未识别；普通候选的 EINVAL/EBADF/EFAULT 不作为驱动确定错误。
驱动回写的负 errno、ABI 查询的权限或资源错误立即返回。候选 FD 返回 ENOTTY/EPROTO 或 ABI 版本、大小不匹配时，关闭该 FD 并继续扫描，兼容同魔数旧驱动共存；全部候选 miss 后保留已观察到的错误，
否则 ENODEV。版本/结构失配为 EPROTO。错误分层见[通信机制](../docs/communication.md)。

大多数包装成功返回 0、失败 -1/errno，但以下返回值不同：

| 接口 | 成功返回 |
|---|---|
| `nbdriver_open` | FD（包括 0） |
| `nbdriver_find_process`、`nbdriver_get_thread_id` | PID / TID |
| `nbdriver_read_random` | 1–64 的字节数 |
| `nbdriver_udp_capture_read` | 0–32 的记录数，0 为空队列 |

`nbdriver_ioctl` 是直接 ioctl 包装。原始请求中的 error 通常是负 errno，不等同于 libc errno 正值。
不能对所有函数统一使用 `result != 0` 判失败。调用前按 feature 检查，调用失败仍按真实 errno 处理。
连接 root/system 门禁、读取例外、写入和 devpts 独立门禁见[访问规则](../docs/current-state.md#访问规则)。

## 读写、查询与批量读取

原始 `nbdriver_read_process` / `nbdriver_write_process` 会清空 copied/error/failed_address，再发 ioctl；
便利 `*_process_memory` 成功为 0、失败 -1，单次最多 64 KiB。失败可能已完成前缀，不自动清零或重试。
需要启动身份校验或精确失败位置时使用原始请求并填写 expected_start_ns。

| 单头接口 | 契约 |
|---|---|
| `nbdriver_find_process(fd, name)` | 精确完整 argv[0]，按线程数/RSS/PID 选候选；超过 512 个候选 E2BIG |
| `nbdriver_get_module_base(fd, pid, name, &base)` | SO basename/数字版本后缀，要求起始文件 VMA，非子串匹配 |
| `nbdriver_get_process_start_time(fd, pid, &start_ns)` | start_boottime 纳秒身份 |
| `nbdriver_get_thread_id(fd, pid, comm)` | 排除组长；on_cpu、累计执行时间、TID 排序 |
| `nbdriver_get_thread_environment(fd, pid, tid, &out)` | TLS/APGA 与 valid；便利包装默认当前身份，指定预期身份用原始 ioctl |
| `nbdriver_get_thread_fpstate(fd, pid, tid, &out)` | 保存的 TLS/FPSR/FPCR，不含 Q0–Q31；current/on_cpu 返回 EAGAIN |
| `nbdriver_get_thread_signal(fd, pid, tid, signo, process_start, thread_start, &out)` | 信号 1–64 的 handler/flags/restorer/mask 快照，无 setter/投递 |
| `nbdriver_readv_process(fd, &batch)` / `nbdriver_read_process_vector(...)` | 最多 1024 项、每项 512 KiB、总 16 MiB、64 KiB 分块 |

THREAD_ENV 对 current 读取实时寄存器，其他线程先固定调度状态；运行中目标在释放调度锁后 IPI 到其 CPU，
核对身份后采样，最多三次尝试。TLS/APGA 零值也可能有效，应看 valid；忙/迁移/密钥未一致时 EAGAIN。
缺少 task_call_func 时不发布远程 THREAD_ENV/THREAD_FP 能力；不能用失败缓冲区当有效结果。

批次和条目先清零、reserved 为 0。READV 派发成功返回 0，**逐项**检查 entry.error/copied，
batch.completed 是完整成功项数，不是字节数。全局错误为 -1/errno；无效回写地址无法保证结果送达。
整批引用同一个 task/mm，不能保证目标数据不变。普通读取可能补页，没有单独的 SDK 驻留页读取命令。

## maps、VMA、FD 和 GPU 元数据

| 单头接口 | 要点 |
|---|---|
| `nbdriver_process_maps` / `nbdriver_get_process_maps` | NULL/0 查询所需长度；capacity 含 NUL，length 不含；上限 4 MiB，ENOSPC 不复制部分文本 |
| `nbdriver_query_vma` | 只查范围/标志，不补页、不走页表；physical/page_size=0；无映射 ENOENT |
| `nbdriver_query_mapping` | 瞬时物理 RAM 叶项和 VMA 权限；冷页 ENODATA，不是 GPU VA 翻译 |
| `nbdriver_get_file_descriptor_size` | 目标 FD 的 VFS size，不等同 GPU 对象大小 |
| `nbdriver_read_gpu_object_info` | 传输成功仍查 out.result / sample_ns；结果体为 out.info |
| `nbdriver_get_gpu_properties` | Mali GET_GPUPROPS；capacity=0 查真实长度，最多64 KiB；检查 result/sample_ns，旧模块返回 ENOTTY |
| `nbdriver_get_gpu_syncsources` | 有界 IDR 占用观察，最多 4096 ID，first_free 不是资源预留；失败查 fail_stage/error |
| `nbdriver_gpu_visibility` / `nbdriver_hide_gpu_process` / `nbdriver_show_gpu_process` | 可撤销的目标可见性状态；共用 PID 身份的 proc 功能另有能力位 |
| `nbdriver_read_random` | 最多 64 字节，返回实际字节数 |

Mali 属性通过目标已打开的 FD 查询，只允许经符号身份核验的 kbase file_operations。
查询写入调用方缓冲区，不改变目标内存、文件位置或 GPU 上下文；不依赖 KGSL 能力位。
0x2e 是 ABI 5 的可选扩展；无法验证厂商后端时返回 EOPNOTSUPP，不补造属性。

KGSL 可选后端需验证 FD、模块归属和实现布局；能力位不保证所有厂商实现支持。
可见性过滤不删除厂商节点、不等于模块隐藏；没有单独的 PROC_VISIBILITY ioctl。
旧移植证据见[元数据记录](../docs/maps-metadata-20261113.md)和[GPU 合并记录](../docs/gpu-kgsl-merge-20261114.md)。

## 自动 devpts 与 SF groups

devpts 在模块加载后自动按完整 argv[0] 发现 `com.tencent.tmgp.dfm`、`com.tencent.rmcn`，
每轮完成后间隔 500ms，再扫描；同名多实例可并存。匹配 PID 对象和 mm 身份，PID 复用/exec 不继承绑定。
只改变目标对 `/dev/pts/0..31` 的 access/stat 观察，普通 open/read/write 语义不变。
`nbdriver_get_devpts_status` 为只读 0x1f 请求；active 表示两个探针存在，无目标仍可为 1。
手动配置 0x1c 已删除。状态查询仍要求 CAP_SYS_PTRACE，不能仅凭 system UID 假定成功。

SF_GROUPS（0x24）目前只提供公共请求结构和命令，无独立便利包装。
调用方清零 `nbdriver_sf_groups_request`、填写 abi_version，通过 ioctl 请求；
内核另查初始用户命名空间、四个 system UID、组长 comm 和真实 SF 可执行文件。
只处理当前调用线程缺少的固定 input/inet 补充组，保留原组，不修改整个宿主 UID、SELinux 或其他已有线程。
能力位表示接口存在，符号缺失、身份不符、分配失败仍会返回实际错误。

## runtime 状态与控制

两套 SDK 都提供 `nbdriver_get_runtime_status` 和 `nbdriver_runtime_control`。
先查询 handle/token，再发 QUIESCE/RESUME；结构先清零，version/size 按 ABI 填写。
QUIESCE 恢复表入口、阻止普通业务并停止 UDP；RESUME 不恢复旧采集会话。
关闭 FD 不卸载驱动，也不替代 QUIESCE。完整顺序见[生命周期](../docs/runtime-lifecycle.md)。

## UDP 下行采集

两套 SDK 都提供 `nbdriver_udp_capture_start/read/status/stop`，能力位 bit 23；
旧模块缺该位或返回 ENOTTY。数据为原始 IPv4 UDP 下行载荷，不负责坐标/密钥解密。

- START 绑定 PID 启动身份、原 mm 与真实 UID；min/max=0 采用 16/2048，输出新的 generation。
- READ 必须带相同 generation，capacity 为 1–32；返回记录数，0 是暂无数据。失配 ESTALE。
- 一次只允许一个 file 所有者；另一个 FD 启动 EBUSY，访问别人会话 EPERM。dup 共享 file。
- STOP/最后一个 file 引用关闭清空队列并撤销接收回调；关闭无关 FD 不影响采集。
- 使用 `poll(fd, POLLIN, 有限超时)`，例如 1000ms，并定期检查目标身份，避免忙等。

按 UID 筛选可能包含共享 UID 的其他进程。IP 为网络序，端口等整数为主机序。
GRO 标志不是独立 UDP 数据报，截断记录不能当完整密文；IPv6/上行不在完整采集范围。
256 条环满时覆盖最旧记录，另外最多 32 条分离记录等待读取确认。
usercopy 失败保留可重试记录，消费者可按 generation+sequence 去重；queued 包括环和分离记录。
计数说明及来源证据见[UDP 记录](../docs/udp-capture-20261115.md)。

## 检查

本地测试源码和结果统一放在 `/tmp/nbg-work/nbdriver/`；`make smoke` 和
`make touch-virtual-test` 输出也位于该临时目录。正式源码保留以下 SDK 语法检查：

```sh
cc -std=c11 -Wall -Wextra -Werror -Isdk/include -include nbdriver_abi.h -x c -fsyntax-only /dev/null
cc -std=c11 -Wall -Wextra -Werror -Isdk/include -include nbdriver.h -x c -fsyntax-only /dev/null
c++ -std=c++17 -Wall -Wextra -Werror -Isdk/include -include nbdriver.h -x c++ -fsyntax-only /dev/null
cc -D_GNU_SOURCE -std=c11 -Wall -Wextra -Werror -Isdk/include -fsyntax-only sdk/src/client.c
```

测试记录保存在项目外的临时目录，不能把历史日志当作当前源码的设备验证。


## 陀螺仪控制（可选扩展）

使用 `nbdriver_gyro_status` 查询 `NBDRIVER_GYRO_AVAILABLE`。ABI 5 的既有
32 位能力掩码不改动；旧模块会返回 `ENOTTY`，不可当作支持。控制请求使用独立
`NBDRIVER_IOC_GYRO`（0x2d），请求版本 1、大小 80 字节。

`nbdriver_gyro_set(fd, pid, start_ns, x, y, type_mask, lease_ms, &status)` 设置
X/Y 附加角速度，单位 rad/s，每轴有限值范围 [-1,1]。type_mask 为 1（校准）、
2（未校准）或 3（两者）。PID 必须绑定实际启动时间，不能传零跳过身份校验。
租约范围 1–250 ms，建议 100 ms；开启期间持续刷新。`nbdriver_gyro_stop` 停止，
最终关闭所有 dup 的 FD、QUIESCE、模块卸载也清理会话。到期不再改写新数据包。
启用前游戏须已打开传感器通道；无候选通道返回 ENODATA，调用方按需重试。

实现只处理 system_server 发往该进程当前持有的 AF_UNIX SOCK_SEQPACKET 通道、
非阻塞发送中的标准 104 字节传感器事件，最多每批 1000 条。绑定文件对象、FD、
进程启动身份和 mm；关闭替换的 FD 或 exec 不继承旧绑定。只在私有内核发送副本
上附加 X/Y，不改源缓冲区、游戏内存、Z、未校准偏置、时间戳或其他类型事件。
候选 FD 扫描限 16384、通道限 64，超过返回 E2BIG，不任意截断绑定。

`SET` 成功只代表配置接受；`modified_events` 仅在整包实际发送成功后增加。
`copy_failures` 表示无故障复制或私有缓冲池申请失败；`missed` 是 kretprobe
实例不足的内核累计值，可能包含其他 Unix 调用，不能当作游戏丢包数。
复制失败、超大批次、未知格式和不匹配通道原样通过，不丢弃游戏数据。
内核不做浮点运算，采用参考驱动的 Q32.32 转换。未刷新或失效会话停止输出。
该接口输出传感器附加角速度，屏幕旋转、触发、选靶和轴向校准属于调用方。

内核后端注册失败只禁用陀螺仪，既有触摸、读取和 UDP 不受其初始化结果影响。
传感器 direct-channel/shared-memory 路径不经过此后端；应以事件计数和设备行为验收，
不能仅凭 AVAILABLE 或 SET 成功声称设备已生效。

稳定会话每 500 ms 复核一次候选通道；字段未变化时复用同一绑定，不清零角速度。
扫描、会话变更和停止可包含 RCU 同步，调用方应放在控制线程，避免放进绘制回调。
