# 许可证与第三方来源

本项目使用 [MIT License](LICENSE)。当前随附的 TiRTC SDK、Voicute/唤醒模型、字库和提示音依据项目所有者的授权声明提供；供应商已有的版权、LICENSE 和 NOTICE 随源码保留，具体范围见下表。

| 资产 | 当前来源入口 | 保留要求 |
| --- | --- | --- |
| P4 TiRTC SDK | [SDK VERSION](components/tirtc_sdk/VERSION.md) | 当前库及配套头文件来自指定的 CI 3043073 Mini 包（Nano 2.4.0），未本地重编译或 strip；保留随包 README、构建契约和哈希，不沿用其他版本补丁说明 |
| Voicute 处理代码 | [来源记录](components/starter_voice/vendor/UPSTREAM.md) | 保留来源和代码中的版权 |
| jsmn 房间快照解析器 | [来源记录](components/starter_runtime/src/vendor/README.md) | MIT；保留头文件中的版权与许可 |
| 配网页 Lucide 图标 | [图标许可](components/wifi_manager/web/ICONS_LICENSE.txt) | ISC / 部分源自 Feather 的 MIT；页面内嵌，无 CDN 依赖 |
| 唤醒模型与 head 参数 | 本工程 `components/starter_voice/model/` | 保留当前模型、来源和固定 SHA-256 校验 |
| 18 px 字库及补充字形 | 工程生成源码中的 SimHei 来源注释 | 所有者授权针对现有字形产物，不扩大为对原字体文件的授权 |
| 门禁 24/32 px 字库 | [Noto Sans SC 来源及生成清单](components/starter_product/assets/fonts/door_fonts_manifest.json) | SIL OFL 1.1；保留原始字体、[OFL](components/starter_product/assets/fonts/notosanssc-license/OFL.txt)、固定来源和生成脚本，未知动态字符回退既有字库 |
| 内机 V4 的 18/24/26/32 px 字库 | 同上 Noto Sans SC 原件及清单 | SIL OFL 1.1；24 px 正文为 Regular、操作为 Medium，按设计行高生成 |
| 内机 V4 时钟字库 | [Inter 原件及清单](components/starter_product/assets/fonts/inter/manifest.json) | SIL OFL 1.1；保留 [OFL](components/starter_product/assets/fonts/inter/OFL.txt)，仅提取 Light 128 px 数字、空格、冒号和连字符 |
| 内机 V4 山湖壁纸及图标 | [来源与生成方式](components/starter_product/assets/door-v4/README.md) | 用户确认的 Figma 设计导出；生成壁纸、设计几何资源与 Lucide 图标分开记录，保留 ISC/相关 MIT 许可和原件摘要 |
| 提示音、铃声及 UI 资源 | 工程内嵌资源与原有来源记录 | 保留现有许可和来源，不改变原作者署名 |
| IDF 组件及本地供应商补丁 | 各工程 dependencies.lock、组件 LICENSE/NOTICE 和 LOCAL_CHANGES | 保留依赖身份及补丁说明，项目 MIT 不替代供应商许可 |
| LVGL 8.4.0 图像覆盖判断 | [本地补丁说明](docs/LVGL_IMAGE_COVER.md) | 保留 LVGL MIT 许可；仅修正事件参数读取与受限原尺寸不透明图像的覆盖判断，不更换版本或字库 |

Voicute 参考源码提交为 `61e76c5a12ac33da0def7bceba2e07228ad03e63`，上游为 [onnx-wakeword](https://github.com/voicute/onnx-wakeword)。项目所有者的声明不改变上游仓库自身许可。

上述授权范围限于本项目现有资产，不扩大为对原始字体文件或其他上游资源的授权。

# WT99P461-S1 新增板级驱动

JD9165BA驱动来自Wireless-Tag官方提交
`b5d292f3292c5d73652f678f76b0771fde166117`，保留Apache-2.0版权声明与许可。
来源及移植范围见 [UPSTREAM](components/esp_lcd_jd9165/UPSTREAM.md)。
GT911使用乐鑫组件`esp_lcd_touch_gt911==1.2.1`，许可随组件保留。
