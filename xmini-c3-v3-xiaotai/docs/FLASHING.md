# Xmini-C3-V3 烧录说明

适用于 **Xmini-C3-V3 / ESP32-C3 rev 0.3–1.99 / 16 MB Flash**，应用 0.1.0 体验候选。不适用于旧 Xmini-C3、C3-4G、MZ28 或 S3/P4。

## 先选文件

| 用途 | 文件与地址 | 数据影响 |
| --- | --- | --- |
| 首次安装或从其他固件迁入 | `xiaotai-xmini-c3-v3-app-v0.1.0-full-16MB.bin`，`0x0` | 覆盖完整 16 MB，清除配网、绑定身份及设置 |
| 已安装本工程且分区相同，仅升级 APP | 固件 ZIP 中 `xiaotai_xmini_c3_v3.bin`，`0x10000` | 不写 NVS；不选择整片擦除 |

**完整包不是保留配置的升级包。** 即使不勾选整片擦除，写满 16 MB 仍会覆盖旧数据。需要旧配置时先停止操作、备份并核对分区；早期 2 MB 应用分区不能直接套用当前方案。

出厂默认音量/麦克风增益为 **40 / 24 dB**，不沿用测试设备已有设置。本应用没有 OTA，使用 USB 更新。

## 下载与校验

从[本版本 Release](https://github.com/tangeai/xiaotai-esp32/releases/tag/xmini-c3-v3-app-v0.1.0)下载完整 BIN、`SHA256SUMS.txt` 和 `release-manifest.json`。`Source code` 是源码，不是固件。需要保留配置升级时，再下载可选固件 ZIP。

完整 BIN 应为 **16,777,216 字节**。在下载目录核对大小和 SHA-256：

```powershell
Get-Item .\xiaotai-xmini-c3-v3-app-v0.1.0-full-16MB.bin | Select-Object Name, Length
Get-FileHash .\xiaotai-xmini-c3-v3-app-v0.1.0-full-16MB.bin -Algorithm SHA256
```

结果必须与同一版本的清单一致。固件 ZIP 使用包内校验表核对解压文件。完整 BIN、APP 和启动分段来自同一套已测输出，公开打包不改变其字节。

## 用浏览器烧录

1. 电脑 Chrome/Edge 打开[乐鑫在线烧录工具](https://espressif.github.io/esptool-js/)，用 USB 数据线直连设备，关闭串口监视器。
2. 点击 **Connect**，选择 Xmini 的串口，确认 ESP32-C3。连接失败时，按住 BOOT、点按 RESET，再松开 BOOT 后重试。
3. 点击 **Add File**，添加所选文件并填入上表地址。首次安装只选完整包，地址 **`0x0`**；不与分段文件同时添加。
4. Flash Mode、Frequency、Size 选择 **`keep`**，保留镜像中的 DIO / 80 MHz / 16 MB 设置。无需另点 **Erase Flash**。
5. 点击 **Program**，保持供电，等待写入和校验完成，再点击 **Disconnect** 并复位。

如果芯片、修订或容量不匹配，停止烧录，不绕过检查。连接不稳定时先检查短数据线、供电、串口占用，再降低波特率重试。

## 命令行方式

使用 esptool 4.12.0，`PORT` 替换为实际串口。

完整安装，**清除本机配置**：

```sh
python -m esptool --chip esp32c3 -p PORT write_flash --flash_mode keep --flash_freq keep --flash_size keep 0x0 xiaotai-xmini-c3-v3-app-v0.1.0-full-16MB.bin
```

仅在分区一致时升级 APP，保留 NVS：

```sh
python -m esptool --chip esp32c3 -p PORT write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB 0x10000 xiaotai_xmini_c3_v3.bin
```

APP 不能写到 `0x0`。底层启动、分区和 APP 的真实偏移分别是 `0x0`、`0x8000`、`0x10000`，以随包分段清单为准。只刷 APP 不能代替不同分区之间的完整迁移。

## 烧录后

等待设备进入页面；按首页指引连接热点、访问 `http://192.168.6.1`、完成配网和六位码绑定。微信通话还需小程序授权。设备结束网页对讲后，网页连接状态可能未及时刷新。

本版保留已测 APP，完整 16 MB 包尚无新首装实刷记录；浏览器烧录说明不是已执行过浏览器实刷的声明。实际验证范围见发布清单。
