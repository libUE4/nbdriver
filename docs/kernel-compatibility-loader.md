# nbdriver 兼容刷入链路

本实现把兼容刷入分为两部分：

1. `src/nbdriver_runtime.c`：内核模块加载完成后生成一次性运行句柄、token、generation，并通过新增 ABI ioctl 报告 ready/resident 状态。
2. `tools/nbdriver_loader.c`：用户层原生加载器，执行 root、内核版本前缀、`/proc/kallsyms` 必需符号、SHA256、`finit_module` 和 ABI/runtime 握手。

## 调用流程

```text
用户层识别内核
  -> 选择匹配的 ko（由上层 manifest/设备配置决定）
  -> 计算 SHA256 并与 manifest 比较
  -> 检查 /proc/kallsyms 的必需符号
  -> finit_module()
  -> nbdriver_open()
  -> GET_ABI + RUNTIME_STATUS
  -> ready/resident/handle/token
```

失败时加载器不伪造成功状态；模块加载失败或握手失败均输出结构化日志并返回非零状态。

## ABI 扩展

- `NBDRIVER_FEATURE_RUNTIME_STATUS`
- `NBDRIVER_IOC_RUNTIME_STATUS`
- `struct nbdriver_runtime_status`
- SDK：`nbdriver_get_runtime_status()`

旧 ioctl 编号和请求结构保持不变。token 仅用于本次模块生命周期内的状态确认，不能替代模块签名或 SHA256 校验。

## 日志格式

加载器输出单行键值日志，例如：

```text
nbdriver-loader event=preflight.pass release=... module=... sha256=...
nbdriver-loader event=load.pass module=nbdriver
nbdriver-loader event=ready abi=5 features=0x... handle=... generation=... flags=0x...
```

不记录内核地址、kallsyms 地址或 token 明文。

## 构建和验证

```sh
make -C nbdriver loader
make -C nbdriver KDIR=/path/to/matching/kernel-build all
```

`nbdriver.ko` 仍必须使用目标内核的正确 `Module.symvers`、vermagic、CFI/KCFI 配置和签名策略构建；加载器不会绕过内核的模块签名校验。

当前实现已覆盖：版本/符号/完整性预检、原生模块刷入、加载后 ABI 握手和 resident 状态。真正的无中断 hot-switch 还需要旧实例与新实例之间的 quiesce/hand-off 协议，不能用简单的 `rmmod && insmod` 代替。
