# AT 指令

[返回首页](../README.md) · [开发指南](开发指南.md) · [台架工具 xt](../tools/xt/README.md)

本页适用于开放物理 AT 的固件。IDF 6.0 默认固件中，CoreS3-Lite 用 USB，微雪 P4 3.5 rev3 用 UART0 / 115200，立创 S3 不开放物理 AT。拿不准设备上是哪一版，先核对产物或启动日志；串口有完整 `AT+VERSION` 回包且带 `at=1`，再按本页操作。

## 第一次连板

1. 核对串口对应的设备，打开**主控制台**。常用速率是 115200，以板卡的实际配置为准。USB 若只接日志口，能看日志也不代表能发指令。
2. 一次发一条，各条以回车结束。先发 `AT`，看到单独一行 `OK` 再发下一条。`AT+VERSION` 的 `+VERSION:` 是板型、固件版本和平台，`+BUILD:` 是 ELF SHA-256；别拿芯片名称或镜像文件的哈希替代它。
3. 用 `AT+HELP` 查当前可用的指令。立创 S3 新产品的默认配置不登记物理 AT 控制台，仍会输出启动日志。没有 AT 回包不能判断启动失败。看到 `CLI_DISABLED` 先核对测试配置，不要让脚本自行解锁。
4. 要查状态，先用 `AT+STATE`、`AT+NET`，再按需要查 `AT+STATUS` 的某个段。动作返回 `OK` 一般只表示已受理。例如拨号后继续看 `AT+CALL` 或通话事件，不凭 `OK` 判断已经接通。

```mermaid
flowchart LR
    Host[开发机：终端、xt] --> Serial[本地 USB / UART]
    Serial --> AT[AT 引擎：行解析、权限、收尾]
    AT --> Local[本地查询：版本、日志、内存]
    AT --> Owner[框架所有者：状态、设置、业务动作]
    Owner --> Service[原来的网络、媒体、界面和会话模块]
```

指令在本机串口执行，没有网络 AT 入口。字节源接在端口层；命令和诊断由功能单元登记，新板只换硬件适配时不用重写命令。无屏、无相机或没选某个模块的产品，只使用对应能力支持的命令。

## 常用检查

| 想知道什么 | 从这里开始 | 看哪里 |
| --- | --- | --- |
| 板型、固件、当前活动 | `AT+VERSION`、`AT+STATE` | `board=`、`elf_sha256=`、`activity=`；`none` 表示当前无前台活动 |
| Wi-Fi 已连，服务也在线了吗？ | `AT+NET`、`AT+PLATFORM` | 前者看 `connected=`，后者看 `online=`，两者不能互相替代 |
| 绑定流程是否正常？ | `AT+BIND` | `phase=`、`verification=`、`left=` 和 `device=`；不要发公开日志中的验证码和设备号 |
| 通话有没有开始、接通或结束？ | `AT+CALL`、`AT+LOG` | 看当前状态和同一 `gen=` 的 `outgoing`、`incoming`、`connected`、`ended`；计数从启动累计时取前后差值 |
| 相机是否出帧？ | `AT+VIDEO`、`AT+VIDEO PROBE` | 先查业务的采集和发送计数；空闲时再做本地探针。`OK` 只表示受理，在异步的 `+VIDEO-PROBE:` 看到 `result=passed frames=3 error=NONE` 才算探针通过 |
| 内存或栈见底了吗？ | `AT+MEM`、`AT+LINK`、`AT+STATUS tasks` | 堆的 `min_free=`、`min_largest=`；传输任务看 `worker_stack_free=`；任务表可用时，所有者看 `name=xiaotai` 的 `stack_min_free=` |
| P4 的联网协处理器是哪一版？ | `AT+COMPANION` | 查当前缓存身份；需要重新读取时看 HELP 是否提供 `LIVE`。微雪为 C6，WT99 为 C61，各自的身份不是 P4 主固件身份 |
| 界面目前呈现什么？ | `AT+UI`、`AT+SCREEN` | 页面以 `view=` 为准；截图不证明物理 LCD 或触摸正常 |

