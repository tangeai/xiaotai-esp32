---
name: xiaotai-verify
description: 小钛设备的故障定界与高效验证。遇到通话断续/听不到/回声/啸叫、AI 不回应或丢字、掉线、卡死、内存逐渐变少、新固件要回归时使用。先用一条命令取证，再按层判定是硬件/声学、驱动、音频处理、传输、网络/平台、业务状态还是内存问题。不代替编译、烧录授权和发布流程。
---

# 小钛故障定界与验证

目标：**用最少的步骤，拿到能定位到“哪一层”的证据**，再改代码。不要凭听感或单个日志猜根因。

工具：`tools/xt/xt.py`（Python 3 + pyserial + esptool）。设备表从 `tools/xt/devices.example.json` 复制成 `devices.json` 后填写，本地文件不提交。所有测试结果自动存到 `tools/xt/runs/<时间>-<测试>/`，含两端原始日志、`summary.json` 和截图。

## 第一步：按症状选命令

已知是开放物理 AT 的 SDK 固件时，先跑 `python tools/xt/xt.py snapshot DEV --commands commands.json --elf-sha256 ELF_SHA256 --output snapshot.json`。只读快照不复位、不开始会话；指纹不符先停。先看 `summary.issues` 的异常、再看 `summary.unknown` 的缺测，需要时再按下表选专项。

| 症状 | 先跑 | 看哪几个数 | 详见 |
| --- | --- | --- | --- |
| 通话一方听不清/断续/小声 | `xt.py trace A B -s 18`（通话中让人对着 A 说话） | 两向校验值匹配率；A 发送帧的门控状态（open/attenuated/closed）；B 的上行门是否同时打开 | [定界方法 · 音频](references/定界方法.md#音频通话) |
| 通话完全听不到某一方 | `xt.py call A B` 再看该方 `uplink gated` 日志 | 发送帧数 tx 是否为 0；`muted=1`/会话/链路哪个条件挡住 | [案例 1](references/案例.md#1-m5-微信上行为-0闭麦偏好) |
| 对方挂断后本机卡在“呼叫失败” | `xt.py call B A`（让 B 挂） | 本机是否打印 `invalid hangup command`、错误码 `-40008` | [案例 2](references/案例.md#2-对端挂断后卡在呼叫失败) |
| AI 回复丢字/卡顿 | `xt.py ai DEV -s 60` | `rx_dropped`、`RX BACKPRESSURE`、堆最低水位 | [定界方法 · AI](references/定界方法.md#ai-会话) |
| 微信通话异常 | `xt.py wx-in DEV` / `xt.py wx-out DEV --phone-hangup` | 接通耗时、两向帧数、挂断后是否回空闲 | [定界方法 · 微信](references/定界方法.md#微信通话) |
| 多跑几轮后内存变少 | `xt.py soak DEV --peer B --rounds 15` | 空闲 free 每轮斜率、最大连续块、最低水位 | [定界方法 · 内存](references/定界方法.md#内存) |
| 掉线/离线 | `xt.py log DEV -s 600` 并 `xt.py at DEV AT+PRODUCT` | MQTT EOF/重连、平台 epoch 增长、Wi-Fi RSSI | [定界方法 · 网络](references/定界方法.md#网络与平台) |
| 一直联不上网 | 读保存的网络（P4/WT99 `AT+WIFI?`，其他板见 `docs/AT指令.md`“串口配网”） | 保存的 SSID 是否在现场；日志里是不是 `reason=201` | [案例 9](references/案例.md#9-p4-一直联不上网存的是旧网络) |
| 升级依赖后开机异常、屏幕不亮 | 启动日志 + `AT+STATUS` | `heap_alloc: failed`、internal-free 和升级前对比 | [案例 10](references/案例.md#10-升级依赖后内部内存骤降只有上板才看得到) |
| 新固件要回归 | `xt.py regress A B --full --phone`（步骤见 `xiaotai-test`） | 全部 `PASS`；与上一版对比关键数 | [定界方法 · 回归](references/定界方法.md#回归清单) |

## 第二步：按层判定（从下往上，找第一处异常）

```text
声学/硬件 → 驱动(I2S/codec/屏) → 音频处理(AEC/回声门/噪声门/AGC) → 编码
→ 传输(TiRTC 帧) → 网络/平台(Wi-Fi/MQTT/HTTP/服务端) → 业务状态机 → UI/交互
                               内存/任务水位贯穿所有层
```

- **传输层没问题的判据**：`xt.py trace` 两向校验值匹配 ≥ 98%，且不匹配的只在首尾。此时“听不清”一定在两端的处理或声学，不要去查网络。
- **发送端处理的判据**：同一帧在 A 的 trace 里麦克风电平高（`pre`）、发送电平低（`db`）、状态是 closed/attenuated → A 的回声门/噪声门压掉了，网络无辜。
- **接收端播放的判据**：B 收到的帧校验值匹配、电平正常，但播放异常 → 查 B 的解码/队列/I2S/音量，再查声学。
- **网络的判据**：校验值大量缺失或乱序、下行欠载（underrun）成段出现、MQTT EOF/重连与故障时间重合。
- **业务状态的判据**：媒体帧都正常，但会话 phase/错误码异常（如 `phase=8 error=-40008`），或命令被判无效。
- **内存的判据**：多轮回到空闲后的 free 持续下降（斜率 < −256 B/轮），而不是只看一次最低水位。长时间待机监测（`xt.py watch`）要找“台阶”：一步掉下去以后又平了，就把台阶和同一时刻的事件（断线重连、某次通话、提示音）对上，再用那个事件重复多次，看每次是不是都掉。只看整体斜率会把一次性的台阶误判成持续泄漏。

## 第三步：改之前的硬规则

1. **先取证再改**：一次只改一个变量，用同一条命令复测，对比 `summary.json`。
2. **回声/噪声处理的改动必须先过仿真**：`sdk/xiaotai_media/test/echo_gate_test.c` 里的两机互呼防啸叫仿真（共振峰 +4/+10/+16 dB、音量 40/100）不过，不许上板。
3. **用真实数据调参**：门限、时间常数先用 `trace.json` 里的真实电平离线回放，再写进固件。
4. **区分“状态”和“缺陷”**：保存的偏好（如闭麦）、保存的旧网络、两端固件不是同一代（先 `AT+VERSION`）、用户操作、测试摆放（两台设备互相能听到）都可能造成“像 bug”的现象，先排除。判断“设备没有某个功能、某个指令”之前，先查源码。
5. **打开串口不能复位设备**：先置 DTR/RTS 为 False 再 open；pyserial 默认 open 会复位 ESP32 USB-JTAG 板。
6. **烧录**：只写 APP（`xt.py flash DEV <bin>`，默认 0x10000），看到 `Hash of data verified` 即可，不做整片擦除。

## 交付要求

每次定界结论写成：现象 → 命令 → 关键数（贴 `summary.json` 片段）→ 判定层 → 修改 → 复测数。新的判例补进 [案例](references/案例.md)，新的命令补进 `tools/xt/README.md`。
