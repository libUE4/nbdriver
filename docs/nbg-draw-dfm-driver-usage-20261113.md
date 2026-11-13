# NBG-Draw-dfm 使用的驱动函数（排除断点）

2026-11-13。按业务源码调用点和 Android.mk 编译范围静态核对，并追踪 coord_driver_bridge 到解密代码的引用；不是设备运行调用轨迹。已排除 shadow/watch/硬件断点相关接口。

原先的九项迁移任务已暂停；本次没有修改驱动内核、ABI 或应用代码。

扫描排除注释与字符串，区分业务调用与对接头中仅有声明/实现的函数。下表包含启动辅助 NBG_InitDriver 内的 setLogLevel。

| 类别 | 函数 | 实际协议 | 用途及条件 | 调用位置 |
|---|---|---|---|---|
| 连接 | `check_driver_available()` | `OP_INIT_KEY` | 扫描 socket family 并验证认证响应 | [main.cpp:68](/root/workspace/NBG-Draw-dfm/jni/main.cpp:68) |
| 连接 | `auto_initialize()` | `OP_INIT_KEY；已连接时更新本地目标 PID` | 启动和目标重连 | [kernel_rw.cpp:1207](/root/workspace/NBG-Draw-dfm/jni/src/driver/kernel_rw.cpp:1207)、[game_runtime.cpp:598](/root/workspace/NBG-Draw-dfm/jni/src/Android_draw/game_runtime.cpp:598) |
| 连接 | `setLogLevel()` | `OP_SET_LOG_LEVEL` | NBG_InitDriver 启动时设置日志等级 | [kernel_rw.cpp:1212](/root/workspace/NBG-Draw-dfm/jni/src/driver/kernel_rw.cpp:1212) |
| 连接 | `is_available()` | `无新增 ioctl` | UI 查询本地已初始化状态 | [MainUI.h:241](/root/workspace/NBG-Draw-dfm/jni/src/ui/MainUI.h:241) |
| 连接 | `cleanup()` | `OP_DEVPTS_FILTER(DISABLE) + close` | 禁用终端过滤并关闭通道 | [main.cpp:208](/root/workspace/NBG-Draw-dfm/jni/main.cpp:208) |
| 进程 | `GetPid()` | `OP_FIND_PID` | 查 com.tencent.tmgp.dfm，并更新封装内的目标 PID | [game_runtime.cpp:590](/root/workspace/NBG-Draw-dfm/jni/src/Android_draw/game_runtime.cpp:590)、[main.cpp:78](/root/workspace/NBG-Draw-dfm/jni/main.cpp:78) |
| SO | `GetModuleBase()` | `OP_MODULE_BASE` | 查 libUE4.so 基址 | [game_runtime.cpp:609](/root/workspace/NBG-Draw-dfm/jni/src/Android_draw/game_runtime.cpp:609) |
| 进程 | `getProcessStartTime()` | `NBG_RO_THREAD / IDENTITY` | 启动身份和读取前后复核，防止 PID 重用或进程切换 | [coord_driver_bridge.cpp:86](/root/workspace/NBG-Draw-dfm/jni/src/decrypt/coord_driver_bridge.cpp:86)、[game_runtime.cpp:618](/root/workspace/NBG-Draw-dfm/jni/src/Android_draw/game_runtime.cpp:618) |
| 读取 | `read()` | `OP_READ_MEM，失败时 OP_READ_MEM_FAULT` | 普通缓冲区读及 read<T>，失败清零 | [coord_driver_bridge.cpp:76](/root/workspace/NBG-Draw-dfm/jni/src/decrypt/coord_driver_bridge.cpp:76)、[game_runtime.cpp:993](/root/workspace/NBG-Draw-dfm/jni/src/Android_draw/game_runtime.cpp:993)、[draw_runtime.cpp:218](/root/workspace/NBG-Draw-dfm/jni/src/Android_draw/draw_runtime.cpp:218) |
| 读取 | `readPresent()` | `OP_READ_MEM` | PhysX 内存读取，先只读已驻留页 | [game_runtime.cpp:842](/root/workspace/NBG-Draw-dfm/jni/src/Android_draw/game_runtime.cpp:842) |
| 读取 | `readFault()` | `OP_READ_MEM_FAULT` | PhysX 普通读失败后的补页读取 | [game_runtime.cpp:845](/root/workspace/NBG-Draw-dfm/jni/src/Android_draw/game_runtime.cpp:845) |
| 读取 | `readVector()` | `OP_READV_MEM；失败项回退 OP_READ_MEM / OP_READ_MEM_FAULT` | 实体、骨骼、坐标解密批量读取 | [coord_driver_bridge.cpp:120](/root/workspace/NBG-Draw-dfm/jni/src/decrypt/coord_driver_bridge.cpp:120)、[dfm_player_esp.cpp:166](/root/workspace/NBG-Draw-dfm/jni/src/Android_draw/dfm_player_esp.cpp:166) |
| 用户态辅助 | `getUTF8()` | `内部调用 read，无独立命令` | 读取 UTF-16 名称并转换 UTF-8 | [dfm_player_esp.cpp:1239](/root/workspace/NBG-Draw-dfm/jni/src/Android_draw/dfm_player_esp.cpp:1239) |
| 用户态辅助 | `GetName()` | `内部调用 read，无独立命令` | 读取并解码 FName/GNames | [draw_runtime.cpp:129](/root/workspace/NBG-Draw-dfm/jni/src/Android_draw/draw_runtime.cpp:129) |
| 线程 | `getThreadId()` | `OP_GET_THREAD_TID` | 按 GameThread 名称选择线程 | [coord_driver_bridge.cpp:184](/root/workspace/NBG-Draw-dfm/jni/src/decrypt/coord_driver_bridge.cpp:184) |
| 线程 | `readThreadEnvironment()` | `OP_THREAD_ENV` | 坐标解密所需 TLS、APGA 密钥和有效位 | [coord_driver_bridge.cpp:189](/root/workspace/NBG-Draw-dfm/jni/src/decrypt/coord_driver_bridge.cpp:189) |
| 线程 | `ReadThreadFloatingState()` | `NBG_RO_THREAD_FPSTATE` | TLS 校验、FPSR/FPCR、启动身份和采样时间；不包含 Q 寄存器 | [coord_driver_bridge.cpp:40](/root/workspace/NBG-Draw-dfm/jni/src/decrypt/coord_driver_bridge.cpp:40) |
| 映射 | `getProcessMaps()` | `NBG_RO_MAPS` | 解密器扫描与映射快照 | [coord_driver_bridge.cpp:144](/root/workspace/NBG-Draw-dfm/jni/src/decrypt/coord_driver_bridge.cpp:144) |
| 映射 | `queryMapping()` | `OP_QUERY_MAPPING` | 页物理映射、页大小和标志 | [coord_driver_bridge.cpp:137](/root/workspace/NBG-Draw-dfm/jni/src/decrypt/coord_driver_bridge.cpp:137) |
| 目标元数据 | `getFileDescriptorSize()` | `NBG_RO_FD` | 用户态重放查询目标 FD 大小 | [coord_driver_bridge.cpp:213](/root/workspace/NBG-Draw-dfm/jni/src/decrypt/coord_driver_bridge.cpp:213) |
| 目标元数据 | `readGpuObjectInfo()` | `OP_KGSL_GPUOBJ_INFO` | 解密/重放所需的目标 GPU 对象元数据 | [coord_driver_bridge.cpp:68](/root/workspace/NBG-Draw-dfm/jni/src/decrypt/coord_driver_bridge.cpp:68) |
| 系统数据 | `readRandom()` | `NBG_RO_RANDOM` | 为用户态重放提供随机字节 | [coord_driver_bridge.cpp:59](/root/workspace/NBG-Draw-dfm/jni/src/decrypt/coord_driver_bridge.cpp:59) |
| 输入 | `setResolution()` | `OP_SET_RESOLUTION` | 初始化触摸坐标范围 | [game_runtime.cpp:655](/root/workspace/NBG-Draw-dfm/jni/src/Android_draw/game_runtime.cpp:655) |
| 输入 | `queryTouchDiag()` | `OP_TOUCH_DIAG` | 查询触摸设备、槽位等状态 | [dfm_aimbot.cpp:542](/root/workspace/NBG-Draw-dfm/jni/src/Android_draw/dfm_aimbot.cpp:542) |
| 输入 | `setTouchSlots()` | `OP_SET_TOUCH_SLOTS` | 配置多触点槽位 | [dfm_aimbot.cpp:544](/root/workspace/NBG-Draw-dfm/jni/src/Android_draw/dfm_aimbot.cpp:544) |
| 输入 | `touchDown()` | `OP_TOUCH_DOWN` | 单触点按下 | [dfm_aimbot.cpp:1159](/root/workspace/NBG-Draw-dfm/jni/src/Android_draw/dfm_aimbot.cpp:1159) |
| 输入 | `touchMove()` | `OP_TOUCH_MOVE` | 单触点移动 | [dfm_aimbot.cpp:1175](/root/workspace/NBG-Draw-dfm/jni/src/Android_draw/dfm_aimbot.cpp:1175) |
| 输入 | `touchUp()` | `OP_TOUCH_UP` | 单触点释放 | [dfm_aimbot.cpp:479](/root/workspace/NBG-Draw-dfm/jni/src/Android_draw/dfm_aimbot.cpp:479) |
| 输入 | `touchSlotDown()` | `OP_TOUCH_SLOT_DOWN` | 指定槽位按下 | [dfm_aimbot.cpp:602](/root/workspace/NBG-Draw-dfm/jni/src/Android_draw/dfm_aimbot.cpp:602) |
| 输入 | `touchSlotUp()` | `OP_TOUCH_SLOT_UP` | 指定槽位释放 | [dfm_aimbot.cpp:603](/root/workspace/NBG-Draw-dfm/jni/src/Android_draw/dfm_aimbot.cpp:603) |
| 输入 | `configureGyro()` | `OP_GYRO_CONFIG` | 配置/关闭陀螺仪输入；受功能开关和运行条件控制 | [MainUI.h:499](/root/workspace/NBG-Draw-dfm/jni/src/ui/MainUI.h:499)、[dfm_aimbot.cpp:444](/root/workspace/NBG-Draw-dfm/jni/src/Android_draw/dfm_aimbot.cpp:444)、[main.cpp:205](/root/workspace/NBG-Draw-dfm/jni/main.cpp:205) |
| 终端过滤 | `configureDevptsFilter()` | `OP_DEVPTS_FILTER(ENABLE)` | 启动和重连时为目标配置终端节点过滤 | [game_runtime.cpp:539](/root/workspace/NBG-Draw-dfm/jni/src/Android_draw/game_runtime.cpp:539) |
| 终端过滤 | `queryDevptsFilter()` | `OP_DEVPTS_FILTER(QUERY)` | 定时查询过滤状态与命中计数 | [game_runtime.cpp:564](/root/workspace/NBG-Draw-dfm/jni/src/Android_draw/game_runtime.cpp:564) |
| 历史清理 | `unhideFile()` | `OP_UNHIDE_FILE` | 启动时清除旧版可执行文件隐藏规则 | [game_runtime.cpp:793](/root/workspace/NBG-Draw-dfm/jni/src/Android_draw/game_runtime.cpp:793) |
| 未启用分支 | `unhideProcess()` | `OP_UNHIDE_PID` | 退出条件分支中保留；当前 selfProcessHidden 始终为 false，hideProcess 调用已注释，不算当前正常执行依赖 | [main.cpp:207](/root/workspace/NBG-Draw-dfm/jni/main.cpp:207) |

