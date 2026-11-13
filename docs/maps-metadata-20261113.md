# maps、映射与只读元数据迁移（2026-11-13）

本轮在已有进程/线程/批量读基础上新增五个接口。ABI 仍为 5，旧命令和结构布局不变；新命令为 0x17–0x1b，对应能力位为 bit 10–14。

| 应用原接口 | 单头 SDK | 内核实现 |
|---|---|---|
| getProcessMaps | nbdriver_process_maps / nbdriver_get_process_maps | metadata_maps |
| queryMapping | nbdriver_query_mapping | metadata_mapping / nbdriver_mapping_locked |
| getFileDescriptorSize | nbdriver_get_file_descriptor_size | metadata_fd_size |
| readGpuObjectInfo | nbdriver_read_gpu_object_info | metadata_gpu |
| readRandom | nbdriver_read_random | metadata_random |

源于工作区 NBG-driver 的 readonly.c、query.c 和 socket_ioctl.c；参考文件 SHA256 保存在 out/maps-metadata-20261113/reference.sha256。NBG-Draw-dfm 的应用适配层本轮尚未切换。

maps 容量上限 4 MiB（包含末尾 NUL），length/copied 均不包含 NUL。buffer=0、capacity=0 查询所需长度；目标在两次请求间可能改变映射，因此下一次仍可能 ENOSPC。容量不足时返回完整所需 length、copied=0、error=-ENOSPC，不向用户缓冲区复制截断文本。生成过程持有 mmap_lock，保留文件路径、设备号、inode、偏移、共享权限、匿名名称、heap/stack 以及 vm_ops 提供的特殊名称。换行、回车和反斜杠转为八进制转义，避免伪造额外记录。文本格式可供现有 maps 解析器读取，不承诺与各厂商 procfs 的列宽、VMA 合并显示规则逐字节一致。

旧 Android 5.10 的匿名名称来自用户空间，使用已持有 mmap_lock 的内部只读路径读取，不递归获取 mmap_lock，也不为名称主动补页；名称不可读时请求明确失败。没有新增用户态驻留页读取命令。5.x/6.1 文件 VMA 的匿名名称 union 不能随意解引用，采用旧实现的保守分支。

映射查询只读取已有页表：返回所查字节的物理地址、PTE/PMD/PUD 粒度、VMA 边界及 READ/WRITE/EXEC/SHARED 标志。缺页返回 ENODATA、没有对应 VMA 返回 ENOENT；不补页、不访问物理内容、不取得额外页引用。允许普通、带标签和 non-cacheable 正常 RAM 属性，拒绝设备内存和非 RAM PFN；hugetlb 因锁协议不同返回 EOPNOTSUPP。结果是瞬时身份信息，不是持久页固定句柄。

进程相关请求均经过 CAP_SYS_PTRACE、ptrace/LSM 和 exec_update_lock 检查，持有 task/mm/file 的相应引用。expected_start_ns 输入非零时校验 PID 启动身份，成功或完成目标识别后回传实际 start_boottime。FD 大小通过真实 fget_task 与 vfs_getattr 查询，保留 VFS/LSM 路径，不直接猜 inode 大小；缺后端时不声明 FD_SIZE 能力，返回 EOPNOTSUPP。

GPU 查询迁移旧驱动的 KGSL 只读处理路径，检查真实字符设备、kgsl_ioctl、被引用文件的模块所有者，以及旧驱动已有的 7 条指令窗口中的三处实现签名。只向 kgsl_ioctl_gpuobj_info 传递内核拥有的 48 字节结构，不调用用户 ioctl 分发器、不投递目标线程任务、不猜厂商结构偏移。文件引用在解析、校验和调用期间保持驱动模块存活。传输返回 0 后，还必须检查 request.result（原始 KGSL 返回值）及 sample_ns。普通文件返回 ENOTTY；缺符号或实现签名不匹配返回 EOPNOTSUPP。GET_ABI 的 GPU_INFO 表示后端符号和解析前提可见，具体文件和实现签名仍在请求时验证；不保证 Adreno、Mali 或任意厂商 GPU 都支持。

KGSL 48 字节 UAPI 和只读函数语义另对照了 [OnePlus 官方 graphics-kernel 源码](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8750/blob/oneplus/sm8750_v_15.0.0_oneplus_13/vendor/qcom/opensource/graphics-kernel/kgsl.c) 与同仓库 include/uapi/linux/msm_kgsl.h。下载内容、URL 和 SHA256 保存在 out/maps-metadata-20261113/kgsl-source/。源码一致不等于当前设备机器码签名已经验证。

随机接口每次 1–64 字节，使用内核 get_random_bytes；便利函数返回实际字节数。复制错误可能已改动用户缓冲区前缀，不保证用户缓冲区事务性回滚。

