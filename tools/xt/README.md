# xt：多设备自动化测试工具

第一次用 xt，一台板就够：先核对固件，再读状态。确认板号、端口和回包都对，再扩到两板通话或手机测试。统一 SDK 的指令和回包见 [AT 指令](../../docs/AT指令.md)；没换固件的设备仍按本页末尾的旧方言使用。

## 准备

1. Python 3，安装 `pyserial` 和 `esptool`。也可以用环境变量 `XT_PYTHON` 指定解释器。
2. 确认哪块板接在哪个端口。`DEV` 可以是实际串口号，也可以是 `devices.json` 里已有的名字（文件格式见 `devices.example.json`）。`python xt.py scan` 可识别在台设备并写回本机设备表；设备号识别不出来时先核对绑定，不要把另一块板的 ID 填进去。
3. 只有测微信通话时才需要手机。USB 调试和点击坐标以获准的测试手机为准；不同机型的坐标不要照抄。

`devices.json` 和 `runs/` 只留在本机，不提交。

xt 按不复位的方式打开串口；USB 驱动、端口和线路接法仍要在实板上核对。只读检查不主动复位设备。

## 统一 SDK：从读状态开始

在 `tools/xt` 下运行，下例的串口和构建目录都只是示意。`commands.json` 要选运行固件对应的构建产物，不能用同芯片另一块板的词表。

```sh
python xt.py at COM25 "AT+STATE" --commands ../../build/at-szpi-diagnostics/generated/commands.json
```

工具先读 `AT+VERSION` 判别 `at=1` 和运行固件，再用实时 `AT+HELP` 复核写法。没出现在 HELP 的指令不会提交；端口不通、设备不匹配、回包超时会失败，不切回旧命令碰运气。

若旧 SDK 固件的 `commands.json` 与最新源码不匹配，用该固件构建时的源码、工具和词表重新发起检查。不要从当前版本重新生成词表去配旧固件，也不要改用 `--legacy` 绕过核对。

台架要换 APP 时，先对候选镜像和回退镜像分别做一次下面的 `--dry-run`，确认各自的源码、工具和词表能配对，再动串口或写入。干跑的结果只说明主机输入可用，烧后仍须核对设备回报的完整 ELF。

旧设备不回答版本时，先用 `AT`、`AT+HELP` 等只读检查核对设备；确认是旧固件后，才可用 `--legacy` 明确走旧动作。已经收到 SDK 就绪、启动中或 `CLI_DISABLED` 的回包时，这个选项也不能改走旧入口。摄像头探针 `AT+VIDEO PROBE` 的 `OK` 只是入队，`xt.py at` 还会等三帧诊断结果。

需要采一份可查阅的原始日志时，从仓库根目录运行 `device_console.py`。先加 `--dry-run` 可以离线查词表、参数和许可，不打开串口或创建日志：

```sh
python scripts/device_console.py --port COM25 --output ../private-logs/state.log --commands build/at-szpi-diagnostics/generated/commands.json --command "AT+STATE" --dry-run
```

拿掉 `--dry-run` 才会连接端口。执行会改变设备或发起会话的指令前，明确授权那一条：重复使用 `--allow "<完整命令及其具体参数>"`，并用 `--elf-sha256 <64 位 ELF SHA-256>` 指定目标构建。工具会再与设备回报的运行 ELF 核对。已有授权不传给同名的其他子指令，也不自动解锁 CLI。旧方言的 `--allow-…` 开关不适用于 SDK 模式。

即使已授权，`AT+EP SET` 的基地址也不能含用户名、密码、查询参数或片段；`AT+RECOGNITION RECORD` 只接受 1 至 60 的整数秒。参数不合规时，工具先在本地报错，不打开串口。

测两台 SDK 设备时，用 `--commands` 和 `--peer-commands` 分别指定两端词表，再用 `--elf-sha256` 和 `--peer-elf-sha256` 核对两端固件。真实操作还须有相应的 `--allow`，包括结束和清理动作。长回包按不出数据的时长判断是否卡住：`xt.py at` 用 `--wait` 调整，`device_console.py` 用 `--idle-timeout`。两种工具在回包途中收到新开机标记时都停止该命令，不在未经核验的新启动上继续执行；重新连接后先核对固件身份。

