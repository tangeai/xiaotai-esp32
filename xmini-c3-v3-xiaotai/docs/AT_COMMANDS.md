# C3 AT 指令

USB Serial/JTAG，115200。每条指令以 CR、LF 或 CRLF 结束，区分大小写，参数使用空格分隔。通常最多 95 个字符，例外见下一段；过长整行丢弃并返回 `ERROR: LINE_TOO_LONG`，不会执行被截断的前缀。

`AT+WIFI ` 前缀允许最多 255 个字符，`AT+CONTACT ` 为 767 个；其他命令为 95 个。Wi-Fi 参数使用十六进制编码，不是加密；只在可信的本地 USB 串口发送，主机日志必须隐藏整条凭据。

| 指令 | 用途 |
| --- | --- |
| `AT` / `AT+HELP` | 连通检查 / 指令列表 |
| `AT+VERSION` | 板型、版本、MAC、完整 ELF SHA-256；不输出设备密钥 |
| `AT+STATUS` | 页面、选择、音频状态、帧数、峰值、音量/增益、内部堆及完整性；独立 `HEAPCAP` 为能力堆分组，`TASKS` 为音频/平台任务栈水位 |
| `AT+PRODUCT` | 引导阶段、错误、真实验证码/剩余秒数、UTC、设备 ID、平台/RTC 状态 |
| `AT+WIFI <SSID_HEX>,<PASSWORD_HEX>` | 仅配网热点开启时可用；与配网页共享提交/验证入口，取得 IP 后才保存。开放网络的密码段留空 |
| `AT+WIFI SETUP` | 仅业务空闲时请求重新配网；冻结新设置，待已有设置真正保存且平台 HTTP 排空后持久化入口标记并重启，保留旧 Wi-Fi 和设备绑定 |
| `AT+WIFI CANCEL` | 取消尚未持久化的重新配网请求；查询 `AT+PRODUCT` 的 `WIFI_SETUP phase=0` 确认取消完成 |
| `AT+NETWORK` | 查询已连接 STA 的 IP、网关、RSSI 和信道，不输出 SSID/密码 |
| `AT+BIND REPLAY` | 仅有效的真实验证码阶段可重播；不生成新码、不改绑定状态 |
| `AT+RETRY` | 显式重试离线/服务失败/过期码；配网已验证但保存失败时只重试该凭据保存；Wi-Fi 已保存而设置保存失败时只重试该设置快照；保存成功后等待音频空闲重启，确认音失败不阻断；不可重试时明确拒绝 |
| `AT+GUIDE 0..12` | 本地语音诊断；`5` 必须额外给六位测试数字，例如 `AT+GUIDE 5 012345`；不修改真实码和账号归属 |
| `AT+UI` | 页面、选择、epoch、注入状态、GPIO9 当前电平、显示传输状态 |
| `AT+CONSOLE` | 最近 4 次结束行的返回/耗时与 USB 状态，以及 SOF 断开计数/采样；只读，不清计数 |
| `AT+KEY CLICK` / `DOUBLE` / `HOLD` | 注入单击 / 双击 / 长按电平，经过原有去抖与手势识别 |
| `AT+SCREEN` | 导出最后一次成功发送给 SSD1306 的 1,024 字节帧缓冲 |
| `AT+DISPLAY` | 显式请求一次屏幕刷新；随后查询显示状态 |
| `AT+AUDIO TONE` | 约 0.5 秒、25 帧 PCMA 提示音 |
| `AT+AUDIO RECORD` | 录制 1 秒、50 帧 PCMA 到 RAM |
| `AT+AUDIO PLAY` / `STOP` | 回放完整录音 / 请求停止 |
| `AT+VOL 0..100` / `AT+MIC 0..36` | 设置音量 / 麦克风增益；音频 worker 应用，变更防抖保存，以 `SETTINGS` 核对结果 |
| `AT+GUIDANCE` / `AT+GUIDANCE 0\|1` | 只读查询 / 关闭或开启自动语音引导，默认开；与音量/增益合并防抖保存；文字、RTC、告警及主动播码不受影响 |
| `AT+RTC` | RTC 建连/回收状态、媒体计数、业务所有者/代次、字幕、GROUP 权威快照；`subscribed` 与业务授权 `permit` 是独立位图 |
| `AT+CONTACTS` / `AT+PENDING` | 只读当前缓存、页码、总数、loading 和 error；不发送 HTTP，不触发刷新 |
| `AT+CONTACTS ALL\|DEV\|WX [页码]` | 按全部/普通设备/微信类型请求权威联系人；页码从 0 开始，省略为第 0 页，每页最多 8 项 |
| `AT+PENDING <页码>` | 请求该页联系人申请；页码从 0 开始，每页最多 4 项；与无参数只读查询区分 |
| `AT+CONTACT REQUEST <id>` | 向目标设备申请普通设备联系人 |
| `AT+CONTACT ACCEPT <id>` / `REJECT <id>` | 明确接受 / 拒绝该待审批设备申请 |
| `AT+CONTACT DELETE <id>` | 删除手动联系人；同账号及微信联系人保护由服务端校验 |
| `AT+CONTACT REMARK <id> <UTF8_HEX>` | 设置备注；`-` 清空；业务层严格校验 UTF-8 及最多 64 个字符 |
| `AT+AI START` / `STOP` / `INTERRUPT` / `SUBMIT` | 开始、结束 AI 对话，打断播放或提交本轮语音 |
| `AT+CALL DEVICE <id>` / `WX <openid>` | 发起当前已加载页中的已接受设备联系人 / 已授权微信联系人呼叫；loading 时拒绝，微信使用该联系人授权的完整 AppID |
| `AT+CALL ACCEPT` / `REJECT` / `HANGUP` / `DISMISS` | 接听、拒绝、取消/挂断、明确重试错误清理；不会强行清掉未释放资源 |
| `AT+GROUP REFRESH` | 查询权威房间及成员 |
| `AT+GROUP CREATE [四位密码]` | 创建公开房间；提供恰好四位数字时创建密码房间 |
| `AT+GROUP JOIN <六位房间号> [四位密码]` | 加入公开或密码房间，保留房间号及密码中的前导零 |
| `AT+GROUP CONNECT` / `DISCONNECT` / `LEAVE` | 连接 / 断开媒体 / 退出房间；断开不等于退出成员关系 |
| `AT+GROUP PTT_DOWN` / `PTT_UP` | 开始 / 停止发言 |
| `AT+MEDIA TONE` | 当前 RTC 音频流插入 500 ms、25 帧诊断音，不生成无限音源 |
| `AT+AUDIO DUMP` | 空闲时导出最后 1 秒 PCMA；`source=0` 为本地麦克风，`source=1` 为网络下行 |

