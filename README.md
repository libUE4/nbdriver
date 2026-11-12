# nbdriver

面向 ARM64 Android 内核的驱动重写工程。

当前阶段：项目结构与最小模块入口已建立。尚未迁移 NBG-driver 的功能，未定义业务 ioctl，也未完成目标内核编译或设备验证。

```text
nbdriver/
├── Makefile                 # 内核模块构建入口
├── build.sh                 # 构建辅助脚本
├── include/
│   └── nbdriver_abi.h       # 内核与用户态共用协议
├── src/
│   ├── Makefile             # Kbuild 对象列表
│   ├── entry.c              # 模块初始化与退出
│   ├── memory.c / memory.h  # 内存模块预留
│   └── process.c / process.h # 进程模块预留
├── sdk/
│   ├── include/             # SDK 对外接口
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

默认参数为 `ARCH=arm64 LLVM=1 LLVM_IAS=1`，可通过 make 参数覆盖。构建成功后模块保存到 `out/nbdriver.ko`。当前仅将 `src/entry.c` 纳入编译。

SDK、功能模块和测试目录目前为预留位置，不代表相应功能已经实现。

新建源码采用 GPL-2.0-only；后续迁移其他来源代码时，应保留并核对原有许可与版权信息。
