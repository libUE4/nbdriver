# nbdriver GPU/KGSL 合并记录

日期：2026-11-14

## 已合入

本次把旧 `NBG-driver` 的目标进程/FD 身份检查保留在 `nbdriver`，并把 `enkernel` 的 KGSL 思路合入现有 `nbdriver_features.c`：

1. GPUOBJ_INFO：目标 PID + 目标进程 KGSL FD + object ID；持有目标 file 引用，校验 KGSL callback、厂商模块归属和对象返回值。
2. syncsource：根据 CREATE/DESTROY 代码形态推导 `process_priv`、IDR 和锁偏移；要求双函数布局一致、调用目标一致、IDR 初始范围一致。通过锁保护返回有界 occupied 位图和 `first_free`。
3. KGSL 可见性：目标 PID 绑定 `struct pid` 和 `start_boottime`，过滤 KGSL `proc/<pid>`、`pagetables/<pid>` 的目录枚举、open/getattr；不删除原节点，不改 KGSL 初始化和销毁路径，show 或卸载后可撤销。

## 用户层接口

单头文件：`sdk/include/nbdriver.h`

```c
nbdriver_read_gpu_object_info(fd, pid, kgsl_fd, object_id, start_ns, &out);
nbdriver_get_gpu_syncsources(fd, pid, kgsl_fd, max_id, start_ns, &out);
nbdriver_hide_gpu_process(fd, pid);
nbdriver_show_gpu_process(fd, pid);
```

GPU 查询不能只传 PID：GPUOBJ_INFO 和 syncsource 还需要目标进程内的 KGSL FD；隐藏只需要 PID。用户层不需要传私有结构偏移、内核地址或函数地址。

新增能力位：`NBDRIVER_FEATURE_GPU_SYNC`、`NBDRIVER_FEATURE_GPU_VISIBILITY`。能力位只在对应后端和 kprobe 入口成功时发布。

## 验证

- 正式 `nbdriver` 源码在 Android 15 / 6.6.127 DDK 编译通过。
- 产物：`out/kgsl-merge-20261114/nbdriver-kgsl-merge-android15-6.6.ko`
- SHA256：`811cfc85fbd15d84cbc5762f2550bb4c50e0aed26130d53e2cf365115966d72c`
- 单头 SDK ABI、C/C++ 编译、多翻译单元测试通过。
- KGSL 主机仿真测试通过：GPUOBJ_INFO、CFI 入口、模块引用、PID/FD 复用、vendor errno、syncsource 位图、NULL 占位、指令布局变化拒绝、节点枚举、open/getattr、show 恢复、退出清理、kprobe 部分安装回滚。
- `make` 通过不等于每个厂商 KGSL 运行时通过；本次没有把这个新模块刷入设备，也没有宣称全机型验证完成。

验证日志与隔离修改前副本：`out/kgsl-merge-20261114/`。正式源码改动清单及前后 SHA256：`changes.json`。

## 已知边界

- syncsource 结果是锁内快照，不能预留下一次分配；代码形态不匹配时返回不支持，不猜测布局。
- GPU 查询仍依赖厂商私有符号和 handler 原型；缺失或模块重载后不应继续使用旧能力缓存。
- KGSL 节点过滤只作用于确认的 `sysfs/debugfs` KGSL 祖先路径；kprobe 不可用时能力位关闭，基础驱动继续初始化。
- 过滤不会删除已有节点，因此不会产生“删除后 show 无法重建”的恢复问题。