返回 `OK` 表示命令已接受，**不表示异步操作已经完成**。查询 `AT+STATUS`，以 `busy=0 error=0` 和预期帧数确认录放结束；业务、联系人及重新配网还需查询对应快照和服务端结果。错误返回 `ERROR: ESP_ERR_...`。忙状态拒绝新音频请求，没有完整录音时拒绝回放。原 `tone/record/play/stop/status/display/help` 命令保留为兼容别名。

联系人请求发出后，轮询无参数 `AT+CONTACTS` / `AT+PENDING`，等待 `loading=0 error=0`，同时核对 filter/page；不要用带页码命令轮询，它会发起新的刷新。翻页立即清旧缓存，旧响应不覆盖新选择。此分页是对服务端完整响应的有界窗口，不是服务端分页接口；完整 JSON 超过 8191 字节时明确报错，不静默截断联系人。

引导音独立验收时还应满足 `frames=expected stopped=0`；`owner=0` 是诊断，`owner=1` 是产品引导。`AT+WIFI` 的接受响应明确为 `verified=0 saved=0`，后续必须验证真实 GOT_IP、Wi-Fi/设置保存及重启；确认音失败不冒充播放通过，但不阻断已完成的保存流程，仍等待音频空闲。测试码播完不代表完成真实平台绑定。

`peak` 是当前或最后一帧的峰值，不是整段最大值；PCMA 静音 `0xD5` 解码为 8，因此提示音尾部 `peak=8` 不表示整段静音。设置需等待 `SETTINGS saved=1 pending=0 error=0` 才算保存；测试脚本必须恢复实际读取的原值并确认保存，恢复失败使该轮失败。

`+TASKS: audio_available=... audio_hwm=... platform_available=... platform_hwm=... unit=bytes` 是现有任务的历史最小剩余栈字节数，直接使用 ESP-IDF 字节单位，不乘字长。初始化前、创建失败或平台任务退出时为 `available=0 hwm=0`，不代表栈已用光；真实采样为零仍标 `available=1`。查询不创建任务或清水位。

`+HEAPCAP` 紧接原 `+HEAP`，每组输出 `*_caps/*_free/*_largest/*_minimum`，单位bytes：`internal`为INTERNAL|8BIT（0x804），`dma`再含DMA（0x80c），`rtc`为RTCRAM（0x8000）。三组有重叠，不能相加；9项为依次只读采样，非同瞬间快照，minimum为历史水位。RTCRAM未注册成堆时查询可全0，不自动表示发生分配失败。查询不移动对象到RTC RAM，也不在heap failure hook中执行。

