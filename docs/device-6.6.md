# 6.6 设备加载与通信验证

结果：验证通过。当前已连接的 OnePlus PKR110（Android 15，`6.6.30-酷安姜北尘`）已加载 nbdriver，最终状态保持运行。未改动 boot、分区或开机加载配置。

加载文件为远程构建的 `nbdriver-android15-6.6.ko` 原件，SHA-256：

```text
dd04b9705001a8c8dd8a72fd7862b71b974d4155bc765f88b256ec86e5a9cc21
```

使用普通 `insmod`，未修改文件，也未请求强制加载。模块 vermagic 中的 `6.6.127` 与设备版本不同，但当前设备接受该原件；接受原因没有调查，不能外推到其他设备或内核补丁版本。

实际调用结果（重新加载后再次验证）：

```text
abi: version=2 size=16 features=0 reserved=0
null_output: result=-1 errno=14 expected_errno=14
unknown_command: result=-1 errno=25 expected_errno=25
closed_fd: result=-1 errno=9 expected_errno=9
reopen_query_close: iterations=16
device_ioctl_smoke: PASS
```

卸载恢复已执行：`rmmod nbdriver` 后 `/dev/nbdriver` 和 `/sys/module/nbdriver` 均消失，SDK 打开返回 `ENOENT`；重新加载同一个文件后，以上查询和错误返回再次通过。最终节点为 root 所有、权限 `0600`。

内核日志：

```text
[101210.892978] nbdriver: 已加载, abi=2
[101391.507413] nbdriver: 已卸载
[101391.670748] nbdriver: 已加载, abi=2
```

本次两个 ADB 入口报告同一 boot ID；最终检查显式使用无线设备地址，避免出现两个 transport 时选错设备。

实际功能仅限通信骨架，`features=0`。内存、进程、触控等业务功能尚未迁移。其他五个 KMI 目前只有编译证据。

文件与复核入口：

- [结构化设备验证记录](validation/device-6.6-20261112.json)：文件哈希、每阶段命令/输出/退出码与限制。
- [设备 smoke 测试源码](../tests/device_ioctl_smoke.c)：使用 SDK 查询 ABI，检查错误返回和 FD 生命周期。
- 完整原始命令及输出：`/root/workspace/nbdriver/out/device-6.6-20261112T090714Z/commands.json`。对应 `.bin` 保存原始 stdout/stderr，显示文本使用替代解码保留不可打印日志证据。
- 设备上的已验证卸载脚本：`/data/local/tmp/nbdriver-device-6.6-20261112T090714Z/unload.sh`。

需要卸载当前模块时：

```sh
adb -s 10.110.150.20:32907 shell "su -c 'rmmod nbdriver'"
```

重新构建同一测试程序的宿主命令（AArch64 Linux 宿主，需静态 libc）：

```sh
cc -D_GNU_SOURCE -std=c11 -O2 -static -Wall -Wextra -Werror \
  -Iinclude -Isdk/include tests/device_ioctl_smoke.c sdk/src/client.c \
  -o out/device_ioctl_smoke
```
