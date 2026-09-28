# P4 rev3.2 版本说明

1.6.0 芯片要求为 P4 rev3.2 及以上。版本改动见[版本记录](../CHANGELOG.md)，构建和验证范围见同版 Release 的 `release-manifest.json`。

## 烧录前

确认设备的 P4 芯片修订为 rev3.2。使用 ESP-IDF 5.5.5 和支持 rev3.2 的 esptool 4.12.0 或更新版本。

## 软件配置与构建

先按[开发指南](GETTING_STARTED_CN.md#2-编译)准备 C6 附件，再运行 `idf.py -B build build`。不要复制其他工程的 `sdkconfig` 或 `build/`。有效配置应满足：`ESP32P4_SELECTS_REV_LESS_V3=n`、最低修订 301、最高修订 399、CPU 400 MHz、`HAL_WDT_USE_ROM_IMPL=n`。CMake 会拒绝错误的有效配置；产品支持范围仍从 rev3.2 起。

`sdkconfig.defaults` 保留 200 MHz PSRAM 配置。构建通过不能代替设备端功能验证。

最终 CMake 配置应报告 `P4 H.264 register map: hw_ver3`。组件发现的首轮可能提示修订尚不可用，不能据此判断最终镜像选用了旧版寄存器头文件。`build/compile_commands.json` 中 `esp_h264_enc_single_hw.c` 的包含目录也应为 `hw_ver3`。

## 真机顺序

1. 在 rev3.2 新板上烧录，核对启动日志中的芯片修订、应用版本、联网和界面状态。
2. 分别验证 AI、设备互呼、微信、H5、多人对讲；弱网和长时间运行单独记录。
