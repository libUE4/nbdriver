# nbdriver

面向 ARM64 Android 内核的驱动重写工程。

当前通信为匿名 FD + ioctl：reboot kprobe 只排入 task_work，FD 安装在任务上下文完成。源码实现合并为三个编译单元，用户态复制 `sdk/include/nbdriver.h` 即可接入。

2026-11-14 已修复审查列出的模块引用尾部竞态、全局 CFI 补丁、GPU 查询上下文、写权限、PID 重用/命名空间、argv[0] 部分读取、线程采样和诊断问题。具体实现、验证哈希与实机/其他 KMI 的状态见 [当前修复记录](docs/audit-fixes-20261114.md)；历史发现保存在 [审查报告](docs/driver-code-review-20261114.md)。

功能包括进程/SO/线程查询、普通读写与批量读、maps/地址映射、FD 大小、KGSL GPU 元数据、随机字节和双包自动 devpts 过滤。各能力受目标后端和权限约束，详见 [SDK 说明](sdk/README.md)。

终端过滤随模块加载自动启动，匹配 `com.tencent.tmgp.dfm` 和 `com.tencent.rmcn`，支持同时运行与重启重新绑定；SDK 仅保留只读状态查询。[本轮实现与实测](docs/devpts-auto-20261114.md)。

```text
nbdriver/
├── include/nbdriver_abi.h       # 公共 ABI、命令和结构
├── sdk/include/nbdriver.h       # 独立 C/C++ 单头 SDK
├── src/
│   ├── nbdriver_core.c          # 生命周期、通信、读写、查询、页表
│   ├── nbdriver_features.c      # maps、mapping、FD/GPU/random、devpts
│   ├── nbdriver_platform.c      # ksym、CFI、入口、日志
│   ├── *.h                      # 私有实现声明（统一 include 根）
│   └── Makefile                 # 三个 Kbuild 对象
├── tests/                       # 主机、SDK、设备测试
├── docs/                        # 迁移、兼容和验证记录
└── out/                         # 本地产物和恢复记录
```

## 构建

需要与目标设备匹配、完成准备的 ARM64 内核构建目录及 LLVM 工具链：

```sh
./build.sh build KDIR=/path/to/kernel/build
./build.sh clean KDIR=/path/to/kernel/build
```

默认参数为 `ARCH=arm64 LLVM=1 LLVM_IAS=1`，可通过 make 参数覆盖。构建成功后模块保存到 `out/nbdriver.ko`。

通信接口为匿名 FD + ioctl；共享 ABI 定义在 `include/nbdriver_abi.h`。入口比较见 [通信方式比较](docs/communication-comparison.md)，兼容策略见 `docs/kernel-compatibility.md`。

CI 配置覆盖八个代表性 Android KMI；当前版本的实际运行范围以修复记录和对应工件哈希为准。历史矩阵记录见 [远程构建](docs/remote-build.md)，历史设备测试见 [元数据记录](docs/maps-metadata-20261113.md)，不将它们当作当前版本的全矩阵运行证明。

用户态可直接复制 `sdk/include/nbdriver.h` 接入，采用 LinyuKernel 风格的独立 C/C++ 单头接口。新查询和批量读接口、限制及调用示例见 [SDK 说明](sdk/README.md)。

新建源码采用 GPL-2.0-only；后续迁移其他来源代码时，应保留并核对原有许可与版权信息。
