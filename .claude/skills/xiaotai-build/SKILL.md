---
name: xiaotai-build
description: 编译和烧录小钛固件。用户要编译某块板、选 ESP-IDF 6.0 或 5.5、准备 TiRTC 库、看配置、开关功能、烧录固件、只更新应用、看设备启动日志、核对设备上跑的是哪个版本、打包发布时使用。覆盖选板、环境核对、预览、编译、产物核对、烧录和确认。新板接入用 xiaotai-new-board，升级 ESP-IDF 或加新版本线、新芯片用 xiaotai-port，测试用 xiaotai-test，改界面用 xiaotai-ui。
---

# 编译与烧录

详细说明在 `docs/开发指南.md#编译与烧录`。这里是执行步骤。

## 1. 确定板子

```bash
python scripts/build.py --list-boards
```

用户说的是芯片名（“S3”“P4”）时，问清完整板型。同一芯片的不同板子不能混用固件。板卡参数和产品名的对应关系见开发指南的“产品是怎么组成的”。

## 2. 选 ESP-IDF 版本线并核对环境

默认用 6.0 线。只有用户明确要 ESP-IDF 5.5 时才用 5.5 线，不要因为本机装的是 5.5 就自作主张换线。

```bash
python scripts/build.py --list-idf      # 两条线、每块板用哪个 TiRTC 包、当前激活的 IDF
```

| 版本线 | ESP-IDF（官方提交） | 工具链 | TiRTC | 参数 | 产物目录 |
| --- | --- | --- | --- | --- | --- |
| 6.0（默认） | 6.0.3（`76f5dedd9950a3012fee8fb7d5586df21fc67802`） | `esp-15.2.0_20251204` | 官方 2.6.3（6.0.3 版） | 无 | `build/<产品>/sdk-firmware/` |
| 5.5（可选） | 5.5.5（`b774170ff46c393eeb5e495ea37936038d3f4f4f`） | `esp-14.2.0_20260121` | 官方 2.6.3（5.5.4 版） | `--idf 5.5` | `build/<产品>/sdk-firmware-idf5.5/` |

当前激活的版本不对时，先切换环境再编：

- **用 EIM 装的 IDF**：所有安装都记录在 `C:\Espressif\tools\eim_idf.json`（Linux/macOS 在 `~/.espressif/tools/eim_idf.json`），每项都有 `name`、`path` 和 `activationScript`。找到需要的版本，在同一条命令里先运行它的激活脚本，再编译，例如：

  ```powershell
  powershell -NoProfile -Command "& 'C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1'; python scripts\build.py m5stack/core-s3 --idf 5.5"
  ```

- **用官方文档安装的 IDF**：执行它的 `export.sh` 或 `export.ps1`，然后再编译。
- **没有装需要的版本**：告诉用户要装哪个官方版本（6.0.3 或 5.5.5），用 EIM 或官方文档安装。不要用改过的 IDF，也不要拿别的补丁版本凑合：构建核对官方提交，版本号相同但源码不同也会被拦下。

## 3. 准备 TiRTC 库

TiRTC 库不在仓库里。第一次编译时 `build.py` 会自动下载官方包、核对 SHA-256，放到 `components/tirtc_sdk/packages/`。也可以先单独准备：

```bash
python scripts/fetch_tirtc.py              # 默认线
python scripts/fetch_tirtc.py --all        # 两条线
python scripts/fetch_tirtc.py --from <目录> # 编译机不能上网：用别处下载好的 .tgz
```

下载失败时脚本会打印每个包的地址和哈希；把这些交给用户下载，不要换别的包或改契约绕过。不要修改网络或代理配置。

## 4. 预览，再编译

```bash
python scripts/build.py <板> [--idf 5.5] --dry-run
python scripts/build.py <板> [--idf 5.5]
```

预览时确认 `board`、`name`、`target`、`idf_line`、`tirtc_package`、`idf_environment`、`build_dir` 都对。S3 产品在 5.5 线上构建时，会提示官方包里有 5 条硬件原子指令；这是官方包原样内容，告诉用户就行，不要设法绕过。

需要别的组合时，用产品清单里声明的变体：`--variant <名称>`（例如立创 S3 的 `no-video`、`diagnostics`）。没有声明的组合不能临时拼，要先在 `products/<产品>/product.json` 里加变体并验证。

同一个仓库里的构建一次只跑一个。

## 5. 核对产物

产物目录下要有以产品命名的应用镜像（例如 `xiaotai_szpi_s3.bin`）、`flasher_args.json`、`build-record.json` 和这版固件的 AT 词表 `generated/commands.json`；写入地址和文件名以 `flasher_args.json` 为准。ELF SHA-256 是 `build-record.json` 里 `artifacts` 下的 `"<应用名>.elf"` 一项（键名本身带点，例如 `"xiaotai_szpi_s3.elf"`，按两层字段去取会取空），把前 8 位告诉用户，烧录后用它核对；用 `xt.py` 测这台设备时要它的完整值和那份词表。

准备提交或交给别人的那一版，再离线核对产物和烧录布局（查问题时的中间版本不用）：

```bash
python tests/verify_build.py --board <板> --entry sdk --require-flash-layout build/<产品>/sdk-firmware
```

`--board` 要写产品清单 `board` 字段里的完整板号（板包目录 `<厂商>/<板>`，例如 `wt/wt99p461-s1`），和 `build-record.json` 的 `board` 比；`--list-boards` 里 WT99 显示的简称 `wt99p461-s1` 只能给 `build.py` 用。在产物那条版本线的 IDF 环境里跑：它调用当前环境的 esptool，换错环境会报 `image_info` 失败。

## 6. 烧录

