# C6 准备与恢复

板载 C6 负责 Wi-Fi，P4 运行应用。体验 1.6.0 时，只需通过 P4 USB 写入[完整 16 MB 包](https://github.com/tangeai/xiaotai-esp32/releases/download/esp32-p4-app-v1.6.0/xiaotai-esp32-p4-app-v1.6.0-full-16MB.bin)。包内已包含 C6 APP `3.0.7-xiaotai.5`：P4 启动后检查兼容性，经 SDIO 更新 C6，重启核验后再联网。**更新期间保持供电，等待进入配网或主页；不要重复刷包或断电。**

目标版本直接跳过。已知 `.2`、`.3`、`.4` 来源按完整身份白名单检查；旧版只返回 `eh_cp_wifi_sta/1` 和 Hosted `3.0.7` 时保留原固件继续联网。其他来源显示恢复提示；无法建立 SDIO 时需按本页的 UART 方法恢复。该功能更新的是 C6 APP，不是 P4 自身的 OTA。

2026-09-27 使用一块未直刷 C6 的出厂板完成了旧 C6 到 `.5` 的迁移：旧版初始化 TLV 为 `12 01 0d 11 01 0d 13 01 00 14 01 14`，旧版 RPC 257 回复 `ESP_ERR_WIFI_NOT_INIT`，不支持版本查询 350 和 APP descriptor 267；P4 仅写入自身 APP 后，重启读到 C6 Hosted `3.0.7`、APP `3.0.7-xiaotai.5`，再次重启跳过 OTA，Wi-Fi 获得 IP。该旧协议分支按上述链路特征准入，内置镜像必须小于旧版上游默认的 1.5 MB OTA 分区，写入失败不调用 OTA end。这个分支的准入是协议指纹，不是旧 APP 的密码学身份；其他来源或改过分区表的 C6 仍需单独验证。写入断电、激活异常和迁移后的音视频回归尚未验收。

本页用于 C6 固件缺失、不兼容或 Hosted/SDIO 初始化失败时的排查与恢复。

## 先确认故障位置

1. 查看 P4 启动日志，确认 Hosted/SDIO 是否初始化成功。
2. 初始化成功后，再检查热点扫描、路由器认证和获得 IP。
3. 初始化失败时，先查供电、板型和 SDIO 配置；保留原始错误，不先擦除 P4 绑定信息。
4. 恢复 C6 前，记录板卡修订、C6 启动版本、固件来源和已有镜像的 SHA-256。

可从 C6 启动串口核对版本。新版 C6 由 P4 独立查询 Hosted 版本与 APP descriptor，项目名、版本及 ELF 摘要必须符合白名单；无法返回这两项的出厂旧协议按上文独立准入，不能把日志中的 `0.0.0` 当作真实固件版本。

## 恢复基线

| 项目 | 配置 |
| --- | --- |
| P4 主机组件 | ESP-Hosted 3.0.7，由 ESP-IDF 组件管理器安装 |
| 官方源码 | espressif/esp-hosted-mcu |
| 固定版本 | `v3.0.7`，P4 与 C6 同版 |
| 链路 | ESP32-C6，4-bit SDIO、40 MHz SDR、Streaming Mode |
| P4 构建环境 | ESP-IDF 5.5.5 |

源码编译使用[同版 C6 APP 附件](https://github.com/tangeai/xiaotai-esp32/releases/download/esp32-p4-app-v1.6.0/c6_app.bin)，大小为 **1,111,904 字节**，SHA-256 为 `8700a7215c77ffc9810ab27dbf7687d6222c620c13d7814dc502ace6a784b4f5`。将核验过的附件放入 `main/assets/c6_app.bin`；P4 构建及设备启动都会核对摘要。该文件不进入源码 Git，**不是 C6 完整恢复镜像，也不能写到 `0x0`**。仅体验完整 P4 包时，无需另下此文件。

## 编译 C6

在 P4 工程外准备独立目录，打开 ESP-IDF 5.5.4 环境：

```sh
git clone --branch v3.0.7 https://github.com/espressif/esp-hosted-mcu.git esp-hosted-c6
cd esp-hosted-c6
git submodule update --init --recursive
git apply "<P4工程>/tools/patches/esp-hosted-3.0.7-c6-full-desc.patch"
git apply "<P4工程>/tools/patches/esp-hosted-3.0.7-c6-sequential-ota.patch"
git apply "<P4工程>/tools/patches/esp-hosted-3.0.7-c6-project-version.patch"
cd examples/wifi/sta/cp
idf.py set-target esp32c6
idf.py menuconfig
```

`<P4工程>` 换成此仓绝对路径；Windows PowerShell 同样可在 C6 仓根目录执行 `git apply`。选择自带的 `partitions_eh_cp_ota_4m.csv` 双 APP 分区，开启完整 APP descriptor；在 ESP-Hosted co-processor 配置中选择 SDIO、Stream、High Speed (40 MHz)，并核对下表的 C6 引脚。P4 端保持 4-bit、40 MHz、Streaming Mode。使用 C6 项目自己的配置与分区，不复用 P4 的 `sdkconfig` 或 `build/`。重新编译产生的摘要可能不同，不能擅自替换 P4 内置的已核准镜像。

按本板原理图核对引脚，两侧 GPIO 编号不同：

| 信号 | P4 GPIO | C6 GPIO |
| --- | --- | --- |
| CLK | 18 | 19 |
| CMD | 19 | 18 |
| D0 | 14 | 20 |
| D1 | 15 | 21 |
| D2 | 16 | 22 |
| D3 | 17 | 23 |

P4 GPIO 54 是主机侧复位控制编号，不能填成 C6 GPIO 54。EN/RST 接线以板卡原理图和从机菜单说明为准。

```sh
idf.py build
```

保存有效配置、工具链版本和 `build/flasher_args.json`。镜像摘要可用：

```powershell
Get-FileHash -Algorithm SHA256 build/eh_cp_wifi_sta.bin
```

Linux 使用 `sha256sum build/eh_cp_wifi_sta.bin`。

## 接线与烧录

按[微雪 FAQ](https://docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-3.5/FAQ)操作：

1. C6_IO9 在上电时拉低，进入下载模式。
2. 同时让 P4 停在下载模式，避免应用干预 C6。
3. 使用 3.3V UART 连接 C6_U0RXD/C6_U0TXD 并共地，接线见[板卡资料](https://docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-3.5/Resources-And-Documents)。不要将 5V 信号接入 C6 IO。

在上述 `examples/wifi/sta/cp` 工程中，将 `PORT` 替换为 C6 实际串口，先执行：

```sh
python -m esptool --port PORT chip_id
```

确认显示 ESP32-C6 后，再烧录：

```sh
idf.py -p PORT flash monitor
```

IDF 按 C6 项目的参数写入镜像。**不要向 C6 写 P4 的 16MB 包，也不要将单独应用 BIN 写到 0x0。**

完成后释放 C6_IO9 的拉低条件，再恢复 P4 正常启动。

## 验证恢复结果

依次确认：

1. C6 启动版本和镜像摘要与本次构建对应。
2. P4 完成 Hosted 初始化，可以扫描 Wi-Fi 并获得 IP。
3. 平台绑定正常，双向音视频通话可用。
4. 挂断后能再次连接，路由器断开再恢复后能重新联网。

保留每步结果和首处错误。串口写入成功后，还需通过上述联网和业务检查，才能确认恢复完成。

从机协议和配置参考[官方 3.0.7 MCU 接入指南](https://github.com/espressif/esp-hosted-mcu/blob/v3.0.7/docs/getting-started-mcu.md)。
