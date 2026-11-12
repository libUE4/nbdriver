# 通信机制与迁移基线

本记录基于工作区内的 enkernel、LinyuKernel 和 NBG-driver 源码静态阅读。未加载原项目驱动，未执行其隐藏入口，未验证原项目设备行为。当前 nbdriver 正在改为匿名 FD + ioctl：只采用 enkernel 的 task_work 延迟 FD 安装与 ARM64 wrapper 参数读取，不采用其 syscall 表改写路径。

## enkernel

主要控制路径：SDK 获取匿名 FD → ioctl(fd, command, request) → drv_ioctl → 功能处理函数。

- `syscall.c` 查找系统调用表内指向未实现处理函数的槽位，将选中的槽位改为 FD 安装入口。
- `sdk/src/driver.cpp` 的初始化函数扫描候选系统调用号，传入握手参数和 FD 输出指针；失败后尝试 reboot 系统调用入口。
- `main.c` 在 reboot 内核入口注册 kprobe，通过 task_work 在进程上下文中创建 FD，并写入用户提供的输出位置。
- FD 使用 `anon_inode_getfile`、`get_unused_fd_flags` 和 `fd_install` 建立，绑定 `drv_fops`；主要业务通过 `.unlocked_ioctl` 分发，关闭时通过 `.release` 清理相关状态。
- 协议统一定义于根目录 `abi.h`，SDK 构建时复制同一份协议。

这是本地用户态与内核态之间的控制接口。UDP 抓取属于业务功能，不代表 SDK 使用 UDP 与驱动通信。

## LinyuKernel

主要控制路径：SDK 调用带握手参数的 reboot 系统调用 → 探针入口创建匿名 FD → SDK 调用 ioctl → linyu_anon_ioctl 查表分发。

- SDK 的 `lingyu_kernel_open` 期望系统调用返回值本身就是 FD。
- `src/entry.c` 优先对 `__arm64_sys_reboot` 注册 kprobe，失败后尝试 `__se_sys_reboot`。
- 探针通过匿名 inode 建立 FD，并尝试修改寄存器以返回 FD。
- 协议使用 `_IO/_IOR/_IOW/_IOWR` 命令定义。协议结构和命令号分别存在于 `src/comm.h` 与 SDK 头文件，两处需要保持一致。
- bindproc 的另一路文件操作表声明了 read/write/mmap，但当前对应函数均返回 `-EINVAL`，不能据此认定实现了共享内存或数据流通信。

需要验证的入口问题：当前 pre_handler 直接将 `regs->regs[0]` 当作用户传入的握手值，而 enkernel 的同名 ARM64 包装入口先将它解释为内层 `pt_regs` 指针。两个探针目标的调用约定和跳过函数的寄存器处理，应对照目标内核确认。SDK 的期望行为不能代替实机验证。

## 现有 NBG-driver

控制路径：创建自定义 socket family 的 FD → 初始化握手 → 在 socket FD 上调用 ioctl → nbg_comm_ioctl_dispatch。

- `src/comm/socket_ioctl.c` 通过 `sock_register` 选择可注册的协议族，建立 socket 专属上下文。
- `OP_INIT_KEY` 校验协议版本与握手令牌，业务分发要求该连接已通过校验。
- `proto_ops.ioctl` 指向业务分发；sendmsg/recvmsg 当前返回 `-EOPNOTSUPP`。
- 因此使用了 socket 对象，但此控制路径不通过 TCP/UDP 收发请求，也不依赖 IP 或端口。

## nbdriver 的迁移拆分

1. `include/nbdriver_abi.h` 是唯一协议定义，用户态与内核态共用，结构使用固定宽度字段。
2. 当前选匿名 FD + ioctl；SDK 通过 reboot magic 握手请求 FD。
3. kprobe pre-handler 只解析 wrapper/参数并将工作排入 `task_work`；分配 FD、anon inode 和用户内存回写在任务上下文执行。
4. 不移植 enkernel 的 syscall table 扫描或改写路径。
5. 入口选择依据见 [communication-comparison.md](communication-comparison.md)。改动完成后必须重跑六 KMI DDK 矩阵，再对 PKR110 执行加载、ioctl 和卸载恢复验证。
6. 后续功能按实际模块命名，并在 `src/Makefile` 显式添加对象。

本阶段只切换 ABI 查询通信入口，没有迁移 NBG-driver 业务功能。