统一 SDK 的 `xt.py wx-in` 要在操作手机之前明确允许接听、拒接和挂断（分别传 `--allow "AT+CALL ACCEPT"`、`--allow "AT+CALL REJECT"`、`--allow "AT+CALL HANGUP"`）。`AT+BIND` 必须显示 `phase=1`（已绑定）和完全匹配的设备 ID；`signed` 表示签名重绑，不作为已绑定的指标。`AT+PLATFORM` 的 `online`、`authorized`、`signaling` 均须为 `1`。手机必须停在微信小钛设备列表；工具不会帮你切页面、滚动或使用旧固定坐标。每次拨号前读取两份新画面，中间复核设备身份、空闲和在线状态，只在目标、账号和按钮都匹配时向指定手机发送一次点击。

配置方法见 `devices.example.json` 的 `phone_wx_in`：`device_id`、`adb_serial` 分别与该板和 `phone.adb_serial` 完全一致；`list_png` 是人工核过的原尺寸 PNG，保存在仓外。`card_box` 框住一张目标设备卡；`identity_box` 只框这张卡中能唯一识别设备的信息，优先用完整设备 ID。现场用不重名、不可复用的板名时，必须先核对它与绑定 ID 的对应。`call_box` 框同卡的微信通话按钮，中心就是落点。设备信息和按钮可以上下或左右排，但裁区不能交叠或跨卡。`account_box` 位于卡片外，框列表上可信的当前账号标记。图标、主题色和通用文案不能代替身份。

页面没有可信标记、目标不在当前视口、截图不一致、账号或设备重复/不明，都报告未验证而不点击。截图无法确认目标时，工具在同一台指定手机上只读界面树；只有明确文案才报出登录过期或设备列表为空，列表有设备但首屏没目标时仍是未验证。界面树不会写入测试日志。截图比对以人工审核过的画面为准，不做 OCR 或登录鉴权；分辨率、主题、设备显示名或列表排列变了，需要重新核对参考。账号、图片和绑定 ID 放在本机私有配置，不提交到 Git。

SDK `wx-in` 成功拨号后默认由板端结束，只收尾已确认的当次通话代次：仍在响铃用拒接，已经建连用挂断。未确认来电或代次时只报告缺口。`--phone-hangup` 对 SDK 入呼显式拒绝，避免触碰未经核对的通话页。旧固件 `wx-in` 兼容入口仍走原固定坐标，未获得这套防误拨检查；不能以 SDK 的通过记录替代旧入口验收。

设备呼手机的 `wx-out` 需要本机 Pillow 和 `devices.json` 中选定的 `phone.adb_serial`。把手机留在小钛设备列表；给待测板登记唯一的显示名和人工核过的原尺寸来电 PNG（`phone_wx_out`，文件留在仓外），再核对来电名字、红绿按钮的两个裁区及接听坐标。`wx-out` 仍用原来的授权；SDK 另校验该板的 `device_id` 和当次通话代次。两块裁区都与参考一致才点一次接听。结果分别记录手机接听动作、设备连接和手机结束页中同名通话的非零时长；找不到结束页证据不算通过。`--phone-hangup` 只点当前可识别的同名通话挂断按钮；缺少证据时由设备端按既有授权收尾。手机换机型、分辨率、主题或设备显示名后，重新核对私有参考图。

`python xt.py hex TEXT` 可离线生成 UTF-8 的十六进制参数，空串用 `-`。不要把真密码写进示例、shell 历史或共享日志。注册为 `LOCAL` 的意图只能从设备本地交互入口触发；工具发送 `AT+INTENT binding.forget` / `AT+INTENT provisioning.forget` 仅用于验证 `ERROR: NOT_ALLOWED`，不解绑、不清网络。

## 台架命令

