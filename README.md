# MemeStudioPlugins — MemeStudio 插件开发模板与官方扩展插件集

本仓库承载 **MemeStudio**（C ABI v2）的标准插件开发脚手架（`main` 主分支）以及从 `main` 同源派生的全部独立扩展插件分支。

> **职责边界说明**：核心创作工作流（**表情角色替换**、**提示词生图**、**局部精修与提示词修图**）已原生内置于 `MemeStudio` 主程序与 `MemeStudioEngine.dll` 中，开箱即用；本仓库专注于提供可按需选装的批处理矩阵、外部超分滤镜、多渠道网关、本地推理引擎与自动化桥接插件。

## 分支导航

| 分支名称 | 插件 ID | 功能定位 |
| :--- | :--- | :--- |
| **`main`** *(开发模板基线)* | `memestudio.example` | **C ABI v2 插件标准模板与共享 SDK / CMake / CI 基线**（演示实例隔离状态、提示词增强钩子与设置面板） |
| `plugin-batch-convert` | `memestudio.batch-convert` | 多张源表情 × 多张角色参考图组合批量转换工作台（支持 $X\times 1$、$1\times Y$ 单图多角色及 $X\times Y$ 矩阵生成） |
| `plugin-waifu2x` | `memestudio.waifu2x` | 调用本地 `waifu2x-ncnn-vulkan` 对输入素材或生成成品执行单进程整批超分放大与降噪 |
| `plugin-api-parameter-test` | `memestudio.api-parameter-test` | 自动检测目标接口对上传字段、画质、尺寸、格式及流式返回的支持情况并导出脱敏报告 |
| `plugin-api-load-balancing` | `memestudio.api-load-balancing` | 多接口渠道权重/成本调度、异常自动故障转移与同提示词多渠道横向出图对比 |
| `plugin-qwen-image` | `memestudio.qwen-image` | 原生托管 `stable-diffusion.cpp` (`sd-server.exe`) 运行本地 GGUF 模型，非破坏性临时接管会话连接 |
| `plugin-deepseek-harness` | `memestudio.deepseek-harness` | 同账户 SDDL 权限保护的本地命名管道服务，支持操作白名单、日配额管控与客户端扩展包 |

## 编译与单测（适用于 `main` 及任意 `plugin-*` 分支）

```powershell
cmake --preset windows-x64
cmake --build --preset x64-release
ctest --preset x64-release --output-on-failure
```

编译产出的纯净插件文件夹位于 `out/build/x64/dist/Release/plugins/<plugin-name>/`，将其复制到 `MemeStudio.exe` 同级的 `plugins/` 目录即可在 **插件管理…** 中勾选启用。

## 派生新插件与同步 SDK 更新

- **创建新插件分支**：`git checkout -b plugin-my-feature main`，修改 `plugin.json` 与 `src/` 即可直接复用整套构建、测试底座与双架构 CI。
- **同步公共 SDK 升级**：在目标插件分支执行 `git merge main`。
