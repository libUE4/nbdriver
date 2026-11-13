# nbdriver 文件结构（2026-11-13）

源码实现合并为三个编译单元，减少跨目录跳转；私有头文件仍单独保留，避免把声明和实现揉成不可维护的大文件。

```text
nbdriver/
├── include/nbdriver_abi.h
│   └── 内核/用户共用 ABI、命令号、能力位、结构体
├── sdk/include/nbdriver.h
│   └── 可独立复制的 C/C++ 单头接口
├── src/
│   ├── nbdriver_core.c
│   │   └── 模块生命周期、匿名 FD/ioctl、读写、PID/SO/线程查询、页表遍历
│   ├── nbdriver_features.c
│   │   └── maps、地址映射、FD/GPU/random 元数据、/dev/pts 过滤
│   ├── nbdriver_platform.c
│   │   └── 入口探针、reboot/task_work、运行时符号、CFI/KCFI、日志
│   ├── nbdriver_*.h
│   │   └── 私有声明，统一从 src/ 引用
│   └── Makefile
├── tests/
└── docs/
```

`include/nbdriver_abi.h` 和 `sdk/include/nbdriver.h` 仍是公共接口，不与内核实现合并。三份 `.c` 使用分段标记保留原始来源边界，便于回溯和主机回归测试。Kbuild 只编译 `nbdriver_core.o`、`nbdriver_features.o`、`nbdriver_platform.o`。
