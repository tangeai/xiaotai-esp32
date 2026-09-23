# 小钛 P4

小钛 P4 是微雪 ESP32-P4-WIFI6-Touch-LCD-3.5 上的音视频设备应用，版本为 **1.5.0**。通过 TiRTC/WebRTC 支持 AI 语音对讲、设备间语音/视频呼叫、微信通话、多人对讲和 H5 实时查看，包含热点配网、设备绑定和联系人管理。

## 准备什么

| 项目 | 要求 |
| --- | --- |
| 开发板 | 微雪 ESP32-P4-WIFI6-Touch-LCD-3.5，16MB Flash，配套屏幕、触摸、ES8311 音频和 OV5647 摄像头 |
| 芯片要求 | ESP32-P4 rev3.2 及以上 |
| 联网 | 板载 C6 运行兼容的 ESP-Hosted SDIO 固件；能正常联网时无需重刷 C6 |
| 编译环境 | ESP-IDF 5.5.4 及配套 RISC-V 工具链 |
| 体验条件 | 可联网的 Wi-Fi、平台账号和手机；设备互呼需要第二台已绑定设备 |

本工程包含所需应用代码和资源，可独立构建。首次编译需联网下载组件依赖。

## 开始使用

1. 下载 [1.5.0 完整 16 MB 烧录包](https://github.com/tangeai/xiaotai-esp32/releases/download/esp32-p4-app-v1.5.0/xiaotai-esp32-p4-app-v1.5.0-full-16MB.bin)，按[网页烧录说明](../README.md#用浏览器烧录)从 `0x0` 写入，无需自行编译。完整包会清除配网、绑定和用户设置，请先备份；校验清单见[发布页](https://github.com/tangeai/xiaotai-esp32/releases/tag/esp32-p4-app-v1.5.0)。
2. 手机连接设备显示的 `XiaoTai-XXXX` 开放热点，访问 `http://192.168.6.1`，选择扫描到的 Wi-Fi 并输入密码；已保存网络可直接复用密码。
3. 设备联网后显示六位验证码。在[平台“我的设备”](https://xiaotai.chat/devices)中添加设备并填写验证码。
4. 完成绑定后，按[功能体验](docs/GETTING_STARTED_CN.md#功能体验)配置 AI、联系人或微信授权，开始通话。

## 使用范围

- AI 对讲使用语音，支持点击表情或说“你好小钛”唤醒、字幕和云端表情事件。
- 设备通话支持语音和视频；微信、H5 的媒体格式见[视频规格](docs/P4_MEDIA_ARCHITECTURE.md#视频)。
- 多人对讲从“三点菜单 → 多人对讲”进入，支持创建或加入房间、查看在线成员、按下讲话；操作见[多人对讲](docs/GETTING_STARTED_CN.md#多人对讲)。
- 音频使用一个物理麦克风和一路扬声器回采参考，接入回声消除、自动增益和自适应播放。
- 未联网时进入配网；手动断开后等待重新配网，被动掉线时重连已保存网络。

AI 角色、设备联系人和微信授权由配套平台配置。当前应用不提供 OTA 更新或 USB 麦克风功能。

## 开发文档

| 要做什么 | 阅读哪里 |
| --- | --- |
| 编译、烧录、配网绑定和体验功能 | [开发指南](docs/GETTING_STARTED_CN.md) |
| 找到负责某项功能的代码 | [系统与媒体架构](docs/P4_MEDIA_ARCHITECTURE.md) |
| 排查故障、选择测试和记录结果 | [测试与排障](docs/TESTING.md) |
| 核对版本、SDK、模型和配置 | [版本与依赖](docs/DEPENDENCIES.md) |
| 查看本版及历史改动 | [版本记录](CHANGELOG.md) |
| 恢复 C6 或采集视频诊断数据 | [专项资料](docs/README.md) |

许可证见 [LICENSE](LICENSE)，第三方组件和资源说明见 [THIRD_PARTY.md](THIRD_PARTY.md)。
