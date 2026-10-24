# Xmini-C3-V3 硬件与适配

本工程仅适用 Xmini-C3-V3：ESP32-C3、16 MB Flash、SSD1306 128×64 OLED、ES8311、BOOT 单键。不要按外观或同为 C3 就复用其他板型固件。

## 引脚

| 功能 | GPIO / 参数 |
| --- | --- |
| I2C SDA / SCL | 0 / 1，400 kHz |
| ES8311 | 7 位地址 `0x18`；驱动接口使用 8 位地址 `0x30` |
| OLED | SSD1306，地址 `0x3C`，128×64，X/Y 镜像 |
| I2S MCLK / BCLK / WS | 8 / 7 / 5 |
| ESP 接收 DIN / 发送 DOUT | 6 / 4 |
| 功放使能 | 10，高电平使能，空闲关闭 |
| BOOT | 9，低电平有效 |

引脚与接口参考[固定版小智板级配置](https://github.com/78/xiaozhi-esp32/blob/49b4380b3faa0ceadd28a3e78aaf0f311c46c57e/main/boards/xmini/c3-v3/config.h)及[实现](https://github.com/78/xiaozhi-esp32/blob/49b4380b3faa0ceadd28a3e78aaf0f311c46c57e/main/boards/xmini/c3-v3/xmini_c3_board.cc)。本工程使用自己的应用、媒体和资源，没有迁入上游固件或唤醒模型。

## 音频与交互

PCM 为 16 bit / 8 kHz / 单声道；网络格式为 G.711 A-law，每帧 20 ms、160 字节。保留五类语音业务，不提供视频或 Opus。没有引入 AEC 算法，也不承诺未经声学验收的全双工效果。

OLED 使用 1,024 字节单色帧缓冲。页面支持中文、联系人和字幕滚动；黄蓝交界保留空白。BOOT 单击、双击、长按分别对应选择、确认、返回或结束会话。

## 构建

使用 **ESP-IDF 5.5.5 / RISC-V GCC 14.2.0**。进入本工程目录并加载 ESP-IDF 环境后执行：

```sh
idf.py -B build-xmini -D SDKCONFIG=sdkconfig.xmini build
```

保留随源码提供的 `sdkconfig.xmini`。默认 `sdkconfig` 属于另一构建入口，不能代替当前已核验配置。实际分段偏移取自本次 `flasher_args.json`，不能套用旧的 2 MB 应用分区。

| 输入 | 保留要求 |
| --- | --- |
| TiRTC C3 库及头文件 | 使用本工程附带版本，不混用 MZ28/S3/P4 SDK |
| `esp_codec_dev 1.6.2` | 本地组件副本，保留 [I2S 补丁](../components/esp_codec_dev/LOCAL_PATCH.md) |
| `esp_driver_usb_serial_jtag` | 本地 IDF 5.5.5 组件，保留[补丁与许可](../components/esp_driver_usb_serial_jtag/LOCAL_PATCH.md) |
| `esp_audio_codec 2.4.0` | 通过固定清单与依赖锁取得，遵循其专用许可 |
| mbedTLS 配置与适配 | 保留 `vendor/`、`cmake/` 和当前 MPI/ABI 配置，不独立升级其中一个输入 |

本地 codec 组件由 `components/xmini_audio/idf_component.yml` 的 `override_path` 固定。可迁移依赖锁保留远端组件的版本和哈希；首次配置时组件管理器会补充本机本地组件路径，不改变所选组件内容。

## 代码入口

| 模块 | 负责什么 |
| --- | --- |
| `xmini_board` / `xmini_ui` | I2C、BOOT、OLED、页面及操作意图 |
| `xmini_audio` | I2S、ES8311、PCMA、提示音与录放音 |
| `demo_platform` / `wifi_provisioning` | 联网、鉴权、身份、配网及真实验证码 |
| `xmini_business` / `xmini_rtc` / `xmini_group` | 五类语音业务、媒体连接、会话与房间生命周期 |
| `xmini_settings` | 音量、麦克风增益和语音引导设置保存 |

串口与主机测试入口见 [AT 指令](AT_COMMANDS.md)。主机测试不访问真实平台或替代目标板验收；更换 SDK、驱动、配置或资源后，应重新构建并验证受影响的实际业务，不能继承旧镜像结果。