先确认串口对应的是哪块板：

```bash
python tools/xt/xt.py scan
```

如果 `xt` 还没配置，就用 `AT+VERSION` 看板型。同时接着多块同款板时，不能只按 COM 号判断，要按 USB 序列号认串口，再核对 MAC 或设备号。

再读板上的分区表，决定写几段。只读 `0x8000` 起的 3072 字节，这不算回读固件：

```bash
python -m esptool --chip <芯片> -p <串口> read-flash 0x8000 3072 board-table.bin
python tools/xt/xt.py partition-plan --build-dir build/<产品>/sdk-firmware --board-table board-table.bin
```

`partition-plan` 也能自己进 ROM 读表，参数见 `tools/xt/README.md` 的“写固件之前核对分区”。按它的输出选写法：

| `partition-plan` 输出 | 写法 | 数据 |
| --- | --- | --- |
| `app-only`：表一致 | `python -m esptool --chip <芯片> -p <串口> write-flash 0x10000 build/<产品>/sdk-firmware/<应用镜像>.bin` | 配网、绑定保留 |
| `three-segment`：表变了、数据分区没变；或者板上原来是另一条版本线的固件；或者全新设备读不到表 | 在构建目录里执行 `python -m esptool --chip <芯片> -p <串口> write-flash @flash_args`，写引导、分区表、应用三段 | NVS 不动，数据保留 |
| `manual-full-review`：数据分区变了 | 停下，告诉用户整片重装会清掉配网和绑定，等他决定 | 会丢 |

芯片名用 `esp32s3` 或 `esp32p4`。ESP-IDF 5.5 环境里的 esptool 是 4.x，命令写作 `write_flash`、`read_flash`、`--before default_reset`。看到 `Hash of data verified` 就算写入成功，不需要再读回校验。不要整片擦除，用户明确要求时才做，并且先告诉他配网和绑定会丢失。原生 USB 的板（S3）写完复位时串口会重新枚举，几秒内打不开是正常的。P4 这类走 CH343 串口的板加 `-b 921600 --before default-reset`，约 7 MB 的应用一分钟内写完；不稳定就退回 115200。`xt.py flash` 会按 USB 桥自动选。

P4 的 C6/C61 协处理器有自己的升级流程，见 `docs/开发指南.md` 的“分区规则”；版本用 `AT+COMPANION` 查。不要把协处理器镜像写到 P4 上。

## 7. 确认

设备重启后：

```bash
python tools/xt/xt.py at <设备> AT+VERSION AT+NET AT+PLATFORM --commands <构建目录>/generated/commands.json
```

ELF 前缀要和第 5 步一致，Wi-Fi 和平台要恢复在线。立创 S3 的默认固件不开物理 AT（产品清单里 `no_platform_console`），只能看启动日志里的 `App version`、`ELF file SHA256` 和联网、平台上线的几行；要 AT 就编 `--variant diagnostics`。联不上网时先用 `AT+NET` 读设备保存的网络；要换网用 `AT+NET WIFI <ssid-hex> <password-hex>`（逐条授权，十六进制用 `xt.py hex` 生成，见 `docs/AT指令.md` 的“统一词表”），其他问题用 `xiaotai-verify` 定位。

## 8. 打包发布

要把固件交给别人时，不要直接拷构建目录，用打包工具生成发布目录，说明见 `docs/开发指南.md` 的“打包发布”：

```bash
python scripts/package_release.py --version <x.y.z> --out releases/v<x.y.z> --level <产品>=A <构建目录>...
```

工具会先核对构建来自当前提交、固件版本号等于 `--version`（版本号只在 `cmake/esp-idf-product/CMakeLists.txt` 的 `PROJECT_VER`），任何一项不对就停下。两条版本线的固件分别打包，5.5 线的构建目录是 `sdk-firmware-idf5.5/`。报错时按提示修源头，不要绕过检查。

## 常见报错

| 报错 | 处理 |
| --- | --- |
| IDF 版本或提交不匹配，或 `needs ESP-IDF x.y.z; the active ESP-IDF is …` | 按第 2 步激活这条线要求的官方版本 |
| `TiRTC package … download failed` / `no archive with the expected SHA-256` | 按提示的地址和哈希下载，用 `fetch_tirtc.py --from` 交给它 |
| `TiRTC package … is not downloaded` | 直接用 CMake/idf.py 构建时会出现；改用 `scripts/build.py`，或先运行 `fetch_tirtc.py` |
| `TiRTC … mismatch`（库哈希、头文件、工具链、libc、ABI） | 包被换过或激活了别的工具链；重新 `fetch_tirtc.py`，按第 2 步激活正确环境 |
| `Dependency lock differs from the product contract` | 比较构建目录里的 `dependencies.lock` 和 `profiles/dependency-locks/`（5.5 线在 `idf5.5/` 子目录）下的锁文件；只是旧构建残留的话，删掉这个构建目录重试 |
| `Build already locked` | 同一个仓库里还有一个构建在跑，等它结束。构建进程被杀留下的锁，同一台机器、同一个系统上会自动清掉；报错里写了锁的主人（PID、主机、系统），确认那个构建已经结束才删这个文件。WSL 和 Windows 互相看不到对方的进程，所以不会互相清 |
| `IDF_TARGET` 冲突 | 清掉环境变量 `IDF_TARGET`，不要在构建目录里执行 `idf.py set-target` |
| `No product <名字> in …/board.json`、`unsupported manifest schema` | 板目录里的 `board.json` 缺了这个产品或格式不对，对照参照板的 `board.json` 修 |
| 串口打不开 | 关掉占用它的监视器；Windows 上的错误 995 一般是 USB 刚重新枚举，过几秒重试 |
