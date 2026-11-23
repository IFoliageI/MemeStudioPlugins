# DeepSeek Harness 本地自动化桥接插件 (`memestudio.deepseek-harness`)

本插件作为完全独立于宿主核心的插件，提供受当前 Windows 登录用户 SID SDDL ACL 保护的重叠 I/O 命名管道（Named Pipe）服务端与自愈日配额账本，供当前账户下的 DeepSeek Harness / Cordis 客户端调用。

## 使用说明

1. 将 `plugins/deepseek-harness/` 放入 `MemeStudio.exe` 同级的 `plugins/` 目录并在 **插件管理** 中启用。
2. 点击顶部扩展栏 **Harness 权限…** 按最小权限原则勾选允许的操作（默认仅允许 `inspect`，生图 `replace` 需显式授权并要求客户端请求携带 `"confirmed": true`）。
3. 配套的 Node.js / Cordis 客户端扩展位于 `resources/cordis-client/`。

## 安全与可靠性保证

- **同账户 ACL 隔离**：通过 `OpenProcessToken` + `ConvertSidToStringSidW` 动态生成 `D:P(A;;GRGW;;;<CurrentUserSID>)`，拒绝跨账户与远程 SMB 访问（`PIPE_REJECT_REMOTE_CLIENTS`）。
- **时钟回拨自愈账本**：当系统时钟修正导致历史账本 UTC 日期不等于今日 UTC 日期时自动归零重置，绝不锁死日配额。
- **失败不扣额度 & 透明错误返回**：仅当 `run_core_replace_job` 实际成功产出结果后才计入配额；超额或底层失败均以 `{ "ok": false, "error": "..." }` 透明返回客户端。