**当前未发现业务调用的常见接口**

- write()/write<T>()、flushInstructionCache()：仅有封装实现，业务侧未发现调用。
- 按 comm 查 PID、枚举同名进程、扩展 SO 基址/过滤大 VMA/完整 SO 路径、线程枚举/线程栈、独立 GET_TLS、QUERY_ACCESS、QUERY_SIGNAL、EXECUTE_PACGA：当前业务侧未发现对应调用。
- targetThreadIoctlSession()：仅有封装实现，未发现业务调用。
- touchSlotMove()、hideFile()、hideProcess()：当前未发现有效业务调用；main.cpp 的 hideProcess 行已注释。
- 内核 nbg_target_bound_pid() 不会直接由该应用调用；本项目使用 DEVPTS_FILTER 命令，绑定处理属于旧内核内部逻辑。

**与前面九项查询需求的对应**

业务直接使用 OP_FIND_PID、OP_MODULE_BASE、NBG_RO_MAPS。除此之外，进程启动时间和线程环境等是当前解密/身份验证的实际依赖，不能只迁移 PID 和 SO 查询就认为接入完整。

GetName/getUTF8 是用户态数据解析，不需要新增独立内核命令。连接封装内部的 initialize/find_socket_family/authenticate_fd/memory_channel/ensure_touch_fd_locked 属于上述公开方法的实现依赖，不另外计为业务功能。
