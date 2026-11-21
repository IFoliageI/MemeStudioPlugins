# 多渠道均衡与故障转移插件 (`memestudio.api-load-balancing`)

独立的 API 渠道池路由与横向对比插件（C ABI v2），通过实现 `intercept_image_api` 传输拦截器钩子透明接管宿主图像与模型列表请求。

## 安装与启用

1. 将 `plugins/api-load-balancing/` 放入 `MemeStudio.exe` 同级的 `plugins/` 目录。
2. 在 **插件管理** 中启用 **多渠道均衡与故障转移**，点击顶部扩展栏 **渠道池均衡…** 添加渠道并开启接管。

## 核心特性

- **会话级持久化健康记忆**：渠道的 429/5xx 冷却计时器、401/403 鉴权熔断与平滑加权相位在插件运行期间跨任务保持记忆，不会因新建任务而失忆。
- **全面故障转移覆盖**：完整覆盖网络连接/TLS/超时异常、HTTP `401/403/429`、上游 `500/502/503/504/52x` 及 `200 + error` 包装异常。
- **故障隔离的多渠道对比**：执行多渠道对比时单个渠道超时或报错仅标记该渠道失败，绝不连坐中断其余健康渠道，且支持随时重复运行。

## 构建与测试

```powershell
cmake --preset windows-x64
cmake --build --preset x64-release
ctest --preset x64-release --output-on-failure
```
