# 本地命名管道自动化桥接插件 (`memestudio.antigravity-harness`)

独立的外部自动化 IPC 桥接插件（C ABI v2），在插件 DLL 内部运行受当前 Windows 账户 SDDL ACL 保护的事件驱动命名管道服务端，无需在 MemeStudio 主程序中保留任何桥接代码。

## 安装与启用

1. 将 `plugins/antigravity-harness/` 放入 `MemeStudio.exe` 同级的 `plugins/` 目录并在 **插件管理** 中启用。
2. 点击顶部扩展栏 **自动化权限…** 勾选允许的操作范围与每日配额上限。
3. 配套的 Node.js / Cordis 客户端扩展位于 `resources/cordis-client/`。

## 核心特性

- **同账户内核级 ACL 隔离**：通过当前进程用户 SID 生成 `D:P(A;;GRGW;;;<SID>)` 安全描述符，拒绝跨账户与网络访问。
- **零轮询事件驱动管道**：全链路基于 `WaitForMultipleObjects` 重叠 I/O 事件通知，卸载插件时毫秒级唤醒退出，无尾部阻塞延迟。
- **自愈型 UTC 日配额账本**：跨自然日或系统时钟校准时自动平滑重置当日计数，且仅在实际执行任务后扣减额度。

## 构建与测试

```powershell
cmake --preset windows-x64
cmake --build --preset x64-release
ctest --preset x64-release --output-on-failure
```
