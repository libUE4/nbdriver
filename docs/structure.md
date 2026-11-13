# 文件组织约定

- `include/nbdriver_abi.h` 仅存放内核与用户态共用协议定义。
- `sdk/include/nbdriver.h` 是独立用户态单头接口。
- `src/nbdriver_core.c` 合并生命周期、通信、读写、进程/线程查询和页表遍历。
- `src/nbdriver_features.c` 合并 maps、mapping、FD/GPU/random 和 devpts 终端过滤。
- `src/nbdriver_platform.c` 合并 ksym、CFI、入口探针和日志。
- `src/nbdriver_*.h` 只保留模块内部声明，作为统一 include 根；公共协议不放入私有头。
- `src/Makefile` 显式只列出三个实现对象。
- `tests/` 存放 ABI、主机和设备测试；`out/` 仅保存构建产物和恢复记录。

三份实现文件内用 `BEGIN/END src/...` 标记保留原始模块边界，修改时仍可按功能定位。迁移其他来源代码时，保留并核对原有许可与版权信息。
