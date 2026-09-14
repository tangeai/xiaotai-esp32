# 许可证与第三方来源

本项目使用 [MIT License](LICENSE)。当前随附的 TiRTC SDK、Voicute/唤醒模型、字库和提示音依据项目所有者的授权声明提供；供应商已有的版权、LICENSE 和 NOTICE 随源码保留，具体范围见下表。

| 资产 | 当前来源入口 | 保留要求 |
| --- | --- | --- |
| S3 TiRTC SDK | [SDK README](third_party/tirtc/README.md) | 保留头文件、静态库、构建契约与原始版权 |
| Voicute 处理代码 | [来源记录](components/starter_voice/vendor/UPSTREAM.md) | 保留来源和代码中的版权 |
| 唤醒模型与 head 参数 | 本工程 `components/starter_voice/model/` | 保留当前模型、来源和固定 SHA-256 校验 |
| 18 px 字库及补充字形 | 工程生成源码中的 SimHei 来源注释 | 所有者授权针对现有字形产物，不扩大为对原字体文件的授权 |
| 提示音、铃声及 UI 资源 | 工程内嵌资源与原有来源记录 | 保留现有许可和来源，不改变原作者署名 |
| 手机配网页图标 | [Lucide](https://github.com/lucide-icons/lucide)，内嵌于 [setup.html](components/wifi_manager/web/setup.html) | 保留页面中的 ISC 和 Feather 衍生图标 MIT 许可；无运行时 CDN 请求 |
| IDF 组件及本地供应商补丁 | 各工程 dependencies.lock、组件 LICENSE/NOTICE 和 LOCAL_CHANGES | 保留依赖身份及补丁说明，项目 MIT 不替代供应商许可 |

Voicute 参考源码提交为 `61e76c5a12ac33da0def7bceba2e07228ad03e63`，上游为 [onnx-wakeword](https://github.com/voicute/onnx-wakeword)。项目所有者的声明不改变上游仓库自身许可。

上述授权范围限于本项目现有资产，不扩大为对原始字体文件或其他上游资源的授权。
