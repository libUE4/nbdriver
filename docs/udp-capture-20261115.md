# UDP 下行抓包移植

移植来源是工作区 `enkernel/udp_capture.c`；原文件及 SHA256 保留在 `out/udp-capture-20261115/reference/` 和 `baseline.json`。实现为 `src/nbdriver_udp.c`，通过现有匿名 FD 接入，没有改变应用的坐标解密算法。

## 接口

保持 ABI 5 和全部旧命令、结构大小；新增 `NBDRIVER_FEATURE_UDP_CAPTURE`（bit 23）及 0x25—0x28 四个命令。内核模块初始化时试注册并立即撤销接收探针，成功才发布能力位；正式抓包只在 START 后注册，STOP/最终关闭后撤销。START 仍可能返回具体注册错误，能力位不能替代调用结果。

- `nbdriver_udp_capture_start(fd, pid, min, max, expected_start_ns, &session)`：绑定进程身份、UID 和原始 mm；返回新 generation。min/max 为 0 时采用 16/2048。长度不足 min 的包不采集，超过 max 的包截断并标记。
- `nbdriver_udp_capture_read(fd, packets, capacity, generation, &status)`：返回 0—32 条记录，空队列返回 0，失败返回 -1/errno。generation 错误返回 ESTALE。
- `nbdriver_udp_capture_status(fd, &status)`：查询会话、队列和计数。
- `nbdriver_udp_capture_stop(fd)`：停止并清空队列；重复调用可成功。
- 标准 `poll(fd, POLLIN, timeout)`：等待新包。应使用有限超时，例如 1000ms，以便定期检查目标退出或执行映像变化。

单头 SDK 和旧 client.c SDK 都提供这四个函数。packet/start/status/read 结构分别为 2096/32/96/120 字节；SDK C/C++、共享 ABI 的大小、偏移和命令编码有测试。

## 采集语义

与原版一致，在 `udp_queue_rcv_one_skb` 入口观察下行 IPv4 UDP；返回 0 后继续原内核路径，不改包、不丢弃目标 socket 的原始数据、不注入目标进程。这里只提供原始载荷，不进行密钥或坐标解密。

筛选条件是目标进程的真实 UID，与接收 socket 的 sk_uid 匹配，不是严格的单 PID socket 归属；共享 UID 的其他进程流量也可能出现。IP 地址为网络字节序，端口和其他整数为主机字节序。timestamp_ns 是抓包入口的内核单调时间，不代表网卡到达时间。

每条记录提供 sequence、源/目标地址和端口、原始长度、保存长度及 socket UID。可见的 GRO/GSO 聚合带 `NBDRIVER_UDP_F_GRO` 和 gso_size；**聚合记录不冒充独立 UDP 包**。超过 2048 字节仍保留截断标志及原始长度，不能把截断记录当完整密文交给解密器。IPv6、上行和完整内容超过固定上限的记录，不在本实现完整采集范围。

## 生命周期与性能

同一时间只允许一个抓包所有者，第二个 FD 启动返回 EBUSY，读取或停止其他所有者返回 EPERM。dup() 共享同一个 file；最终关闭该 file 才停止抓包，关闭无关 nbdriver FD 没有影响。PID 启动时间或 mm 不再匹配时不继续发布或读取旧会话记录。

采用每 CPU scratch、固定内存池和指针交换；回调不分配内存，不在自旋锁中复制 2KiB 载荷或访问用户内存。256 条环形队列满时覆盖最旧排队记录；最多另有 32 条已分离、等待用户读取成功的记录。STATUS 的 queued 包含两者。

数据或回执 usercopy 失败时保留已分离数据供重试；失败调用可能已部分复制到用户缓冲区，消费者可按 generation+sequence 去重。正常成功读取不会主动重放旧包。

captured 统计成功入队记录，overflow 统计覆盖旧记录，truncated 统计截断，malformed 统计格式/载荷复制错误，probe_missed 报告 kprobe 漏采。它们不能证明网络链路本身没有丢包，也不能证明目标 socket 后续一定接收了该包。没有逐包 printk；只输出初始化、启动、停止及注册错误日志。

沿用 root/system/CAP_SYS_PTRACE 调用门禁，并复用目标访问检查。运行时 QUIESCE 会停止抓包并清理队列；RESUME 不自动恢复旧会话，需要重新 START。

## 验证与产物

`python3 tests/test_udp_capture.py` 执行生产后端的主机适配测试；`python3 tests/test_udp_sdk.py` 验证两套 SDK。主机适配不能替代真实内核网络栈/探针测试。

服务器已有 android15-6.6 DDK 完成构建，导入符号检查无未解析项，FD 回调尾调用检查通过。产物：`out/udp-capture-20261115/nbdriver-android15-6.6-udp.ko`。只验证这一 DDK 的编译，没有声称其他 KMI 已验证。

ARM64 实机测试程序：`out/udp-capture-20261115/device-udp-smoke`，对应 `tests/device_udp_smoke.c`。它只向自身 loopback UDP socket 发送合成数据，检查内容、截断、poll、无关 FD 关闭、dup、重启和最终关闭，不操作游戏。当前设备旧驱动 features=0x7fffff，缺少 UDP 能力位，实际返回 SKIP/77；新驱动刷入后的采集测试待验证。

原始文件、构建日志、源差异与恢复验证保存在同一 out 目录。不要把“编译通过”当作“已在当前内核接包成功”。

补充回归记录：单头 SDK、旧 SDK、进程查询授权、信号查询和映射检查通过。原有 READV 主机适配测试缺少 `nbdriver_sf_read_caller()` 定义；用修改前的源函数也复现相同编译失败，保留了基线日志，没有把它记作通过。设备只读核对确认存在 `udp_queue_rcv_one_skb` 符号，但符号存在不能替代新模块注册和收包实测。
