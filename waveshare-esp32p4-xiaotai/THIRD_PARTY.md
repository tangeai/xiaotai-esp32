# 许可证与第三方来源

本项目使用 [MIT License](LICENSE)。当前随附的 TiRTC SDK、Voicute/唤醒模型、字库和提示音依据项目所有者的授权声明提供；供应商已有的版权、LICENSE 和 NOTICE 随源码保留，具体范围见下表。

| 资产 | 当前来源入口 | 保留要求 |
| --- | --- | --- |
| P4 TiRTC SDK | [SDK VERSION](components/tirtc_sdk/VERSION.md) | 保留 P4 补丁、strip、版本及哈希依据；不改称未经修改的官方包 |
| Voicute 处理代码 | [来源记录](components/starter_voice/vendor/UPSTREAM.md) | 保留来源和代码中的版权 |
| jsmn 房间快照解析器 | [来源记录](components/starter_runtime/src/vendor/README.md) | MIT；保留头文件中的版权与许可 |
| 配网页 Lucide 图标 | [图标许可](components/wifi_manager/web/ICONS_LICENSE.txt) | ISC / 部分源自 Feather 的 MIT；页面内嵌，无 CDN 依赖 |
| 唤醒模型与 head 参数 | 本工程 `components/starter_voice/model/` | 保留当前模型、来源和固定 SHA-256 校验 |
| 16 px、18 px 字库及补充字形 | 工程生成源码中的 SimHei 来源注释 | 所有者授权针对现有字形产物，不扩大为对原字体文件的授权 |
| 提示音、铃声及 UI 资源 | 工程内嵌资源与原有来源记录 | 保留现有许可和来源，不改变原作者署名 |
| IDF 组件及本地供应商补丁 | 各工程 dependencies.lock、组件 LICENSE/NOTICE 和 LOCAL_CHANGES | 保留依赖身份及补丁说明，项目 MIT 不替代供应商许可 |

Voicute 参考源码提交为 `61e76c5a12ac33da0def7bceba2e07228ad03e63`，上游为 [onnx-wakeword](https://github.com/voicute/onnx-wakeword)。项目所有者的声明不改变上游仓库自身许可。

上述授权范围限于本项目现有资产，不扩大为对原始字体文件或其他上游资源的授权。
