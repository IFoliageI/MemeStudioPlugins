# Qwen-Image 本地推理插件 (`memestudio.qwen-image`)

本插件以原生 C++20 直接托管 `stable-diffusion.cpp` 的 `sd-server.exe` 进程，为 `MemeStudio` 提供本地多参考图角色替换与文生图能力，彻底移除了旧版内嵌 Python 环境与中间转发进程。

## 使用说明

1. 将 `plugins/qwen-image/` 放入 `MemeStudio.exe` 同级的 `plugins/` 目录并在 **插件管理** 中启用。
2. 点击顶部扩展栏 **Qwen-Image…** 选择本地 `sd-server.exe` 与 GGUF 组件路径（扩散模型、VAE、文本编码器及可选 `mmproj`）。
3. 点击 **启动引擎并接管**：插件将以**非破坏性会话端点**方式临时接管当前主界面连接（绝不覆盖用户保存的商业 API 地址与密钥）；关闭面板或点击 **停止并释放显存** 即可一键终止 `sd-server.exe` 子进程。

## 架构与数据隔离保证

- **零安装目录写入**：所有配置文件、运行日志与运行时状态统一存放在 `%LOCALAPPDATA%\MemeStudio\plugins\memestudio.qwen-image\`，支持安装在只读 `C:\Program Files\` 目录。
- **容错模型扫描**：目录内若存在下载未完成或损坏的 `.gguf` 文件，扫描器仅跳过该损坏文件并记录于 `skipped_corrupt_files`，不会中断同目录有效模型的识别。
- **组件区分提示**：单独发布的 GGUF 文本编码器与 `mmproj` 需搭配对应的扩散模型权重与专用 VAE 使用，具体组件规范参见 [stable-diffusion.cpp Qwen-Image 部署文档](https://github.com/leejet/stable-diffusion.cpp/blob/master/docs/models/qwen_image_2.1.md)。
