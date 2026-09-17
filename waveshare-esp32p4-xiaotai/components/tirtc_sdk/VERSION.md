# TiRTC SDK 2.5.0

| 项目 | 内容 |
| --- | --- |
| 目标 | ESP32-P4 / ESP-IDF 5.5.4 |
| 工具链 | riscv32-esp-elf-gcc 14.2.0 / esp-14.2.0_20260121 |
| 输入包 | tirtc__espressif_esp32p4__esp-idf5.5.4-riscv32-esp14.2.0_20260121__v2.5.0__mini.tgz |
| 库内版本标识 | v2.5.0-9088239c |
| libTiRTC.a SHA-256 | `9b4e35c7a2cb203fc463417739199429057c8b036936faa11f844c726455301f` |
| 替换日期 | 2026-09-16 |

库和 `include/tirtc/` 下的五个头文件来自同一输入包，未修改、重编译或 strip。旧的本地 2.3.0 补丁未额外叠加。根层 `include/tiRTC.h`、`include/basedef.h` 仅转发到包内头文件，保持应用 include 路径不变；原有应用辅助头文件保留。

包内构建契约见 `manifest/build-contract.env`：1000 Hz、trace/stats 关闭、`StaticSemaphore_t` 为 84 B。包声明 `CONFIG_LWIP_MAX_SOCKETS=10`，应用当前保持 16，未为替换 SDK 修改应用配置；此差异需要在联网回归中关注。

静态检查确认库包含 TGTRP sender/receiver 及 `TiRtcConnSetVideoBitrateParams` 符号；最终 transport 以实际连接协商和运行结果为准。随包 README 原样保留，不将其中 KCP/noSCTP 的打包描述当作每条连接的运行结论。

2026-09-17 已完成文件哈希、架构/符号核对，Windows ESP-IDF 5.5.4 APP 构建及传输诊断主机检查通过。APP 保持 1.2.0，发送缓存上限为 2 MiB（2,097,152 bytes）；临时 2.3.0 / 16 KiB / 预发域名实验不属于当前配置。构建环境的组件版本检查规避见 [构建排障](../../docs/TESTING.md#组件版本检查误报)。文件校验见 `SHA256SUMS.txt`。

本次收口未烧录或进行音视频、TLS、断连重连、弱网及长稳真机回归。默认 SDK endpoint 保持 `http://ep-tirtc.tange365.com`；旧包的诊断、调度补丁和证书校验结论不自动适用于此包，构建通过也不证明上述 socket 配置差异已通过运行验证。
