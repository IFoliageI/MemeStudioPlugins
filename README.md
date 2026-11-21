# MemeConvertStudio — MemeStudio 独立插件开发脚手架与官方插件集

本仓库承载 **MemeStudio**（C ABI v2）的插件开发模板（`main` 主分支）以及从 `main` 同源派生的全部独立功能插件分支。每个插件均编译为真正独立自治的动态链接库（`.dll`），不包含任何空壳转发逻辑。

## 分支导航表

| 分支名称 | 插件 ID | 独立插件职责概述 |
| :--- | :--- | :--- |
| **`main`** *(当前默认基线)* | `memestudio.example` | **插件开发标准模板与共享 SDK v2 / CMake / CI 基线**（演示实例状态隔离、提示词增强钩子与设置命令） |
| `plugin-local-redraw` | `memestudio.local-redraw` | 独立选区蒙版画布、局部重绘请求与外围像素无损合成 |
| `plugin-waifu2x` | `memestudio.waifu2x` | 整目录单次批处理 Waifu2x 输入/输出超分滤镜与参数配置面板 |
| `plugin-batch-convert` | `memestudio.batch-convert` | 独立多源批量转换队列工作台（文件头极速扫描、防覆盖校验与单条容错续传） |
| `plugin-prompt-draw` | `memestudio.prompt-draw` | 独立文生图与画风参考绘制工作流面板 |
| `plugin-api-parameter-test` | `memestudio.api-parameter-test` | 自适应基线图像接口参数兼容性探测引擎与报告导出面板 |
| `plugin-api-load-balancing` | `memestudio.api-load-balancing` | 透明 API 传输拦截路由器（平滑加权/花销路由/自动故障转移）与多渠道对比面板 |
| `plugin-qwen-image` | `memestudio.qwen-image` | 原生 C++ 直管 `sd-server.exe` 本地推理引擎（AppData 读写隔离、非破坏性会话端点挂载） |
| `plugin-deepseek-harness` | `memestudio.deepseek-harness` | 事件驱动本机命名管道自动化服务、配额权限面板与配套客户端包 |

## 构建与测试（适用于 `main` 及任意 `plugin-*` 分支）

```powershell
cmake --preset windows-x64
cmake --build --preset x64-release
ctest --preset x64-release --output-on-failure
```

编译生成的纯净插件目录位于 `out/build/x64/dist/Release/plugins/<plugin-name>/`，将其复制到 `MemeStudio.exe` 同级的 `plugins/` 目录即可启用。

## 派生新插件与同步 SDK 更新

所有插件分支均以 `main` 为公共祖先：
- **新建插件分支**：`git checkout -b plugin-my-feature main`，修改 `plugin.json` 与 `src/` 即可直接复用整套 CMake、测试底座与双架构 CI。
- **同步 SDK / 构建脚手架升级**：在目标插件分支执行 `git merge main`。
