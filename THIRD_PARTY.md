# 许可证与第三方来源

项目采用 [MIT License](LICENSE)。随附 SDK、唤醒模型、字库和提示音由项目所有者授权提供；第三方版权和 LICENSE/NOTICE 继续有效。

| 资产 | 当前来源入口 | 保留要求 |
| --- | --- | --- |
| S3 TiRTC SDK | [SDK README](lckfb-szpi-esp32s3-tirtc/third_party/tirtc/README.md) | 保留头文件、静态库、构建契约与原始版权 |
| P4 TiRTC SDK | [SDK VERSION](waveshare-esp32p4-xiaotai/components/tirtc_sdk/VERSION.md) | 保留 P4 补丁、strip、版本及哈希依据；不改称未经修改的官方包 |
| Voicute 处理代码 | [S3 来源记录](lckfb-szpi-esp32s3-tirtc/components/starter_voice/vendor/UPSTREAM.md)、[P4 来源记录](waveshare-esp32p4-xiaotai/components/starter_voice/vendor/UPSTREAM.md) | 保留来源和代码中的版权 |
| 唤醒模型与 head 参数 | [S3 模型](lckfb-szpi-esp32s3-tirtc/components/starter_voice/model/)、[P4 模型](waveshare-esp32p4-xiaotai/components/starter_voice/model/) | 两工程各自保留模型、参数与现有一致性检查 |
| 字库及补充字形 | 工程生成源码中的 SimHei 来源注释 | 所有者授权针对现有字形产物，不扩大为对原字体文件的授权 |
| 提示音、铃声及 UI 资源 | 工程内嵌资源与原有来源记录 | 保留现有许可和来源，不改变原作者署名 |
| IDF 组件及本地供应商补丁 | 各工程 dependencies.lock、组件 LICENSE/NOTICE 和 LOCAL_CHANGES | 保留依赖身份及补丁说明，项目 MIT 不替代供应商许可 |

Voicute 来源：[onnx-wakeword](https://github.com/voicute/onnx-wakeword)，参考提交 `61e76c5a12ac33da0def7bceba2e07228ad03e63`，仍遵循上游许可。

授权仅覆盖本项目现有资产，不扩展到原字体文件或其他上游资源。