排查黑屏时，`AT+VIDEO` 的 `+VIDEO:` 概览用 `up_y_state`、`up_y_mean`、`up_y_n` 报告上行相机打开后首个有效原始帧的亮度；降分辨率重开或重新打开相机会重新统计。空闲执行 `AT+VIDEO PROBE`，可在原最终结果前看到 `+VIDEO-PROBE-LUMA:` 的 `state`、`y_mean`、`samples`。状态为 `valid` 时，均值来自可见区域最多 64 个网格点的原始 Y 分量（0 到 255，未归一化，不计填充行）。均值接近黑电平但采集、发送计数在走，意味着有帧但画面暗，不能单凭这个值判断镜头是否被挡。`no_frame`、`invalid` 和 `unavailable` 分别表示未拿到帧、帧布局无效和格式没有可直接采样的 Y 分量；这些状态的均值为 `-1`、样本数为 `0`，不会改变探针或上行原有结果。

回包的终行是独立的 `OK` 或 `ERROR: ...`；裸日志中的词不算终行。脚本遇到超时把本次记为未完成。若发的是动作，先读状态确认没有执行，再决定是否重试。长截图或追踪看输出有没有持续推进，不按整次传输设置一个短倒计时。

任务表依赖构建提供的 FreeRTOS 任务追踪。`AT+STATUS tasks` 返回 `supported=0` 表示这版不提供全任务读数，不代表任务数或栈余量为零。`AT+LINK` 的读数只覆盖传输工作任务，不能代替所有任务的栈检查。

在支持 RTC 快速内存的芯片上，`AT+MEM RTC` 单独读取这块堆的当前空闲、最大连续块和开机以来最低空闲，单位是字节。读到全零是有效结果；芯片不支持时返回 `ERROR: NOT_SUPPORTED`。原 `AT+MEM` 和 `AT+STATUS mem` 的回包不变。

`AT+PLATFORM` 和 `AT+STATUS platform` 先给一行 `+PLATFORM:`，再给同一快照的四行 `+PLATFORM-STATS:`，依次是连接生命周期、请求与传输、信令与绑定、档案与工作任务。每行不超过 512 个字符，当前 38 个计数键按原顺序各出现一次。SDK 串口工具在收到 `OK` 后按键合并校验，仍按原文显示和保存回包；缺行、重复键或 `ERROR` 都不能作为完整快照。

`AT+VIDEO` 的 `+VIDEO-UP-RECOVERY:` 记录业务会话中首帧超时后的相机恢复。`reopens` 是重开次数，`recovered` 是重开后拿到原始帧的次数，`exhausted` 是重开后仍失败的次数。每次首帧等待最多尝试一次，保留失败原因；拿到原始帧也要继续看 `sent=` 和对端画面。产品可以用 `parameters.video.disable_first_frame_recovery` 关闭。这个机制不改变本地探针的通过条件。

选择了伴侣应用的产品可用 `AT+STATUS app.companion` 查应用是否在前台、当前页和交互状态，以及是否停在失败结果页（`error_held=`）。`page=-` 表示应用没在呈现页面，例如它在后台，或产品没有屏幕。要查显示是否就绪、屏幕是否点亮，仍读 `AT+UI`。退出结果页继续用已登记的 `AT+CALL DISMISS`；已有通话不会因此挂断。

立创 S3 和 CoreS3-Lite 响铃时，用 `AT+CALL REJECT` 拒接设备或微信来电。此时发送 `AT+CALL HANGUP` 会返回 `ERROR: INVALID_STATE`，来电仍保留。外呼、连接中和通话中继续用 `AT+CALL HANGUP`。

