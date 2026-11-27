---
name: xiaotai-echo
description: 测量和调优小钛设备的回声消除。对方或 AI 听到设备自己的声音、通话或 AI 对讲时回声没消干净、近端说话被吞、换了喇叭/麦克风/外壳/功放要重新调、要比较两块板的回声效果时使用。先做单讲测量（设备自己的喇叭是唯一声源），按麦克风削顶、输出失真、线性部分没消好、残余太大逐项定位，一次只调一个量，过防啸叫仿真再上板，最后真人听感。其他音频问题的分层定界用 xiaotai-verify。
---

# 回声消除测量与调优

目标：**先量清楚回声被压低了多少、剩下的回声比底噪高多少，再动参数。** 听感会受摆放、房间、说话人影响，不能单独当依据。

## 链路和读数

```text
麦克风 / 回采 / 监听 ──▶ 处理器（回声消除）──▶ processed ──▶ 采集效果（自动增益、噪声门）──▶ clean ──▶ 上行
```

`AT+STATUS media` 的 `+MEDIA-LEVELS:` 给出这五路的 RMS 和峰值（满量程 32768），说明见 `docs/AT指令.md`。产品打开 `parameters.media.diagnostics.capture_window` 才是窗口平均值（`window=1`），否则只是最近一块，测量前先打开。处理器的名字在 `+MEDIA-PROCESSOR: name=`：`aec.dual`（双麦）、`aec.single`（单麦加回采），没有回采的板用 `echo_gate`（半双工回声门，`media.echo_gate.enabled`）。

不要拿 `mic` 直接和 `clean` 比：`clean` 已经过自动增益和限幅，处理器本身也可能带固定增益。

## 1. 单讲测量

**摆放**：近处没人说话；附近别的设备不出声。两台设备挨着通话时，这台的麦克风也在听另一台的喇叭，那部分在它看来是近端讲话，算不成回声，所以评回声消除用单机测量，不用两机互呼。

**命令**（设备要开放物理 AT；不开放的产品用带 AT 的变体，例如立创 S3 的 `--variant diagnostics`）：

```bash
python tools/xt/xt.py echo DEV --commands <构建目录>/generated/commands.json --elf-sha256 <完整 ELF> \
  --volumes 40,60,80,100 \
  --allow "AT+FEEDBACK SET companion.incoming 1" --allow "AT+FEEDBACK SET companion.incoming 0" \
  --allow "AT+SET audio.volume 40" --allow "AT+SET audio.volume 60" \
  --allow "AT+SET audio.volume 80" --allow "AT+SET audio.volume 100" \
  --allow "AT+SET audio.volume <原来的音量>" --output echo.json
```

设备自己的喇叭循环播放来电铃声；每档音量先读几秒静音底噪，再读播放时的电平，最后关掉铃声、恢复原音量。产品不开放写音量时，去掉 `--volumes` 和那几条 `AT+SET`，只测当前音量。

**读表**：

| 列 | 意思 | 怎么看 |
| --- | --- | --- |
| `reduction_dB` | 回声被压低多少：麦克风比底噪抬高的量，减去处理后比底噪抬高的量，和处理器内部的固定增益无关 | 越大越好；残余抑制同时压掉底噪时，这个数会偏大，以下一列为准 |
| `residual_dB` | 处理后比静音底噪高多少，就是对方能听到的残余回声 | ≤ 0：听不出；0～10：小声能听出；> 10：明显 |
| `mic/ref_dB` | 喇叭到麦克风的耦合（麦克风比回采高多少） | 越大，需要压低得越多；结构和功放增益决定 |
| `clip` | 播放时麦克风峰值到满量程 | 出现就先解决削顶 |

同一条件连测两次，差 3 dB 以内才算稳定。

**输出是不是线性的**：音量律是 dB = (百分比 − 100) × 0.5，音量每升 10%，喇叭输出应升 5 dB，`mic` 和 `ref` 也应各升约 5 dB。到某一档以后涨得明显不到 5 dB，从那一档往上就是压缩区：喇叭或功放在失真，线性滤波器消不掉。

**参考值**（单讲、近处无人、窗口平均）：

| 板子的情况 | `reduction_dB` | `residual_dB` |
| --- | --- | --- |
| 双麦（`aec.dual`），输出在线性区 | 31–43 | ≤ 0 |
| 单麦加回采（`aec.single`），输出在线性区 | 24–28 | 约 0–5 |
| 输出进入压缩区 | 18–20 | 15–25 |

