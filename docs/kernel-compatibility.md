# ARM64 Android 内核兼容策略

## 结论

不承诺一个 `.ko` 可在所有 5.x/6.x 内核刷入。Android GKI 的 KMI 按 Android/LTS 分支管理；厂商回移、配置、KMI 符号表、`Module.symvers`、模块签名和设备权限都会影响模块构建或加载。采用**一份源码、按目标 KMI/设备构建、分别交付与验证**。

本工程通信入口选用常规 misc 字符设备：模块注册 `/dev/nbdriver`，用户态 `open()` 后通过 `ioctl()` 请求 ABI 信息。它不从 `sys_call_table` 取空槽、不 hook reboot、不通过 kallsyms/kprobe 取得函数地址。匿名 FD 方案需要额外的 FD bootstrap 入口；原型的私有 syscall hook 和 reboot kprobe 不是稳定 KMI 接口，故不作为跨版本适配层。

misc 字符设备减少了对 syscall 内部实现的依赖，但它**不自动保证** `misc_register` 在某一目标的模块 KMI 中可用。应使用该目标的 DDK、配置、符号版本表和 KMI 符号列表构建核实；不得把一条 AOSP 分支列表外推到所有厂商内核。

## 目标矩阵

下面是准备覆盖的代表性 Android common KMI 基线，不是全部 Linux 5.x/6.x 厂商内核列表。

| Android common KMI | 当前证据 | 模块构建/加载/调用状态 |
|---|---|---|
| `android12-5.10` | 源码比较已记录；目标 misc KMI 列表和 DDK 未取得 | 未验证 |
| `android13-5.15` | 源码比较已记录；目标 misc KMI 列表和 DDK 未取得 | 未验证 |
| `android14-6.1` | 源码比较已记录；目标 misc KMI 列表和 DDK 未取得 | 未验证 |
| `android15-6.6` | 源码比较已记录；目标 misc KMI 列表和 DDK 未取得 | 未验证 |
| `android16-6.12` | 源码比较已记录；目标 misc KMI 列表和 DDK 未取得 | 未验证 |
| `android17-6.18` | 源码比较已记录；目标 misc KMI 列表和 DDK 未取得 | 未验证 |

`android13-5.10` 的 `abi_gki_aarch64_generic` 文件确认包含 `misc_register` 和 `misc_deregister`；这不是 `android12-5.10`、其他 KMI 或任意厂商模块符号清单的证据。[该 GKI 符号列表](https://android.googlesource.com/kernel/common/+/refs/heads/android13-5.10/android/abi_gki_aarch64_generic)

对六个 common 分支 `arch/arm64/kernel/syscall.c` 的源码比较及 SHA-256 见 `docs/communication-sources.sha256`。该比较确认 syscall 内部实现有演进，但不作为外部模块 API 兼容证据。系统调用实现细节不再作为通信接口依赖。

此前匿名 FD 原型的两个阻断点也留档如下：`nbdriver_entry_kprobe.c` 的 pre-handler 直接调用 `copy_to_user()`；Kprobes 文档说明 pre-handler 从断点 trap 路径调用，locking 文档将 `copy_to_user()` 列为可能睡眠的函数并禁止在不可睡眠上下文使用。这里是静态审查结论，不是设备上触发过的运行观察。[Kprobes 文档](https://docs.kernel.org/trace/kprobes.html)、[locking 文档](https://docs.kernel.org/kernel-hacking/locking.html)

## 构建适配流程

1. 对目标设备识别 Android common KMI 分支、架构、厂商构建标识、`.config`、编译器、`Module.symvers`、KMI 符号列表和模块签名策略。仅凭 `uname -r` 不足以识别全部 ABI 条件。
2. 使用与设备匹配的 DDK/内核构建目录，分别编译 ARM64 模块。构建日志记录具体 KDIR、配置和工具链，并检查 modpost 输出中没有未解析或受保护符号。
3. 静态核对 `misc_register`、`misc_deregister`、ioctl 所需符号是否在目标模块允许使用的 KMI 列表中且 CRC 一致。头文件里有声明、内核源码里有 `EXPORT_SYMBOL`，均不能单独证明某厂商目标允许 vendor module 使用。
4. 生成每个目标独立命名的模块与 SHA-256 清单，例如 `nbdriver-android15-6.6.ko`。不要将一种 KMI 的产物复制标注成所有版本。
5. QEMU 每个目标至少验证模块加载、节点创建、打开、ABI ioctl、未知 ioctl 错误、关闭及卸载；之后还要在真实目标设备验证节点权限、SELinux、签名与调用。QEMU 结果不替代真机结果。

只有在实际目标都通过这些步骤后，才为这些**明确列出的**目标报告兼容。不能据此声称覆盖所有 5.x/6.x。

## 本机环境观察（2026-11-12）

- 已连接设备报告 ARM64、Android 15、`6.6.30-酷安姜北尘`。该设备不是可识别的上游 `android15-6.6` 构建产物，因此只能作为独立厂商目标。
- 设备 config 可读，观察到 `CONFIG_MODULES=y`、`CONFIG_MODVERSIONS=y`、`CONFIG_MODULE_SIG=y`、`CONFIG_KPROBES=y`、`CONFIG_KALLSYMS=y`、`CONFIG_IKHEADERS=m`；未观察到 `CONFIG_MODULE_SIG_FORCE=y`。这不证明该设备接受任意模块，也不等于 DDK 或符号版本表已经取得。
- 构建容器、QEMU ARM64 system binary 和目标 `Module.symvers` 当前未在宿主可用环境中发现。
- 没有在设备加载本模块，也没有进行 ioctl 往返。真机集成状态为未验证。

## 依据与限制

- [Android stable KMI 文档](https://source.android.com/docs/core/architecture/kernel/stable-kmi)：KMI 稳定承诺针对指定 Android/LTS 分支，而非跨所有 Linux 版本通用。
- [Android ABI monitoring 文档](https://source.android.com/docs/core/architecture/kernel/abi-monitor)：KMI 使用符号列表及 ABI/CRC 检查；某符号在某内核实现中存在不表示任意外部模块都可引用。
- [Android kernel modules 文档](https://source.android.com/docs/core/architecture/kernel/modules)：GKI 与 vendor 模块有不同的符号访问范围。
- [Linux 6.6 外部模块构建文档](https://kernel.org/doc/html/v6.6/kbuild/modules.html)：外部模块需匹配目标内核构建；`modules_prepare` 本身不会生成启用 MODVERSIONS 所需的 `Module.symvers`。
- [Android common `android13-5.10` generic ABI 符号列表](https://android.googlesource.com/kernel/common/+/refs/heads/android13-5.10/android/abi_gki_aarch64_generic)：该特定列表明确包含 misc 注册/注销符号；不能泛化为所有分支。

更细的实际命令、编译输出和设备事实见 `docs/validation/latest.json`。不完整的目标符号清单不在这里推断为存在或缺失；逐目标 DDK 编译与设备验证仍是后续必需项。