重配网期间 `SETTINGS quiesced=1` 拒绝新音量/增益变更。若保存失败，`WIFI_SETUP phase=3` 保留维护与原错误，既不重启也不自动重写；先 `AT+WIFI CANCEL`，等 `phase=0` 和 `quiesced=0`，再显式设置同值重试保存。UI 的准备中/失败页也有真实取消入口。设置偏好已接纳但实时音频应用失败时仍返回原音频错误，偏好保存状态与硬件应用状态分别核对。

首次配网成功后的重启同样冻结设置并等真实保存。若该设置写入失败，屏幕保留错误及“双击重试”；`AT+RETRY` 或双击只重试已接纳的快照，不开放后到设置，也不重写已保存 Wi-Fi。`attempt_pending=1` 表示已显式提交的设置正在等待/写入，旧错误在真正保存成功前仍保留；`attempt_pending=0 error!=0` 需要再次显式处理，不会自动循环。

`GUIDANCE enabled` / `SETTINGS guidance` 是已接纳的开关偏好，不等于已保存；仍需 `saved=1 pending=0 error=0`。开关仅控制自动启动、配网步骤/成功、自动绑定码及在线讲解；离线/失败、来电/拨号/结束告警不关闭。关闭会停止正在播放的自动讲解，不停止诊断或 RTC 音频；`AT+BIND REPLAY`、绑定页双击及 `AT+GUIDE` 始终是显式播报。保存失败后显式提交同值才重试，维护冻结时拒绝修改。

`AT+KEY` 在主循环中按时间注入电平，不直接改页面或选择。单击/双击占用约 700 ms，长按约 1,600 ms；等待 `key_active=0` 再发下个手势。注入期间再次注入、实体键仍按下或已有待判定点击时拒绝启动。实体 GPIO 输入仍参与同一手势识别，不被关闭。

`AT+SCREEN` 每行返回 `offset` 和 32 字节十六进制数据，按 SSD1306 的页格式排列：`frame[x + (y / 8) * 128]` 的第 `y % 8` 位表示像素。它证明发送帧及驱动返回，不能读取面板像素，也不能确认双色区域位置。

## 自动回归

脚本：[tools/regress_at.py](../tools/regress_at.py)。需要 Python、`pyserial`；使用 `--reset` 时还需要 `esptool`。ESP-IDF Python 环境已提供这两个依赖。

```text
python tools/regress_at.py COMx --mac BOARD_MAC --elf-sha ELF_SHA256 --output OUT_DIR --reset
```

将示例中的串口、MAC、SHA-256 和输出目录替换为本次目标。`ELF_SHA256` 使用本次构建 ELF 文件的完整 SHA-256；脚本会与设备返回值比对。输出目录必须不存在，避免覆盖旧证据。脚本不烧录、不清 NVS；`--reset` 只做一次明确的启动复位，要求设备从主页开始。

默认覆盖 20 轮录放、单/双/长按及菜单往返、UI 音量/增益和音频操作、停止/忙状态/无录音拒绝、参数和行协议边界、6 个页面帧、30 秒空闲观察、内部堆完整性和首尾差值。日志、JSON 和原始帧/PBM 留在指定目录。有限观察不算长稳；注入不能证明实体按键触点，录放帧数和峰值不能代替声音听感。

产品引导回归：[tools/regress_onboarding.py](../tools/regress_onboarding.py)。目标须已绑定；脚本核对 MAC、ELF 和设备 ID，等待正式在线，覆盖全部 13 类提示音、含前导零的数字拼接、停止/所有权、20 轮录放、错误与忙状态及绑定恢复。不会配网、绑定/解绑或修改云端账号。

```text
python tools/regress_onboarding.py COMx --mac BOARD_MAC --elf-sha ELF_SHA256 --device-id DEVICE_ID --output OUT_DIR --reset
```

双机业务回归使用 [regress_product.py](../tools/regress_product.py)，引导和重复录放使用 [regress_audio_soak.py](../tools/regress_audio_soak.py)。运行前锁定两台 MAC、设备 ID 和本轮 ELF SHA-256，分别保存证据目录。脚本自测不等于执行真机测试；微信手机、实体触点、听感、弱网和长稳仍需各自门禁。本产品没有 OTA 指令。

联网时堆会受 TLS、TCP 和 MQTT 临时分配影响，产品脚本保留逐次水位，不把单次首尾相同作为无泄漏证明。早期 `regress_at.py` 的纯 BSP 基线和固定页面数量不适用于完整产品验收。