立创 S3 和 CoreS3-Lite 的 `AT+CALL DEVICE <device-id>` 在最近一次有效名单的前 8 个有效联系人中查找，必须恰好有一个同 ID 的在线设备。没有匹配返回 `ERROR: NOT_FOUND`，重复匹配返回 `ERROR: INVALID_STATE`；名单不可用时也不拨号。`AT+CALL INDEX <n>` 不受前 8 项的限制，但要先用 `AT+CONTACTS` 核对当前序号。序号格式错误或不存在会返回 `ERROR: INVALID_ARG`；名单刷新后请重新核对。

`AT+CALL ACCEPT`、`REJECT`、`HANGUP` 或 `MUTE` 没有可操作的通话时，返回 `ERROR: INVALID_STATE NO_OWNER`。拨号返回 `OK` 后继续读 `AT+CALL`。会话结果看 `+CALL:` 的 `error=`，与发命令时的终行分开。

两款 S3 处理声音识别时，语音结果下载遇到请求队列满，报告 `TIMEOUT`（`263`）；内存申请失败仍报告 `NO_MEM`（`257`）。上传的应答正文收到一部分后中断时，报告 `INVALID_RESPONSE`（`264`），保留待核验任务，不自动重新上传。对已经受理的识别操作，用 `AT+RECOGNITION` 读 `+RECOGNITION-STATE:` 的 `error=`，不要把操作命令的 `OK` 当成完成。

回声门调试只在开启 `MEDIA_TEST_COMMANDS`、且 HELP 列出命令的构建中使用。`AT+AUDIO ECHO 0` 临时关闭，`AT+AUDIO ECHO 1` 恢复；从下一采集块生效，不重开会话、不保存到设置。用 `AT+STATUS media` 查 `+ECHO: enabled=`。产品未配置回声门时，返回 `ERROR: NOT_SUPPORTED`。

## 怎样编译

SDK 的物理控制台由产品配置决定，命令以该次固件的 `AT+HELP` 为准。微雪 P4 的普通固件不开放 `AT+SCREEN`。立创 S3 需要串口调试时，用独立的诊断变体构建：

```sh
python scripts/build.py lckfb/szpi-esp32s3 --entry sdk --variant diagnostics
```

产物在 `build/xiaotai-szpi-s3/sdk-firmware--diagnostics/`，不会覆盖默认镜像。显式开启物理 AT 的变体用主 USB Serial/JTAG 或 UART；只有 USB CDC 日志或次级输出时，构建会报错。

## 词表与权限

台架使用与运行固件对应的 `commands.json`。已有构建目录就用那一份；只想离线检查选型时，可以单独生成立创 S3 的诊断词表：

```sh
python scripts/sdk_selection.py --product products/xiaotai-szpi-s3/product.json --variant diagnostics --out-dir build/at-szpi-diagnostics
```

这条命令只生成选择文件，不编译或烧录固件。词表记录产品选择和每种写法的权限，在线发送还须核对设备 `AT+HELP`。表里有、HELP 里没有，先查固件身份、变体、编译开关和运行条件，不要照表硬发。

如果工具提示 `commands.json` 与当前仓库的产品清单不同，回到生成板上固件的源码版本，用它的串口工具和构建目录里的词表。即使是同一产品，新增一条只读指令也会让两版词表不同；在新版本重做词表不会更新板上的固件。

- `read`：查询或导出。少数查询需要服务已在线，例如声音识别配置，不能拿来验证设备完全离线时可用。
- `action`：执行动作，可能占用会话、麦克风或扬声器。只在获准的台架和测试账号上用。
- `guarded`：逐项允许，例如换网、切换测试环境、重启、写设置、更新协处理器。`AT+SET [<name>]` 和写入用的 `AT+SET <name> <value>` 是不同写法，许可不互通。框架仍会检查范围、忙闲、注册权限和产品允许名单。

