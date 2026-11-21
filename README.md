# Waifu2x 超分放大插件 (`memestudio.waifu2x`)

独立的外部 `waifu2x-ncnn-vulkan` 超分滤镜插件（C ABI v2），通过挂载 `process_frames_batch` 批量帧钩子在输入预处理或成品输出阶段自动执行超分降噪。

## 安装与启用

1. 准备本地 [waifu2x-ncnn-vulkan](https://github.com/nihui/waifu2x-ncnn-vulkan) 可执行文件与模型目录（包含 `.param` 与 `.bin` 权重文件）。
2. 将 `plugins/waifu2x/` 放入 `MemeStudio.exe` 同级的 `plugins/` 目录并在 **插件管理** 中启用。
3. 点击顶部 **Waifu2x 超分设置…** 配置程序路径、倍率与降噪等级。

## 核心特性

- **整目录单次 Vulkan 批处理**：多帧 GIF 动画一次性导出至隔离批次目录并单次调用 `waifu2x-ncnn-vulkan -i <in_dir> -o <out_dir>`，彻底避免逐帧重复初始化 GPU。
- **Job Object 进程托管与日志诊断**：子进程通过 Windows Job Object 强绑定生命周期；执行失败时自动提取尾部标准错误日志返回明确原因。

## 构建与测试

```powershell
cmake --preset windows-x64
cmake --build --preset x64-release
ctest --preset x64-release --output-on-failure
```
