# 版本与依赖

本页用于核对编译环境、配置和随附资源。首次使用见[开发指南](GETTING_STARTED_CN.md)，本版功能见[版本记录](../RELEASE_NOTES.md)。

## 版本要求

| 项目 | 版本或位置 |
| --- | --- |
| 应用 | `1.2.0`，项目名 `xiaotai_esp32p4`，定义在 [CMakeLists.txt](../CMakeLists.txt) |
| 开发板 | Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5，16MB Flash |
| 芯片修订 | 默认镜像接受 P4 rev 1.0–1.99；其他修订需另行适配 |
| 开发环境 | ESP-IDF 5.5.4，riscv32-esp-elf 14.2.0_20260121 |
| TiRTC SDK | 2.3.0 P4 补丁库，详情见 [SDK VERSION](../components/tirtc_sdk/VERSION.md) |
| Wi-Fi | C6 + ESP-Hosted 1.4.7 主机补丁组件；从机单独核验，见 [C6 指南](C6_PREPARATION.md) |

SDK 的版本号相同不代表二进制相同。保留附带库和头文件，按 [SHA256SUMS](../components/tirtc_sdk/SHA256SUMS.txt) 核对；Hosted 修改见 [LOCAL_CHANGES](../components/espressif__esp_hosted/LOCAL_CHANGES.md)。

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

工程可独立构建，不读取 S3 或父目录文件。ESP-IDF 和下载组件按开发指南安装。

`main/` 保留启动入口、配置和仍被使用的板级、摄像头、视频呈现及内存策略。启动源文件由 `main/CMakeLists.txt` 指定，板级源文件由 `components/p4_hardware/CMakeLists.txt` 指定。界面与业务统一维护在 `components/starter_*`，不再保留旧 Monitor 的平行实现。

## 配置与缓存

- `sdkconfig` 是本机有效配置，优先于 defaults。调整后通过 `build/config/sdkconfig.json` 核对。
- `build/` 保存编译缓存和产物；只保留当前环境的一套构建目录。
- `managed_components/` 是组件管理器下载的依赖缓存。
- 构建会将本机路径写入 `build/dependencies.lock`，根目录锁文件保持可迁移。

分发源码时保留代码、配置模板、分区、锁文件、SDK、模型、字体和提示音。无需附带构建缓存、本机配置、编辑器配置或 Git 数据库。

## 唤醒模型与适配

模型来自 `nihaoxiaotai_v9.3_20260915_nhwc_tflite.zip`，模型文件与分类头必须成对替换，不能混用不同批次。当前输入为 INT8 `[1,98,32]`，输出为 INT8 `[1,256]`；NHWC 转换已在模型内部完成。

默认唤醒阈值为 0.8，对应 `CONFIG_XIAOTAI_WAKE_THRESHOLD_MILLI=800`。已有本机配置优先于默认值；调整阈值不需要替换模型。

| 文件 | SHA-256 |
| --- | --- |
| `components/starter_voice/model/head.h` | `cc0364fa603a43c90d9de0b0fc39587c7c4d3350c3d1537b7018cf3a923d1a1e` |
| `components/starter_voice/model/nihaoxiaotai.tflite` | `1dcfe29a10733ec272854bfbafb8a231f10bf3b0399cf6192cc08b38006fac78` |

当前使用 P4 ANSI FFT。TFLM 1.3.5 的卷积通道修正由 [conv_channels.cmake](../components/starter_voice/conv_channels.cmake)生成到 `build/p4_tflm/conv.cc`，不修改下载缓存。升级依赖前阅读[唤醒组件说明](../components/starter_voice/README.md)。

## 来源与许可

应用来自 GitLab 小钛工程，P4 所需代码和资源已独立保存。供应商原始文件和补丁说明随对应组件保留；唤醒算法来源见 [UPSTREAM](../components/starter_voice/vendor/UPSTREAM.md)。

许可证和资产来源统一见 [THIRD_PARTY.md](../THIRD_PARTY.md)。维护过程中的历史提交、复制清单和测试原始记录另行归档，不作为编译输入。

## 记录一次构建

记录源码 commit 或文件清单、未提交改动、有效配置、SDK/工具链、构建命令及 BIN/ELF 的 SHA-256。上板后再核对启动日志中的版本和 ELF 摘要。

这些记录用于将故障定位到具体固件。验证步骤与结果分类见[测试与排障](TESTING.md)。
