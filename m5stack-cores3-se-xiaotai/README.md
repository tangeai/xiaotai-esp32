# 小钛 CoreS3-SE

小钛 CoreS3-SE **1.5.0** 使用 TiRTC/WebRTC 提供 AI 对讲、设备间语音呼叫、多人对讲、微信语音通话和 H5 双向语音。先烧录完整包体验，二次开发再看[开发指南](docs/GETTING_STARTED_CN.md)。

[版本改动](CHANGELOG.md) · [发布页](https://github.com/tangeai/xiaotai-esp32/releases/tag/m5stack-cores3-se-app-v1.5.0) · [体验平台](https://xiaotai.chat/)

## 先体验

下载 [CoreS3-SE 1.5.0 完整 16 MB 包](https://github.com/tangeai/xiaotai-esp32/releases/download/m5stack-cores3-se-app-v1.5.0/xiaotai-m5stack-cores3-se-app-v1.5.0-full-16MB.bin)，无需安装 ESP-IDF 或自行编译。此包仅适用于 **M5Stack CoreS3-SE，16 MB Flash / 8 MB Quad PSRAM**，不能替代立创 S3、P4 或其他 M5Stack 型号的固件。

完整 BIN 应为 **16,777,216 字节**；SHA-256 与同一发布页的 `SHA256SUMS.txt` 核对。`Source code` 是源码，不是烧录包。附件 `release-manifest.json` 记录源码、配置、SDK 与固件身份；固件为 Pre-release 体验版，部署边界见[已知限制](KNOWN_LIMITATIONS.md)。

## 用浏览器烧录

> 完整包会清除配网、绑定和用户设置，请先备份。保持 USB 供电，不在写入过程中断开。

1. 用 USB-C 数据线直连电脑，关闭占用串口的软件。长按侧边 **RST 约 2 秒，绿色指示灯亮后松开**，进入下载模式。见 [M5Stack 官方说明](https://docs.m5stack.com/en/arduino/m5cores3_se/program)。
2. 用电脑 **Chrome/Edge** 打开[乐鑫在线烧录工具](https://espressif.github.io/esptool-js/)，在 **Program** 点击 **Connect**，选择本板串口。
3. **Add File** 只添加上述完整 BIN，**Flash Address 填 `0x0`**；Flash Mode、Frequency、Size 均选 **`keep`**。单独的 APP BIN 不能按此地址烧录。
4. 点击 **Program**，等待无报错完成，再点 **Disconnect**，短按 **RST** 重启。连接不稳时换短数据线或降低 Baudrate 后重试。

离线说明见同一发布页的 [FLASHING_CN.md](https://github.com/tangeai/xiaotai-esp32/releases/download/m5stack-cores3-se-app-v1.5.0/FLASHING_CN.md)。

## 准备什么

| 项目 | 要求 |
| --- | --- |
| 开发板 | M5Stack CoreS3-SE，16 MB Flash、8 MB Quad PSRAM，320×240 触摸屏、双麦和扬声器 |
| 编译环境 | ESP-IDF 5.5.4，配套 Xtensa GCC 14.2.0（`esp-14.2.0_20260121`） |
| 联网 | 可访问平台的 2.4 GHz Wi-Fi |
| 体验条件 | 平台账号和手机；设备互呼需要第二台已绑定设备 |

本工程包含应用代码、TiRTC 2.5.0 SDK、模型和资源，可独立构建。首次编译需联网下载组件依赖。

## 开始使用

1. 按上面的[浏览器烧录步骤](#用浏览器烧录)写入 CoreS3-SE 完整包。
2. 手机连接屏幕显示的 `XiaoTai-XXXX` 开放热点，访问 `http://192.168.6.1`，点选附近网络并输入密码；已保存的网络可直接复用密码。
3. 设备联网后显示六位验证码，在[体验平台](https://xiaotai.chat/)的设备页面填写验证码完成绑定。
4. 按[功能体验](docs/GETTING_STARTED_CN.md#功能体验)配置 AI、联系人、多人房间或微信授权，开始通话。

## 使用范围

- AI 对讲支持点击表情或说“你好小钛”唤醒，显示字幕和云端表情。
- 设备和微信使用语音；CoreS3-SE 没有摄像头，H5 只提供双向语音，不上报视频能力。
- 多人对讲按住房间页面的按钮发言，松开收听；返回菜单断开连接并保留房间，明确退出才解除入房关系。
- 第一麦和 ES7210 MIC3 扬声器电气回采进入官方 ESP-SR 单麦 AFE（回声消除与抑制），随后经过自动增益和自适应播放；第二麦当前只用于电平诊断，没有双麦增益。
- 未联网时进入配网；手动断开后等待重新配网，被动掉线时重连已保存网络。

AI 角色、联系人和微信授权由配套平台配置。部署前请阅读[网络与凭据风险](KNOWN_LIMITATIONS.md#网络与凭据)。

## 开发文档

| 要做什么 | 阅读哪里 |
| --- | --- |
| 编译、烧录、配网绑定和体验功能 | [开发指南](docs/GETTING_STARTED_CN.md) |
| 找到负责某项功能的代码 | [架构与代码入口](ARCHITECTURE.md) |
| 使用构建检查和主机测试 | [工具说明](tools/README.md) |
| 核对兼容性、安全和音频边界 | [已知限制](KNOWN_LIMITATIONS.md) |
| 核对 SDK 配置、依赖和资源来源 | [SDK 说明](third_party/tirtc/README.md)、[依赖锁](dependencies.lock)、[第三方清单](THIRD_PARTY.md) |

许可证见 [LICENSE](LICENSE)，第三方组件和资源说明见 [THIRD_PARTY.md](THIRD_PARTY.md)。
