# DeepSeek Harness 调用桥接插件 (`memestudio.deepseek-harness`)

`plugins/deepseek-harness/` 目录**同时**是：
1. **MemeStudio 原生 C ABI v2 插件**（`deepseek-harness.dll` + `plugin.json`）：在主程序进程内启动受当前登录用户 SID SDDL ACL 保护的命名管道服务端与自愈日配额账本；
2. **DeepSeek Harness (Cordis) 本地工具扩展包**（`package.json` + `cordis.patch.yml` + `index.js` + `locale/` + `icon.svg`）：向 DeepSeek Harness 注册 `memestudio_status`、`memestudio_inspect`、`memestudio_models`、`memestudio_replace`、`memestudio_draw`、`memestudio_retouch` 六项工具。

## 安装与启用步骤

1. **在 MemeStudio 中启用服务端**：
   将 `plugins/deepseek-harness/` 放入 `MemeStudio.exe` 同级的 `plugins/` 目录，打开右上角 **插件管理…** 勾选 **DeepSeek Harness 桥接** 并点击 **保存并应用**（启用后本机命名管道 `\\.\pipe\MemeStudio.DeepSeekHarness.v1` 将自动启动监听）。
2. **在 DeepSeek Harness 中安装本地扩展包**：
   打开 DeepSeek Harness 的插件管理，从本地文件夹安装 **`plugins/deepseek-harness` 整个目录**（即包含 `package.json` 与 `cordis.patch.yml` 的这一层目录，不要只选单个 `.dll` 或外层 `plugins` 目录）。
3. **按需配置权限与额度**：
   默认遵循最小权限原则（仅开放无计费消耗的 `inspect` 图像检查）；如需允许 Agent 调用角色替换、生图或修图，请点击 MemeStudio 顶部 **Harness 权限…** 勾选对应权限并保存（每次生成调用仍要求客户端显式传入 `confirmed: true`）。
