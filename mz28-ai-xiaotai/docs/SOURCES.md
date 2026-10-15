# 依赖与资源来源

本工程为 MZ28 无屏移植，使用 ESP32-C3、ESP-IDF 5.5.5、TiRTC 2.5.0。文件身份以工程所附锁定文件及 SHA-256 为准，不沿用早期预录示例的库哈希。

| 依赖 | 位置 | 核对方式 |
|---|---|---|
| TiRTC 2.5.0 C3 | `components/tirtc_sdk` | `release-lock.json` 固定库哈希，`sdk_contract.c` 检查目标、版本和 RTOS ABI |
| ESP-IDF 5.5.5 | 外部工具链 | 根 CMake 校验版本 |
| esp_audio_codec 2.4.0 | 依赖管理器 | `idf_component.yml` 与 `dependencies.lock` |
| VB6824 UART 适配 | `components/vb6824` | MZ28-AI-PPD V1.2 原理图和模块协议 |
| Wi-Fi 配网 | `components/wifi_provisioning` | 小钛 SoftAP/Captive Portal 设计，C3 独立实现，无蓝牙依赖 |

当前 TiRTC 库 SHA-256：`dd9ee6548dd3e595e07289653f53aef78a4e664bb01c95d67eb9d8fd5603cae0`。库元数据版本不作为应用版本号，应用版本以根 CMake 为准。

ESP-IDF 保留 [Apache-2.0 许可](ESP_IDF_LICENSE.txt)；esp_audio_codec 使用 [Espressif Modified MIT 许可](ESP_AUDIO_CODEC_LICENSE.txt)，仅限与 Espressif 产品配合使用，不能作为通用 MIT 组件移植到其他芯片。固件压缩包也附带项目、上游及这些资产/组件的许可原文。

## 音频

`components/product_audio/assets/prompts/` 内每份 `.opuspkt.json` 记录来源、格式、包数和校验值。

- 数字 0–9 取自 [TboxAI/tbox-iot-esp32-s3](https://github.com/TboxAI/tbox-iot-esp32-s3/tree/f9eef243849c383a16fb0ac486f59ba7285abdba/main/assets) 的固定版本 `f9eef243849c383a16fb0ac486f59ba7285abdba`，保留来源元数据及原始 [MIT 许可](TBOX_LICENSE.txt)（Copyright 2025 yichone）。
- 绑定及配网引导语由 Windows Microsoft Huihui Desktop 离线生成，原文和声音名称保存在元数据中。
- 来电、拨号、点击、成功、警告及结束声效由 `tools/generate_interaction_sounds.py` 生成，没有嵌入苹果系统铃声。
- `components/wechat_voip/assets/audio.opuspkt` 仅为主机测试资产，不用于 MZ28 的实时麦克风上行，也不嵌入固件。

提示音为 16 kHz / 单声道 / 20 ms Opus，驻留 Flash。线上音频来自模块实时采集，下行由 ESP 音频解码器还原 PCM 后送回模块。

## 协议参考

- [TiRTC C API](https://docs.tange.ai/products/tirtc/api-reference/c.html)
- [微信 VoIP](https://docs.tange.ai/products/wxvoip/)
- [ThingConnect 参考工程](https://github.com/tangeai/tirtc-server-example/tree/main/thing-connect)

参考资料不替代本工程附带 SDK 的实际 API/ABI 契约。公开分发保留[项目许可](../LICENSE.txt)、上游声明及资产元数据；新增资产单独核对来源。
