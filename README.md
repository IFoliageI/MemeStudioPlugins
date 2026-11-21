# 多源批量转换插件 (`memestudio.batch-convert`)

独立的多文件与文件夹批量表情转换队列插件（C ABI v2），提供自治的队列管理面板与无头批量执行命令。

## 安装与启用

1. 将 `plugins/batch-convert/` 放入 `MemeStudio.exe` 同级的 `plugins/` 目录。
2. 在 **插件管理** 中启用 **多源批量转换**，点击顶部扩展栏 **批量队列…** 打开独立队列工作台。

## 核心特性

- **零像素解码极速扫描**：导入数百个 PNG/GIF 文件时仅读取容器头元数据并自动去重跳过损坏图像，不占用动画展开内存。
- **预哈希防冲突命名**：按 `001_<原名>-edited.png/.gif` 生成输出序列，通过预计算规范路径哈希集合确保绝不覆盖任何输入原图或参考图。
- **单条容错隔离**：支持遇到单个素材生成失败时记录错误详情并继续处理后续队列条目。

## 构建与测试

```powershell
cmake --preset windows-x64
cmake --build --preset x64-release
ctest --preset x64-release --output-on-failure
```
