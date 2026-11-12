# 文件组织约定

- `include/nbdriver_abi.h` 仅存放内核与用户态共用的协议定义，不依赖内核内部类型。
- `src/entry.c` 管理生命周期；功能实现按模块拆分，同名头文件存放模块内部接口。
- `src/Makefile` 明确列出参与编译的源文件。预留文件实现完成后再加入对象列表。
- `sdk/include/` 为 SDK 对外接口，`sdk/src/` 为内部实现，`sdk/examples/` 为调用示例。
- `tests/` 存放后续测试；设备测试应注明目标内核及实际执行结果。
- `out/` 仅保存构建产物，Git 只跟踪目录占位文件。

本次仅初始化独立工程，不复制或修改原 NBG-driver、enkernel、LinyuKernel。
