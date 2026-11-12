# 远程编译验证记录

GitHub Actions：[43186897551](https://github.com/libUE4/nbdriver/actions/runs/43186897551)。构建源码提交：`8d2732a9db69cc76d4e7a2fab2e8586e445df4c7`。

六个 KMI 模块构建及 SDK/ABI 检查全部成功；六份下载的模块均通过 SHA-256 和 AArch64 ELF 核验。模块编译与 modpost 日志未发现 warning/error。

| KMI | DDK 产物 vermagic | 大小（字节） |
|---|---|---|
| `android12-5.10` | `5.10.252-dirty SMP preempt mod_unload modversions aarch64` | 270952 |
| `android13-5.15` | `5.15.202-android13-5.15.202_r00-dirty SMP preempt mod_unload modversions aarch64` | 275864 |
| `android14-6.1` | `6.1.166-dirty SMP preempt mod_unload modversions aarch64` | 273448 |
| `android15-6.6` | `6.6.127-4k-g46a034eca005-dirty SMP preempt mod_unload modversions aarch64` | 188280 |
| `android16-6.12` | `6.12.76-4k SMP preempt mod_unload modversions aarch64` | 201792 |
| `android17-6.18` | `6.18.32-4k-g47cc22554442-dirty SMP preempt mod_unload modversions aarch64` | 210576 |

完整哈希、符号导入、实际 job 状态和源码哈希见 [latest.json](validation/latest.json)。本地产物位于 `out/remote-43186897551/`；六目标汇总包为 `out/nbdriver-kmi-modules.tar.gz`，包内包含 `SHA256SUMS`。

修复过两类真实问题：主模块缺少 ABI 头文件已补齐；6.18 镜像展开耗尽 runner 磁盘，已改为清理临时 runner 的无用预装 SDK 后运行容器，并恢复输出文件归 runner 所有。原始失败 annotation 见 [runner-disk-failure.json](validation/runner-disk-failure.json)。

编译通过仅覆盖本次六个明确列出的 DDK 目标。后续在当前 PKR110 厂商 `6.6.30` 设备上，使用原始 6.6 产物通过普通 insmod 加载和 ioctl/卸载/重新加载验证，见 [设备验证记录](device-6.6.md)。版本字符串不同被该设备接受的原因未调查，不据此外推。QEMU 及其余目标设备运行仍未验证。

重构前 14 个文件的备份位于 `/root/workspace/.aicode/nbdriver/compat-audit-before-20261112`，带 `sha256.json`。恢复检查应复制该目录到独立目录并核验哈希，保留当前产物；不用将新模块产物覆盖原驱动。
