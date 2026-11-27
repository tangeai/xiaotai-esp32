---
name: xiaotai-test
description: 给小钛的改动选测试并执行：静态检查、主机测试、受影响产品构建、两台设备的自动化回归（通话、AI、微信、抓包、内存）。用户说“跑测试”“回归一下”“这个改动要测什么”“发版前测一遍”时使用。测出问题后要定位原因，用 xiaotai-verify。
---

# 测试

原则：能在低层证明的，就不留到高层；真人只确认机器判断不了的东西，比如听感。说明文档在 `docs/测试与诊断.md`。

## 1. 算出影响范围

```bash
python scripts/check_change_impact.py --files <改动的文件...>
```

输出会列出受影响的产品、要编译的板和建议的主机测试。没被列出不等于没有影响：改了公共头文件、构建脚本或 SDK 时，按全部产品处理。

## 2. 静态检查和主机测试

```bash
python scripts/check_structure.py
python3 tests/test_products.py        # 产品清单和生成代码，要编译生成的 C，需要 Linux 或 WSL
python3 tests/sdk/run.py              # SDK 组件、板包和所有产品，需要 Linux 或 WSL
python tests/fetch_components.py      # 准备锁定的组件源码；找不到时按输出手动下载
python tests/run_host.py              # 全部主机测试，需要 Linux 或 WSL
```

`tests/` 下按模块分了专项测试，见 `tests/README.md`。只改一个组件时，先跑它和它的依赖（`python3 tests/sdk/run.py <组件>`），再跑产品级 `python3 tests/sdk/product/run.py --product <产品>`。改了回声门、噪声门或 AEC，必须跑 `sdk/xiaotai_media` 的 `media_echo_gate`（`python3 tests/sdk/run.py xiaotai_media`），其中有两机互呼防啸叫仿真，不过不许上板。

- 按能力选择的写法（例如没有外部内存时的精简形态），要拿一款有外部内存的产品也打开这个形态跑一遍，证明不是某款产品专用的路径。
- 改到会碰 Flash 的端口代码或任务栈的放置，看主机测试有没有报 `cache-unsafe task stack`。
- 台架上的偶发问题，修之前先写一个在原代码上能复现的夹具（红），修后变绿。

## 3. 构建

受影响的每个产品都在默认的 6.0 线上完整编译一次，见 `xiaotai-build`。准备提交的那一版再离线核对产物和烧录布局：`python tests/verify_build.py --board <板> --entry sdk --require-flash-layout build/<产品>/sdk-firmware`；查问题时的中间版本不用。

ESP-IDF 5.5 是可选线，日常改动不编。只有两种情况要编 5.5：

- 影响检查输出里出现了 `--idf 5.5` 的命令，说明改到了 5.5 专用的输入：官方 SDK 包、`profiles/dependency-locks/idf5.5/` 下的锁、板级 `sdkconfig.idf5.5`。按输出的命令执行。
- 发版前：把支持 5.5 的产品全部编一遍。

发布的固件用 6.1 线编（`components/tirtc_sdk/packages.json` 的 `release_line`）。发版前，每个要发布的产品在 6.1 线上编一次（`--idf 6.1`），实机回归用的就是这份固件。

ESP-IDF 5.5 下的主机测试只有一项和版本线有关：`tests/test_port_usb_monitor.py`。改到 USB 连接监测时，用 5.5 的 IDF 再跑一次它（`--idf-path <5.5 的 IDF>`，或者设好 `IDF_PATH`）。其他主机测试只跑默认线。

## 4. 设备回归

