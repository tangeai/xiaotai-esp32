<div align="center">

# 小钛 S3 APP

[![MIT License](https://img.shields.io/badge/License-MIT-2EA043?style=flat-square)](../LICENSE)
[![ESP32-S3](https://img.shields.io/badge/ESP32-S3-E7352C?style=flat-square&logo=espressif&logoColor=white)](../README.md)
[![Source 1.2.0](https://img.shields.io/badge/Source-1.2.0-0969DA?style=flat-square)](https://github.com/tangeai/xiaotai-esp32/tree/esp32-s3-app-v1.2.0)
[![ESP-IDF 5.5.4](https://img.shields.io/badge/ESP--IDF-5.5.4-0969DA?style=flat-square)](https://github.com/espressif/esp-idf/releases/tag/v5.5.4)

[返回项目首页](../README.md)

</div>

---

用**立创·实战派 ESP32-S3 N16R8**，体验 TiRTC 的 WebRTC 双向语音通信。另一端可以是设备、微信或 H5：你说的话传过去，对方的声音播出来。

S3 支持 H5 按需查看 GC2145 摄像头画面，默认 240×176、目标 12fps；设备和微信通话仍使用语音。需要双向视频通话时，使用 [P4 APP](../waveshare-esp32p4-xiaotai/README.md)。

## 先烧录，再体验

**[下载 1.2.0 的 16 MB 完整 BIN](https://github.com/tangeai/xiaotai-esp32/releases/download/esp32-s3-app-v1.2.0/xiaotai-esp32-s3-app-v1.2.0-full-16MB.bin)** · [烧录指南与校验文件](https://github.com/tangeai/xiaotai-esp32/releases/tag/esp32-s3-app-v1.2.0)

按[浏览器烧录步骤](../README.md#用浏览器烧录)从 `0x0` 写入，不用自己编译。固件适用 S3 芯片 rev 0.0–0.99，当前为体验版（Pre-release）；验证情况见发布说明。

> **完整包会清除配网、绑定和用户设置，请先备份。**

## 打通第一通电话

准备好平台账号和已授权联系人，接下来按顺序操作：

1. **连接网络**：未配网时，连接 `XiaoTai-XXXX` 热点，打开 `http://192.168.6.1`，选择 Wi-Fi 并输入密码；已保存的网络可复用密码。
2. **绑定设备**：在[小钛体验平台](https://xiaotai.chat/)输入屏幕上的六位绑定码。
3. **发起通话**：用通讯录呼叫已添加的设备联系人；微信电话入口呼叫第一个微信联系人，没有联系人时显示小程序二维码。
4. **说两句话**：轮流讲话，再同时讲话，听听双方是否清楚。挂断后再拨一次，确认下一通也能正常开始。

只有一块板也能开始，用已授权的微信或 H5 做另一端即可。点击表情、说“你好小钛”或短按 BOOT，还可以和 AI 对话，再按 BOOT 结束。本仓提供设备端源码，联网业务使用平台服务。

多台设备可以进入[多人对讲](docs/GETTING_STARTED_CN.md#多人对讲)：创建或加入同一个房间，按住讲话、松开收听。返回菜单会断开对讲，保留房间关系。

## TiRTC 与 WebRTC

**TiRTC 负责连接和媒体传输，本工程负责采集和播放。** WebRTC 通信接入封装在 SDK 中，应用通过 `starter_tirtc` 收发音频，并在 H5 查看时发送 JPEG 视频；绑定、联系人和呼叫请求由 `platform_client` 处理。

麦克风声音先经过双麦 AFE 处理，再编码成 **8 kHz、单声道 G.711 A-law** 发送。AEC 抑制回声，AGC 调整增益，下行缓冲减轻到包抖动。想知道一句话经过了哪些函数，接着读[通信与媒体链路](ARCHITECTURE.md)。

## 开始改代码

开始改之前，先用 ESP-IDF 5.5.4 构建一次原版，给后续改动留一个可比较的起点。

| 项目 | 本版配置 |
| --- | --- |
| 板卡 | 立创·实战派 ESP32-S3 V1.0.1，16 MB Flash、8 MB Octal PSRAM |
| 音频 | ES7210 双麦采集，ES8311 播放 |
| H5 摄像头 | GC2145，240×176、目标 12fps；按需启停 |
| 工具链 | Xtensa GCC 14.2.0，`esp-14.2.0_20260121` |
| TiRTC SDK | 2.3.0，[SDK 编译配置](third_party/tirtc/README.md) |
| 主要组件 | LVGL 8.3.11、ESP-SR 2.4.7，[完整依赖锁](dependencies.lock) |
| 应用版本 | 1.2.0，[变更记录](../CHANGELOG.md#120) |

换了板卡修订，先核对音频、屏幕、触摸和电源引脚。本目录可独立构建；SDK、`components/starter_voice/model/` 中的模型、字体和提示音均为构建输入。

打开 ESP-IDF 终端，进入本 README 所在目录。首次配置或目标选错时先执行 `idf.py set-target esp32s3`，然后构建：

```sh
idf.py --version
idf.py build
```

后续改动沿用同一个 `build/`；Windows 和 WSL 各用自己的缓存。默认配置见 [sdkconfig.defaults](sdkconfig.defaults)，Flash 布局见[分区表](partitions.csv)。调整 Flash、PSRAM 或 FreeRTOS 时，同时核对 SDK 编译配置。

### 试试自己的版本

确认串口属于目标 S3，将 `<SERIAL_PORT>` 换成实际端口：

```sh
idf.py -p <SERIAL_PORT> flash monitor
```

命令会按 `build/flasher_args.json` 写入各段，不需要手填地址。保持分区布局时默认不清 NVS，调整布局前先备份；**完整合并包从 `0x0` 写入，单独 APP BIN 应按分区地址烧录。** 启动后核对版本与联网日志，按 `Ctrl+]` 退出监视器。

## 带着问题读代码

- **一通电话怎样完成？** [通信与媒体链路](ARCHITECTURE.md)串起 SDK、会话和音频驱动。
- **连上了，为什么没声音？** [排障与安全说明](KNOWN_LIMITATIONS.md)从连接、到包一路查到播放。
- **怎样接自己的设备？** 看 [TiRTC C SDK 接入](https://docs.tange.ai/products/tirtc/guides/sdk-integration/c.html)和[音视频帧要求](https://docs.tange.ai/products/tirtc/guides/real-time-audio-video.html)；API 以本版附带的头文件为准。

代码与资源许可见 [LICENSE](../LICENSE) 和[第三方说明](../THIRD_PARTY.md)。
