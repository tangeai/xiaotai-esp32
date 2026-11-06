# 多芯片适配指南

从这里开始，先选实际板型，再选构建环境。**同一套源码不等于同一份固件**：板型、分区、SDK、摄像头、显示和联网协处理器仍有各自的约束。

- 已有板卡，只想按原流程使用或维护独立工程：看 [ESP32 main 使用引导](https://github.com/tangeai/xiaotai-esp32/tree/main)。
- 要在统一工程里维护多块板，或接入新板：留在 `codex/multi-chip-v2`。分支名区分架构路线，不代表已发布固件 `2.0.0`。

## 选板

| 板型 | 芯片 | 构建参数 `<board>` | 当前固定 IDF 环境 |
| --- | --- | --- | --- |
| 立创实战派 | ESP32-S3 | `lckfb/szpi-esp32s3` | 6.0.3 |
| M5Stack CoreS3-Lite | ESP32-S3 | `m5stack/core-s3` | 6.0.2 |
| 微雪 ESP32-P4-WIFI6-Touch-LCD-3.5 | ESP32-P4 | `waveshare/esp32-p4-wifi6-touch-lcd-3.5` | 6.0.2 |
| WT99P461-S1 | ESP32-P4 | `wt99p461-s1` | 6.0.2 |
| Xmini-C3-V3 | ESP32-C3 | `xmini/c3-v3` | 6.0.2 |
| MZ28-AI-PPD | ESP32-C3 | `mz28-ai-ppd` | 6.0.2 |

表中是已登记的板型和构建入口，不是所有同芯片板卡的兼容承诺。固件分别保留屏幕或无屏交互，以及该板的媒体和联网能力。

## 构建

在本分支根目录查看板型及配置。以立创 S3 为例：

```sh
python scripts/build.py --list-boards
python scripts/build.py lckfb/szpi-esp32s3 --dry-run
```

确认输出中的 `board`、`idf_environment`、`sdk_directory` 和 `build_dir`，按[架构开发指南](架构开发指南.md)准备对应工具链和依赖，再移除 `--dry-run` 执行构建：

```sh
python scripts/build.py lckfb/szpi-esp32s3
```

其他板型换成表中的参数。不要复制旧分支的 `sdkconfig`、SDK 库或构建缓存。配置变更时按工具提示使用 `--fresh-config`；`--stage sdk-check` 只做 SDK 链接探针，不产出可交付的整机固件。构建命令不会烧录或发布。

**微雪 P4 的额外构建输入：**整机构建需要配套 C6 APP。源码分支不包含此固件，需先向维护者取得同版文件，核对大小为 **1,111,904 字节**、SHA-256 为 `8700a7215c77ffc9810ab27dbf7687d6222c620c13d7814dc502ace6a784b4f5`，再放到 `main/xiaotai/application/p4/startup/assets/c6_app.bin`。缺少或校验不符时整机构建会停止；这不是 C6 整片镜像，不可从 `0x0` 写入。

## 接入新板

1. 选择接近的产品配置，核对芯片、Flash/PSRAM、媒体格式和交互方式，再按[兼容性契约](新板适配与兼容性契约.md)补齐板级能力。
2. 板级差异留在 `main/boards/` 和对应适配层。只有跨板确实需要的行为才进入公共接口，保持原有业务、媒体和协议契约。现阶段仍有运行时代码与板型绑定，不能只换板名就复用。
3. 先查结构和配置，再逐板编译；真机另验启动、联网、TiRTC 会话、该板的媒体与交互。公共接口变更需验证实际消费者，不把一块板的结果当成所有板通过。

## 验证范围

此分支以 [V0.1.0 开发基线](releases/v0.1.0.md)为起点。已有的构建、SDK 探针和真机证据见[验证状态](VALIDATION.md)，按对应版本和板型解读。当前未完成六板全功能真机验收；新增板型、随意裁剪功能以及跨板固件互换不属于当前承诺。
