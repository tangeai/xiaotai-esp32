<div align="center">

# 小钛 ESP32

**TiRTC · WebRTC 实时音视频设备应用**

[![MIT License](https://img.shields.io/badge/License-MIT-2EA043?style=flat-square)](LICENSE)
[![ESP32-S3](https://img.shields.io/badge/ESP32-S3-E7352C?style=flat-square&logo=espressif&logoColor=white)](lckfb-szpi-esp32s3-tirtc/README.md)
[![ESP32-P4](https://img.shields.io/badge/ESP32-P4-E7352C?style=flat-square&logo=espressif&logoColor=white)](waveshare-esp32p4-xiaotai/README.md)
[![ESP-IDF 5.5.4](https://img.shields.io/badge/ESP--IDF-5.5.4-0969DA?style=flat-square)](https://github.com/espressif/esp-idf/releases/tag/v5.5.4)
[![GitHub Stars](https://img.shields.io/github/stars/tangeai/xiaotai-esp32?style=flat-square&label=Stars&color=B8860B)](https://github.com/tangeai/xiaotai-esp32)
[![GitHub Issues](https://img.shields.io/github/issues/tangeai/xiaotai-esp32?style=flat-square&label=Issues&color=0969DA)](https://github.com/tangeai/xiaotai-esp32/issues)

</div>

---

我们是探鸽，专注 WebRTC 实时音视频通信，**TiRTC 是我们的核心产品**。小钛把它装进开发板，让设备与设备、微信、H5 通话。**S3 做双向语音，P4 在此基础上增加视频。**

先让设备聊起来，再看代码。下面准备好了固件、烧录步骤，以及从一次通话读懂 TiRTC 的代码入口。

## 先用 16 MB 包体验

选好开发板，下载完整固件，用浏览器烧录。第一次体验，不必先安装 ESP-IDF。

| 开发板 | 16 MB 固件 | 指南 |
| --- | --- | --- |
| 立创·实战派 ESP32-S3 N16R8 | [S3 1.0.0](https://github.com/tangeai/xiaotai-esp32/releases/download/esp32-s3-app-v1.0.0/xiaotai-esp32-s3-app-v1.0.0-full-16MB.bin) | [烧录与校验](https://github.com/tangeai/xiaotai-esp32/releases/tag/esp32-s3-app-v1.0.0) |
| 微雪 ESP32-P4-WIFI6-Touch-LCD-3.5 | [P4 1.0.0](https://github.com/tangeai/xiaotai-esp32/releases/download/esp32-p4-app-v1.0.0/xiaotai-esp32-p4-app-v1.0.0-full-16MB.bin) | [烧录与校验](https://github.com/tangeai/xiaotai-esp32/releases/tag/esp32-p4-app-v1.0.0) |

当前为体验固件（Pre-release），验证范围见各版发布说明。

### 烧录前准备

按指南校验下载文件。固件适用芯片修订：S3 rev 0.0–0.99、P4 rev 1.0–1.99；S3/P4 不可互刷，P4 只刷主芯片、不刷 C6。

> **完整包会清除配网、绑定和用户设置，请先备份。**

### 用浏览器烧录

用 USB 数据线连接开发板，关闭串口终端，在 Chrome 或 Edge 中打开[乐鑫在线烧录工具](https://espressif.github.io/esptool-js/)。

1. 点 **Connect**，选择开发板串口。
2. 点 **Add File**，添加完整 BIN，地址填 **`0x0`**；Flash Mode / Frequency / Size 均选 `keep`。
3. 点 **Program**，等待成功后点 **Disconnect**，复位开发板。

### 开机体验

按屏幕或热点提示连接 Wi-Fi，在[小钛体验平台](https://demo-open.tange-ai.com)输入六位绑定码。绑定后，呼叫已授权联系人，试试双向语音；P4 还可以视频通话。只有一块板时，也可点击首页表情或说“你好小钛”体验 AI 对话。

具体操作见 [S3 使用说明](lckfb-szpi-esp32s3-tirtc/README.md) 和 [P4 使用说明](waveshare-esp32p4-xiaotai/README.md)。

## 体验之后，再看源码

读代码不必从第一行开始。跟着一通电话，看它怎样连接、怎样把声音和画面送出去，又怎样在挂断后释放资源。要改自己的板卡，再去看采集、播放和显示驱动。

| 工程 | 源码版本 | 构建与配置 | 架构 |
| --- | --- | --- | --- |
| S3 APP | [1.0.0 Tag](https://github.com/tangeai/xiaotai-esp32/tree/esp32-s3-app-v1.0.0) | [S3 README](lckfb-szpi-esp32s3-tirtc/README.md) | [通信与音频链路](lckfb-szpi-esp32s3-tirtc/ARCHITECTURE.md) |
| P4 APP | [1.0.0 Tag](https://github.com/tangeai/xiaotai-esp32/tree/esp32-p4-app-v1.0.0) | [P4 README](waveshare-esp32p4-xiaotai/README.md) | [通信与音视频链路](waveshare-esp32p4-xiaotai/docs/P4_MEDIA_ARCHITECTURE.md) |

使用 ESP-IDF 5.5.4，并保留完整仓库：这版 P4 会用到同级 S3 源码，模型检查也需要 `common/`。切换到某个 Tag 时，按该版本附带的 README 构建。

这版适合在受控网络中体验。准备部署时，先读 [S3 安全说明](lckfb-szpi-esp32s3-tirtc/KNOWN_LIMITATIONS.md) 和 [P4 连接安全说明](waveshare-esp32p4-xiaotai/docs/TESTING.md#网络与凭据)：两套 SDK 的认证能力不同，应用服务还需单独检查。

## 许可与反馈

项目采用 [MIT License](LICENSE)，允许使用、修改、分发和商业使用，须保留版权与许可声明；软件按现状提供，不附带担保。[第三方 SDK 与资源](THIRD_PARTY.md) 仍遵循各自许可。

遇到问题，欢迎提 [Issue](https://github.com/tangeai/xiaotai-esp32/issues)。带上板型、固件版本、复现步骤和报错前后的日志，比一句“连不上”更容易找到原因。记得去掉 Wi-Fi 密码、设备密钥和个人信息。
