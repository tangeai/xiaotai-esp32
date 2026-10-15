# MZ28 移植指南

本工程面向 MZ28 无屏实时语音硬件。首次使用看[使用说明书](USER_GUIDE.md)，不要套用其他 C3 预录示例或 S3/P4 的 PSRAM、分区和 GPIO 配置。

## 分层与替换

| 位置 | 职责 | 换板处理 |
|---|---|---|
| `main/app_config.h` | GPIO、Wi-Fi 缺省值、服务发现、小程序版本 | 私有参数放被忽略的 `app_config.local.h` |
| `main/app_main.c` | 启动、按键、状态、提示音调度 | 保留非阻塞主循环；设置写入不放按键定时器 |
| `components/vb6824` | UART 协议和音频/语音事件 | 更换模块时在驱动层替换 |
| `components/product_audio` | 实时 Opus 上行、下行解码/PCM 播放、本地提示音 | 按模块格式对齐，不在 SDK 回调里同步播放 |
| `components/status_led` | 七颗 RGB 灯 | 按实际引脚、数量及颜色顺序修改 |
| `components/wifi_provisioning` | SoftAP、DNS、网页、异步扫描、凭据验证 | 与正常网络启动互斥，获 IP 后交主任务保存 |
| `components/demo_platform` | Wi-Fi/SNTP、NVS、绑定、鉴权、HTTP/MQTT | 已有网络管理器时避免重复初始化资源 |
| `components/wechat_voip` | 联系人、主被叫、拒接/取消/挂断 | 上层投递命令，控制任务唯一持有会话 |
| `components/rtc_adapter` | SDK 生命周期、永久回调、代次和参数所有权 | 与硬件驱动分离 |
| `components/tirtc_sdk` | TiRTC 2.5.0 库、头文件和 ABI 检查 | 成套使用，禁止混用架构 |

## 硬件基线

| 项目 | 当前值 |
|---|---|
| 主控 / Flash | ESP32-C3 / 8 MB，无 PSRAM |
| 音频芯片 | VB6824，通过 UART1 连接 |
| UART TX / RX | C3 GPIO20 / GPIO10，接模块 RX / TX |
| 波特率 | 2,000,000 bit/s |
| 模块复位 GPIO | `-1`，原理图未提供 C3 可控复位线 |
| BOOT | GPIO9，低电平按下 |
| RGB | GPIO8，七颗 WS2812 兼容灯 |
| 上行 | 模块原始 Opus，16 kHz / 单声道 / 20 ms，当前每包 40 B |
| 下行 | 接收 Opus，C3 解码 PCM16 / 16 kHz / 单声道，按 10 ms 块送回模块 |

更换模块固件时重新验证格式、UART 封装和音量命令，不能只改引脚便视为完成移植。

## 配置与构建

环境固定 ESP-IDF 5.5.5、1000 Hz tick，关闭 FreeRTOS trace/stats，`sizeof(StaticSemaphore_t)=84`。保留 `sdk_contract.c` 断言；不匹配时更换匹配库，不能删断言绕过。

默认 `WECHAT_VERSION_RELEASE=0`；1 为开发版、2 为体验版。应用、驱动和 SDK 配置留在各自责任层。

```c
/* main/app_config.local.h: private, excluded from packages */
#pragma once
#define APP_WIFI_SSID "YOUR_SSID"
#define APP_WIFI_PASSWORD "YOUR_PASSWORD"
```

手机配网不需要此文件。四连击写入强制配网标志，下次启动忽略编译期 Wi-Fi 宏，避免固定网络阻止重新配网。

标准构建见[开发指南](GETTING_STARTED_CN.md)。当前 NVS 为 `0x9000/0x6000`、PHY 为 `0xf000/0x1000`、factory 为 `0x10000/0x5f0000`。更改分区前规划迁移；应用 BIN 写 `0x10000`，不可当完整镜像写 `0x0`。

## 生命周期契约

1. 无 Wi-Fi/强制配网走 SoftAP，其余走正常平台联网与 VoIP；一次启动不并行初始化两套 netif/事件循环。
2. 取得 IP 并同步时间后进行服务发现、绑定鉴权与 MQTT。ID/key 由绑定下发，不写死、不打印。
3. 联系人来自服务端，不落 NVS。`voip_demo` 分别保存 Wi-Fi、完整身份和强制配网标志。
4. 按键或上层只调用 `wechat_voip_command()`；成功仅代表入队，不代表接通。
5. 外呼先占用主叫方向，收到对应 `call_incoming` 即提交 WHIP；连接成功且收到 `0x2000` 后才开媒体。`0x2001` 结束连接上的通话。
6. peer/token 成对原样复制持有，不截断、不跨代复用。来电无连接拒接与已连接挂断分开处理。
7. SDK 回调只复制借用数据并投递事件，不做 HTTP、NVS、阻塞播放或生命周期重入。旧代次不得修改新会话。
8. 退出先关媒体入口，再清队列与连接。普通挂断不重复 Stop/Start SDK；完整停止按 STOPPED 再 Uninit。

本工程没有动态卸载整套应用的接口，不能直接删除任务代替退出。长按 7 秒只清本机身份，云端解绑仍由小程序负责。

## 内存与验收

C3 无 PSRAM。平台与 VoIP 控制栈分别为 8 KiB、16 KiB，不按空闲截图盲目缩栈。模块直接输出上行 Opus，C3 不二次编码；下行复用解码队列。提示音驻留 Flash，不整段复制为常驻 RAM。

`sample_audio.c` 的非 ESP 分支仅供历史预录媒体主机测试；MZ28 固件实际调用 `product_audio`，不链接预录上行或诊断录音缓冲。

保留证书校验及 SDK 的 TLS 配置契约。`sdkconfig.defaults` 不会完整覆盖已有配置，验证时记录有效配置、生成头、依赖锁、SDK 与固件哈希。

按[设备验收标准](TESTING.md#设备验收标准)覆盖配网、错密码、绑定、主被叫、双方取消/挂断、20 轮呼叫、30 分钟语音和网络恢复。主机、构建、烧录、信令、听感、长稳分别取证，不复用旧镜像结果。
