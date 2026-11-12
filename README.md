# nbdriver

面向 ARM64 Android 内核的驱动重写工程。

当前阶段：最小 `/dev/nbdriver` + ioctl 通信样例已通过六个 Android common KMI 的远程 DDK 编译，并在当前 PKR110 的厂商 6.6.30 内核通过加载、ioctl 和卸载/重新加载验证。尚未迁移业务功能；其他设备和 QEMU 运行待验证。

```text
nbdriver/
├── Makefile                 # 内核模块构建入口
├── build.sh                 # 构建辅助脚本
├── include/
│   └── nbdriver_abi.h       # 内核与用户态共用协议
├── src/
│   ├── Makefile             # Kbuild 对象列表
│   ├── nbdriver_main.c      # 模块生命周期
│   └── nbdriver_comm.c/.h   # misc 节点与 ioctl ABI
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

通信接口为常规 `/dev/nbdriver` 字符设备与 ioctl；共享 ABI 定义在 `include/nbdriver_abi.h`。兼容策略和未验证项见 `docs/kernel-compatibility.md`。

GitHub Actions 的 5.10、5.15、6.1、6.6、6.12、6.18 六个 KMI 构建和 SDK/ABI 检查均已通过：[运行记录](https://github.com/libUE4/nbdriver/actions/runs/43186897551)。具体构建源码、vermagic、哈希和限制见 [远程编译记录](docs/remote-build.md)。构建通过不等于目标设备已验证。

当前 6.6 设备的实际 ABI、错误返回和卸载恢复结果见 [设备验证记录](docs/device-6.6.md)。加载为临时操作，没有添加开机加载配置。

SDK 目前只封装打开设备与查询 ABI；业务模块、调用示例和设备回归测试尚未迁移。

新建源码采用 GPL-2.0-only；后续迁移其他来源代码时，应保留并核对原有许可与版权信息。