固件都是统一 SDK。对设备的每条命令都带这台设备本次镜像的词表（`--commands`）和完整 ELF（`--elf-sha256`），两台设备时再带对端的 `--peer-commands`、`--peer-elf-sha256`；会改变设备状态的指令逐条 `--allow "<完整指令>"`，结束和清理也要列（见[工具说明](../../../tools/xt/README.md#统一-sdk从读状态开始)）。词表是构建目录里的 `generated/commands.json`，完整 ELF 是 `build-record.json` 里 `artifacts` 下的 `"<应用名>.elf"` 一项；手上只有发布包时，ELF 用 `release-manifest.json` 里的 `elf_sha256`，词表在同一提交的源码里运行 `python scripts/sdk_selection.py --product products/<产品>/product.json --out-dir <目录>` 生成，和构建出来的相同。对端设备号用 `xt.py at B AT+BIND` 读 `device=`。

先跑基础关：

```bash
python tools/xt/xt.py gate DEV --product PRODUCT --elf-sha256 ELF_SHA256 --allow-file gate.json
```

授权清单只放本机，不带动作时只读。只在无其他会话的专用台架开启 `isolated_bench`，并逐条列出动作和清理，格式见[工具说明](../../../tools/xt/README.md#一次跑完该跑的基础检查)。

再按改动跑专项（下表省略了上面那几个参数）：

| 改了什么 | 至少跑 |
| --- | --- |
| 界面、文案 | `xt.py screen DEV out.png` 截图对比，再加一次互呼。`AT+SCREEN` 只在打开 `UI_CAPTURE` 的产品上有（两款 P4 的默认固件没有），任何会话进行中（通话、对讲、闹钟响铃）都回 `BUSY ACTIVITY`；这时用 `AT+UI`（`app=`、`view=`）、`AT+UI ACTIONS`（每个按钮的坐标、意图和文字）和应用自己的状态指令核对，用 `AT+UI TAP <x> <y>` 点按钮。`AT+UI TAP` 回 `INVALID_STATE` 多半是屏幕正被真的按着，先查屏上有没有东西 |
| 新应用、跨应用的页面 | 翻页（`AT+UI TAP` 点首页两侧箭头）进新应用，走一遍它的页面；再看事件归属：新应用该显示的（到点、门禁来电）由它显示，原来归首页应用的（普通来电、实时查看）仍由首页应用显示 |
| 通话、会话、信令 | `xt.py call A B` 和 `xt.py call B A`；允许 `AT+CALL DEVICE <对端设备号>`、`AT+CALL ACCEPT`、`AT+CALL HANGUP` |
| 微信相关 | `xt.py wx-in DEV`、`xt.py wx-out DEV`（要 USB 接手机） |
| 音频处理、编码、抖动缓冲 | 双向互呼，再加 `xt.py trace A B -s 18` 看匹配率和门控分布；改了回声消除、输出或麦克风增益，再用 `xt.py echo` 单机对比前后（见 `xiaotai-echo`） |
| AI | `xt.py ai DEV -s 30`；按键流程再跑 `xt.py ai-keys DEV` |
| 内存分配、任务、缓冲 | `xt.py soak DEV --rounds 15`（多轮 AI）；通话路径连续互呼几次，比较前后 `AT+MEM`；判法见下 |
| 视频、相机 | 空闲时 `xt.py at DEV "AT+VIDEO PROBE"`，再开一次实时查看，看 `AT+VIDEO` 的计数和 `+VIDEO-UP-RECOVERY` |
| 板包、端口、启动顺序 | 走一遍 `xiaotai-bringup` 的十项 |
| 发版前 | 每块要发布的板烧 6.1 线的固件（发布包里就是它），跑基础关和双向互呼，有手机再加微信；带摄像头的板再开一次实时查看看画面；再给每种芯片挑一块板烧 5.5 线的固件，跑一次双向互呼 |

多轮内存的判法：

- 可用内存：最后一轮后静置 130 秒再读，和第一轮开始前比，下降不超过 256 字节。每轮的 TCP 控制块要约 2 分钟才释放，不静置会把它误判成泄漏。
- 最大连续块单独记，和上一版同样的轮次比，不比它差。
- 每个应用任务的最小剩余栈不少于 1024 字节。

`xt.py regress` 是旧固件的组合回归，遇到 SDK 固件会直接拒绝，不要用。结果在 `tools/xt/runs/<时间>-<测试>/`，有两端日志和 `summary.json`，看 `pass`。

## 5. 汇报

给用户的结果按这个格式写，每一层分开写，不能互相替代：

| 层 | 结果 | 证据 |
| --- | --- | --- |
| 静态 / 主机 | 通过 N 项 | 命令和输出 |
| 构建 | 产品名、版本线、ELF 前 8 位 | `build-record.json`（`idf_line`、`tirtc_package`） |
| 设备回归 | 每一步 PASS/FAIL | `runs/...` 目录 |
| 真人确认 | 听感等 | 用户反馈 |

没跑的项目要写明原因，比如没有对应的板、手机没接、需要 Linux。测试失败时，贴出 `summary.json` 里的关键字段，再转到 `xiaotai-verify` 定位。
