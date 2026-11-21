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
python tests/test_products.py
python3 tests/sdk/run.py              # SDK 组件、板包和五款产品，需要 Linux 或 WSL
python tests/fetch_components.py      # 准备锁定的组件源码；找不到时按输出手动下载
python tests/run_host.py              # 全部主机测试，需要 Linux 或 WSL
```

`tests/` 下按模块分了专项测试，见 `tests/README.md`。改了回声门、噪声门或 AEC，必须跑 `sdk/xiaotai_media` 的 `media_echo_gate`（`python3 tests/sdk/run.py xiaotai_media`），其中有两机互呼防啸叫仿真，不过不许上板。

## 3. 构建

受影响的每个产品都在默认的 6.0 线上完整编译一次，见 `xiaotai-build`。

ESP-IDF 5.5 是可选线，日常改动不编。只有两种情况要编 5.5：

- 影响检查输出里出现了 `--idf 5.5` 的命令，说明改到了 5.5 专用的输入：官方 SDK 包、`profiles/dependency-locks/idf5.5/` 下的锁、板级 `sdkconfig.idf5.5`。按输出的命令执行。
- 发版前：把支持 5.5 的产品全部编一遍。

ESP-IDF 5.5 下的主机测试只有一项和版本线有关：`tests/test_usb_connection_monitor.py`。改到 USB 连接监测时，用 5.5 的 `IDF_PATH` 再跑一次它。其他主机测试只跑默认线。

## 4. 设备回归

新 SDK 固件先跑 `python tools/xt/xt.py gate DEV --product PRODUCT --elf-sha256 ELF_SHA256 --allow-file gate.json`。授权清单用本次镜像的词表和完整指纹；不带动作时只读。只在无其他会话的专用台架开启 `isolated_bench`，并逐条列出动作和清理。按[工具说明](../../../tools/xt/README.md#一次跑完该跑的基础检查)补未验证项，不把旧方言命令直接套给新固件。

以下命令用于还在跑旧固件的设备（`xt` 的旧方言）：

```bash
python tools/xt/xt.py scan                         # 识别设备，第一次用要先准备 devices.json
python tools/xt/xt.py regress A B                  # 快速回归：双向呼叫和 AI 流程
python tools/xt/xt.py regress A B --phone          # 再加微信双向（需要 USB 接手机）
python tools/xt/xt.py regress A B --full           # 再加逐帧抓包和 10 轮内存测试
```

怎么选：

| 改了什么 | 至少跑 |
| --- | --- |
| 界面、文案 | 快速回归，再加 `xt.py screen` 截图对比 |
| 通话、会话、信令 | 快速回归，两个方向都要跑 |
| 微信相关 | `--phone` |
| 音频处理、编码、抖动缓冲 | `--full`，再看 `trace` 的匹配率和门控分布 |
| 内存分配、任务、缓冲 | `--full`，或者单独跑 `xt.py soak DEV --peer B --rounds 15` |
| 发版前 | `--full --phone`，每块受影响的板都要当一次 A；再给每种芯片挑一块板，烧 5.5 线的固件，跑一次快速回归 |

每一步打印 `PASS` 或 `FAIL`，详细结果在 `tools/xt/runs/<时间>-regress-A-B/`。

## 5. 汇报

给用户的结果按这个格式写，每一层分开写，不能互相替代：

| 层 | 结果 | 证据 |
| --- | --- | --- |
| 静态 / 主机 | 通过 N 项 | 命令和输出 |
| 构建 | 产品名、版本线、ELF 前 8 位 | `build-record.json`（`idf_line`、`tirtc_package`） |
| 设备回归 | 每一步 PASS/FAIL | `runs/...` 目录 |
| 真人确认 | 听感等 | 用户反馈 |

没跑的项目要写明原因，比如没有对应的板、手机没接、需要 Linux。测试失败时，贴出 `summary.json` 里的关键字段，再转到 `xiaotai-verify` 定位。
