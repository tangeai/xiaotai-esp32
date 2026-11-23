<h1 align="center">小钛 XiaoTai</h1>

<p align="center">面向 ESP32 开发板的开源 AI 语音与音视频固件</p>

<p align="center">
  <a href="docs/assets/home/xiaotai-overview.gif"><img src="docs/assets/home/xiaotai-overview.gif" alt="小钛功能概览：AI 聊天、微信通话、设备对讲与网页实时查看" width="750"></a>
</p>

<p align="center">
  <a href="docs/使用说明.md">使用说明</a> ·
  <a href="docs/快速开始.md">快速开始</a> ·
  <a href="#支持的开发板">支持的开发板</a> ·
  <a href="#开发者入口">开发者入口</a> ·
  <a href="docs/开发指南.md">开发指南</a>
</p>

把小钛烧进一块 ESP32 开发板，它就能：

- **和 AI 语音聊天**：说唤醒词或点一下表情就开始，随时可以打断；
- **和另一台设备对讲**：在通讯录里点一下就能呼叫；
- **打微信电话**：手机微信和设备双向通话；
- **多人对讲**：几台设备进同一个房间，按住说话；
- **网页实时查看**：带摄像头的板子可以在手机上看画面、双向说话。

同一套源码支持多块开发板，每块板按自己的硬件选择功能，编译出各自的固件。

## 支持的开发板

| 开发板 | 芯片 | 屏幕 | 功能 |
| --- | --- | --- | --- |
| 立创实战派 S3 | ESP32-S3 | 320×240 触摸屏 | AI 聊天、AI 看图、设备对讲、微信通话、多人对讲、网页音视频 |
| M5Stack CoreS3-Lite | ESP32-S3 | 320×240 触摸屏 | AI 聊天、设备对讲、微信通话、多人对讲、网页音视频 |
| 微雪 ESP32-P4 3.5 寸（rev3） | ESP32-P4 + C6 | 480×320 触摸屏 | AI 聊天、设备对讲、微信通话、多人对讲、网页音视频 |
| WT99P461-S1 | ESP32-P4 + C61 | 1024×600 门禁屏 | 可视门铃、AI 聊天、通话、网页音视频 |

“S3”“P4”只是芯片名，同一种芯片的不同板子不能混刷固件，请按完整型号选择。

<p align="center">
  <a href="docs/assets/home/lckfb-s3-device-front.webp"><img src="docs/assets/home/lckfb-s3-device-front.webp" alt="立创实战派 S3 运行小钛" width="48%"></a>
  <a href="docs/assets/home/waveshare-p4-device-front.webp"><img src="docs/assets/home/waveshare-p4-device-front.webp" alt="微雪 P4 运行小钛" width="45%"></a>
</p>

## 5 分钟体验

不需要安装开发环境：

