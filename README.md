<div align="center">

# 小钛 ESP32

**简体中文** | [English](README.en.md)

**ESP32 实时音视频与 AI 对讲**

[![MIT License](https://img.shields.io/badge/License-MIT-2EA043?style=flat-square)](LICENSE)
[![ESP32-S3](https://img.shields.io/badge/ESP32-S3-E7352C?style=flat-square&logo=espressif&logoColor=white)](lckfb-szpi-esp32s3-tirtc/README.md)
[![ESP32-P4](https://img.shields.io/badge/ESP32-P4-E7352C?style=flat-square&logo=espressif&logoColor=white)](waveshare-esp32p4-xiaotai/README.md)
[![ESP-IDF 5.5.4](https://img.shields.io/badge/ESP--IDF-5.5.4-0969DA?style=flat-square)](https://github.com/espressif/esp-idf/releases/tag/v5.5.4)
[![GitHub Stars](https://img.shields.io/github/stars/tangeai/xiaotai-esp32?style=flat-square&label=Stars&color=B8860B)](https://github.com/tangeai/xiaotai-esp32)
[![GitHub Issues](https://img.shields.io/github/issues/tangeai/xiaotai-esp32?style=flat-square&label=Issues&color=0969DA)](https://github.com/tangeai/xiaotai-esp32/issues)

[![TiRTC SDK](https://img.shields.io/badge/TiRTC-SDK-0969DA?style=flat-square)](lckfb-szpi-esp32s3-tirtc/third_party/tirtc/README.md)
[![WebRTC](https://img.shields.io/badge/WebRTC-333333?style=flat-square&logo=webrtc&logoColor=white)](lckfb-szpi-esp32s3-tirtc/ARCHITECTURE.md)
[![LVGL](https://img.shields.io/badge/UI-LVGL-008FBE?style=flat-square)](lckfb-szpi-esp32s3-tirtc/dependencies.lock)
[![ESP-SR](https://img.shields.io/badge/Audio-ESP--SR-E7352C?style=flat-square&logo=espressif&logoColor=white)](lckfb-szpi-esp32s3-tirtc/ARCHITECTURE.md#音频经过哪些节点)
[![MQTT](https://img.shields.io/badge/MQTT-660066?style=flat-square&logo=mqtt&logoColor=white)](lckfb-szpi-esp32s3-tirtc/components/platform_client/src/platform_client.c)
[![WeChat VoIP](https://img.shields.io/badge/WeChat-VoIP-07C160?style=flat-square&logo=wechat&logoColor=white)](#微信呼叫)

[下载固件](#下载固件) · [在线烧录](https://espressif.github.io/esptool-js/) · [体验平台](https://demo-open.tange-ai.com) · [服务端与小程序](https://github.com/tangeai/tirtc-server-example)

</div>

小钛通过 TiRTC/WebRTC 提供实时查看、微信呼叫、设备互呼、多人对讲和 AI 对讲功能。下载对应固件，按以下步骤完成烧录、配网和绑定即可体验，无需编译或自建服务器。

**下载 BIN → USB 烧录 → Wi-Fi 配网 → 网站绑定 → 开始体验**

<a id="下载固件"></a>

## 步骤 1：下载 BIN 固件

准备一块开发板、一根 USB 数据线、一台电脑和可上网的 **2.4 GHz Wi-Fi**。平台注册需要邮箱。按板型下载对应固件：

| 开发板 | 完整 BIN 下载（16 MB） | 功能 |
| --- | --- | --- |
| 立创·实战派 ESP32-S3 V1.0.1/N16R8 | [下载 S3 1.1.2](https://github.com/tangeai/xiaotai-esp32/releases/download/esp32-s3-app-v1.1.2/xiaotai-esp32-s3-app-v1.1.2-full-16MB.bin) | 语音通话、AI 对讲；无摄像头画面 |
| 微雪 ESP32-P4-WIFI6-Touch-LCD-3.5 | [下载 P4 1.1.2](https://github.com/tangeai/xiaotai-esp32/releases/download/esp32-p4-app-v1.1.2/xiaotai-esp32-p4-app-v1.1.2-full-16MB.bin) | 音视频通话、实时画面、AI 对讲 |

烧录前请核对芯片修订版本：**S3 rev 0.0–0.99，P4 rev 1.0–1.99**。P4 rev 2.x/3.x 不适用；芯片修订版本与 PCB 版本不同。S3/P4 固件不可互刷。

当前固件为体验版（Pre-release）。烧录说明、校验文件及验证范围见 [S3 发布页](https://github.com/tangeai/xiaotai-esp32/releases/tag/esp32-s3-app-v1.1.2)、[P4 发布页](https://github.com/tangeai/xiaotai-esp32/releases/tag/esp32-p4-app-v1.1.2)。

<a id="用浏览器烧录"></a>

## 步骤 2：用浏览器烧录

> 烧录完整 BIN 会清除设备中的配网信息、绑定信息和用户设置，请先备份。

1. 用 USB 数据线连接开发板，关闭占用串口的软件。**P4 接主芯片烧录口，不刷板载 C6。**
2. 在电脑的 **Chrome/Edge** 中打开[乐鑫在线烧录工具](https://espressif.github.io/esptool-js/)，点击 **Connect**，选择开发板串口。
3. 点击 **Add File**，选择下载的 `*-full-16MB.bin` 文件，**Flash Address 填 `0x0`**；Flash Mode、Flash Frequency、Flash Size 均选 **`keep`**。
4. 点击 **Program**。烧录成功后点击 **Disconnect**，按复位键重启设备。

找不到串口时，请检查 USB 线是否支持数据传输。连接失败时，可按住 **BOOT**，按一下 **RESET**，松开 BOOT 后重试。

## 步骤 3：连接 Wi-Fi

1. 按设备屏幕提示使用热点配网，S3/P4 均可通过手机填写 Wi-Fi 信息。
2. 用手机连接屏幕显示的 `XiaoTai-XXXX` 开放热点，访问 `http://192.168.6.1`。
3. 在列表中选择 **2.4 GHz Wi-Fi**，输入密码并提交；已保存的网络可直接复用密码，隐藏网络可手动添加。设备联网并完成校时后，屏幕会显示 **6 位绑定码**。

配网完成后，将手机或电脑切回可上网的网络，再访问体验平台。

## 步骤 4：在网站绑定设备

1. 打开[小钛体验平台](https://demo-open.tange-ai.com)，注册并登录账号。
2. 进入[我的设备](https://demo-open.tange-ai.com/devices)，点击 **添加设备 → 验证码绑定**。
3. 输入设备屏幕上的 **6 位绑定码**，点击 **绑定设备**。
4. 返回设备列表，确认设备显示“在线”。绑定码过期时，按设备提示重新获取。

## 步骤 5：开始体验

实时查看、微信呼叫和 AI 对讲只需一台设备；设备互呼需要两台。切换功能前，请先退出实时查看或结束当前通话。

### 实时查看

1. 在网站设备列表中，点击目标设备的 **实时**。
2. 点击声音按钮，收听设备端声音。P4 接入摄像头后还可查看实时画面；S3 仅支持音频。
3. 允许浏览器使用麦克风，点击并按住 **按住说话**，向设备端发送语音。

### 微信呼叫

1. 点击设备上的 **微信电话**。没有微信联系人时，屏幕会显示小程序二维码。
2. 用微信扫码，按小程序提示授权设备发起语音/视频通话。
3. 在设备通讯录中同步联系人，选择已授权的微信联系人发起呼叫，在手机上接听。

已有微信联系人时，点击 **微信电话**会直接呼叫列表中的第一个微信联系人。S3 支持语音通话，P4 支持音视频通话。

### 设备互呼

1. 按步骤 1–4 配置第二台设备。绑定到同一账号的设备会自动互为联系人。
2. 在设备通讯录中同步联系人。若两台设备属于不同账号，需先在网站的 **联系人** 页面添加对方，并由对方确认。
3. 选择对方设备发起呼叫，在另一台设备上接听。两端均支持视频时，可选择视频呼叫。

### 多人对讲

1. 准备两台或更多已绑定、在线的设备，在“三点菜单 → 多人对讲”中创建房间。
2. 其他设备输入同一个六位房间号加入；设有密码时填写对应的四位密码。
3. 等待房间连接后，按住讲话，松开收听。返回菜单会断开对讲并保留房间关系；“退出房间”才会解除关系。

多人对讲需要平台提供房间服务，S3/P4 都使用语音。详细操作见 [S3 指南](lckfb-szpi-esp32s3-tirtc/docs/GETTING_STARTED_CN.md#多人对讲)、[P4 指南](waveshare-esp32p4-xiaotai/docs/GETTING_STARTED_CN.md#多人对讲)。

### AI 对讲

1. 结束当前通话，回到设备首页。
2. **点击首页表情**，或对设备说 **“你好小钛”**。
3. 设备响应后即可对话，例如“介绍一下你自己”。

## 开发资料

- **构建与配置**：[S3 开发指南](lckfb-szpi-esp32s3-tirtc/README.md)、[P4 开发指南](waveshare-esp32p4-xiaotai/README.md)。构建使用 ESP-IDF 5.5.4。两个工程各自包含所需代码、SDK、模型与资源，可独立构建。
- **架构与代码入口**：[S3 通信与音频链路](lckfb-szpi-esp32s3-tirtc/ARCHITECTURE.md)、[P4 通信与音视频链路](waveshare-esp32p4-xiaotai/docs/P4_MEDIA_ARCHITECTURE.md)。
- **排障与使用限制**：[S3 已知问题](lckfb-szpi-esp32s3-tirtc/KNOWN_LIMITATIONS.md)、[P4 排障说明](waveshare-esp32p4-xiaotai/docs/TESTING.md)。
- **服务端、Web 与微信小程序**：[tirtc-server-example](https://github.com/tangeai/tirtc-server-example)。自建服务请参阅[部署指南](https://github.com/tangeai/tirtc-server-example/blob/main/thing-connect/deployment.md)。

## 反馈与许可

[提交 Issue](https://github.com/tangeai/xiaotai-esp32/issues)时，请提供板型、固件版本、复现步骤及相关日志，并删除日志中的密码和密钥。

项目采用 [MIT License](LICENSE)，第三方 SDK 与资源遵循[各自许可](THIRD_PARTY.md)。
