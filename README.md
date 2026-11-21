# API 参数兼容性探测插件 (`memestudio.api-parameter-test`)

独立的图像接口参数兼容性探测插件（C ABI v2），通过最小合成图像向目标接口验证 `quality`、`size`、`output_format`、`output_compression`、`stream` 及上传字段名 `image[]` / `image` 的实际兼容表现。

## 安装与启用

1. 将 `plugins/api-parameter-test/` 放入 `MemeStudio.exe` 同级的 `plugins/` 目录。
2. 在 **插件管理** 中启用 **API 参数兼容性探测**，点击顶部扩展栏 **接口探测…** 打开探测面板。

## 核心特性

- **自适应基线探测**：基线请求优先遵循当前配置的上传字段；若默认上传字段返回 400 拒绝，自动尝试备选上传字段（`image` ↔ `image[]`）并在成功后用于后续参数探测。
- **响应格式与流式真伪核验**：通过解析返回图像文件头魔数与 `text/event-stream` 事件流，区分“真实生效 (`accepted`)”、“参数被忽略 (`ignored`)”与“明确拒绝 (`rejected`)”。
- **结构化字段级密钥脱敏**：递归清洗 JSON 字符串值中的密钥信息，确保导出报告安全完整。

## 构建与测试

```powershell
cmake --preset windows-x64
cmake --build --preset x64-release
ctest --preset x64-release --output-on-failure
```
