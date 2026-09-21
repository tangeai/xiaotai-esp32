# TiRTC SDK 2.5.0

| 项目 | 内容 |
| --- | --- |
| 目标 | ESP32-S3 / ESP-IDF 5.5.4 |
| 工具链 | xtensa-esp32s3-elf-gcc 14.2.0 / esp-14.2.0_20260121 |
| 输入包 | tirtc__espressif_esp32s3__esp-idf5.5.4-xtensa-esp14.2.0_20260121__v2.5.0__mini.tgz |
| 库内版本标识 | v2.5.0-9088239c |
| libTiRTC.a SHA-256 | `7334e846ed4261b5297607c85acafe1d586a22374f7c8e37b102b2742e47c7aa` |
| 替换日期 | 2026-09-16 |

库和 `include/tirtc/` 下的五个头文件来自同一输入包，未修改、重编译或 strip。包内构建契约见 `manifest/build-contract.env`：1000 Hz、trace/stats 关闭、`StaticSemaphore_t` 为 84 B。

静态检查确认库包含 TGTRP sender/receiver 及 `TiRtcConnSetVideoBitrateParams` 符号；最终 transport 以实际连接协商和运行结果为准。随包 README 原样保留，不将其中 KCP/noSCTP 的打包描述当作每条连接的运行结论。

本次保留应用现有线程栈 wrapper、CMake 和 sdkconfig，只更新 SDK 文件及版本校验记录。未构建 APP、烧录或验证音视频、TLS、断连重连。文件校验见 `SHA256SUMS.txt`。
