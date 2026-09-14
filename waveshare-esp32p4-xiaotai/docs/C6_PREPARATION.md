# C6 准备与恢复

板载 C6 负责 Wi-Fi，P4 运行应用。首次使用先[烧录 P4](GETTING_STARTED_CN.md#编译与烧录)并尝试联网；能正常联网的 C6 保持原样。

本页用于 C6 固件缺失、不兼容或 Hosted/SDIO 初始化失败时的排查与恢复。

## 先确认故障位置

1. 查看 P4 启动日志，确认 Hosted/SDIO 是否初始化成功。
2. 初始化成功后，再检查热点扫描、路由器认证和获得 IP。
3. 初始化失败时，先查供电、板型和 SDIO 配置；保留原始错误，不先擦除 P4 绑定信息。
4. 恢复 C6 前，记录板卡修订、C6 启动版本、固件来源和已有镜像的 SHA-256。

可从 C6 启动串口核对版本；主机接口 `esp_hosted_get_coprocessor_fwversion` 也能查询，但版本号不能替代镜像哈希。

## 恢复基线

| 项目 | 配置 |
| --- | --- |
| P4 主机组件 | ESP-Hosted 1.4.7，含[主机侧修正](../components/espressif__esp_hosted/LOCAL_CHANGES.md) |
| 官方源码 | espressif/esp-hosted-mcu |
| 固定提交 | `d24d1dc3f709965fa61e3a1304c0896a9f3d497c` |
| 链路 | ESP32-C6，4-bit SDIO、40 MHz SDR、Streaming Mode |
| 构建环境 | ESP-IDF 5.5.4 |

本工程修改的是主机组件，没有修改 C6 从机源码或协议。上述提交提供可追溯的恢复起点；仓库尚未提供逐板验收的 C6 固件包，恢复后需完成下文验证。

## 编译 C6

在 P4 工程外准备独立目录，打开 ESP-IDF 5.5.4 环境：

```sh
git clone https://github.com/espressif/esp-hosted-mcu.git esp-hosted-c6-recovery
cd esp-hosted-c6-recovery
git checkout --detach d24d1dc3f709965fa61e3a1304c0896a9f3d497c
git submodule update --init --recursive
cd slave
idf.py set-target esp32c6
idf.py menuconfig
```

在 `Example Configuration` 选择 SDIO，核对 Streaming Mode、High Speed (40MHz) 和普通 4-bit 模式。使用 C6 项目自己的配置与分区，不复用 P4 的 `sdkconfig` 或 `build/`。

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
Get-FileHash -Algorithm SHA256 build/network_adapter.bin
```

Linux 使用 `sha256sum build/network_adapter.bin`。

## 接线与烧录

按[微雪 FAQ](https://docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-3.5/FAQ)操作：

1. C6_IO9 在上电时拉低，进入下载模式。
2. 同时让 P4 停在下载模式，避免应用干预 C6。
3. 使用 3.3V UART 连接 C6_U0RXD/C6_U0TXD 并共地，接线见[板卡资料](https://docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-3.5/Resources-And-Documents)。不要将 5V 信号接入 C6 IO。

在 `slave/` 中，将 `PORT` 替换为 C6 实际串口，先执行：

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

从机协议和配置参考[固定提交的 SDIO 指南](https://github.com/espressif/esp-hosted-mcu/blob/d24d1dc3f709965fa61e3a1304c0896a9f3d497c/docs/sdio.md)。
