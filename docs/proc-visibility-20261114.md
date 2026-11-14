# proc 全局过滤（2026-11-14）

## 设计

当前 proc 过滤复刻 NBG-driver 的全局思路，不绑定游戏或 devpts 访问者：

- `NBDRIVER_GPU_VISIBILITY_HIDE` 登记的 PID 身份作为被保护对象；
- 替换真实 `/proc` 根 inode 的 `lookup` 与 `iterate_shared`，枚举时跳过被保护 TGID；
- 通过 `pid_revalidate` kretprobe 使已缓存的 `/proc/<pid>` dentry 失效；
- `security_file_open` / `security_inode_getattr` 的既有 KGSL 路径继续处理 GPU 节点，proc 路径由 ancestry + PID 身份判断；
- PID 判断复用 KGSL 的 `struct pid + start_boottime` 身份，不只比较数字 PID；
- devpts 自动识别仅用于终端过滤，不参与 proc 全局隐藏。

## 本次编译验证

- kernel module build：通过（clang-18 / kernel-build，arm64）
- `test_single_header.py`：通过
- `test_devpts_auto.py`：通过
- `test_kgsl.py`：通过
- 修复后模块 SHA256：`81dd3fd581d04f544a19d9715ba66bb63a961f421f47a75a1916fddb94782c31`

## 43113 设备崩溃证据

设备：`192.168.1.4:43113`，OnePlus PKR110，内核 `6.6.30`。

两份持久化 minidump（12:15:44、12:24:47）均记录同一根因：

```
CFI failure at nbdriver_kgsl_init+0x200/0x308 [nbdriver]
(target: path_put+0x0/0x38; expected type: 0x59dc3d37)
CPU: ... Comm: insmod
pc : nbdriver_kgsl_init+0x200/0x308 [nbdriver]
Kernel panic - not syncing: Oops - CFI: Fatal exception
```

旧实现将通过 kallsyms 获取的 `path_put` 保存为 `void (*)(struct path *)`，而 6.6 内核的实际原型是 `void path_put(const struct path *)`。在 `nbdriver_kgsl_init()` 直接调用该函数指针时触发内核 CFI，故不是 watchdog 首因，也不是游戏运行期间崩溃。

修复：将 typedef 改为 `void (*)(const struct path *)`，重新编译通过。原始日志、gz minidump、解包后的 minidump 和修复前源码备份在：

`nbdriver/out/crash-43113-20261114/`

## 设备部署状态

修复后模块尚未再次刷入 `192.168.1.4:43113`；当前结论为“根因已定位、源码和构建产物已修复、设备复测待执行”。