同一款板差得比这多，先怀疑摆放、增益或输出上限，再怀疑回声消除参数。

## 2. 判断问题在哪

| 现象 | 说明 | 先试 |
| --- | --- | --- |
| `clip` 出现 | 麦克风削顶，失真的回声线性滤波器消不掉 | 降麦克风增益（板包 `board.cc` 的 `input_gain_db`），或限制最大输出 |
| 音量越大 `reduction_dB` 越低，而且音量调高时 `mic` 和 `ref` 每 10% 涨不到 5 dB | 输出进了压缩区，喇叭或功放在失真 | 把输出上限设在还线性的那一档（见第 3 节“输出”） |
| 小音量时 `reduction_dB` 也低于约 20 dB | 线性部分没消好：回采对齐、滤波长度、回采取点 | 核对板包声明的输出延迟、`aec.ring_frames`、`aec.stamp_wait_ms`；回采要取在喇叭实际播放的那一路 |
| 任何音量下 `reduction_dB` 都接近 0，`processed` 和 `mic` 一样大 | 消除器在直通：配置不对，或者消除器本身没工作 | 先核对板上到底有几个麦克风：两个“麦克风槽”安静时相关接近 ±1，就是同一个麦克风，要按单麦配（GUITION S31 就是这样）。`aec.single` 会把一直消不掉回声的消除器换掉，`+MEDIA-PROCESSOR` 的 `restarts=` 记次数；ESP32-S31 上开机建的那个就会被换一次 |
| `reduction_dB` 不低，但 `residual_dB` 仍 > 10 | 耦合太强，压低以后剩下的还是大 | 降输出或改结构隔离；通话上行可开噪声门（`call.uplink_noise_gate`）；残余抑制档位见下 |

## 3. 一次只调一个量

可以调的地方：

- **输出**：板包 `board.cc` 的 `audio.volume_max_percent`（用户 100% 落到编解码音量的这个百分比，1–100% 按比例缩到 1% 到上限），`board.json` 的 `audio_output` 写同一个值，主机产品测试会核对两处一致。用户已保存的音量由设置模块在升级后第一次开机换算一次，响度不变，不用另写迁移。默认音量在产品清单 `settings.volume_default`。
- **麦克风增益**：板包 `input_gain_db`、`gain_min_db`、`gain_max_db`。
- **回声消除参数**：产品清单 `parameters.media` 下的 `aec.*`。`aec.single` 和语音前端都读：`mode`（0 省算力，1 高性能）、`filter_length`（以块计，1–8，默认 4）、`nlp`（残余抑制 0–2，默认 1）。只有语音前端读：`gain_cdb`（输出增益，默认 600 即 +6 dB）、`separation`（双麦分离，默认开）、`noise_suppression`（WebRTC 降噪，默认关），以及 `ring_frames`、`stamp_wait_ms`、`strict_feed`、`input_slots`。改配置要同时看 `+MEDIA-PROCESSOR` 的 `max_us` 和 `deadline_misses`：每块的处理时间不能超过块长。
- **后级**：自动增益 `capture.agc_db`，上行噪声门。

规矩：

1. 每次只改一处，用同一条 `xt.py echo` 命令、同样的摆放和音量对比前后。
2. 改回声门、噪声门或回声消除，先过 `sdk/xiaotai_media` 的 `media_echo_gate` 两机互呼防啸叫仿真（`python3 tests/sdk/run.py xiaotai_media`），不过不许上板。
3. 默认音量、上限、增益的改动用户听得出来，进默认值之前先给用户看数据、请他确认。
4. 改参数而不是写个例：差异写进产品清单或板包，不在代码里判断板名。

## 4. 验收

- `xt.py echo` 改前改后两份结果，按音量列出 `reduction_dB`、`residual_dB`、`clip`。
- 双讲不被吞：两机互呼时让两边同时说话，用 `xt.py trace A B` 看发送帧的门控状态，近端讲话的帧不能大量被压成 closed。
- 真人听感：通话和 AI 对讲各一次，远端听有没有回声，近端插话能不能被听见。听感由用户确认。

汇报写成：板子和 ELF、音量、摆放 → 改了什么 → 前后两张表 → 双讲和听感结论 → 还没验证的项。