| 命令 | 作用 |
| --- | --- |
| `xt.py scan` | 扫描串口，识别板型、固件、设备号和在线状态 |
| `xt.py regress A B [--full] [--phone]` | 旧固件组合回归。统一 SDK 请分别运行下面的专项，有清楚的授权和结果 |
| `xt.py call A B -s 20` | A 呼叫 B，B 接听，通话 N 秒后 A 挂断；检查两端收发、丢帧、挂断同步、是否回到待机 |
| `xt.py ai DEV -s 60` | 发起 AI 对话，采集收发计数，结束后检查是否回到待机 |
| `xt.py ai-keys DEV` | 通过 AT 注入按键手势，验证开始、打断、结束和回页；实体按键需另验 |
| `xt.py wx-in DEV` / `xt.py wx-out DEV --phone-hangup` | 手机微信呼入设备 / 设备呼叫手机微信 |
| `xt.py trace A B -s 18` | 通话中记录两端帧校验值、电平和门控，结束后对齐找缺口；根因需结合两端日志判断 |
| `xt.py soak DEV --rounds 15` | 统一 SDK 多轮 AI 前后各静置至少 130 秒，核对同一固件、同次启动，再比较空闲内存；旧固件可加 `--peer B` |
| `xt.py watch DEV --hours 2 --every 60` | 只读挂机观察，记录上线状态、重启和内存走势，结束时再取一次样本 |
| `xt.py at DEV "AT+STATE" --commands <词表>` | 向统一 SDK 设备提交已复核的指令；每条含参数的命令用引号包起来 |
| `xt.py log DEV -s 20` | 只看日志 |
| `xt.py boot-check PORT ... --allow USB_CHIP_RESET` | 核对原生 USB 芯片复位后的固件、应用、网络和平台 |
| `xt.py app-stacks DEV ... --expect-task xiaotai` | 读取当前运行任务的历史最低空闲栈，只判定明确指定的任务 |
| `xt.py gate DEV ... --allow-file FILE` | 跑支持且获准的十项基础关，写 Markdown 和 JSON |
| `xt.py snapshot DEV --commands FILE --elf-sha256 SHA --output FILE` | 验证指纹后一次读全常用诊断，输出脱敏 JSON |
| `xt.py echo DEV --commands FILE --elf-sha256 SHA [--volumes 40,60,80] --allow ... --output FILE` | 单讲回声测量：设备自己的喇叭循环播放一个反馈效果（默认来电铃声），每档音量先读静音底噪再读播放时的 `+MEDIA-LEVELS`，算回声压低量和残余回声比底噪高多少。要逐条允许效果的开和关、每档音量和最后恢复原音量那一档，开始前先全部核对，缺一条就不动设备；不写 `--volumes` 时保持当前音量 |
| `xt.py screen DEV out.png --commands <词表>` | 抓支持 `AT+SCREEN` 的统一 SDK 屏幕；旧固件不带 `--commands`，日志可按 [离线解码](../../docs/AT指令.md#截图与离线解码) 处理 |
| `xt.py partition-plan --build-dir DIR --board-table FILE` | 离线比较板上已保存的分区表和已核验构建，给出写入范围建议 |
| `xt.py flash DEV app.bin` | 用 esptool 写应用，默认地址 `0x10000` |
| `xt.py phone shot/tap/text/key/ui` | 通过 adb 操作手机 |

需要保留仓外的台架数据时，可以设置 `XT_STATE_DIR` 为已存在的本地绝对路径；工具从该目录读写 `devices.json`，把自动生成的结果放到 `runs/`。不设置时仍用 `tools/xt/`。这不改变源码、词表、授权或显式传入的文件路径；`regress` 启动的子进程也会继承这个设置。只有显式执行 `scan` 等需要写入的命令才更新台架数据。

表中通话、AI、手机、烧录等会操作设备的任务只在各自获准后执行；不把连上串口当作授权。长任务结束后到 `runs/<时间>-<测试名>/` 看日志和 `summary.json`。`pass` 只覆盖该次脚本断言，不能代替听感、实体触摸或长稳验收。

`flash` 只在 USB 枚举明确识别为 CH343（`1A86:55D3`）时默认用 `921600`，其余仍用 `460800`。显式 `--baud` 只尝试一次。自动档仅在 esptool 明确报告写入前无法设置 `921600`、且再次核对同一 USB 设备和同一镜像后，才以 `115200` 重试一次。身份、镜像、擦写或校验有疑问时不重试。速率选择不代替板型和分区核对，执行中不要拔插或替换镜像。

## 一次跑完该跑的基础检查

用当前固件的产品、完整 ELF 和词表。先用空动作表试跑只读项：

```sh
python xt.py gate COM25 --product xiaotai-szpi-s3 --elf-sha256 "$ELF_SHA256" --allow-file gate.json
```

`gate.json` 只放在本机。把下面的 USB 描述符、完整 ELF 和路径换成实际值，`commands` 相对于清单位置：

```json
{
  "schema": 1,
  "isolated_bench": false,
  "primary": {
    "usb_serial": "CONFIRMED_USB_SERIAL",
    "elf_sha256": "FULL_ELF_SHA256",
    "commands": "build/xiaotai-szpi-s3/sdk-firmware/generated/commands.json",
    "actions": []
  },
  "stack_tasks": ["xiaotai"]
}
```

只在专用台架上做主动项：确认这些设备和手机没有其他人在操作，也不会收到无关的实时呼叫或会话，再设 `"isolated_bench": true`。省略或填写 `false` 时只跑只读项。工具会复核会话归属；现有 AT 命令无法把“匹配代次”和“执行动作”做成原子操作，因此不能用在有并发业务的设备上。

需要复位才列 `USB_CHIP_RESET`；需要 AI 才同时列 `AT+AI START`、`AT+AI STOP`。房间要列 `AT+ROOM OPEN`、`AT+ROOM PTT 1`、`AT+ROOM PTT 0`、`AT+ROOM CLOSE`；相机探针要列 `AT+VIDEO PROBE`，有 AI 视觉诊断且许可启停时可另采一轮视觉。`--peer` 需要独立的 `peer` 条目（同 `primary`，加 `product`），双方各列实际拨号目标、接听、拒绝与挂断。`--phone` 还需独立 `phone` 条目，列 `serial` 和允许的 `PHONE_WX_OUT_ANSWER`；设备仍要给准确的 `AT+CALL INDEX …` 与 `AT+CALL HANGUP`。手机呼入与实时查看须补可核对的手机目标和退出证据，首版不会自动点击。

设备互呼的自动关口要求两台固件都能在 `AT+STATUS media` 的 `+MEDIA-RX` 行报告 `pcm_written` 和 `write_failed`。只有同一次启动、同一通话内的 RTC 收发、下行解码和成功写入音频输出的采样数在两端均增加，才算通过；它不验扬声器的真实听感。任一端缺这组诊断时，不拨号，记为未验证。独立的 `call` 和 `trace` 命令不改变。

报告在 `runs/`：`基础功能关-<产品>.md`、`summary.json` 和每项的 JSON 证据。失败的项目不会阻止后续只读项；主动操作仍须核对设备、完整 ELF、空闲状态及其清理权限。截图内容、无屏提示、实物听感、实时查看及 30 分钟挂机缺证据时留作未验证。退出 `1` 表示有失败，`2` 表示仍有未验证项。

内存和栈一项即使读不到全任务表，也会单列传输和平台可用的工作任务栈水位；实测低于 1024 B 则判失败。两条都有余量仍不代替全应用任务取样和长时挂机。

AI 关口会先查 `AT+STATUS media`，最多等 5 秒让音频就绪、上一场提示音或录音结束。仍未空闲就记录最后一次媒体状态，不发 `START`。会话提前结束时，只有能核对同一次启动、同一轮启动计数和一致的空闲状态，才在证据里记录设备报告的 `terminal_end`。这轮仍判失败，不再发送停止指令，也不把设备的结束原因当成根因或把应用空闲当作功放断电。

## 一次诊断快照

设备离线、通话异常或切换固件后，先读取一份快照，再决定是否需要专项测试：

```sh
python xt.py snapshot DEV --commands /path/to/commands.json --elf-sha256 FULL_SHA256 --output snapshot.json
```

关闭已占用该串口的监视器。工具只打开指定设备一次，先核实固件的完整 ELF 指纹、词表的板型和实际开放的命令，随后读取网络、平台、内存、固定状态节、日志、通话和视频状态。不重启、不配网、不跟随日志，也不需要 `--allow`。不会执行裸 `AT+STATUS`（它会调用自定义诊断节）、读取绑定码或保存原始回显；SSID、IP、MAC、对端标识和可能含私有内容的十六进制字段不会入文件。

文件开头 `summary.issues` 列出明确异常，`summary.unknown` 记录缺测项。日志先验证环容量最多 64 条，最多写入最近 32 条。应用栈只检查已知所有者或固件自报应用子集，低于 1024 B 时给出提示。没有采到 ROM 重启行，最近一次的重启原因就是未知。退出成功只表示写好了这份证据，不等于全功能通过。

## 写固件之前核对分区

用目标板已单独采集的原始分区表文件（从 `0x8000` 开始，正好 `3072` 字节）。构建目录应包含工具生成的 `build-record.json`、`flasher_args.json` 和清单中的原始镜像路径。这个命令使用 Python 3.11 或更新版本，只读这些文件，不连接设备，也不需要 pyserial 或 esptool：

```sh
python xt.py partition-plan --build-dir ../../build/xiaotai-waveshare-p4-rev3/sdk-firmware --board-table ../private/partitions-0x8000.bin
```

| 输出 | 怎么看 |
| --- | --- |
| `app-only` | 表字节一致，APP 能放入唯一的工厂分区。核对现有 bootloader 能否启动新 APP 后，才考虑只写应用。 |
| `three-segment` | 表有变动，但 NVS、PHY 和其它数据分区的记录完全相同，新 APP 放得下。预览只列 bootloader、分区表和 APP，不包含填充整片的合并镜像。 |
| `manual-full-review` | 有数据分区变化，需要另审迁移或整片重装的方案；退出非零，不给写入或擦除命令。整片重装不等于保留数据。 |

格式、校验、边界、镜像或清单不匹配时直接报错；OTA 多槽、加密或特殊标志不在规划范围。离线模式的 `--port` 只用于显示命令参数。`write_authorized` 始终为 `false`，工具不执行预览、写入或擦除。

没有旧表文件时，可单独授权一次 ROM 读表。先确认设备空闲、串口没人占用，并从本机记录独立核对板型、USB 序列号和 MCU 的 ROM 基础 MAC。USB 桥的序列号不能代替 MCU MAC；缺信息时，不猜端口，继续用离线表。以下是 WT99 示例，端口、修订和身份按实物填写：

```sh
python xt.py partition-plan --build-dir "$BUILD_DIR" --port COM15 \
  --read-table --allow PARTITION_TABLE_ROM_RESET \
  --expect-board wt/wt99p461-s1 --expect-chip esp32p4 --revision 3.2 \
  --usb-vid-pid 1a86:55d3 --usb-serial "$USB_SERIAL" --rom-mac "$ROM_MAC"
```

使用已核验的 `esptool 5.3.1` 和 `pyserial`。读表会中断应用并复位；仅在身份、容量和安全状态符合时读 `0x8000` 开始的 3072 字节，不上传 stub，不写 Flash、NVS 或 eFuse。确认身份后退出会请求硬复位。工具不把复位请求当成开机通过，也不会自动换口或重试。原生 USB Serial/JTAG 和 CH343 在支持范围内，其他桥仍用离线表。

## 复位后开机检查

支持 ESP32-S3 和 ESP32-C3 的 Espressif 原生 USB Serial/JTAG，固件须能回报完整 ELF 指纹。选一台已配网、已绑定且空闲的设备，先关闭监视器及其他占用串口的程序。用台架认定的端口、USB 序列号、该镜像的词表和完整 ELF 指纹替换示例值。不要填设备别名，也不要从短启动日志补猜指纹。

```sh
python xt.py boot-check COM25 --usb-serial "$USB_SERIAL" --commands "$COMMANDS_JSON" --elf-sha256 "$ELF_SHA256" --expect-app companion --allow USB_CHIP_RESET --rounds 3 > boot-check.json
```

每轮先重新核对目标和空闲状态，然后只尝试一次 USB 芯片复位。ROM 芯片型号必须和复位前识别到的固件一致；还要核实本次重启原因、运行时间重新起算、芯片和完整 ELF 不变、预期应用启动、网络地址可用及平台在线。复位后板子会用已存配置自行联网；工具不改配置、不擦写、不操作手机。JSON 只记录各阶段状态和时间，不保存原始启动日志、SSID、地址或绑定信息；第一轮失败后不继续复位。

这是芯片复位，不是整板断电，也不能证明屏幕、听感或 RTC 会话正常。立创 S3 的默认无 AT 镜像、微雪 P4 的 CH343 和其他串口桥接板没有这条自动复位门禁，继续分别做被动启动或人工台架复核。

## 看任务栈还剩多少

在要检查的场景中读取一次 `AT+STATUS tasks`。工具先用该固件的 `commands.json` 核对板型和完整运行 ELF，再收任务表。不重启设备，也不代为开启 AI 或私有探针。若固件未启用 trace，会再查看只读的 `AT+STATUS port`；仅当它明确回报 `tasks_scope=application_subset unit=bytes`，才采用已采到的任务子集。

```sh
python xt.py app-stacks COM25 --commands "$COMMANDS_JSON" --elf-sha256 "$ELF_SHA256" --expect-task xiaotai --min-free 1024 > app-stacks.json
```

- `--expect-task` 按当前构建和场景的实际任务名选，可重复填写。默认门槛 `1024` 字节，正好等于门槛算通过。`pass` 只覆盖这些明确列出的任务。
- `selected_min_free_bytes` 是指定任务里已读到的最低值，`observed_min_free_bytes` 是收到的所有有效数值中的最低值。两者是各任务创建以来的历史水位。全表会混有系统及厂商任务，不按名称前缀猜测归属。
- 加 `--workers` 会在同一已核验的串口会话中另读 `AT+LINK`、`AT+PLATFORM`，在 `worker_watermarks` 里报告传输和平台任务的水位。它们不混入任务表，也不改变原有退出码。未提供、未采到或返回 0 时记为 `not_measured`（提供者也用 0 表示未采样）；低于门槛记为 `below_threshold`。失败会话后也能按同样方式取数，但只代表实际走过的路径，不等于完整会话峰值。
- 没有选任务、固件没有可用的只读任务表、回复不完整、指定任务未采样或同名无法区分时，结果标 `not_measured`，不会给绿。已确认的水位低于门槛标 `fail`；这两种结果都退出非零。

整应用的任务普查、已经退出的任务和未运行的业务均未验证。默认固件不提供上述 `port` 任务子集；脚本不主动开启采样，也不拿单个 UI 水位补齐任务表。

## 统一 SDK 多轮内存对照

选一台已配网、已绑定、没有其他会话的设备，测试期间不要从手机或屏幕发起操作。指定对应的 `commands.json` 和完整 ELF SHA-256，分别授权开始和停止。统一 SDK 的设备通话单独跑 `call`，不用 `soak --peer`。

```sh
python xt.py soak COM25 --commands "$COMMANDS_JSON" --elf-sha256 "$ELF_SHA256" --allow "AT+AI START" --allow "AT+AI STOP" --rounds 10 --secs 8
```

首轮前、末轮回到空闲后各静置至少 130 秒，期间工具不向设备发指令。每轮和两次内存读数都核对运行 ELF 和启动连续性。十轮验收要求静置后的内部空闲下降不超过 256 B，超过即返回非零；其他轮数仅报告这项差值。最大连续块另报，需用同口径旧固件数据比较。任何一轮不完整、发生重启、缺少可比样本或未记录双向媒体增量，也返回非零；已有证据保留在 `runs/`。

`free_delta_bytes` 和 `largest_delta_bytes` 分别记空闲总量与最大连续块，均为末次减首次。`internal`、`bulk`、`dma` 的容量可能重叠，不相加。`min_free` 和 `min_largest` 是自开机以来的低点。`pass` 只说明达到采集和轮次门槛，不作泄漏或听感结论。

连续挂机用 `watch`，不发起会话。对 SDK 固件同样传对应的 `--commands` 和 `--elf-sha256`。工具在打开串口、创建运行目录之前校验本地词表；如果词表与当前检出的源码不符，立即退出。设备重启后仍会重新核对板型、运行 ELF 和指令。`window_completed` 需要跑满指定时长并完成收尾采样；提前中断、采样失败、掉线或观察到重启都保留已有日志并退出非零。`pass` 只覆盖实际采样和串口观察，不代表两个采样之间完全没有掉线。

## 用固定序列测 UDP 接收

这项只测电脑到开发板的 UDP 接收路径，不经过 TiRTC 或播放队列。需要同一测试局域网、已核对的测试板和带 `NET_PATTERN_PROBE` 的诊断镜像。默认产品镜像不提供 `PROBE2`，旧 `AT+NET PROBE` 不受影响。

先离线确认标准图样 `burst5`：开头一批发 5 包，相当于 100 ms 的音频量，之后每隔 20 ms 发一包。

```sh
python xt.py udp-pattern plan --flow 77 --seed 123 --count 100 --pattern burst5
```

在第一个终端按精确命令授权接收，再在第二个终端按接收窗口的 `ready` 行发送。下面的变量应是两台已确认的 RFC1918 私网测试设备地址。换轮时用新的流号，种子、包数和图样保持一致。

```sh
PROBE="AT+NET PROBE2 $PC_IP 55161 77 123 100 160"
python xt.py at COM9 "$PROBE" --commands "$COMMANDS_JSON" --elf-sha256 "$ELF_SHA256" --allow "$PROBE"

python xt.py udp-pattern send --local "$PC_IP:55161" --peer "$BOARD_IP:49161" \
  --flow 77 --seed 123 --count 100 --pattern burst5 \
  --allow "XTP2 SEND $PC_IP:55161 -> $BOARD_IP:49161"
```

接收窗口为 12 秒，只收指定源 IP 和端口，不回包。命令省略 `--pattern` 仍用匀速 `paced`；压力图样可用 `burst10`、`burst20`、`burst40` 或 `burst50`（开头一批相当于 1 秒的音频量），另有固定种子的 `jitter`。正文默认 160 B。主机若被系统调度拖慢，会停止并报告已发包数，不追赶补发。只有发送者确认传出了全部包且板端 `rc=0 complete=1`，才比较计数：

| 结果字段 | 含义 |
| --- | --- |
| `rx / good / loss` | 窗口记录的不同序号、通过长度和内容校验的不同序号、结算时窗内缺失。损坏包和过窗迟到单列，不能把 `loss` 直接当无线丢包。 |
| `ooo / dup / late / bad / len` | 窗内乱序、重复、过窗迟到、内容不匹配、长度不匹配。 |
| `max_gap / burst / overflow / bins` | 最长连续缺口、5 ms 内到达峰值、峰值超出定长窗口只报下界、相邻到达间隔分桶（<1、<5、<15、<25、≥25 ms）。 |

相同图样每组至少跑两次。单独记录主机调度和实际首批持续时间；接收端时间是本机相对时间，不能用来算单向延迟。电脑的 `receive` 使用相同校验规则；设备主动发送和 TiRTC 路径另验。

## 新媒体引擎的内存对照

在同一台、同一次启动的测试固件上，分别于空闲、持续双向传输中、挂断并恢复空闲后采集一次或多次回包。每份日志都包含 `AT+STATUS media`、`AT+STATUS tasks` 和 `AT+MEM`，单独留在本机 `runs/`。例如，经台架授权后采集空闲样本：

```text
python xt.py at DEV "AT+STATUS media" "AT+STATUS tasks" "AT+MEM" --commands ../../build/at-szpi-diagnostics/generated/commands.json > idle.log
```

采集结束后，可拔掉设备离线汇总。下例还会检查合并形态只建采播、发送两个媒体任务；已编码输入形态用 `--topology encoded`。

```text
python media_memory.py --idle idle.log --active active-1.log active-2.log --after after.log --topology folded
```

回包里的当前空闲和最大连续块用来对照三个阶段；`AT+MEM` 的 `min_free` 另列为开机以来的低水位。已确定验收下限时，再传入 `--min-free <字节数>`；脚本缺少原生低水位时会报错，不拿离散快照代替。脚本不打开串口，也不判断听感、长时间泄漏或整机能否切换。

统一 SDK 下，普通设备拨号用 `AT+CALL DEVICE <id>`；微信联系人按 `AT+CONTACTS` 的 `source=wechat` 和 `index=` 选，再用 `AT+CALL INDEX <n>`。如果没有对应的 HELP 项，先看设备当前的应用和能力，别拿另一个板子的命令试。

出了问题怎么判断是哪一层，见 [测试与诊断](../../docs/测试与诊断.md) 和 `.claude/skills/xiaotai-verify/`。