用台架脚本发动作前，除了词表和实时 HELP，还要提供确切的 `--allow` 和目标 ELF SHA-256；命令行示例见 [xt 工具说明](../tools/xt/README.md#统一-sdk从读状态开始)。

Wi-Fi 凭据等文本按各命令的十六进制语法发送。十六进制是编码，不能保护密码；不要把真密码、令牌、绑定信息或原始日志放进 Git。

切换测试环境只用本地串口。先用 `AT+EP` 查 `active`、`saved` 和 `next`；获准后用 `AT+EP SET <base>` 或 `AT+EP RESET`。`OK` 只表示受理，保存成功会发 `+EP: result=saved` 并自动重启，随后再查环境和平台上线。等待期间可用 `AT+EP CANCEL`，以 `+EP: result=cancelled` 确认完成；正在保存时不能撤回。不改网站链接，也不顺带改 MQTT 协议。

| 本次请求未受理 | 串口结果 |
| --- | --- |
| AT 命令队列已满（包括切换和取消） | `ERROR: NO_MEM`；本次请求未执行，也不改变已有的切换状态 |
| 已有环境切换，或配网验证正占用网络 | `ERROR: INVALID_STATE` |
| 地址无效 | `ERROR: INVALID_ARG` |
| 正在通话等前台活动占用设备 | `ERROR: BUSY`；切换请求不会主动结束原会话 |

WT99 处于网络维护期间、尚未进入待重启状态时，仍允许用 `AT+COMPANION SEND INFO` 读取 C61 信息；这不放行 `LIVE`、更新、回退或其他直发命令。已受理的更新如果完整下载后摘要不符，会报告 `+COMPANION-UPDATE: stage=failed percent=0 error=265`（`INVALID_CRC`），不会写入镜像。

### 截图与离线解码

用 `AT+SCREEN` 单独抓一帧，保留原始回包，包括 `+SCREEN:`、连续的 `+SCREEN-DATA:`、`+SCREEN-END:` 和最后的 `OK`。日志留在仓库外，再离线解码：

```sh
python scripts/decode_screen.py <截图日志> <输出.png> --sdk-only
```

块序号、尺寸、字节数或者 FNV 校验不匹配时，先重查日志，别拿残帧当屏幕故障。

## 统一词表

这张表汇总仓库当前产品与变体选中的单元清单。它包含候选写法，**不是任何一台设备的实时清单**。例如 `AT+PORT PROBE` 只有端口提供探针时才登记；支持截图的构建还要有对应显示能力。使用时以该台设备的 HELP 和返回码为准。

<!-- GENERATED: sdk AT vocabulary -->

| 写法 | 权限 | 登记单元 | 相关构建选项 |
| --- | --- | --- | --- |
| `AT` | `read` | `at` | - |
| `AT+AI` | `read` | `ai_talk` | - |
| `AT+AI INTERRUPT` | `action` | `ai_talk` | - |
| `AT+AI START` | `action` | `ai_talk` | - |
| `AT+AI STOP` | `action` | `ai_talk` | - |
| `AT+AI SUBMIT` | `action` | `ai_talk` | - |
| `AT+AUDIO` | `read` | `media` | - |
| `AT+AUDIO DUMP` | `read` | `media` | `XIAOTAI_MEDIA_TEST_COMMANDS` |
| `AT+AUDIO ECHO <0\|1>` | `action` | `media` | `XIAOTAI_MEDIA_TEST_COMMANDS` |
| `AT+AUDIO PLAY` | `action` | `media` | `XIAOTAI_MEDIA_TEST_COMMANDS` |
| `AT+AUDIO RECORD` | `action` | `media` | `XIAOTAI_MEDIA_TEST_COMMANDS` |
| `AT+AUDIO SENDTONE` | `action` | `media` | `XIAOTAI_MEDIA_TEST_COMMANDS` |
| `AT+AUDIO STOP` | `action` | `media` | `XIAOTAI_MEDIA_TEST_COMMANDS` |
| `AT+AUDIO TONE` | `action` | `media` | `XIAOTAI_MEDIA_TEST_COMMANDS` |
| `AT+AUDIOTRACE?` | `read` | `media` | `XIAOTAI_MEDIA_TRACE_ARRIVALS` |
| `AT+BIND` | `read` | `binding` | - |
| `AT+BIND CLEAR` | `guarded` | `binding` | `XIAOTAI_BINDING_DEV_COMMANDS` |
| `AT+BIND REPLAY` | `action` | `binding` | - |
| `AT+BIND RETRY` | `action` | `binding` | - |
| `AT+BIND SET <device-id> <key-hex> [<client-id>]` | `guarded` | `binding` | `XIAOTAI_BINDING_DEV_COMMANDS` |
| `AT+CALL` | `read` | `call`、`video_call`、`wechat` | - |
| `AT+CALL ACCEPT` | `action` | `call`、`wechat` | - |
| `AT+CALL CAMERA 0\|1` | `action` | `video_call` | - |
| `AT+CALL DEVICE <device-id>` | `action` | `call` | - |
| `AT+CALL DISMISS` | `action` | `app.companion` | - |
| `AT+CALL HANGUP` | `action` | `call`、`wechat` | - |
| `AT+CALL INDEX <n>` | `action` | `call`、`wechat` | - |
| `AT+CALL MUTE 0\|1` | `action` | `call`、`wechat` | - |
| `AT+CALL REJECT` | `action` | `call`、`wechat` | - |
| `AT+CALL VIDEO <n>` | `action` | `video_call` | - |
| `AT+CALL WX <contact-id>` | `action` | `wechat` | - |
| `AT+CLI` | `read` | `at` | - |
| `AT+CLI OFF` | `action` | `at` | - |
| `AT+CLI ON` | `action` | `at` | - |
| `AT+COMPANION` | `read` | `net` | - |
| `AT+COMPANION LIVE` | `read` | `net` | - |
| `AT+COMPANION RETRY` | `action` | `net` | - |
| `AT+COMPANION ROLLBACK` | `guarded` | `net` | - |
| `AT+COMPANION SEND <word>` | `action` | `net` | - |
| `AT+COMPANION UPDATE <url> <sha256-hex>` | `guarded` | `net` | - |
| `AT+CONSOLE` | `read` | `at` | - |
| `AT+CONTACT ACCEPT <id>` | `action` | `contacts` | - |
| `AT+CONTACT DELETE <id>` | `action` | `contacts` | - |
| `AT+CONTACT REJECT <id>` | `action` | `contacts` | - |
| `AT+CONTACT REMARK <id> <hex\|->` | `action` | `contacts` | - |
| `AT+CONTACT REQUEST <id>` | `action` | `contacts` | - |
| `AT+CONTACTS` | `read` | `contacts` | - |
| `AT+CONTACTS PAGE <ALL\|kind> [<page>]` | `action` | `contacts` | - |
| `AT+CONTACTS PENDING [<page>]` | `action` | `contacts` | - |
| `AT+CONTACTS REFRESH` | `action` | `contacts` | - |
| `AT+DOOR` | `read` | `app.door` | - |
| `AT+DOOR BACK` | `action` | `app.door` | - |
| `AT+DOOR DEVICES` | `action` | `app.door` | - |
| `AT+DOOR HOME` | `action` | `app.door` | - |
| `AT+DOOR NEXT` | `action` | `app.door` | - |
| `AT+DOOR PREVIOUS` | `action` | `app.door` | - |
| `AT+DOOR SENDER` | `guarded` | `app.door` | - |
| `AT+DOOR VIEWER` | `guarded` | `app.door` | - |
| `AT+EP` | `read` | `environment` | - |
| `AT+EP CANCEL` | `action` | `environment` | - |
| `AT+EP RESET` | `guarded` | `environment` | - |
| `AT+EP SET <base>` | `guarded` | `environment` | - |
| `AT+FEEDBACK` | `read` | `feedback` | - |
| `AT+FEEDBACK CANCEL` | `action` | `feedback` | - |
| `AT+FEEDBACK FIRE <effect> [FORCE]` | `action` | `feedback` | - |
| `AT+FEEDBACK GROUP <n> 0\|1` | `action` | `feedback` | - |
| `AT+FEEDBACK SET <state> 0\|1` | `action` | `feedback` | - |
| `AT+FEEDBACK STATES` | `read` | `feedback` | - |
| `AT+HELP` | `read` | `at` | - |
| `AT+INTENT <intent> [<value> [<text-hex>]]` | `guarded` | `at` | `XIAOTAI_AT_INTENT_COMMAND` |
| `AT+KEY` | `read` | `interaction` | - |
| `AT+KEY CLICK [<role>]` | `action` | `interaction` | - |
| `AT+KEY DOUBLE [<role>]` | `action` | `interaction` | - |
| `AT+KEY HOLD [<role>]` | `action` | `interaction` | - |
| `AT+LINK` | `read` | `transport` | - |
| `AT+LINK TRACE` | `read` | `transport` | - |
| `AT+LOG [<after>]` | `read` | `at` | - |
| `AT+LOG FOLLOW 0\|1` | `action` | `at` | - |
| `AT+LOG STATS` | `read` | `at` | - |
| `AT+LOG TOPICS` | `read` | `at` | - |
| `AT+MEM` | `read` | `at` | - |
| `AT+MEM RTC` | `read` | `at` | - |
| `AT+MEMTRACE` | `read` | `transport` | `XIAOTAI_TRANSPORT_MEMTRACE` |
| `AT+NET` | `read` | `net` | - |
| `AT+NET CANCEL` | `action` | `provisioning` | - |
| `AT+NET DIAG` | `read` | `net` | - |
| `AT+NET DISCONNECT` | `action` | `net` | - |
| `AT+NET FORGET` | `guarded` | `net` | `XIAOTAI_NET_CONSOLE_FORGET` |
| `AT+NET LIST` | `read` | `net` | - |
| `AT+NET PROBE` | `action` | `net` | - |
| `AT+NET PROBE2 <peer-ipv4> <peer-port> <flow> <seed> <count> <bytes>` | `action` | `net` | `XIAOTAI_NET_PATTERN_PROBE` |
| `AT+NET PROVISION FORGET [FORCE]` | `guarded` | `provisioning` | `XIAOTAI_NET_CONSOLE_FORGET` |
| `AT+NET PROVISION [FORCE]` | `action` | `provisioning` | - |
| `AT+NET RETRY` | `action` | `net` | - |
| `AT+NET SCAN` | `action` | `net` | - |
| `AT+NET STORE <ssid-hex> <password-hex\|->` | `guarded` | `net` | - |
| `AT+NET TRACE [START\|STOP]` | `action` | `net` | `XIAOTAI_NET_PATH_TRACE` |
| `AT+NET WIFI <ssid-hex> <password-hex\|->` | `guarded` | `net` | - |
| `AT+PLATFORM` | `read` | `platform` | - |
| `AT+PLATFORM RECONNECT [<1..50>]` | `guarded` | `platform` | `XIAOTAI_PLATFORM_RECONNECT_COMMAND` |
| `AT+PORT PROBE [<name>]` | `guarded` | `at` | - |
| `AT+RECOGNITION` | `read` | `recognition` | - |
| `AT+RECOGNITION ARCHIVE <request-id> <revision>` | `action` | `recognition` | - |
| `AT+RECOGNITION CANCEL` | `action` | `recognition` | - |
| `AT+RECOGNITION CONFIG` | `read` | `recognition_config` | - |
| `AT+RECOGNITION DISMISS <request-id> <item-id>` | `action` | `recognition` | - |
| `AT+RECOGNITION QUERY` | `action` | `recognition` | - |
| `AT+RECOGNITION RECORD <seconds:1..60>` | `action` | `recognition` | - |
| `AT+RECOGNITION RESUME <request-id>` | `action` | `recognition` | - |
| `AT+RESTART` | `guarded` | `at` | `XIAOTAI_AT_RESTART_COMMAND` |
| `AT+RESTART FORCE` | `guarded` | `at` | `XIAOTAI_AT_RESTART_FORCE` |
| `AT+ROOM` | `read` | `room` | - |
| `AT+ROOM CLOSE` | `action` | `room` | - |
| `AT+ROOM CONNECT` | `action` | `room` | - |
| `AT+ROOM CREATE [<4-digit-password>]` | `action` | `room` | - |
| `AT+ROOM DISCONNECT` | `action` | `room` | - |
| `AT+ROOM JOIN <6-digit-code> [<4-digit-password>]` | `action` | `room` | - |
| `AT+ROOM LEAVE` | `action` | `room` | - |
| `AT+ROOM OPEN` | `action` | `room` | - |
| `AT+ROOM PTT 0\|1` | `action` | `room` | - |
| `AT+ROOM REFRESH` | `action` | `room` | - |
| `AT+SCREEN [<display>]` | `read` | `ui` | `XIAOTAI_UI_CAPTURE` |
| `AT+SET <name> <value>` | `guarded` | `at` | `XIAOTAI_AT_SETTING_WRITE` |
| `AT+SET [<name>]` | `read` | `at` | - |
| `AT+SOUND` | `read` | `media` | `XIAOTAI_MEDIA_SOUND_COMMANDS` |
| `AT+SOUND PLAY <name> [<6 digits>]` | `action` | `media` | `XIAOTAI_MEDIA_SOUND_COMMANDS` |
| `AT+SOUND STOP` | `action` | `media` | `XIAOTAI_MEDIA_SOUND_COMMANDS` |
| `AT+STATE` | `read` | `at` | - |
| `AT+STATUS [<section>]` | `read` | `at` | - |
| `AT+TRACE ARM [<50..limit>]` | `action` | `media` | `XIAOTAI_MEDIA_TRACE` |
| `AT+TRACE DUMP` | `read` | `media` | `XIAOTAI_MEDIA_TRACE` |
| `AT+TRACE OFF` | `action` | `media` | `XIAOTAI_MEDIA_TRACE` |
| `AT+UI` | `read` | `ui` | - |
| `AT+UI ACTIONS` | `read` | `ui` | - |
| `AT+UI PERF` | `read` | `ui` | - |
| `AT+UI POINTER <x> <y> <0\|1>` | `action` | `ui` | - |
| `AT+UI REDRAW` | `action` | `ui` | - |
| `AT+UI SLEEP` | `guarded` | `ui` | - |
| `AT+UI SWIPE <x> <y> <x2> <y2> <ms>` | `action` | `ui` | - |
| `AT+UI TAP <x> <y>` | `action` | `ui` | - |
| `AT+VERSION` | `read` | `at` | - |
| `AT+VIDEO` | `read` | `video` | - |
| `AT+VIDEO CAPTURE START\|STOP\|STATUS\|CLEAR\|DUMP` | `action` | `video` | - |
| `AT+VIDEO PROBE` | `action` | `video` | - |
| `AT+WAKE` | `read` | `wake_engine` | - |
| `AT+WAKE SELFTEST` | `read` | `wake` | - |
| `AT+WAKE TEST <phrase-hex>` | `action` | `wake` | - |
| `AT+WAKE THRESHOLD <50..990>` | `action` | `wake_engine` | - |
| `AT+WAKE WATCH [<5..30>]` | `read` | `wake_engine` | - |

<!-- END GENERATED: sdk AT vocabulary -->

维护指令时改所属组件的 `commands` 清单和实际登记，再运行 `python scripts/generate_at_reference.py --write`。`--check` 只核对是否漏更新，不写文件。
