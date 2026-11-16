# nbdriver

面向 ARM64 Android 内核的匿名 FD / ioctl 驱动。本文按 2026-11-16 工作区源码核对；
ABI 为 **5**，模块元数据 `MODULE_VERSION` 仍为 `0.1.0`，两者含义不同。

连接有两条独立路径：动态选择系统调用表中的未实现槽位，以及 reboot kprobe 后备入口。
两条路径都通过 task_work 在调用线程上下文安装匿名 FD；拿到 FD 后使用 ioctl，UDP 采集另支持 poll。
18 只是首选候选号，不保证最终安装在 18，也不是固定 syscall 18 kprobe。没有 `/dev/nbdriver` 节点。

当前源码包括进程/SO/线程查询、读写与 READV、maps/VMA/页表映射、FD/KGSL 元数据、
可撤销的进程/GPU 可见性过滤、随机字节、自动 devpts 绑定、运行状态与控制、SF 线程权限组和 UDP 下行采集。
功能是否可用以 `GET_ABI.features`、具体请求结果与目标设备证据为准；ABI 相同不等于能力相同。

## 阅读入口

- [文档索引](docs/README.md)：当前接入说明与历史证据的分类。
- [当前实现与完整 ABI 清单](docs/current-state.md)：27 个命令、能力位、权限和边界。
- [SDK 接入](sdk/README.md)：单头与编译型 SDK、返回值和使用示例。
- [通信机制](docs/communication.md)、[运行与卸载生命周期](docs/runtime-lifecycle.md)。
- [构建/测试说明](docs/testing.md)、[内核兼容策略](docs/kernel-compatibility.md)、[原生加载器](docs/kernel-compatibility-loader.md)。

## 源码结构

```text
nbdriver/
├── sdk/include/nbdriver_abi.h   # 内核与用户态共用协议基准
├── sdk/include/nbdriver.h       # 独立 C/C++ 单头：镜像协议 + inline 包装
├── sdk/include/nbdriver/client.h # 编译型 SDK 声明，接口是单头的子集
├── sdk/src/client.c             # 编译型 SDK 实现，需链接并包含共享 ABI
├── src/
│   ├── nbdriver_core.c          # 初始化、匿名 FD/ioctl、读写、查询、页表
│   ├── nbdriver_features.c      # maps/FD/KGSL/random、devpts、proc 过滤
│   ├── nbdriver_runtime.c       # ready/resident、handle/token、QUIESCE/RESUME
│   ├── nbdriver_platform.c      # 双入口、task_work、ksym、CFI、日志
│   ├── nbdriver_sf_groups.c     # SF 调用线程的固定补充组请求
│   ├── nbdriver_udp.c           # FD 所有权、IPv4 UDP 下行队列与 poll
│   └── Makefile                # 上述六个 Kbuild 对象
├── tools/                      # 加载器、导入符号/回调尾调用审计
├── tests/nbdriver_smoke.c       # 保留的综合设备测试源码
└── docs/                       # 当前说明、历史记录、绑定版本的验证数据
```

分段 `BEGIN/END src/...` 标记是前三个合并文件的历史来源边界，不是磁盘上的现有子目录。
完整职责见[文件组织](docs/structure.md)。

## 构建

在 `nbdriver/` 内，用与目标设备匹配且完成准备的 ARM64 内核构建目录执行：

```sh
./build.sh build KDIR=/path/to/kernel/build
make loader SMOKE_CC=cc
```

默认 `ARCH=arm64 LLVM=1 LLVM_IAS=1`，支持 make 参数覆盖。模块复制到 `out/nbdriver.ko`；
原生加载器默认输出 `out/nbdriver-loader`。`loader` 使用指定编译器的默认目标，不自动交叉编译为 Android。
`smoke` 才内置 Android ARM64 工具链参数；工具链位置、交叉编译和校验命令见[测试说明](docs/testing.md)。

`./build.sh clean KDIR=...` 会清理 Kbuild 输出并删除 `out/nbdriver.ko`，需要时显式执行。

## 加载与退出边界

原生加载器做 root、可选内核版本前缀、必需符号名称、原件 SHA256、私有副本及 finit_module 检查，
最后执行 ABI/runtime 握手。它不下载模块、不自动选择内核分支、不提供服务器上报或透明热切换。
上层 NBG 应用的下载、授权、菜单和退出命令不属于本驱动 SDK。

关闭辅助程序或最后一个连接 FD，不等于卸载驱动。表入口持有模块引用时，需先完成 QUIESCE，
再关闭全部 FD 并正常卸载；失败不应强制卸载。RESUME 不恢复旧 UDP 会话。
详细顺序、保留资源和失败检查见[生命周期](docs/runtime-lifecycle.md)。

CI 配置列出八个 KMI 目标，历史远程记录是六目标构建。它们都不能证明当前源码已在全部目标实机运行。
本次文档更新不刷入设备、不改变运行代码；测试结果及未覆盖范围见[测试说明](docs/testing.md)。

新建源码采用 GPL-2.0-only；保留原有来源、版权及许可标记。
