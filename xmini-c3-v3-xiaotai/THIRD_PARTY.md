# 来源与许可

| 内容 | 来源与许可 |
| --- | --- |
| 应用、按键、页面模型、硬件封装 | 本工程新增，MIT，见 [LICENSE](LICENSE) |
| 平台 HTTP/MQTT、身份及配网页基础 | 一次性本地化自同工作区 `mz28-ai-xiaotai` 的 `demo_platform` / `wifi_provisioning`；原工程未改。本工程另加状态快照、`xmini` 命名空间、可信校时和共享 AT 提交入口；没有共享源码路径、身份或 SDK |
| 12 px 中文及 ASCII 扩展字形 | Noto Sans CJK SC 位图字表，ASCII 与 U+4E00..9FFF，共 21087 项，SIL OFL 1.1；原字体来源、字体索引和 SHA-256 见 `components/xmini_ui/font_source.json`，保留 `LICENSES/Noto-OFL.txt` |
| 语音提示资源 | 23 段录音由资源提供者交付，经裁剪转为 8 kHz / 单声道 G.711 A-law；文本与哈希见 `tools/guidance.json`、`components/xmini_audio/assets/guidance_manifest.json`。提供者已于 2026-10-25 确认可随本工程源码和固件公开分发；该确认不构成语音的 MIT 重新授权，也不授予未约定的其他用途。原录音不随本仓提供。`tools/prepare_guidance.ps1` 为历史合成入口，不用于重建当前音色，不应直接运行覆盖资源 |
| 5×7 字模与画布绘制适配 | `tangeai/xiaotai-lierda` 提交 `366dec0ef2da3dc1061a1e77bbccf741f0aa6c6e` 的 `examples/NT26FxDx_OpenKit/tirtc_lite/src/ui/ssd1306_display.c`，Apache-2.0，保留 Lierda 与 Tange 版权，见 [许可证](LICENSES/Apache-2.0.txt) |
| Xmini-C3-V3 引脚与面板方向依据 | `78/xiaozhi-esp32` 提交 `49b4380b3faa0ceadd28a3e78aaf0f311c46c57e`；只对照硬件定义与接口调用，不包含其应用、唤醒模型、音频资产或固件 |
| ESP-IDF 5.5.5 与 SSD1306 驱动 | Espressif，Apache-2.0；由开发环境提供 |
| `esp_driver_usb_serial_jtag` | ESP-IDF 5.5.5 组件完整本地副本，保留供应方文件与 Apache-2.0；本地 SOF 计数恢复及只读诊断变化见 [补丁说明](components/esp_driver_usb_serial_jtag/LOCAL_PATCH.md)，不修改全局 SDK |
| `esp_codec_dev 1.6.2` | Espressif `esp-adf` 组件，Apache-2.0；组件管理器原包复制到本工程，保留授权与 97 个原始文件，仅修改 I2S 状态切换，见 [本地补丁](components/esp_codec_dev/LOCAL_PATCH.md) |
| `esp_audio_codec 2.4.0` | Espressif `esp-adf-libs` 组件，包含 Espressif 专用许可与预编译库，只用于 Espressif 产品；保留组件包内的逐文件授权，不套用本工程 MIT |
| TiRTC C3 SDK | 已接入的 ESP32-C3 / ESP-IDF 5.5.5 RISC-V 预编译库和头文件，见 `components/tirtc_sdk/`；按已确认的供应方授权保留原件和来源说明，本地发布不替换 SDK 或库 |

`esp_codec_dev` 使用本工程的本地组件覆盖，`esp_audio_codec` 继续通过版本清单与依赖锁取得。没有导入利尔达 ARM 库、MZ28 Opus 资源、S3 Xtensa 库或 P4 库。TiRTC C3 SDK 的实际交付身份和已测镜像 SHA-256 随本地烧录包记录，不以其他板型 SDK 或历史候选说明替代。
