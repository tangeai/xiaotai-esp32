# TiRTC C3 SDK 身份

随 Xmini-C3-V3 0.1.0 体验候选提供的 **TiRTC 2.5.0 项目专用构建**，目标为 ESP32-C3 / ESP-IDF 5.5.5 / RISC-V GCC 14.2.0。它不是通用 SDK 的独立正式版本，不直接用于替换其他板型的库。

| 输入 | SHA-256 |
| --- | --- |
| `lib/libTiRTC.a` | `f95b97601c9ef6d541c1dd039196f272e589fb4139773c09586a77fb08b65ff5` |
| `include/tiRTC.h` | `e08a64f93fa9da042c2b5fe66d1f3b6c481871ec8038f75e131477e155fe6fa3` |
| `include/basedef.h` | `23883b8040838fcda10927523536def667cfe7be7fcd8b6f4a1c9410d3e03c04` |

库、头文件、应用有效配置、本地组件与 `vendor/` 中的适配输入需配套使用。本次公开整理不修改库、头文件、驱动或已测 APP/ELF；详细文件身份及构建来源由 Release 附件的 `release-manifest.json` 记录。

应用使用 PCMA 语音，不提供 Opus 或视频。库文件哈希和构建成功不能代替目标板业务、声学、弱网或长稳验收。第三方使用条件见工程根目录的 [THIRD_PARTY](../../THIRD_PARTY.md)。
