# MZ28 开发指南

本指南用于编译和调试小钛 MZ28 1.0.0。只使用设备时，直接阅读[本机使用说明书](USER_GUIDE.md)；已有固件包时无需安装编译环境。

## 准备源码与硬件

准备完整的 `mz28-ai-xiaotai/`、MZ28-AI-PPD V1.2、USB 数据线和 2.4 GHz Wi-Fi。工程包含 TiRTC 2.5.0 C3 库、头文件和提示音，其他组件按 [dependencies.lock](../dependencies.lock) 下载。

本板为 ESP32-C3 / 8 MB Flash / 无 PSRAM，音频通过 VB6824 提供。不能替换成其他平台 SDK，也不要照搬 S3 的分区、驱动或 GPIO。

## 编译与烧录

### 1. 打开 ESP-IDF 环境

使用 ESP-IDF 5.5.5 的终端；Linux/macOS 加载对应环境的 `export.sh`。进入 MZ28 工程目录后检查：

```sh
idf.py --version
riscv32-esp-elf-gcc --version
```

预期为 ESP-IDF **5.5.5**、RISC-V GCC **14.2.0 / esp-14.2.0_20260121**。

### 2. 编译

```sh
idf.py build
```

随包配置已经锁定 ESP32-C3。不要为了重新配置而删除现有 `sdkconfig`；defaults 不会完整覆盖已有配置。切换电脑或 Windows/WSL 时重新建立构建缓存，不共用旧 `build/`。

| 输出 | 用途 |
| --- | --- |
| `build/xiaotai_mz28_ai.bin` | 应用镜像，不是完整镜像 |
| `build/xiaotai_mz28_ai.elf` | 调试和异常回溯 |
| `build/flasher_args.json` | 本次分段烧录的真实偏移和参数 |

### 3. 烧录并查看启动

列出串口，拔插设备核对新增的一项：

```sh
python -m serial.tools.list_ports -v
```

确认目标为 MZ28 的 ESP32-C3 / 8 MB，关闭占用串口的其他程序，将 `PORT` 替换为实际端口：

```sh
idf.py -p PORT flash monitor
```

退出监视器用 `Ctrl+]`。下载模式、不同分区迁移和完整包清除配置的影响见[烧录说明](FLASHING.md)。只有已核对相同分区时才使用保留 NVS 的分段升级，APP 地址为 `0x10000`。

**预期结果：**启动显示应用 1.0.0，VB6824、RGB 和非阻塞控制台就绪；随后进入配网、绑定或在线待机。设备实际操作见[使用说明书](USER_GUIDE.md)。

## 串口开发检查

串口速率为 **115200**，命令以回车结束。日常配网和通话使用手机与 BOOT，不需要串口工具。

| 命令 | 用途 |
| --- | --- |
| `help` | 查看当前固件命令 |
| `status` / `memory` / `id` | 状态、连续内存块/最低水位和设备 ID |
| `contacts` / `call 1` | 刷新联系人 / 呼叫第一位 |
| `answer` / `reject` / `hangup` | 接听 / 拒接 / 取消或挂断 |
| `prompt 123456` | 试听六位码，不修改绑定身份 |
| `ring incoming` / `ring outgoing` / `ring stop` | 非通话时试听、停止铃声 |
| `cue wifi` | 试听配网提示，不启动配网 |
| `retry` | 根据错误提示，在冷却结束后重新请求平台启动 |

串口备用配网：

```text
wifi {"ssid":"YOUR_SSID","password":"YOUR_PASSWORD"}
```

该命令直接保存参数，不包含网页先验证联网的步骤。按设备返回提示处理；Windows USB Serial/JTAG 写入超时时，先检查端口占用和线材，不能只凭主机超时判断设备卡死。`rx-record`、`rx-dump` 在实时版本中不提供录音功能。

## 配置与代码入口

GPIO、缺省 Wi-Fi、服务发现和小程序版本配置位于 [main/app_config.h](../main/app_config.h)，私有覆盖放在被忽略的 `main/app_config.local.h`，不要提交凭据。

默认正式版小程序参数为 0；服务发现决定 API、MQTT 和 RTC 地址。六位码在[体验平台](https://xiaotai.chat/)完成设备归属绑定，随后在“小钛”微信小程序完成通话授权。不把联系人或设备密钥写成固定值。

绑定提示音包含“登录微信小程序搜索小钛进行绑定”的引导，本版保留用户已验收的音频资源。六位码的填写入口和微信通话授权是两个步骤，具体操作见[使用说明书](USER_GUIDE.md#首次配网与绑定)。

驱动、音频、平台和会话职责见[移植指南](PORTING.md)。按改动选择[测试与验证](TESTING.md)；构建、烧录、信令和实际听感分开记录。
