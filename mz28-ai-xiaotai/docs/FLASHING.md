# MZ28 烧录说明

适用：MZ28-AI-PPD V1.2，ESP32-C3，8 MB Flash，应用 1.0.0。不能用于 ESP32-S3、P4 或其他 Flash 容量的设备。

启动镜像支持芯片修订 **v0.3 至 v1.99**，当前真机为 v0.4。更早修订不能直接使用本包。

## 选择烧录方式

| 场景 | 文件与地址 | 数据影响 |
|---|---|---|
| 首次安装、其他固件迁入、恢复出厂 | `xiaotai-mz28-1.0.0-full-8MB.bin`，`0x0` | 覆盖整个 8 MB，清除设备端 Wi-Fi、身份和旧分区数据，需重新配网/绑定 |
| 已安装本工程且分区完全一致，只更新应用 | `xiaotai_mz28_ai.bin`，`0x10000` | 不写 NVS，保留 Wi-Fi 和本机身份 |
| 已确认相同分区，更新启动程序和应用 | 本包 `flash_args.txt` 中的三个分段 | 不写 NVS；必须先确认旧分区一致 |

**完整镜像会清除本机数据，不能作为保留配置的升级方式。** 如需保留旧设备数据，先停止操作，备份并确认迁移方案；更换分区不保证旧 NVS 可读。清本机身份不等于解绑云端账号。

本工程分区为 NVS `0x9000/0x6000`、PHY `0xf000/0x1000`、factory `0x10000/0x5f0000`。其他固件即使 NVS 起始地址相同，也可能在 `0xd000` 存有 OTA 数据；直接覆盖分区表会把这些内容留在扩大的 NVS 内。不能只看 APP 偏移便判断兼容。

## 文件校验

首次体验可从[1.0.0 发布页](https://github.com/tangeai/xiaotai-esp32/releases/tag/mz28-ai-app-v1.0.0)直接下载完整 BIN 和 `SHA256SUMS.txt`。需要 APP 或分段文件时，再下载并解压可选固件 ZIP。两种入口来自同一次构建。

在下载目录或解压后的固件目录计算哈希，与各自的 `SHA256SUMS.txt` 对照：

```powershell
Get-FileHash .\xiaotai-mz28-1.0.0-full-8MB.bin -Algorithm SHA256
```

完整镜像长度应为 **8,388,608 字节**，包含启动程序、分区表和应用；未使用区域填充 `0xff`，不含任何设备的凭据或读回数据。保留配置升级时应校验对应的 APP BIN。

## 用浏览器烧录

1. 电脑使用支持串口的 Chrome 或 Edge，打开 [Espressif ESP Tool](https://espressif.github.io/esptool-js/)。使用数据线连接 MZ28，关闭占用该串口的终端和监视器。
2. 选择连接并授权 **MZ28 的串口**，核对识别结果是 ESP32-C3。连接失败时按住 BOOT、点按 RESET，再松开 BOOT，重新连接。
3. 按上表选择一个文件与对应地址。首次安装用完整镜像 `0x0`；保留配置仅用 APP `0x10000`。不要同时选择完整镜像与分段文件。
4. Flash Mode 选择 DIO、Frequency 80 MHz、Size 8 MB。**保留配置升级不要点整片擦除**；完整镜像本身已覆盖全片，无需额外擦除。
5. 开始烧录，等待写入和校验全部完成，再按 RESET。烧录过程中不要断电。

网页界面可能随工具更新变化，以文件、地址、容量和校验结果为准。此入口已核对可访问；浏览器实刷与命令行实刷的验收记录分别保存。

## 用命令行烧录

使用 esptool 4.x，在解压后的固件目录执行，`COMx` 替换为 MZ28 实际端口。下面的方式选择一种即可。

未安装时先执行：

```sh
python -m pip install esptool==4.12.0
```

首次安装或恢复出厂，**清除设备端配置**：

```sh
python -m esptool --chip esp32c3 -p COMx write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB 0x0 xiaotai-mz28-1.0.0-full-8MB.bin
```

相同分区、保留设备端配置，只升级应用：

```sh
python -m esptool --chip esp32c3 -p COMx write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB 0x10000 xiaotai_mz28_ai.bin
```

相同分区、保留 NVS，更新三个分段：

```sh
python -m esptool --chip esp32c3 -p COMx write_flash "@flash_args.txt"
```

APP BIN 不能写到 `0x0`。不同分区不得直接使用保留配置命令。不要把私有 Flash/NVS 备份放入交付包。

## 烧录后检查

- 写入工具报告各段校验通过；设备重启后串口无 panic、看门狗或持续复位。
- 完整安装后出现 `XiaoTai-XXXX` 热点，手机打开 `http://192.168.6.1`，可扫描网络并配网。
- 联网后在[体验平台](https://xiaotai.chat/)填写语音六位码绑定设备，再在正式版“小钛”小程序授权通话，检查双方呼叫、BOOT 接听/双击挂断及双向持续语音。
- 保留配置升级应恢复原网络和身份。新固件仍须做功能验收，哈希正确或烧录成功不代替通话与听感测试。
