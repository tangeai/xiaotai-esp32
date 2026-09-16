<div align="center">

# 小钛 P4 APP

[![MIT License](https://img.shields.io/badge/License-MIT-2EA043?style=flat-square)](../LICENSE)
[![ESP32-P4](https://img.shields.io/badge/ESP32-P4-E7352C?style=flat-square&logo=espressif&logoColor=white)](../README.md)
[![Source 1.2.0](https://img.shields.io/badge/Source-1.2.0-0969DA?style=flat-square)](https://github.com/tangeai/xiaotai-esp32/tree/esp32-p4-app-v1.2.0)
[![ESP-IDF 5.5.4](https://img.shields.io/badge/ESP--IDF-5.5.4-0969DA?style=flat-square)](https://github.com/espressif/esp-idf/releases/tag/v5.5.4)

[返回项目首页](../README.md)

</div>

---

用**微雪 ESP32-P4-WIFI6-Touch-LCD-3.5**，体验 TiRTC 的 WebRTC 音视频通信。另一端可以是设备、微信或 H5：既听到声音，也看到画面。

相比 [S3 APP](../lckfb-szpi-esp32s3-tirtc/README.md) 的语音通话和 H5 摄像头查看，P4 还支持双向视频通话。P4 处理音视频，板载 C6 负责 Wi-Fi，两颗芯片各做擅长的事。

## 先烧录，再体验

**[下载 1.2.0 的 16 MB 完整 BIN](https://github.com/tangeai/xiaotai-esp32/releases/download/esp32-p4-app-v1.2.0/xiaotai-esp32-p4-app-v1.2.0-full-16MB.bin)** · [烧录指南与校验文件](https://github.com/tangeai/xiaotai-esp32/releases/tag/esp32-p4-app-v1.2.0)

按[浏览器烧录步骤](../README.md#用浏览器烧录)从 `0x0` 写入，不用自己编译。当前为体验版（Pre-release），验证情况见发布说明。

固件适用 **P4 芯片 rev 1.0–1.99**，不适用 rev 2.x / 3.x。使用 P4 主芯片烧录口，不刷 C6；芯片修订与 PCB 版本是两回事，请分别核对。

> **完整包会清除配网、绑定和用户设置，请先备份。**

## 打通第一通电话

准备好平台账号和已授权联系人，接下来按顺序操作：

1. **连接网络**：连接屏幕显示的 `XiaoTai-XXXX` 热点，打开 `http://192.168.6.1`，选择 Wi-Fi 并输入密码；已保存的网络可复用密码。
2. **绑定设备**：在[小钛体验平台](https://xiaotai.chat/)输入屏幕上的六位绑定码。
3. **发起通话**：用通讯录呼叫已添加的设备联系人；微信电话入口呼叫第一个微信联系人，没有联系人时显示小程序二维码。
4. **说话、挥手**：双方轮流讲话，再同时讲话；视频通话时向镜头挥挥手，看看对方是否听得清、看得见。挂断后再拨一次。

只有一块板也能开始，用已授权的微信或 H5 做另一端即可。点击表情或说“你好小钛”，还可以和 AI 对话。AI 与纯语音会话不启动摄像头。本仓提供设备端源码，联网业务使用平台服务。

多台设备可以进入[多人对讲](docs/GETTING_STARTED_CN.md#多人对讲)：创建或加入同一个语音房间，按住讲话、松开收听。返回菜单会断开对讲，保留房间关系。

## TiRTC 与 WebRTC

**TiRTC 负责连接和媒体传输，本工程负责采集、编解码和呈现。** WebRTC 通信接入封装在 SDK 中，应用通过 `starter_tirtc` 收发媒体；绑定、联系人和呼叫请求由 `platform_client` 处理。

麦克风编码成 **G.711 A-law**，摄像头画面编码成 **H.264**，再交给 TiRTC 发送。收到的视频按对端区分：设备呼叫用 H.264，微信用 MJPEG。AEC、AGC 辅助声音处理，PPA 辅助画面变换；格式、分辨率和代码入口见[通信与媒体链路](docs/P4_MEDIA_ARCHITECTURE.md)。

## 开始改代码

开始改之前，先用 ESP-IDF 5.5.4 构建一次原版，给后续改动留一个可比较的起点。

| 项目 | 本版配置 |
| --- | --- |
| 摄像头 | OV5647，MIPI-CSI |
| 屏幕 | 480×320 显示布局 |
| 音频 | ES8311 单麦克风、DAC 回采参考 |
| 联网 | C6 / ESP-Hosted，SDIO 4 线 40 MHz SDR |
| 工具链 | riscv32-esp-elf GCC 14.2.0，`esp-14.2.0_20260121` |
| TiRTC SDK | 2.3.0 P4 验证补丁库，[版本与校验值](docs/DEPENDENCIES.md) |
| 应用版本 | 1.2.0，[变更记录](CHANGELOG.md#120) |

**本目录可独立构建。** 应用、SDK、音视频驱动、字体、模型和提示音均已包含，无需另外下载 S3 工程或公共资源目录。

打开 ESP-IDF 终端，进入本 README 所在目录。首次配置或目标选错时先执行 `idf.py set-target esp32p4`，然后构建：

```sh
idf.py --version
idf.py build
```

工程名为 `xiaotai_esp32p4`，输出到 `build/`，后续改动沿用这个目录；Windows 和 WSL 各用自己的缓存。更换芯片修订时，先按[版本与依赖](docs/DEPENDENCIES.md)核对配置，再构建。

### 试试自己的版本

确认串口属于 P4 主芯片，将 `<SERIAL_PORT>` 换成实际端口：

```sh
idf.py -p <SERIAL_PORT> flash monitor
```

命令会按 `build/flasher_args.json` 写入各段，不需要手填地址。保持[分区布局](partitions.csv)时默认不清 NVS，调整布局前先备份；**完整合并包从 `0x0` 写入，单独 APP BIN 应按分区地址烧录。** 此命令不刷 C6。启动后核对应用与 SDK 版本，按 `Ctrl+]` 退出监视器。

## 带着问题读代码

- **一通电话怎样完成？** [通信与媒体链路](docs/P4_MEDIA_ARCHITECTURE.md)串起 SDK、会话和音视频驱动。
- **有声音，为什么没画面？** [排障与验证](docs/TESTING.md)从连接、到包一路查到解码和显示。
- **怎样接自己的设备？** 看 [TiRTC C SDK 接入](https://docs.tange.ai/products/tirtc/guides/sdk-integration/c.html)和[音视频帧要求](https://docs.tange.ai/products/tirtc/guides/real-time-audio-video.html)；API 以本版附带的头文件为准。

代码与资源许可见 [LICENSE](../LICENSE) 和[第三方说明](../THIRD_PARTY.md)。
