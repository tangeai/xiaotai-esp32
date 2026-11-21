# 第三方来源与许可

小钛固件以 [MIT License](LICENSE) 发布。仓库里随源码分发的第三方代码、预编译库和资源都保留各自的版权和许可，项目的 MIT 不替代它们。许可原文放在 [docs/licenses](docs/licenses/)，组件自带的 LICENSE 文件和源码中的版权头一律保留原样。

更新第三方内容时，在这里改对应的那一行，并在组件目录里保留 `LOCAL_CHANGES.md`、`LOCAL_PATCH.md` 或 `UPSTREAM.md` 说明本地改动。

## 架构参考

单工程多板的组织方式（按板目录选择配置、板级与业务分离）参考了 [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)，基线提交 `a78ca19d5bc9a12ee0853b9f509530276d36aa03`，MIT。根 [LICENSE](LICENSE) 保留了它的版权行。Board 和 Application 接口按小钛的职责重新编写，不包含该项目的云服务、激活、MCP 或 OTA 实现。


## 随源码分发的代码

| 位置 | 来源 | 许可 | 本地改动 |
| --- | --- | --- | --- |
| `vendor/esp32p4/` | 乐鑫组件：esp_cam_sensor 2.6.0、esp_sccb_intf 0.0.9、esp_wifi_remote 1.6.5、esp_h264 1.4.1 | Apache-2.0 | esp_h264 见 [LOCAL_CHANGES](vendor/esp32p4/espressif__esp_h264/LOCAL_CHANGES.md)，其余未改 |
| `vendor/esp32p4/` | 乐鑫组件：esp_ipa 2.4.0、esp_video 2.5.0 | Espressif MIT，只能用于乐鑫芯片 | 未改 |
| [esp_lcd_jd9165](sdk/parts/xiaotai_part_panel_jd9165/esp-idf/UPSTREAM.md) | Wireless-Tag 官方提交 `b5d292f3292c5d73652f678f76b0771fde166117` | Apache-2.0 | 见 UPSTREAM |
| [bounded_lcd_i2c.c](sdk/parts/xiaotai_part_panel_ssd1306/esp-idf/bounded_lcd_i2c.c)、[USB 连接监测](sdk/xiaotai_port/esp-idf/usb_monitor/UPSTREAM.md) | ESP-IDF 的 `esp_lcd_panel_io_i2c.c` 和 USB Serial/JTAG 连接监测 | Apache-2.0，版权 Espressif | 前者加每个实例的传输超时，后者加 SOF 计数恢复和只读诊断 |
| [mono_glyph5.c](sdk/themes/xiaotai_theme_mono128/src/mono_glyph5.c) 中的 5×7 字模 | `tangeai/xiaotai-lierda` 提交 `366dec0ef2da3dc1061a1e77bbccf741f0aa6c6e` 的 `ssd1306_display.c` | Apache-2.0，版权 Lierda、Tange | 改为适配 SSD1306 页式帧缓冲 |
| [mono_glyph5.c](sdk/themes/xiaotai_theme_mono128/src/mono_glyph5.c) 中的 5×7 字模 | 沿用上行字模 | [Apache-2.0](docs/licenses/Apache-2.0.txt)，保留 Lierda、Tange 版权 | 字模与新帧绘制代码分开 |
| [CoreS3 电源与屏幕初始化](boards/m5stack/core-s3/board.cc) | M5Unified `fd40d58b8405ad1e7ed7afab548dab5e7cec5bbb`、M5GFX `c5a3fefad0b38a52cc750e2c4761e339a69a7208`、M5CoreS3 GC0308 参考 | [MIT，版权 M5Stack](docs/licenses/M5Stack-MIT.txt) | 参照其初始化序列，用 IDF 驱动实现 |
| [唤醒前处理](sdk/xiaotai_wake/src/model/PREPROCESS_UPSTREAM.md) | [voicute/onnx-wakeword](https://github.com/voicute/onnx-wakeword) 提交 `61e76c5a12ac33da0def7bceba2e07228ad03e63` | 项目所有者确认按 MIT 再分发，仅限当前随附的代码 | FFT 初始化失败时返回错误 |
| 配网页内嵌图标（各 `wifi_manager/web/`、`wifi_provisioning/web/`） | [Lucide](https://github.com/lucide-icons/lucide) | [ISC；部分源自 Feather，MIT](sdk/xiaotai_net/web/ICONS_LICENSE.txt) | 内嵌在页面里，运行时不访问 CDN |
| P4 家族 ESP-Hosted 补丁（各板 `build-support/tools/patches/`） | espressif/esp-hosted 3.0.7 | Apache-2.0 | WT99 的 SPI 诊断见 [来源记录](docs/provenance/WT-SPI诊断来源.md) |

## 预编译库

| 位置 | 内容 | 许可 |
| --- | --- | --- |
| [`components/tirtc_sdk/`](components/tirtc_sdk/README.md) | TiRTC 通话 SDK。仓库里只有构建契约和官方 2.6.3 的头文件；库文件不随仓库分发，构建时由 `scripts/fetch_tirtc.py` 从 TiRTC 官方发布站下载 | 按 TiRTC 官方发布条款使用；下载地址和哈希见 `packages.json`，构建契约见 `contracts/` |
| TiRTC 官方包内的加密实现 | ESP-IDF 6.0.3 版官方包自带加 TiRTC 私有前缀的 Mbed TLS 3.2.1 对象，随下载的官方包提供，不在本仓库 | 见官方包内说明 |
| `vendor/esp32p4/` 内的 `.a` 文件 | esp_h264 软件编解码等预编译库 | 随所在组件的许可 |

## 资源

| 资源 | 来源 | 许可 |
| --- | --- | --- |
| [唤醒模型“你好小钛”](sdk/xiaotai_wake/esp-idf/model/README.md) | 项目所有者提供 | 所有者确认按 MIT 再分发，仅限当前随附的模型 |
| 18 px 中文字库（[components/xiaotai_lvgl/fonts](components/xiaotai_lvgl/fonts/)，四款彩屏产品共用） | 主字库和补充字库中的 97 个字形由 Noto Sans SC Medium（500）生成。主字库只省略上游源没有的 `U+2252`；补充字库另外保留 34 个原位图，清单及来源哈希见 [font_source.json](components/xiaotai_lvgl/fonts/font_source.json) | Noto 生成部分：[SIL OFL 1.1](components/xiaotai_lvgl/fonts/source/OFL-NotoSansSC.txt)。34 个原位图沿用原授权，未改为 OFL；这些旧位图的原始字体文件不随附。 |
| [彩屏程序化表情](sdk/themes/xiaotai_theme_lcd_kit/assets/NOTICE.md) | 小钛现用的造型、配色和动作；彩屏主题套件只编译一份 | 项目所有者确认可随小钛 SDK 和固件内部使用、公开分发；不含其他素材 |
| WT99 门禁 18–32 px 字库 | Noto Sans SC，[生成清单](apps/xiaotai_app_door/assets/fonts/manifest.json) | [SIL OFL 1.1](components/xiaotai_lvgl/fonts/source/OFL-NotoSansSC.txt) |
| WT99 时钟字库 | Inter Light，[生成清单](apps/xiaotai_app_door/assets/source/inter/manifest.json) | [SIL OFL 1.1](apps/xiaotai_app_door/assets/source/inter/OFL.txt) |
| WT99 壁纸与图标 | [设计稿导出的壁纸、几何资源和 Lucide 图标](apps/xiaotai_app_door/assets/source/door-v4/README.md) | 图标 ISC；其余见 README |
| 单色主题 12 px 字形 | Xmini 在用的 Noto Sans CJK SC 位图和 Noto Sans SC Regular 拼音补充，[来源与哈希](sdk/themes/xiaotai_theme_mono128/assets/font_source.json) | [SIL OFL 1.1](sdk/themes/xiaotai_theme_mono128/assets/OFL.txt) |
| [Xmini 语音提示](sdk/xiaotai_sounds/packs/guide_zh_alaw8k/source_manifest.json) | 项目所有者提供的录音，转为 8 kHz A-law | 由所有者提供，不在本项目 MIT 授权范围内 |
| [文档配图](docs/assets/home/SOURCES.md) | 项目维护者提供 | 见来源说明 |

## 构建时下载的依赖

其余 IDF 组件由组件管理器按 [依赖锁](profiles/dependency-locks/) 下载，许可随组件包保留。主要有：

| 组件 | 版本 | 许可 |
| --- | --- | --- |
| lvgl/lvgl | 9.5.0 | MIT |
| espressif/esp-tflite-micro | 1.4.1 | Apache-2.0 |
| espressif/esp-nn | 1.4.2 | Apache-2.0 |
| espressif/esp-sr | 2.6.1 | Espressif MIT，含预编译库，只能用于乐鑫芯片 |
| espressif/esp-dl、dl_fft | 3.3.14、0.8.0 | MIT |
| espressif/esp32-camera（含 GC2145 驱动） | 2.1.7 | Apache-2.0 |
| espressif/esp_new_jpeg | 1.0.2 | Espressif MIT，只能用于乐鑫芯片 |
| espressif/esp_audio_codec | 2.6.2 | Espressif 许可，含预编译库，只能用于乐鑫芯片 |
| espressif/esp_hosted | 3.0.7 | Apache-2.0 |
| espressif/mqtt、cjson、esp-dsp、esp_lvgl_port、esp_lcd_* 等 | 见依赖锁 | 见各组件 |

ESP-IDF 本身由开发环境提供，Apache-2.0，见 [许可原文](docs/licenses/Apache-2.0.txt)。
