# P4 构建配置

芯片要求：**ESP32-P4 rev3.2 及以上**。直接体验见[烧录入口](../README.md#开始使用)，本页供编译和排障时核对配置。

## 烧录前

使用 ESP-IDF 5.5.4 及配套 esptool。烧录工具提示芯片不兼容时，停止操作，不要强制写入。

## 软件配置与构建

全新工作树运行 `idf.py -B build build`，由 `sdkconfig.defaults` 生成配置，不复用其他工程的缓存。有效配置为 `ESP32P4_SELECTS_REV_LESS_V3=n`、镜像修订字段 301–399、CPU 400 MHz、`HAL_WDT_USE_ROM_IMPL=n`。CMake 会检查这些值。

`sdkconfig.defaults` 保留 200 MHz PSRAM 配置。构建通过不能代替设备端功能验证。

最终 CMake 配置应报告 `P4 H.264 register map: hw_ver3`。组件发现的首轮可能提示修订尚不可用，不能据此判断最终镜像选用了旧版寄存器头文件。`build/compile_commands.json` 中 `esp_h264_enc_single_hw.c` 的包含目录也应为 `hw_ver3`。

## 真机顺序

1. 烧录后核对启动日志中的芯片修订、应用版本、联网和界面状态。
2. 分别验证 AI、设备互呼、微信、H5、多人对讲；弱网和长时间运行单独记录。
