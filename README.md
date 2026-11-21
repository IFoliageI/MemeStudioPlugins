# Qwen-Image 本地推理插件 (`memestudio.antigravity-image`)

独立的原生 C++ 本地模型推理插件（C ABI v2），直接通过 Windows Job Object 托管 `stable-diffusion.cpp` (`sd-server.exe`) 进程并通过 `intercept_image_api` 透明对接多参考图编辑与文生图请求，无需部署中间 Python HTTP 转发服务。

## 安装与启用

1. 将 `plugins/antigravity-image/` 放入 `MemeStudio.exe` 同级的 `plugins/` 目录并在 **插件管理** 中启用。
2. 点击顶部扩展栏 **Qwen-Image…** 选择本地 `sd-server.exe` 与 GGUF 组件路径（扩散模型、VAE、文本编码器及可选 `mmproj`）。
3. 点击 **启动并接管当前会话**，宿主将以非破坏性会话覆盖模式连接本地引擎，原有保存的云端商业 API 密钥保持完好无损。

## 架构与数据隔离说明

- **零安装目录写入**：所有配置文件、运行日志与运行时组件统一存放在 `%LOCALAPPDATA%\MemeStudio\plugins\memestudio.antigravity-image\`，支持安装在只读 `C:\Program Files\` 目录。
- **内核级进程树回收**：`sd-server.exe` 绑定至 `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` 作业对象，停用插件或宿主退出/异常终止时由操作系统内核立即回收推理进程释放显存。
- **组件区分提示**：单独发布的 GGUF 文本编码器与 `mmproj` 需搭配对应的扩散模型权重与专用 VAE 使用，具体组件规范参见 [stable-diffusion.cpp Qwen 2.1 部署文档](https://github.com/leejet/stable-diffusion.cpp/blob/master/docs/models/antigravity_image_2.1.md)。

## 构建与测试

```powershell
cmake --preset windows-x64
cmake --build --preset x64-release
ctest --preset x64-release --output-on-failure
```
