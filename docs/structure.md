# 文件组织约定

- `include/nbdriver_abi.h` 仅存放内核与用户态共用的协议定义，不依赖内核内部类型。
- `src/nbdriver_main.c` 管理模块生命周期；`src/nbdriver_comm.c/.h` 管理 misc 设备和 ioctl 分发。
- 后续功能按实际模块拆分，同名头文件提供模块内部接口；完成实现后显式加入 `src/Makefile`。
- `include/nbdriver_abi.h` 是内核和用户态共用协议的唯一来源。
- `sdk/include/nbdriver/` 是 SDK 对外接口；`sdk/src/` 实现调用封装；示例按实际需求添加到 `sdk/examples/`。
- `tests/` 存放 ABI 和功能测试；设备测试注明目标内核、命令和实际结果。
- `.github/workflows/kernel-matrix.yml` 分别构建六个代表性 Android common KMI；CI 构建结果不代表 QEMU 或真机运行通过。
- `out/` 仅保存构建产物，Git 只跟踪目录占位文件。

nbdriver 是独立工程。迁移其他来源代码时，保留并核对原有许可与版权信息。
