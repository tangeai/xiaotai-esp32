# TiRTC 2.5.0 / ESP32-P4 本地特调批次（https1 + feedback-stack）

本地批次：`20260919-p4-feedback-stack1`。在原厂 2.5.0 P4 基线上叠加 HTTPS 定向修改与 TGTRP 反馈栈修复，自 `esp32p4_wt9932p4c61_tiny_device_app` 特调批次移植。运行时 BuildInfo 仍显示 `v2.5.0-9088239c-https1`，以批次号、补丁和 SHA-256 区分，不冒充原厂新版本。

| 项目 | 内容 |
| --- | --- |
| 平台 | ESP32-P4 / ESP-IDF 5.5.4 |
| 工具链 | riscv32-esp-elf-gcc 14.2.0 / esp-14.2.0_20260121 |
| FreeRTOS | 1000 Hz，trace/stats 关闭，StaticSemaphore_t=84 B |
| SDK 版本 | 2.5.0，BuildInfo `v2.5.0-9088239c-https1` |
| 本地批次 | `20260919-p4-feedback-stack1` |
| 原厂基线库 SHA-256 | `9b4e35c7a2cb203fc463417739199429057c8b036936faa11f844c726455301f` |
| 上一批次库 SHA-256 | `6265a3516035cf9921076da0dd637f83f0a9f0e2b4ef8202b20d6100e416ec25` |
| 当前 libTiRTC.a SHA-256 | `7cf1569eb936280476410b2739f859b90a931c52851c790fdc926f705bb84f11` |
| libTiRTC.a | 2,046,176 bytes |
| 替换日期 | 2026-09-21 |

## 移植来源

本库与补丁自 `esp32p4_wt9932p4c61_tiny_device_app`（启明）特调批次移植，与本仓原厂 2.5.0 基线同源 `v2.5.0-9088239c`。原厂基线库 `9b4e35c7…` 即本仓替换前的 `libTiRTC.a`，与启明 `build-contract.env` 记录的 `VENDOR_LIB_SHA256` 一致。

| 来源 | 完整提交 |
| --- | --- |
| Nano 2.5.0 | `9088239cc654ec863869d4adc5433ddec3572fad` |
| TGWebRTC v1.5.18 | `f72f5d3ce04c2be369d88f499c7720d0ed7d788f` |
| TgSysAdpt headers | `983a086f2bf472f89655f1d62b228ab4d5241635` |

只带入编译好的静态库与补丁源、审计证据；未获得 Nano/TGWebRTC 完整源码，无法在本仓重编或拆分单个补丁。头文件内容与原厂基线一致（本仓保留扁平 `include/*` 转发到 `include/tirtc/*` 的布局），CMake 与 SHA-256 门禁已同步到 `7cf1569e…`。

## 补丁

- **feedback-stack**（`manifest/feedback-stack.patch`，SHA-256 `0b45559f…`）：只改 `tgtrp_sender.c`，将 ACK/NACK 解码移入禁止内联的私有函数，使 BWE 同步回调路径固定栈从 5312 B 降到 288 B。修 P4 在 IPC 上行 + 100 ms/5% 丢包下 `rtc_thread` 栈保护异常。无 API、日志、重传、jitter、线程栈大小变化，无新增堆/常驻内存。全量归档 107 个成员中仅 `tgtrp_sender.o` 变化。
- **https**（`manifest/https.patch`）：改 `httpclt.c`，对 ESP32-S3/P4 启用 `MBEDTLS_SSL_VERIFY_REQUIRED` + `esp_crt_bundle_attach` 证书 bundle 校验、TLS hostname 校验及握手失败时的 socket 归属/关闭修复。

## 接入前提与影响

- 运行期需 `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y`（本仓 sdkconfig / sdkconfig.defaults 已开）；新库引用 `esp_crt_bundle_attach`，由 `mbedtls` 组件提供，CMake REQUIRES 未变。
- 启明特调默认端点为 `https://ep-tirtc.tange365.com`（verify-required）。本仓 main 当前未显式设置 TiRTC service_endpoint，改用 HTTPS 端点属行为变更，需真机联网复测。
- `CONFIG_LWIP_MAX_SOCKETS`：本库按 16 编译（原厂 mini 包声明 10），本仓应用当前保持 16。

## 验证边界

补丁的目标库构建、对象审计、Release 主机回归与 ASan/UBSan 已在来源工程通过（见 `BUILD_EVIDENCE.md`）。本仓尚未用新库重新链接 APP、未构建、未烧录，也未做 100 ms/5% 丢包、IPC 上行、码率反馈及 HTTPS 端点的真机复测。文件校验见 `SHA256SUMS.txt`。
