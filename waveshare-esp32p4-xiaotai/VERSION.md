# P4 版本与依赖

复现构建时，按下表准备环境。应用和 SDK 各有版本号：一个对应设备应用，另一个对应通信库；库、头文件与 ESP-IDF 配置需要配套使用。

| 项目 | 当前值 |
| --- | --- |
| 应用版本 | 1.0.0 |
| CMake 项目名 | `xiaotai_esp32p4` |
| 版本入口 | [CMakeLists.txt](CMakeLists.txt) 中的 `PROJECT_VER` |
| 开发板 | 微雪 ESP32-P4-WIFI6-Touch-LCD-3.5 |
| 联网 | P4 + C6，ESP-Hosted/SDIO |
| ESP-IDF | 5.5.4 |
| 工具链 | riscv32-esp-elf GCC 14.2.0_20260121 |
| 芯片配置 | 源码选择 P4 rev < 3 路径，可用范围还受依赖配置限制 |
| 1.0.0 完整固件 | 镜像头接受 P4 芯片 rev 1.0–1.99，不适用 rev 2.x / 3.x |
| TiRTC SDK | 2.3.0，含 P4 适配与传输补丁，详见下方组件说明 |
| SDK 大小 | 4,748,802 bytes |
| SDK SHA-256 | `a7a01ffd496a55364c7e4d665ff3884d078147bba96752a965d97befca12e451` |

需要核对通信库时，看[组件版本](components/tirtc_sdk/VERSION.md)和[校验清单](components/tirtc_sdk/SHA256SUMS.txt)；排查联网适配时，看 [Hosted 修改说明](components/espressif__esp_hosted/LOCAL_CHANGES.md)。

构建还需要 [sdkconfig.defaults](sdkconfig.defaults)、[分区表](partitions.csv)、[依赖锁](dependencies.lock) 和同仓 S3 共享实现。两平台的 SDK 与音频配置不能互换。

SDK 支持 HTTPS 服务端认证，应用服务仍按发现配置连接。部署前分别检查平台 API、MQTT 和 RTC 的传输安全。
