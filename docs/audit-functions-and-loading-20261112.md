# nbdriver 全函数与实机加载审计（2026-11-12）

对象是 `nbdriver` 的源码提交 `b03910ee301968804c808d6e7b0511402f819551`
和由它编译的 6.6 模块，不把另一个 `NBG-driver` 或旧 misc 入口的测试结果
算作本轮验证。源码及模块原始 SHA-256 在
`out/audit-20261112/SHA256SUMS`。本轮未修改模块源码、未重新加载模块。

## 函数清单与证据

| 范围 | 实际函数 | 判断 |
| --- | --- | --- |
| 生命周期 | `nbdriver_init`, `nbdriver_exit` | `init`: comm→ksym→cfi→entry，各失败分支回收已初始化部分；`exit`: 入口→CFI→符号→通信。已实机验证初始化；本版卸载未实测。 |
| 入口分发 | `nbdriver_entry_init`, `nbdriver_entry_exit`, `nbdriver_entry_target` | 仅转接 reboot 探针，无第二入口。初始化目标从实机日志确认为 `__arm64_sys_reboot`；卸载调用顺序静态检查。 |
| 探针与异步回调 | `nbdriver_queue_task_work`, `nbdriver_fd_work_fn`, `nbdriver_schedule_fd`, `nbdriver_reboot_pre`, `nbdriver_register_probe`, `nbdriver_reboot_entry_init`, `nbdriver_reboot_entry_exit`, `nbdriver_reboot_entry_target` | 动态解析 `task_work_add`，探测系统调用魔数后在 task_work 中检查 root 并创建 FD。实机 root 路径及非 root 拒绝均通过；低内存、任务退出和卸载竞争未实机模拟。 |
| 符号解析 | `nbdriver_ksym_init`, `nbdriver_ksym_lookup`, `nbdriver_ksym_exit` | 使用 kprobe 取 `kallsyms_lookup_name` 后即时注销探针；当前设备 init 日志 `status=ready`。其他厂商内核禁用/隐藏 kprobe 符号时可能失败。 |
| CFI | `nbdriver_cfi_init`, `nbdriver_cfi_exit`；旧 CFI 构建的 `nbdriver_cfi_patch`, `__ubsan_handle_cfi_check_fail_abort` | 6.6 实机采用 `local_calls`，原崩溃点 task_work 间接调用已通过。5.10/5.15 编译了旧版分支，**没有对应实机验证**；若能解析补丁 API，会修改内核全局 slowpath，两条指令的回滚/卸载恢复失败时有系统性风险，不能把编译通过当成安全。 |
| FD/IOCTL | `nbdriver_ioctl`, `nbdriver_comm_install_fd`, `nbdriver_comm_init`, `nbdriver_comm_exit` | `comm_init/exit` 当前是空操作（没有注册设备节点）；匿名 FD 的 ABI 查询、空参数、未知命令、关闭与重开已实机验证。只实现 `NBDRIVER_IOC_GET_ABI`，返回 `features=0`。 |
| 诊断 | `nbdriver_log_missing_once` | 记录符号解析失败；当前设备没有出现符号缺失，其他设备的失败路径未触发。 |
| 用户态 SDK | `nbdriver_open`, `nbdriver_get_abi` | root 查询与循环调用通过；非 root 返回 `ENODEV`，设备日志显示 `reason=not_root`。SDK 在未得到 FD 时统一报 `ENODEV`，不保留底层各阶段精确错误码，排障须读 dmesg。 |

上表覆盖 `src/*.c` 的所有具体函数定义（不含 `module_init/exit` 宏和外部内核 API）。
没有漏编译本项目已有的对象：`src/Makefile` 的 7 个对象均参与 8 KMI 构建。
**但这不是一个具备内存读写、进程操作、触控等业务接口的驱动**：当前 ABI
仅有查询命令且 `features=0`，这些功能不是“已存在却漏加载”，而是尚未实现。

## 编译和设备检查

- GitHub Actions 运行 `43229836652`：8 KMI 构建及 ABI job 全成功；8 份
  工件均核对清单 SHA-256 与 ELF64 AArch64；`out/audit-20261112/imports.json`
  记录每份未定义导入，均没有静态导入未导出的 `task_work_add`。日志在
  `out/remote-43229836652/logs/`。这只是目标 DDK 的构建和符号验证。
- 当前设备 `192.168.1.4:41353` 是 PKR110，内核
  `6.6.30-酷安姜北尘`；目标模块的 `vermagic` 是
  `6.6.127-4k-g46a034eca005-dirty ...`。虽然不相同，这台设备已接受
  **原样** `.ko` 的普通 root `insmod`。不能外推到其他设备或固件。
- 设备与宿主模块 SHA-256 相同：
  `e097729d6e1c3aa6699573438f1ad5a0b72a15e0c70080eab28f36bfcaa57765`。
  `insmod` 退出码 0；`/proc/modules` 为 Live；boot ID 前后均为
  `8624d3fb-728a-4e07-b428-cabad0f98ea2`。
- root smoke：`ABI 2 / size 16 / features 0`，3 项错误返回符合预期，
  16 次 reopen/query/close 通过；非 root `--expect-absent` 通过。
  原始命令、输出、退出码在 `out/device-cfi-load-20261112T135947Z/`。

## 尚可能“刷不进去”或运行失败的情况

1. **已经加载同名模块**：再次 `insmod nbdriver` 不等于升级；需要先结束
   所有打开的 FD、卸载旧模块，再加载新模块。当前实机模块仍在运行；
   本轮没有主动卸载或重载。
2. **别的设备或内核**：`CONFIG_MODULES`、模块签名强制、vermagic/符号
   CRC、Kprobe 限制、厂商符号裁剪等都可能令加载失败；仅本台 6.6
   设备已实机验证，不保证 5.10/5.15/6.1/6.12/6.18 的加载。
3. **入口初始化依赖**：即使 `.ko` 的导出符号齐全，运行时还需用 kprobe
   找到 `kallsyms_lookup_name`，再找到 `task_work_add` 和
   `__arm64_sys_reboot`，且探针必须允许注册；任一缺失都会在 init 返回
   非零并记录具体 stage，不应绕过失败强制加载。
4. **旧 CFI 分支**：5.10/5.15 的全局 slowpath 补丁在实际厂商内核
   上未验证；特别是 `nbdriver_cfi_exit()` 恢复失败只记录错误但依然清空
   本地状态，不能保证内核文本已恢复。**不要把这四份模块当成已经
   实机安全验证的版本**。6.6 当前构建不走该分支。
5. **安装方式**：本轮是临时 `insmod`，没有写 boot 分区，也没有配置
   开机自动加载；重启后是否自动存在不能由本次测试推出。

结论：在这台 PKR110 上，**现有通信骨架可用，当前没有已复现的 6.6
刷入失败**；长期稳定性、新版本卸载恢复、其他 KMI 的厂商设备仍待验证。
实机保留模块原样运行。卸载命令（仅在确认没有进程使用 FD 后执行）：
`adb -s 192.168.1.4:41353 shell "su -c 'rmmod nbdriver'"`。
