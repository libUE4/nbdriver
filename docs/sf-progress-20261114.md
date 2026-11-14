# SF 驱动适配进度（2026-11-14）

本次保存当前 syscall 18 通讯入口和 SF 线程权限组适配，不包含新的绘制接入。

- ABI 仍为 v5；bootstrap 使用 syscall 18 与 DFMP/PMFD 标记，探针将认证和 FD 安装工作交给 task_work。
- 增加 SF_GROUPS feature 和固定 input/inet groups 的请求接口；用户层在触摸初始化前显式调用。
- groups_alloc、groups_sort、groups_search、set_current_groups 等使用本机解析的函数；释放遵守引用计数，避免此前 groups 释放错误。
- 主项目的 nbdriver 头文件、驱动客户端和解密桥与此通讯入口对应。

已使用的模块 SHA256：
`6d5f46bbc29c0a057297fb7d73cdc030c57efb35db03b76ab5e50965e6e9636b`

主项目现场记录：`vmp/xiaoxue/cp/sf_shared_runtime_20261114/`。
已记录 syscall18/ABI v5 连接、SF groups 获取及 event6 打开。此次提交只保存源码，不重新编译或加载模块。

尚未验证：主项目最新开启隐藏的共用运行库在 SF 中的实际隐藏结果与稳定性。
复用 SF RenderEngine 而不新增独立图层的方案尚未实现。
