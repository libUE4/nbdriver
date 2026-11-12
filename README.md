# nbdriver

面向 ARM64 Android 内核的驱动重写工程。

当前通信入口改为匿名 FD + ioctl：reboot kprobe 只排入 task_work，FD 安装在任务上下文完成。此次改动尚未完成远程构建或设备复测；业务功能仍未迁移。

本轮已按工作区 lsdriver 接入 CFI/KCFI 兼容层，并修正 task_work
动态函数指针类型。崩溃证据、迁移范围与验证限制见
[CFI/KCFI 迁移记录](docs/cfi-port-20261112.md)。

```text
nbdriver/
├── Makefile                 # 内核模块构建入口
├── build.sh                 # 构建辅助脚本
├── include/
│   └── nbdriver_abi.h       # 内核与用户态共用协议
├── src/
│   ├── Makefile             # Kbuild 对象列表
│   ├── nbdriver_main.c      # 模块生命周期
│   ├── nbdriver_comm.c/.h   # anon FD 与 ioctl ABI
│   ├── nbdriver_entry.c/.h  # 安装入口生命周期
│   └── nbdriver_entry_reboot.c # reboot probe 与 task_work
├── sdk/
│   ├── include/nbdriver/    # SDK 对外接口
│   ├── src/                 # SDK 实现
│   └── examples/            # 使用示例
├── tests/                   # 后续功能测试
├── docs/                    # 结构与开发说明
└── out/                     # 本机构建产物，不提交 Git
```

## 构建

需要与目标设备匹配、完成准备的 ARM64 内核构建目录及 LLVM 工具链：

```sh
./build.sh build KDIR=/path/to/kernel/build
./build.sh clean KDIR=/path/to/kernel/build
```

默认参数为 `ARCH=arm64 LLVM=1 LLVM_IAS=1`，可通过 make 参数覆盖。构建成功后模块保存到 `out/nbdriver.ko`。

通信接口为匿名 FD + ioctl；共享 ABI 定义在 `include/nbdriver_abi.h`。入口比较见 [通信方式比较](docs/communication-comparison.md)，兼容策略见 `docs/kernel-compatibility.md`。

GitHub Actions 的 5.10、5.15、6.1、6.6、6.12、6.18 六个 KMI 构建和 SDK/ABI 检查均已通过：[运行记录](https://github.com/libUE4/nbdriver/actions/runs/43186897551)。具体构建源码、vermagic、哈希和限制见 [远程编译记录](docs/remote-build.md)。构建通过不等于目标设备已验证。

针对后续 `nbdriver_reboot_pre` 崩溃的 CFI 修复、8 KMI 远程编译和 6.6 实机通信验证见 [CFI 迁移与验证](docs/cfi-port-20261112.md)。其余 KMI 的新模块尚未在目标设备验证。

之前 misc 版本在 PKR110 6.6.30 的验证记录见 [设备验证记录](docs/device-6.6.md)，只作为历史基线；本轮入口切换后需重新远程构建和复测。

SDK 目前只封装打开设备与查询 ABI；业务模块、调用示例和设备回归测试尚未迁移。

新建源码采用 GPL-2.0-only；后续迁移其他来源代码时，应保留并核对原有许可与版权信息。
