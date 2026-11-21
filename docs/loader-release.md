# 多内核构建与受信任发布

内核兼容性按已验证的目标构建集合提供，不能通过更换 vermagic 或写入同名函数地址推定。
NBG 下载路径现在按完整 Android KMI（例如 `android15-6.6`）、ARM64 和运行页面大小精确选择，
没有匹配项就停止，不回退到其他内核分支。签名、符号版本和内核 ABI 限制仍由目标构建及内核检查。


## 自动更新（支持签名的新客户端）

新客户端内置 Ed25519 公钥，启动时验证服务器清单原始字节及发布序号，不需要每次手动更新清单摘要。私钥仅存放在发布服务器的 root 私有目录。旧的显式固定摘要配置仍可用于受管理的离线/固定版本流程。

发布时在原有验证流程上增加 `--signing-key /path/to/private-ed25519.pem`，即可在生成 `nbdriver-manifest.json` 后生成 `nbdriver-manifest.signature.json`。已核对过的现有发布也可执行：

```sh
python3 tools/sign_manifest.py --directory /path/to/release --key /path/to/private-ed25519.pem
```

签名覆盖域标记、十进制发布序号和完整清单字节。服务通过响应头提供签名和序号；客户端验证内置公钥、完整 KMI/架构/页面大小、模块长度及 SHA256，并在 root 私有目录保留已接受序号以拒绝回退。

启动检查仅在进入采集和绘制前执行。同版本只检查小清单，不重复下载/刷入模块；版本记录绑定内核 boot_id 和驱动随机会话 token 的摘要，不能用 ABI 号或重载后会归零的 handle 当作版本。新模块先下载、验证并进行元数据预检，再 QUIESCE/关闭连接/正常卸载/加载/握手。占用导致无法卸载时通过保留的 reboot 引导恢复旧模块；没有恢复入口则延期更新。新模块加载失败时尝试恢复上次成功加载且校验过的私有缓存；首次接管的未知旧模块若没有缓存，不能声称具备该恢复能力。

网络或验证失败时保留当前可用驱动并记录更新未完成。此功能不会在对局或 UI 帧中刷入。当前私有版本记录路径来自 `paths.h` 的 `NBG_DATA_ROOT`（`/data/adb/nbg`）。

## 构建并发布

每个目标需要完成准备的内核构建目录及其 Module.symvers。`make all` 现在先运行导入策略与
实际回调机器码检查，通过后才复制到 `out/nbdriver.ko`。也可独立检查：

```sh
make verify-module KDIR=/path/to/target/build MODULE=/path/to/nbdriver.ko
```

本地旧 `.ko` 不是新的发布凭据；对禁止静态导入项检查失败时，必须从当前源码按目标重新构建。
不要将禁止项改成绝对地址来掩盖发布检查失败。

将验证过的模块放入发布目录，准备 profiles.json。例如：

```json
[
  {
    "name": "nbdriver-android15-6.6.ko",
    "kmi": "android15-6.6",
    "arch": "aarch64",
    "page_size": 4096,
    "symvers": "/path/to/android15-6.6/build/Module.symvers"
  }
]
```

配置必须来自该产物的构建记录；工具不会根据文件名推测页面配置。

```sh
python3 tools/publish_manifest.py --directory /path/to/release \
  --profiles /path/to/profiles.json --output /path/to/release/nbdriver-manifest.json
```

工具再次执行导入和回调检查，独占创建清单，并打印
`NBG_NBDRIVER_MANIFEST_SHA256=<64位摘要>`。清单包含各文件实际 SHA256、长度及目标配置。
发布清单不能原地覆盖；更新发布时生成新文件，完成校验后再切换服务配置及客户端可信摘要。

后端 `DRIVER_DIR` 指向发布目录；`NBDRIVER_MANIFEST_PATH` 默认为目录下的
`nbdriver-manifest.json`。服务保留清单原始字节，并拒绝文件缺失、被修改、配置不合法或旧版动态清单。
本次源码修改不自动部署后端服务。

## 受管理的固定摘要配置

清单摘要必须通过可信的应用发布或设备管理途径分发，不能再从下载清单的同一个 HTTP 响应取得。
普通进程可设置 `NBG_NBDRIVER_MANIFEST_SHA256`。SF 注入模式不会继承启动终端的全部环境，
可在设备建立 root 所有、不可被组或其他用户写入的 `/data/adb/nbg` 目录，并将可信摘要
单独写入 `manifest.sha256` 文件。文件也必须归 root 所有、不可被组或其他用户写入，建议目录 0700、文件 0600。
客户端拒绝目录或文件符号链接。环境中的显式设置优先。

客户端先校验整个清单的固定摘要，再解析目标和模块摘要，随后校验下载的模块字节。
HTTP 响应的摘要头只做一致性检查。没有可信摘要就停止下载。
传输仍是 HTTP：固定摘要解决产物真实性与完整性，不能提供下载令牌或报告内容的传输保密性；
需要保密时仍应给服务配置 HTTPS。

标准 release 中的 `-androidNN-` 用于识别 KMI。缺少该标记的定制内核必须明确提供经过验证的
`NBG_NBDRIVER_KMI=android15-6.6`，或在同一私有目录写入 `kmi` 文件。
配置不能跨越运行内核的主次版本，也不能覆盖 release 已识别出的其他 Android 代际。

## 符号与离线模块

默认保留未定义导入，由内核正常解析；默认流程不为符号改写调整 kptr_restrict。
只有调用方显式设置 `NBG_NBDRIVER_PATCH_SYMBOLS=name1,name2` 才进行指定符号的地址改写。
调用方负责逐个验证该目标的 ABI；缺失、歧义或其他模块所属的地址不构成有效匹配。
允许名单中的符号未被成功改写时停止加载，不静默回退。

`NBG_NBDRIVER_MODULE` 与 `NBG_NBDRIVER_SHA256` 仍提供显式离线模式；调用方须选择已验证的构建。
离线模式检查 ELF 架构和可识别的内核分支，但不以显式文件路径作为页面配置或完整 ABI 兼容性的证明。

原生 `nbdriver_patch_vermagic` 接收完整 `uname -r`，替换 release token 并保留空格后的特性/架构字段。
release 内的连字符属于 release 本身。新 `.modinfo` 追加到文件，不移动原 section。
输出使用独占创建，重用路径报 `output_exists errno=17`。改写的绝对地址产物只限其取址的内核启动实例，
外部保存或复用时必须验证来源和 boot_id；默认加载流程每次生成私有副本并清理。

## 测试

```sh
python3 tests/nbdriver_patch_test.py
python3 ../NBG-Draw/tests/test_driver_load_transaction.py
python3 ../NBG-Draw/tests/test_driver_loader_errors.py
python3 ../NBG-Draw/tests/test_driver_manifest.py
python3 ../NBG-Draw/tests/test_driver_publication.py
```

发布端路由测试需要 Flask。宿主模拟测试不执行真实内核加载，也不代替对应设备的功能验证。
