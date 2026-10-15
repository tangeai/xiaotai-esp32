<div align="center">

# 小钛 ESP32

**简体中文** | [English](README.en.md)

**ESP32 实时音视频与 AI 对讲**

[![MIT License](https://img.shields.io/badge/License-MIT-2EA043?style=flat-square)](LICENSE)
[![ESP32-S3](https://img.shields.io/badge/ESP32-S3-E7352C?style=flat-square&logo=espressif&logoColor=white)](lckfb-szpi-esp32s3-tirtc/README.md)
[![ESP32-P4](https://img.shields.io/badge/ESP32-P4-E7352C?style=flat-square&logo=espressif&logoColor=white)](waveshare-esp32p4-xiaotai/README.md)
[![ESP32-C3](https://img.shields.io/badge/ESP32-C3-E7352C?style=flat-square&logo=espressif&logoColor=white)](mz28-ai-xiaotai/README.md)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-5.5.4%20%7C%205.5.5-0969DA?style=flat-square)](#开发资料)
[![GitHub Stars](https://img.shields.io/github/stars/tangeai/xiaotai-esp32?style=flat-square&label=Stars&color=B8860B)](https://github.com/tangeai/xiaotai-esp32)
[![GitHub Issues](https://img.shields.io/github/issues/tangeai/xiaotai-esp32?style=flat-square&label=Issues&color=0969DA)](https://github.com/tangeai/xiaotai-esp32/issues)

[![TiRTC SDK](https://img.shields.io/badge/TiRTC-SDK-0969DA?style=flat-square)](lckfb-szpi-esp32s3-tirtc/third_party/tirtc/README.md)
[![WebRTC](https://img.shields.io/badge/WebRTC-333333?style=flat-square&logo=webrtc&logoColor=white)](lckfb-szpi-esp32s3-tirtc/ARCHITECTURE.md)
[![LVGL](https://img.shields.io/badge/UI-LVGL-008FBE?style=flat-square)](lckfb-szpi-esp32s3-tirtc/dependencies.lock)
[![ESP-SR](https://img.shields.io/badge/Audio-ESP--SR-E7352C?style=flat-square&logo=espressif&logoColor=white)](lckfb-szpi-esp32s3-tirtc/ARCHITECTURE.md#音频经过哪些节点)
[![MQTT](https://img.shields.io/badge/MQTT-660066?style=flat-square&logo=mqtt&logoColor=white)](lckfb-szpi-esp32s3-tirtc/components/platform_client/src/platform_client.c)
[![WeChat VoIP](https://img.shields.io/badge/WeChat-VoIP-07C160?style=flat-square&logo=wechat&logoColor=white)](#微信呼叫)

[下载固件](#下载固件) · [立创 S3 变更](lckfb-szpi-esp32s3-tirtc/CHANGELOG.md) · [CoreS3-Lite 变更](m5stack-cores3-se-xiaotai/CHANGELOG.md) · [P4 变更](waveshare-esp32p4-xiaotai/CHANGELOG.md) · [在线烧录](https://espressif.github.io/esptool-js/) · [体验平台](https://xiaotai.chat/) · [服务端与小程序](https://github.com/tangeai/tirtc-server-example)

</div>

小钛通过 TiRTC/WebRTC 提供实时查看、微信呼叫、设备互呼、多人对讲和 AI 对讲功能。下载对应固件，按以下步骤完成烧录、配网和绑定即可体验，无需编译或自建服务器。

各板型的功能见下表。无屏 MZ28 专注微信双向语音，通过语音播报码、BOOT 按键和 RGB 灯操作；不使用下文的屏幕菜单、H5 查看或 AI 功能。

**下载 BIN → USB 烧录 → Wi-Fi 配网 → 网站绑定 → 开始体验**

<a id="下载固件"></a>

## 步骤 1：下载 BIN 固件

准备一块开发板、一根 USB 数据线、一台电脑和可上网的 **2.4 GHz Wi-Fi**。平台注册需要邮箱。按板型下载对应固件：

| 开发板 | 完整 BIN 下载 | 功能 |
| --- | --- | --- |
| 立创·实战派 ESP32-S3 V1.0.1/N16R8 | [下载 S3 1.5.0](https://github.com/tangeai/xiaotai-esp32/releases/download/esp32-s3-app-v1.5.0/xiaotai-esp32-s3-app-v1.5.0-full-16MB.bin) | 语音通话、AI 对讲、H5 摄像头查看 |
| M5Stack CoreS3-Lite | [下载 CoreS3-Lite 1.5.1](https://github.com/tangeai/xiaotai-esp32/releases/download/m5stack-cores3-lite-app-v1.5.1/xiaotai-m5stack-cores3-lite-app-v1.5.1-full-16MB.bin) | 语音通话、AI 对讲、H5 摄像头查看与双向语音 |
| 微雪 ESP32-P4-WIFI6-Touch-LCD-3.5 | [下载 P4 1.6.1](https://github.com/tangeai/xiaotai-esp32/releases/download/esp32-p4-app-v1.6.1/xiaotai-esp32-p4-app-v1.6.1-full-16MB.bin) | 音视频通话、实时画面、AI 对讲 |
| MZ28-AI-PPD V1.2（ESP32-C3，无屏） | [下载 MZ28 1.0.0（8 MB）](https://github.com/tangeai/xiaotai-esp32/releases/download/mz28-ai-app-v1.0.0/xiaotai-mz28-1.0.0-full-8MB.bin) | 微信双向语音、语音绑定码、BOOT 操作与七颗 RGB 灯 |

芯片要求：**S3 rev 0.0–0.99；ESP32-P4 rev3.2 及以上**。按板型选择固件，立创 S3 和 CoreS3-Lite 的固件不能互换；烧录工具提示不兼容时，请停止，不要跳过校验。

MZ28 使用 **8 MB Flash、C3 rev 0.3–1.99**，不能刷入 S3/P4 的 16 MB 包。下载后按 [MZ28 使用说明](mz28-ai-xiaotai/docs/USER_GUIDE.md)完成配网、语音验证码绑定和微信通话授权。

立创 S3、CoreS3-Lite 和 P4 当前为体验版（Pre-release）。[立创 S3 发布页](https://github.com/tangeai/xiaotai-esp32/releases/tag/esp32-s3-app-v1.5.0)、[CoreS3-Lite 发布页](https://github.com/tangeai/xiaotai-esp32/releases/tag/m5stack-cores3-lite-app-v1.5.1)、[P4 发布页](https://github.com/tangeai/xiaotai-esp32/releases/tag/esp32-p4-app-v1.6.1)的 **Assets** 中还提供烧录指南 `FLASHING_CN.md`、校验清单 `SHA256SUMS.txt` 和构建记录 `release-manifest.json`。`Source code` 是源码，不能直接烧录。

[MZ28 1.0.0 发布页](https://github.com/tangeai/xiaotai-esp32/releases/tag/mz28-ai-app-v1.0.0)提供正式版完整包及上述校验文件；另附固件 ZIP，包含同一次构建的 APP、启动分段和许可，用于已核对相同分区的保留配置升级。

**P4 只需通过一个 USB 烧录上述完整包。** 包内已带配套 C6 镜像，首次启动会检查并按兼容范围更新 C6，请保持供电直到进入配网或主页。无需另刷 C6 附件；出现恢复提示时按 [C6 指南](waveshare-esp32p4-xiaotai/docs/C6_PREPARATION.md)处理，不反复擦除或强刷。

<details>
<summary>校验下载文件与查看构建记录</summary>

S3/P4 完整 BIN 为 **16,777,216 字节**，MZ28 为 **8,388,608 字节**。在下载目录打开 PowerShell，核对大小和 SHA-256：

```powershell
Get-Item ./*-full-*.bin | Select-Object Name, Length
Get-FileHash ./*-full-*.bin -Algorithm SHA256
```

结果须与同一发布页的 `SHA256SUMS.txt` 一致。Linux 可运行 `sha256sum -c SHA256SUMS.txt`，macOS 可运行 `shasum -a 256 -c SHA256SUMS.txt`；先下载清单列出的全部文件。

`release-manifest.json` 记录对应源码、SDK、构建配置、固件哈希及验证范围。构建和文件校验不等于目标板音视频或长稳验收；历史固件的板型与使用说明以其附件为准。

</details>

<a id="用浏览器烧录"></a>

## 步骤 2：用浏览器烧录

> 烧录完整 BIN 会清除设备中的配网信息、绑定信息和用户设置，请先备份。

1. 用短 USB 数据线直连电脑，关闭占用串口的软件。**P4 接主芯片烧录口；板载 C6 负责无线通信，不刷本包。**
2. 在电脑的 **Chrome/Edge** 中打开[乐鑫在线烧录工具](https://espressif.github.io/esptool-js/)，在 **Program** 区域保留 Baudrate `921600`，点击 **Connect**，选择开发板串口。**Console** 用于查看串口日志，烧录时无需点击 **Start**。

   <img src="lckfb-szpi-esp32s3-tirtc/docs/images/browser-flash-connect.png" alt="在 Program 区域点击 Connect，不使用 Console 的 Start" width="760">

   图中 `No port selected` 表示取消了串口选择，重新点击 **Connect** 即可。

   <img src="lckfb-szpi-esp32s3-tirtc/docs/images/browser-flash-port.png" alt="选择开发板对应的串口，再点击连接" width="480">

   图示为 S3，串口号以实际设备为准，不必与图中的 `COM42` 一致。

3. 点击 **Add File**，只添加一个对应板型的完整 BIN（MZ28 为 `full-8MB.bin`，其余为 `full-16MB.bin`），**Flash Address 填 `0x0`**（图中的 `0` 与 `0x0` 相同）；Flash Mode、Flash Frequency、Flash Size 均选 **`keep`**，无需另点 **Erase Flash**。

   <img src="lckfb-szpi-esp32s3-tirtc/docs/images/browser-flash-program.png" alt="选择完整 16 MB BIN，地址为 0，三项 Flash 参数选 keep，再点击 Program" width="760">

4. 点击 **Program**，等待写入完成且无报错，再点击 **Disconnect**，按开发板的 **RESET/RST** 键重启。

- **串口选哪个？** 拔插开发板，对比列表中消失或新增的一项；名称可能为 `USB JTAG/serial debug unit`、`USB Serial` 或 `COM…`，不要选蓝牙串口。
- **连接失败？** 确认使用数据线。立创 S3/P4/MZ28 按住 **BOOT**，按下并松开 **RESET**，再松开 BOOT；**CoreS3-Lite 长按 RST 约 3 秒，看到绿色指示灯亮后松开**，再选择下载模式串口。详见 [M5Stack 下载模式说明](https://docs.m5stack.com/en/core/CoreS3-Lite#download-mode)。
- **出现 `Serial data stream stopped`？** 先换短数据线、避开 Hub/延长线；仍失败时，断开连接，将 **Program → Baudrate** 降到 `115200` 后重连重试。

## 步骤 3：连接 Wi-Fi

**MZ28 无屏：**按语音提示连接 `XiaoTai-XXXX`，打开 `http://192.168.6.1` 选择 2.4 GHz 网络并输入密码；联网后会循环播报六位绑定码。四连击 BOOT 可重新配网。以下屏幕操作用于另外三款板型。

1. 按设备屏幕提示使用热点配网，三个板型均可通过手机填写 Wi-Fi 信息。
2. 用手机连接屏幕显示的 `XiaoTai-XXXX` 开放热点，访问 `http://192.168.6.1`。
3. 在列表中选择 **2.4 GHz Wi-Fi**，输入密码并提交；已保存的网络可直接复用密码，隐藏网络可手动添加。设备联网并完成校时后，屏幕会显示 **6 位绑定码**。

配网完成后，将手机或电脑切回可上网的网络，再访问体验平台。

## 步骤 4：在网站绑定设备

1. 打开[小钛体验平台](https://xiaotai.chat/)，注册并登录账号。
2. 进入[我的设备](https://xiaotai.chat/devices)，点击 **添加设备 → 验证码绑定**。
3. 输入设备屏幕显示或 MZ28 语音播报的 **6 位绑定码**，点击 **绑定设备**。
4. 返回设备列表，确认设备显示“在线”。绑定码过期时，按设备提示重新获取。

## 步骤 5：开始体验

实时查看、微信呼叫和 AI 对讲只需一台设备；设备互呼需要两台。切换功能前，请先退出实时查看或结束当前通话。

### 实时查看

1. 在网站设备列表中，点击目标设备的 **实时**。
2. 点击声音按钮，收听设备端声音。立创 S3/P4 接入对应摄像头后可查看实时画面；立创 S3 使用 GC2145，CoreS3-Lite 使用板载 GC0308，两者默认 240×176、目标 12 fps，同时支持双向语音。
3. 允许浏览器使用麦克风，点击并按住 **按住说话**，向设备端发送语音。

### 微信呼叫

**MZ28：**在正式版“小钛”微信小程序中授权通话。在线待机时单击 BOOT 呼叫第一位联系人，来电时单击接听；双击拒接或挂断。完整操作见 [MZ28 使用说明](mz28-ai-xiaotai/docs/USER_GUIDE.md)。以下是带屏工程的操作。

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

多人对讲需要平台提供房间服务，三个板型都使用语音。详细操作见 [立创 S3 指南](lckfb-szpi-esp32s3-tirtc/docs/GETTING_STARTED_CN.md#多人对讲)、[CoreS3-Lite 指南](m5stack-cores3-se-xiaotai/docs/GETTING_STARTED_CN.md#多人对讲)、[P4 指南](waveshare-esp32p4-xiaotai/docs/GETTING_STARTED_CN.md#多人对讲)。

### AI 对讲

1. 结束当前通话，回到设备首页。
2. **点击首页表情**，或对设备说 **“你好小钛”**。
3. 设备响应后即可对话，例如“介绍一下你自己”。

## 开发资料

- **版本改动**：[立创 S3 变更记录](lckfb-szpi-esp32s3-tirtc/CHANGELOG.md)、[CoreS3-Lite 变更记录](m5stack-cores3-se-xiaotai/CHANGELOG.md)、[P4 变更记录](waveshare-esp32p4-xiaotai/CHANGELOG.md)，分别查看各版本的功能、修正与依赖变化。
- **MZ28 无屏应用**：[使用说明](mz28-ai-xiaotai/docs/USER_GUIDE.md)、[开发指南](mz28-ai-xiaotai/docs/GETTING_STARTED_CN.md)、[变更记录](mz28-ai-xiaotai/CHANGELOG.md)。ESP32-C3 / ESP-IDF 5.5.5 / TiRTC 2.5.0，工程独立构建。
- **构建与配置**：[立创 S3 开发指南](lckfb-szpi-esp32s3-tirtc/README.md)、[CoreS3-Lite 开发指南](m5stack-cores3-se-xiaotai/README.md)、[P4 开发指南](waveshare-esp32p4-xiaotai/README.md)。两个 S3 工程使用 ESP-IDF 5.5.4，P4 使用 5.5.5；各工程独立构建。编译 P4 前需按指南下载同版 C6 附件。
- **架构与代码入口**：[S3 通信与音频链路](lckfb-szpi-esp32s3-tirtc/ARCHITECTURE.md)、[P4 通信与音视频链路](waveshare-esp32p4-xiaotai/docs/P4_MEDIA_ARCHITECTURE.md)。
- **排障与使用限制**：[S3 已知问题](lckfb-szpi-esp32s3-tirtc/KNOWN_LIMITATIONS.md)、[P4 排障说明](waveshare-esp32p4-xiaotai/docs/TESTING.md)。
- **服务端、Web 与微信小程序**：[tirtc-server-example](https://github.com/tangeai/tirtc-server-example)。自建服务请参阅[部署指南](https://github.com/tangeai/tirtc-server-example/blob/main/thing-connect/deployment.md)。

体验固件仅用于受控网络，不使用生产凭据。生产接入前请核对 [S3 网络与凭据限制](lckfb-szpi-esp32s3-tirtc/KNOWN_LIMITATIONS.md#网络与凭据)及 [P4 服务配置](waveshare-esp32p4-xiaotai/docs/GETTING_STARTED_CN.md#配置服务)，分别处理 SDK、API 与 MQTT 的传输认证。

## 反馈与许可

[提交 Issue](https://github.com/tangeai/xiaotai-esp32/issues)时，请提供板型、固件版本、复现步骤及相关日志，并删除日志中的密码和密钥。

项目采用 [MIT License](LICENSE)，第三方 SDK 与资源遵循[各自许可](THIRD_PARTY.md)。
