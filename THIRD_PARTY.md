# 第三方开源声明 (Third-Party Notices)

本仓库（`MemeStudioPlugins`）及其插件分支在编译或可选外部工具集成中涉及以下开源组件：

- **nlohmann/json** (`vendor/nlohmann/json.hpp`) — 采用 MIT 许可证，版权及完整许可证文本见 `vendor/nlohmann/LICENSE.MIT`。
- **waifu2x-ncnn-vulkan**（`plugin-waifu2x` 分支外部调用工具，不随仓库分发） — 上游项目 [nihui/waifu2x-ncnn-vulkan](https://github.com/nihui/waifu2x-ncnn-vulkan)，采用 MIT 许可证。
- **stable-diffusion.cpp & ggml**（`plugin-qwen-image` 分支外部推理引擎，不随源码提交） — 上游项目 [leejet/stable-diffusion.cpp](https://github.com/leejet/stable-diffusion.cpp)，采用 MIT 许可证。
- **Qwen-Image**（`plugin-qwen-image` 分支模型与管线规范参考） — 上游仓库 [QwenLM/Qwen-Image](https://github.com/QwenLM/Qwen-Image)，模型权重与代码遵循其上游发布的许可证。
- **Cordis**（`plugin-deepseek-harness` 分支客户端扩展接口） — 遵循上游宿主插件接口约定。