本次实际验证：

- 19 项构建/运行检查全部通过，包括隔离单头 C11/C++17、多翻译单元、ABI 大小/偏移/命令一致性、原有批量读/页表/查询回归、新 SDK 模拟 ioctl 检查、用户原有 smoke 编译。
- 实际 metadata.c 调度代码在合成 task/mm/file/VFS/KGSL 环境下运行，覆盖旧 5.10 用户名称与 5.15/6.6 匿名名称分支、ENOSPC、权限/身份、错误 FD、GPU 所有者/签名/返回值、随机边界和引用回收；这不是这些版本内核的实测。
- 实际 mapping.c 在合成 4K/16K/64K 页表上运行，覆盖普通页、大页、属性、缺失/无效映射、重试与锁释放；不代表对应真实内核的锁竞争已经测试。
- 使用本地 Android 6.6 GKI 头文件和 Clang 18 编译、链接 12 个对象为 nbdriver.o，通过 -Wdeclaration-after-statement 检查。新增静态导入另与该源码 KMI 列表核对，记录在 symbol-delta.json。
- 测试程序 tests/device_metadata_smoke.c 已编译为 Android ARM64/API 24、16 KiB ELF 段对齐的 out/maps-metadata-20261113/device_metadata_smoke。

本轮随后连接用户提供的 172.19.0.1:38529：PKR110、Android 15、内核 6.6.30-酷安姜北尘。已从 DDK 镜像读取真实 Module.symvers（SHA256 与此前独立审计记录一致），并在独立 modpost 副本中恢复导出检查和 CRC 赋值；未使用伪造符号表、跳过导出检查或强制加载。正式模块 65 个静态导入全部在该表中，保留 __versions，编译/modpost/链接成功。因没有 vmlinux，BTF 生成跳过。

旧模块 srcversion=AE2E8F6B87D2088B1F1BE93 已备份，确认引用数 0 后普通 rmmod 卸载，并验证 /sys/module/nbdriver 消失，再普通 insmod 新模块。新增五项设备探针以 --require-gpu 运行：23 项 PASS，failures=0、skipped=0，GPU 与设备原生 KGSL GPUOBJ_INFO 比较一致。

追加进程/线程/批量读回归时发现 Android malloc 的 0xb4 顶字节标签导致源 VMA 查找失败；已在普通/批量读取的共同内部入口和写入目标地址处理处使用 untagged_addr，保留 ABI 原地址用于失败诊断。诊断记录保留原失败，再次加载修正版后，带标签 malloc 单读/单写、512 KiB 混合成功/失败批量读、PID/SO/线程/TLS/FP 均通过。最终 srcversion=2D43C240DD36647AD976CDE，模块 SHA256=48c6c79c8e8cc878a21e94c806c19884dc4428d00082478156e9069d85efbe65。

用户已有 tests/nbdriver_smoke.c 未改动，重新编译后在新模块运行：108 通过、0 失败、0 跳过。另完成最终修正版的 rmmod/缺席检查/insmod/版本检查，并再次运行五项探针（0 失败、0 跳过），结束时模块引用数 0、新版保持加载。

boot ID 始终为 8624d3fb-728a-4e07-b428-cabad0f98ea2，未发生重启。保存的本次内核日志窗口未匹配 BUG/Oops/panic/CFI failure 等检查模式；这不是长期稳定性保证。实际测试仅覆盖这台明确设备，其余品牌/KMI 未因此获得实机验证。

设备程序创建自身文件与映射，验证 maps/ENOSPC/路径转义、映射/缺页、FD 大小、身份过期和随机字节。若 KGSL 后端存在，则自行打开设备、分配一个 GPU 对象，比较原生 GPUOBJ_INFO 与驱动结果，再释放；没有后端或不匹配的实现打印 SKIP。用 --require-gpu 将 GPU 缺项视为失败，不能把带 SKIP 的结果称作五项全部通过。

```sh
adb push out/maps-metadata-20261113/device_metadata_smoke /data/local/tmp/
adb shell su -c 'chmod 755 /data/local/tmp/device_metadata_smoke'
adb shell su -c /data/local/tmp/device_metadata_smoke
# 需事先加载包含这些新接口的匹配模块。
# 必须验收 GPU 时：/data/local/tmp/device_metadata_smoke --require-gpu
```

修改前副本、SHA256、最终工作副本、逐项命令/输出/退出码、差异和恢复工具保存在 out/maps-metadata-20261113/。restore.py 检查当前文件与本轮最终哈希一致后恢复修改前文件；在额外副本上验证，保留最终工作副本和测试程序。
