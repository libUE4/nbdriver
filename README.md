# nbdriver

面向 ARM64 Android 内核的驱动重写工程。

当前阶段：具备最小 `/dev/nbdriver` + ioctl 通信样例和共享 ABI 查询。尚未迁移业务功能；目标内核模块编译、QEMU 与设备调用都待验证。

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

GitHub Actions 为 5.10、5.15、6.1、6.6、6.12、6.18 六个 Android common KMI 配置独立构建任务。任务尚未运行；通过构建也不等于目标设备已验证。

SDK 目前只封装打开设备与查询 ABI；业务模块、调用示例和设备回归测试尚未迁移。

新建源码采用 GPL-2.0-only；后续迁移其他来源代码时，应保留并核对原有许可与版权信息。