1. 在 [Releases](https://github.com/tangeai/xiaotai-esp32/releases) 下载对应板型的完整固件；
2. 用浏览器烧录工具写入开发板；
3. 按屏幕上的三步，用手机给设备配好 Wi-Fi；
4. 在小钛小程序里绑定设备；
5. 说“你好小钛”，或在通讯录里呼叫另一台设备。

烧录和配网的细节见 **[快速开始](docs/快速开始.md)**，日常怎么用见 **[使用说明](docs/使用说明.md)**。

在线功能需要联网、绑定设备，并使用小钛的云端服务。本仓库只包含设备端程序，不包含云端服务和小程序。

## 开发者入口

### 编译一块板

用官方 ESP-IDF 编译，默认 6.0.3，也可以选 5.5.5。TiRTC 通话库不放在仓库里，第一次编译时会自动从 TiRTC 官网下载正式版 2.6.3 并核对哈希。

```bash
python scripts/build.py --list-boards
python scripts/build.py lckfb/szpi-esp32s3 --dry-run
python scripts/build.py lckfb/szpi-esp32s3              # ESP-IDF 6.0.3
python scripts/build.py lckfb/szpi-esp32s3 --idf 5.5    # ESP-IDF 5.5.5
```

产物在 `build/<产品>/sdk-firmware/`（5.5 线在 `sdk-firmware-idf5.5/`）。环境准备、烧录和产物核对见[开发指南](docs/开发指南.md#编译与烧录)。

### 文档

| 你想做什么 | 看这里 |
| --- | --- |
| 作为用户使用设备 | [使用说明](docs/使用说明.md) |
| 烧录固件、配网、绑定 | [快速开始](docs/快速开始.md) |
| 了解代码怎么分层、从哪里改 | [开发指南](docs/开发指南.md) · [SDK 架构](docs/SDK架构.md) |
| 选产品功能、改参数 | [产品选项](docs/产品选项.md) |
| 接入自己的开发板 | [新板接入](docs/新板接入.md) |
| 跑测试、在电脑上预览界面、定位问题 | [测试与诊断](docs/测试与诊断.md) |
| 用串口查状态、发命令 | [AT 指令](docs/AT指令.md) |
| 查功能接口 | [通话与房间](docs/功能/通话与房间.md) · [配网与绑定](docs/功能/配网与绑定.md) · [声音识别](docs/功能/声音识别.md) · [闹钟](docs/功能/闹钟.md) · [门禁](docs/功能/门禁.md) |
| 从 v0.2 升级 | [迁移说明](docs/迁移说明.md) |

### 给 AI 编程助手用的技能

仓库自带几个 Claude Code 技能，放在 `.claude/skills/` 下。在仓库里直接说出你的需求，助手会按这些技能里的步骤工作：

| 技能 | 用途 |
| --- | --- |
| `xiaotai-build` | 选板和 ESP-IDF 版本、编译、核对产物、烧录、确认版本 |
| `xiaotai-new-board` | 写板包和产品声明，把一块新板从拿到资料接到业务跑通 |
| `xiaotai-bringup` | 一款产品第一次在真板上跑：十项基础功能逐项过 |
| `xiaotai-test` | 按改动选测试：分层检查、主机测试、界面预览、构建核验、真机专项 |
| `xiaotai-ui` | 改界面：在电脑上出图、按设计规范改主题和页面、核对四款彩屏 |
| `xiaotai-verify` | 开不了机、黑屏、通话异常、内存不够，判断出在哪一层 |
| `xiaotai-echo` | 回声消除：单讲测量回声压低了多少、剩下多少，按削顶、失真、对齐逐项调 |
| `xiaotai-refactor` | 在分层里改结构或加能力，差异按能力选择，并证明行为不变 |
| `xiaotai-feature` | 加功能或应用（闹钟、表盘、门禁、门铃）：先对到已有模块，再按清单约定写组件、选进产品、写测试 |
| `xiaotai-port` | 移植到新的 ESP-IDF 版本或新芯片：先用盘点脚本列出要改的地方，再做版本登记、TiRTC 包和契约、档案和依赖锁、统一编译和代表板验证 |

### 目录

```text
boards/             板包：引脚、外设、能力和分区
products/           产品：板包、应用、模块、变体、参数
apps/               伴侣应用、门铃应用和语音终端
sdk/                框架、端口、零件、运行引擎、业务模块和界面主题
components/         TiRTC 构建契约与跨模块公共组件（字库等）
vendor/             随源码分发的乐鑫组件
partitions/         共享分区表
scripts/            编译、下载 TiRTC、打包和检查脚本
tests/              主机测试与契约检查
tools/              多设备自动化测试工具（xt）和界面预览
```

## 安全提示

终端服务发现和 HTTP 接口使用明文 HTTP，MQTT 按服务端下发的地址决定是否启用 TLS。请在可信网络中使用；不要公开配网密码、绑定码、令牌或未脱敏的原始日志。

## 许可

小钛以 [MIT](LICENSE) 许可开源。第三方组件、字体与模型的来源和许可见 [THIRD_PARTY.md](THIRD_PARTY.md)，配图来源见 [SOURCES](docs/assets/home/SOURCES.md)。
