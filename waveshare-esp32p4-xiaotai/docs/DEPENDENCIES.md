# 版本与依赖

本页用于核对编译环境、配置和随附资源。首次使用见[开发指南](GETTING_STARTED_CN.md)，本版功能见[版本记录](../CHANGELOG.md)。

## 版本要求

| 项目 | 版本或位置 |
| --- | --- |
| 应用 | `1.6.0`，仅适用于 rev3.2 新板；项目名 `xiaotai_esp32p4`，定义在 [CMakeLists.txt](../CMakeLists.txt) |
| 开发板 | Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5，16MB Flash |
| 芯片修订 | 本版用于 rev3.2 新板，详见[版本说明](P4_REV3_VALIDATION.md) |
| 开发环境 | ESP-IDF 5.5.5，riscv32-esp-elf 14.2.0_20260121 |
| TiRTC SDK | 2.5.0 P4 包，库内标识 `v2.5.0-9088239c`，详情见 [SDK VERSION](../components/tirtc_sdk/VERSION.md) |
| Wi-Fi | C6 + ESP-Hosted 3.0.7 主机组件；从机单独核验，见 [C6 指南](C6_PREPARATION.md) |

SDK 的版本号相同不代表二进制相同。保留附带库和头文件，按 [SHA256SUMS](../components/tirtc_sdk/SHA256SUMS.txt) 核对。Hosted 3.0.7 使用组件管理器安装的官方版本；C6 配套要求见 [C6 准备与恢复](C6_PREPARATION.md)。

应用版本在根 `CMakeLists.txt` 的 `PROJECT_VER` 中维护，启动日志和运行状态页读取生成的应用描述。

## 目录用途

| 路径 | 用途 |
| --- | --- |
| `main/xiaotai_main.c` | 应用启动入口 |
| `components/` | 平台接入、业务、UI、音视频、唤醒、板级和 SDK |
| `components/starter_product/` | 字体、表情、图标和提示音 |
| `components/starter_voice/` | 唤醒代码、特征提取和模型 |
| `tools/` | 构建检查与主机测试 |
| `docs/` | 使用、开发和排障说明 |
| `sdkconfig.defaults` | 全新源码的默认配置 |
| `partitions.csv` | Flash 分区 |
| `dependencies.lock` | 固定依赖版本的可迁移模板 |

工程不读取 S3 或父目录文件。1.6.0 需要独立交付、经 SHA-256 校验的 C6 APP 附件 `c6_app.bin`，放入 `main/assets/` 后再构建；镜像被 Git 忽略，不随源码提交。单独下载源码而没有该附件不能构建，这是明确的外部构建输入。已用拟交付源码文件和单独附件在隔离目录完成 ESP-IDF 5.5.5 从零构建；ESP-IDF 和下载组件仍需按开发指南安装。

`main/` 保留启动入口、配置和仍被使用的板级、摄像头、视频呈现及内存策略。启动源文件由 `main/CMakeLists.txt` 指定，板级源文件由 `components/p4_hardware/CMakeLists.txt` 指定。界面与业务统一维护在 `components/starter_*`，不再保留旧 Monitor 的平行实现。

## 配置与缓存

- `sdkconfig` 是本机有效配置，优先于 defaults。调整后通过 `build/config/sdkconfig.json` 核对。
- `build/` 保存编译缓存和产物；只保留当前环境的一套构建目录。
- `managed_components/` 是组件管理器下载的依赖缓存。
- 构建会将本机路径写入 `build/dependencies.lock`，根目录锁文件保持可迁移。

分发源码时保留代码、配置模板、分区、锁文件、SDK、模型、字体和提示音。无需附带构建缓存、本机配置、编辑器配置或 Git 数据库。

## 唤醒模型与适配

模型来自 `nihaoxiaotai_v9.3_20260915_nhwc_tflite.zip`，模型文件与分类头必须成对替换，不能混用不同批次。当前输入为 INT8 `[1,98,32]`，输出为 INT8 `[1,256]`；NHWC 转换已在模型内部完成。

唤醒阈值固定为 0.95，定义在 `components/starter_voice/src/starter_voice.c` 的 `WAKE_THRESHOLD_MILLI`。调整阈值不需要替换模型；开发控制台的临时覆盖不会改写源码。

| 文件 | SHA-256 |
| --- | --- |
| `components/starter_voice/model/head.h` | `cc0364fa603a43c90d9de0b0fc39587c7c4d3350c3d1537b7018cf3a923d1a1e` |
| `components/starter_voice/model/nihaoxiaotai.tflite` | `1dcfe29a10733ec272854bfbafb8a231f10bf3b0399cf6192cc08b38006fac78` |

当前使用 P4 ANSI FFT。TFLM 锁定官方 v1.4.1 Tag 对应提交；ESP-NN 为 1.4.1。语音链路使用 ESP-SR 2.5.4、与其清单配套的 ESP-DSP 1.8.0 和 ESP-DL 3.3.12。界面使用 LVGL 8.4.0、ESP-LVGL Port 2.9.0；视频组件为 ESP Video 2.5.0、摄像头驱动 2.6.0、ISP 算法 2.4.0。升级依赖前阅读[唤醒组件说明](../components/starter_voice/README.md)。

以上组合已在 Windows / ESP-IDF 5.5.5 下完成源码编译与链接。附带 TiRTC 静态库仍由 5.5.4 构建；5.5.5 的真机音视频与长稳表现需要另行验证。

组件均取自乐鑫公开来源，不需要 Git 账号。ESP-SR、ESP-DL、ESP-NN、ESP Video 与 Wi-Fi Remote 在本工程内固定为公开源码快照；对应 Git 提交依次为 `75dc1efa`、`4f4efd7f`、`d9be5422`、`dbbdcbd5`、`ad549729`。ESP-LVGL Port 2.9.0 来自组件仓库，保留本板 SPI 同步刷屏修正。`dependencies.lock` 保存相对路径，构建目录中的锁文件则由组件管理器写入本机绝对路径。

Windows Git 的忽略匹配会让 ESP-NN 原有的 `*.s` 规则漏掉构建所需的 `.S` 汇编；本工程只移除了该忽略条目，未改 ESP-NN 算法。根目录只排除产品固件目录，保留供应商编解码器组件自带的资源文件。

## 来源与许可

应用来自 GitLab 小钛工程，P4 所需代码和资源已独立保存。供应商原始文件和补丁说明随对应组件保留；唤醒算法来源见 [UPSTREAM](../components/starter_voice/vendor/UPSTREAM.md)。

许可证和资产来源统一见 [THIRD_PARTY.md](../THIRD_PARTY.md)。维护过程中的历史提交、复制清单和测试原始记录另行归档，不作为编译输入。

## 记录一次构建

记录源码 commit 或文件清单、未提交改动、有效配置、SDK/工具链、构建命令及 BIN/ELF 的 SHA-256。上板后再核对启动日志中的版本和 ELF 摘要。

这些记录用于将故障定位到具体固件。验证步骤与结果分类见[测试与排障](TESTING.md)。
