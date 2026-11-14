# NBG-Draw GPU/KGSL 接入与实机验证

## 接入位置

- `NBG-Draw/jni/include/driver/nbdriver.h` 和 `unicorn-dfm-master/jni/include/driver/nbdriver.h` 已同步到 `nbdriver/sdk/include/nbdriver.h`。
- `unicorn-dfm-master/jni/src/nbdriver_coord_driver_bridge.cpp`：只负责绑定游戏 PID、校验 start time、查询模块和读取；不触发 GPU 隐藏，也不把游戏 PID 写入隐藏表。
- `NBG-Draw/jni/src/driver/kernel_rw.cpp`：`NbdriverMemory` 只做目标进程读取。`NbgDriverClient::ProtectCurrentGpuNodes()` 使用当前 NBG-Draw 自身 PID 和 start time 调用 `NBDRIVER_IOC_GPU_VISIBILITY(HIDE)`，析构/断开时恢复；KGSL visibility 不可用时不阻断普通读进程功能。
- `NBG-Draw/jni/main.cpp`：连接驱动后显式保护当前进程节点，目标游戏 PID 只进入只读请求。
- 已有的 `CoordDriverBridge::QueryGpuObjectInfo()` 继续使用新的 GPUOBJ_INFO 接口；新增 syncsource 接口由 SDK 可直接调用。

## 编译与部署

- `nbdriver` Android 15 / 6.6.127 模块：已在设备加载，`srcversion=8B3A6D7C6EE4A7367DC7399`。
- `NBG-Draw` Android ARM64 编译通过。
- 部署文件：`/data/local/tmp/NBG.sh`（另保留 `/data/local/tmp/NBG-kgsl-merge-20261114`）。
- 部署文件 SHA256：`246a1e2c8445b3767bc3ddb74da450629526086c6533facfd1b73a69c4077663`。
- 部署前旧文件已保存到 `out/app-integration-20261114/device-before/NBG.sh`，SHA256：`5516952dced80c1eb911e3124c4f88fbd4dd86ce06d9e428c05fe1e72ebb2dcb`。

## 实机结果

设备：`emulator-5554` / Android 15 / 6.6。

- ABI：5，feature：`0xfffff`。
- 目标 DFM PID：24896，start time：130909171437300；受控验证的 NBG-Draw PID：19394，start time：131945433978311。
- 应用日志：`self_pid=19394 hidden=1 errno=0`，随后目标 DFM 正常完成 maps、TLS/APGA、批量读取和解密循环。
- visibility 查询：DFM `hidden=0 count=1`（没有隐藏游戏；count 是当前隐藏表总数），NBG-Draw `hidden=1 count=1`。
- NBG-Draw 退出后全局查询 `pid=0` 返回 `hidden=0 count=0`，隐藏项已清理。
- 当前设备上重新启动的 NBG-Draw PID 8110 查询为 `hidden=1`，同一时刻 DFM PID 24896 仍为 `hidden=0`；对应记录在 `kgsl-visibility-current.log`。
- nbdriver 模块仍为 `srcversion=8B3A6D7C6EE4A7367DC739`，ABI 5 / feature `0xfffff`；未观察到驱动加载错误、目标读取错误或因本次改动导致的崩溃。

完整命令与输出在 `out/app-integration-20261114/`，包括 `kgsl-query-device.log`、`kgsl-visibility-device.log`、`kgsl-visibility-after-exit.log`、桥接测试和 SDK/应用构建记录。
