# 测试与排障

先找出问题发生的阶段，再选择对应检查。正常流程见[开发指南](GETTING_STARTED_CN.md)，模块和参数见[系统架构](P4_MEDIA_ARCHITECTURE.md)。

## 构建与启动

| 现象 | 检查顺序 |
| --- | --- |
| 模型、源码或检查脚本缺失 | 核对完整 P4 目录；旧缓存引用其他工程时运行 `idf.py reconfigure`，再构建 |
| 组件下载失败 | IDF 5.5.4 环境、组件仓库访问和依赖锁；保留本地补丁组件 |
| Windows 链接后报 Bash 或旧路径错误 | 确认使用当前源码与缓存；当前后处理使用 Python |
| SDK ABI、trace 或 tick 检查失败 | 对照附带 SDK 的版本说明和有效配置，保留第一条错误 |
| 芯片或 Flash 不匹配 | 串口、P4 修订、16MB 配置及分区；确认后再烧录 |
| Hosted/SDIO 初始化失败 | C6 固件、供电、板型和 SDIO 配置，见 [C6 指南](C6_PREPARATION.md) |
| panic、栈溢出或异常复位 | 保存完整回溯及匹配的 ELF，核对固件身份后定位第一处异常 |

构建错误应从最早出现的错误向后分析，避免被最终的 Ninja 或链接失败信息带偏。

### 组件版本检查误报

本机 ESP-IDF 5.5.4 环境中的 `idf-component-manager 2.4.9` 曾在可选的组件新版本检查阶段，将 `LV_USE_LIBJPEG_TURBO`、`LV_USE_LIBPNG`、`LV_USE_LZ4` 报为缺失。本工程锁定的 LVGL 8.3.11 不要求这些配置。先用 `python -m pip show idf-component-manager` 核对版本，并保留最早的配置错误；其他组件下载或编译错误不适用此处理。

命中上述情况时，可仅对当前构建关闭新版本提示。Windows ESP-IDF PowerShell 终端执行：

```powershell
$previous = $env:IDF_COMPONENT_CHECK_NEW_VERSION
try {
    $env:IDF_COMPONENT_CHECK_NEW_VERSION = "0"
    idf.py reconfigure build
} finally {
    $env:IDF_COMPONENT_CHECK_NEW_VERSION = $previous
}
```

Linux/WSL 对应命令：

```sh
IDF_COMPONENT_CHECK_NEW_VERSION=0 idf.py reconfigure build
```

这只是组件管理器版本检查的临时规避，不是其根因修复。正常依赖解析、锁文件及下载校验仍启用，不修改 `sdkconfig`、锁定版本或 SDK；不要设置全局环境变量，也不要添加上述无关配置项。后续更换组件管理器环境时，先在不设置此变量的条件下验证，再决定是否仍需使用。

## 业务与媒体

| 现象 | 先找哪段数据 |
| --- | --- |
| 有 IP，没有六位码 | 校时、DNS、服务发现、HTTP 请求及临时 MQTT 订阅 |
| 热点已开启或验证码已播报，屏幕却不切换 | 核对固件 ELF，再看 `setup view` 页面转换；配网/绑定刷新不能被尚未初始化或加锁中的业务快照拦住 |
| 平台绑定后设备仍等待 | 平台结果、绑定消息接收和确认 |
| AI 唤醒后不回答 | 角色配置、token 请求、TiRTC 建连和 `start_session` 结果 |
| AI 查询联系人没有回包 | `AI tool rx` 是否包含 `query_contact_status` 和有效 `id`，随后是否有 `AI query tx` |
| 微信联系人为空或手机未响铃 | 小程序授权、绑定关系、通讯录同步及实际被呼联系人 |
| H5 有画面，对讲无声 | 浏览器麦克风权限和输入、对讲开关、设备接收计数与播放 |
| 声音小或回声大 | MIC、DAC 参考与 AEC 输出录音，检查增益和限幅 |
| 音频卡顿 | 到包时间、缓冲水位、变速、I2S 写入耗时与错误 |
| 视频延迟或缺帧 | 入站、解析、解码、转换、显示各级计数与耗时 |

接收槽丢弃、I2S 写失败和网络未到包要分别记录。每次只改变一个主要变量，保留故障前后时间线。

### 获得 IP 后校时超时

`network clock unavailable` 表示本轮等待没有获得有效同步结果，后续绑定和 TiRTC 初始化仍未放行。当前依次使用 `ntp.aliyun.com`、`pool.ntp.org`，请求走 UDP 123；45 秒是应用等待窗口，不代表 Wi-Fi 建连用了 45 秒。

失败后保留同一轮的 `network clock snapshot / DNS / peer` 日志：

- `active=0`：校时服务未运行，先检查初始化或停止路径。
- `DNS slot`：当前解析服务器配置；有地址不代表该 DNS 可达。
- `peer slot=0/1`：对应上述两台时间服务器。地址为零可能是尚未解析、解析中或未轮到该服务器，不能直接判定 DNS 故障；非零只说明有已解析地址，不证明请求成功发出。
- `reach=0x00`：近期没有记录成功响应，不能把它换算为丢包率或认定防火墙拦截。确认 DNS 与 UDP 123 的具体失败位置仍需设备侧日志或热点侧抓包。

`UI intent rejected: action=1` 对应启动 AI。校时/绑定完成前，业务队列尚未创建，此时返回 `ESP_ERR_INVALID_STATE` 并提示稍候；就绪后队列满返回 `ESP_ERR_TIMEOUT`。按状态区分未就绪和排队失败，不把错误名称直接当作 UI 阻塞时长。

新增快照只在失败时读取现有网络状态，不新增任务、不主动发送探测包、不更改校时超时、服务器或鉴权顺序。排查时不要用伪造日期或跳过校时来制造上线成功。

若三个 DNS 均为 `0.0.0.0`，当前 IDF 的解析器会直接返回错误，未缓存的域名无法发起 DNS 查询。这已经足以解释校时无法推进，但不能证明 DHCP 服务端没有下发 DNS。网络管理层在 GOT_IP 时保留 `Wi-Fi DNS: stage=got-ip` 快照，再向空备用槽配置 `CONFIG_XIAOTAI_FALLBACK_DNS_IPV4`（默认 `223.5.5.5`），最后记录 `stage=after-portal-stop`。主、备 DNS 和已存在的备用地址均不覆盖。

前一快照就为空时，检查 DHCP ACK 中的 DNS 选项及解析；前一快照非空、后面被清空时，检查网卡生命周期和 DNS 写入路径。必要时抓 DHCP/DNS/NTP 包。验证须覆盖服务器地址解析、`network clock synchronized` 及平台实际上线，不能仅凭备用地址写入成功判为通过。

### 启动等待计时

画面能动但功能未就绪时，依次对齐获得 IP、`network clock synchronized`、`SDK started`、`device token obtained`、主页状态、MQTT 连接及资料上报。主页出现后实际发起 AI，确认 `ai-active` 与首个上下行音频包，不能只把页面切换视为全功能就绪。

慢 HTTP 日志中，`dns` 为解析，`link` 为扣除解析的连接耗时，`sock` 为 socket connect 调用耗时，`tcp_wait` 为非阻塞 TCP 建连的首次等待；`wait` 为最后一次写入到首次收到响应的间隔，`q` 为业务排队。`conn=0` 表示复用了连接。`HTTP t` 与日志前缀可能使用不同时间基准，只在同一基准内相减。

校时等候与 HTTP 建连分别定位。没有收到 NTP 应答，不能直接认定服务器、热点或设备丢包；需要请求/响应及路径对照证据。复用连接的回归应覆盖签名登录、Bearer 请求、POST 后 GET、空闲释放、跨站、服务器主动关闭、解绑及失败后手动重试。

## 设备能力上报

正常日志顺序为 MQTT 上线、`device profile queued`、`api=/v1/device/profile`、`device profile reported`。检查服务器保存的 `profiles.stream/call/voip` 与[能力表](P4_MEDIA_ARCHITECTURE.md#设备能力上报)一致，再验证微信语音/视频、设备呼叫和 H5。平台显示能力并不等于媒体实测通过。

逐项核对三个场景的 12 / 12 / 16 个字段，以及服务器补充的摄像头、屏幕和微信通话类型。重点确认实时查看比例为 0.75、设备通话为 1.5，画面无新增旋转或镜像；微信新增 `down_video_rotation=0`、`video_res_mode=auto` 后，双向视频尺寸、方向和本地 MJPEG 播放应与原来一致。请求中不得出现示例里有、但本业务未实现的编码或只读字段。

错误日志保留 HTTP 状态、业务码和重试间隔，不输出 token。回归覆盖首次绑定、已绑定冷启动、MQTT 重连、HTTP 超时/500、参数 400、鉴权 401、解绑 410/6006、响应队列满及解绑后迟到响应。失败不能打印上报成功；同一上线周期最多 3 次，成功后不周期性重复提交。确认新请求没有在 UI、MQTT 或音频回调中执行网络 I/O。

## 采集音量与播放音质

固定同一台对端、扬声器档位、说话距离和测试语句，对比安静近讲、安静远讲及扬声器播放时双讲。上行新增的 1.5 倍是限幅前的标称幅度，不是声压、响度或信噪比提高 1.5 倍；不要只听一段已经触顶的录音来判断增益是否生效。

- `CP rms/peak` 的三个值依次为 AEC 输出、高通后和 16 kHz 自动增益后；先检查声音是否在前端被压低。
- `TX level rms/peak` 的两个值为 8 kHz 上行增益前后，结合 `age`、`n` 和 `clip` 判断新鲜度与限幅。若最终发送电平足够而对端仍很轻，继续查对端解码和播放，不继续堆采集增益。
- 播放短渐入覆盖首次播放、断流恢复、同会话多次发声和写入失败恢复。连续数字和短句不得逐包衰减、重复或吞尾；输出锁竞争时，同一块重试的波形与进度应一致。
- 同时观察 `CP dspmax/late/ovf`、`TX gainmax` 和 `AP drop/wrerr/wrmax`。缓冲空事件不等于网络丢包，主机波形检查不证明实际扬声器或 AEC 听感。

本轮声学回归应覆盖 AI、设备语音/视频、微信、H5、多人 PTT，并对照受控抖动/丢包后的恢复。不通过改变视频、I2S 时钟或关闭错误日志来制造音质改善。

## 多人对讲排障

| 现象 | 检查顺序 |
| --- | --- |
| 创建或加入未完成 | 联网、绑定、6 位房间号和可选 4 位密码，再看对应 HTTP 返回；不要连续重复提交 |
| 有房间号但不能讲话 | TiRTC 连接、`join_room` 应答和媒体格式是否成功；“有房间归属”不等于已经入会 |
| 在线人数或成员不更新 | 房间快照是否到达、是否过期；刷新查询，不把 HTTP 修改回执当作在线证明 |
| 按住后仍无上行 | 麦克风静音、当前连接代次、PTT 状态与发送计数；松手后检查计数是否停止 |
| 网络恢复后入房慢 | 旧会话释放 HTTP、归属查询、connect-token、SDK 和 join 应答各阶段耗时 |
| 断电后返回 `40921` | 保留服务端返回与时间线，检查旧关系/会话租约；重启会丢失 PSRAM 内释放事务，不能伪造释放成功 |

### 当前跟踪项

下面是开发回归中已出现、根因尚未闭环的问题，排查时应保留原始错误：

- **SDK 开机注册：**设备开机后的第一轮 TiRTC 注册曾返回 `service_code=40001`、`TiRtcStart=-40012`，服务端详情为 `nonce replayed`，约 5 秒后的重试成功。该提示表示防重放随机串被判为重复，不等于 Wi-Fi 密码、六位绑定码或内存错误。启动链路已要求先确认本次开机校时，SDK 随机源保持不变；尚不能据此认定重复随机串的根因已修复。后续仍需核对跨复位请求摘要，不把重试成功当作根因证据，也不要在日志中记录完整鉴权请求。

先在普通路由器建立基线，再对照电脑热点和受控弱网。记录发送时间、SDK 到包、PCM 消费和 I2S 写入四段；仅在接收端注入丢包不能代表完整的双向弱网。

## 主机检查

在 P4 根目录运行脚本。首次体验无需先运行全部测试；修改哪个模块，就先检查该模块及相邻路径。

| 修改范围 | 脚本入口 | 目标板还需检查 |
| --- | --- | --- |
| 配网与绑定页面生命周期 | `tools/test_setup_lifecycle.py` | 无 Wi-Fi 启动先显示配网，热点就绪后更新名称/网址；首次绑定显示和更新六位码，不等播报结束；重试、绑定成功、解绑及通话中断网的页面顺序 |
| 启动校时 | `tools/test_startup_clock.py` | 已绑定/首次绑定、冷启动/保留日期复位；阻断 SNTP 后恢复，确认同步日志先于 SDK 初始化及平台请求 |
| Wi-Fi DNS 策略 | `tools/test_wifi_dns.py` | DHCP 有/无 DNS、备用地址不可达、断网重连与 AP 退出前后快照；校时和后续业务实际上线 |
| AI 握手和 HTTP | `tools/test_ai_start_contract.py`、`tools/test_platform_http_requests.py`、`tools/test_platform_http_trace.py`、`tools/test_platform_http_reuse.py` | 重复建连、完整应答后开放音频、DNS/连接/响应等待时序、复用和空闲释放 |
| NVS 与建连资源 | `tools/test_nvs_store.py`、`tools/test_runtime_resources.py` | 快速改设置后复位、配网/绑定保存、连续呼叫时 MQTT 在线及内存水位 |
| 多人房间与 UI 快照 | `tools/test_room_release.py`、`tools/test_room_navigation.py` | 创建/加入/退出、返回断连再进入、PTT 松手停止、AI 切换、断网释放后重连、断电后的租约恢复 |
| 任务内存与热点生命周期 | `tools/test_memory_placement.py` | 热点反复启停、DNS/页面访问、配网保存重启、提示音及铃声；记录内部/PSRAM 最大连续块和栈余量 |
| 音量控制 | `tools/test_speaker_controls.py`、`tools/test_playback_ownership.py` | 验证码/铃声播放期间连续调音量、静音与恢复；检查请求值、硬件已应用值、NVS 读回及 UI 耗时 |
| 采集与高通 | `tools/test_audio_pipeline.py`、`tools/test_capture_highpass.py` | 录音、连续性、处理耗时 |
| 播放缓冲与变速 | `tools/test_audio_playout.py`、`tools/test_playback_ownership.py` | 欠载、积压、尾音和听感 |
| I2S 供音节拍 | `tools/test_i2s_playback_cadence.py` | 持续播放、三种策略的起播和收尾、`AP level/srcclip/dma_est/late`；驱动仿真通过不代表真机爆音消失 |
| 播放诊断隔离 | `tools/test_audio_diagnostics.py` | 连续播放的 `late/pubmax/stack`、日志拥塞时的错误计数 |
| 唤醒 FFT | `tools/test_wake_fft.py` | 不同语速、距离及播放中唤醒 |
| 卷积适配 | `tools/test_conv_channels.py` | P4 向量计算输出和推理耗时 |
| 界面 | `tools/test_product_layout.py` | 快速点击、字幕、表情与视频并发 |
| 源码整理 | `tools/test_source_layout.py` | 首页、菜单、配网、绑定及通话页面；主机检查只证明当前入口与所需源码保留 |
| 表情与切换 | `tools/test_face_animation.py` | 23 种表情、44 套有效姿态，快速切换、眨眼、聆听/思考、隐藏后恢复；观察轮廓、装饰、残影、触摸和绘制耗时 |
| 联系人与微信 | `tools/test_contact_query.py`、`tools/test_voip_incoming_media.py`、`tools/test_voip_profile.py` | 查询回包、双向语音/视频呼叫 |
| 设备能力上报状态 | `tools/test_device_profile.py` | 首次上线、重连、解绑、平台能力展示及失败重试边界 |
| 视频 | `tools/test_p4_video.py`、`tools/test_full_frame_uplink.py`、`tools/test_video_ingress.py`、`tools/test_p4_profile_switch.py` | 首帧、方向、摄像头开关与连续显示 |
| 码率 | `tools/test_video_bitrate.py`、`tools/test_bitrate_governor.py` | SDK 反馈及真实发送码率 |
| Hosted | `tools/test_hosted_rpc_routing.py`、`tools/test_hosted_init_lifecycle.py` | 并发请求、掉线和资源回收 |
| 手机热点配网 | `tools/test_wifi_portal.py`、`tools/test_nvs_store.py` | 中文名称、开放/隐藏网络、已保存密码复用和修改、超过 4 个网络的替换、失败密码不入历史；零条/多条扫描、反复刷新、扫描中重连及退出；手机 320/390/480 像素宽度 |
| 依赖 | `tools/test_dependency_lock.py` | 固件构建 |
| I2C 驱动链接 | `tools/test_i2c_driver_family.py` | `tools/check_i2c_driver_family.py` 检查实际 ELF/MAP，随后验证启动、触摸和音频 |

运行前阅读脚本的编译器要求。需要 gcc/g++ 或 sanitizer 的检查使用 Linux/WSL 主机环境；IDF 交叉编译器不能直接替代主机编译器。卷积适配检查需要 CMake 和 Ninja。

HTTP 复用检查还需设置当前环境的 `IDF_PATH`，用于编译 IDF 的实际 URL 解析器；不进行网络请求。

配网生命周期检查执行真实 UI 定时器前段、配网页状态更新及页面构建后的刷新分支，以 stub 模拟网络、LVGL 和快照锁。可用 `--revision <commit>` 对照历史源码，不切换工作树。它能验证首次启动的依赖顺序、锁竞争和文本更新，不能替代实际屏幕、热点与绑定消息测试。

表情检查读取本工程 `managed_components` 中的 LVGL 源码，不自动下载依赖；覆盖 44 套有效姿态与局部重绘一致性，其中“思考”“放松”仅使用第一套。主机结果不能替代屏幕目视检查与运行时耗时验证。启动校时检查使用 SNTP stub，不能证明真实服务器可达或 SDK 防重放错误已消失。

例如：

```sh
python tools/test_audio_playout.py
python tools/test_i2s_playback_cadence.py
python tools/test_audio_diagnostics.py
python tools/test_playback_resampler.py
python tools/test_prompt_resampler.py
python -B -X utf8 tools/test_conv_channels.py
python -B -X utf8 tools/test_dependency_lock.py
```

依赖锁检查不要求已有 build。检查已生成的锁时增加：

```sh
python -B -X utf8 tools/test_dependency_lock.py --resolved build/dependencies.lock
```

播放所有权测试默认检查当前代码。复现历史实现时显式传入 `--baseline SOURCE_FILE`，不要从当前 HEAD 猜测旧文件位置；参数检查入口为 `tools/test_playback_ownership_cli.py`。

主机检查可能使用真实 C 函数、stub 或源码断言，脚本说明决定其覆盖范围。通过结果不代表目标板音视频效果已验收。

## 真机回归

| 顺序 | 操作 | 通过条件 |
| --- | --- | --- |
| 1. 启动 | 核对固件后复位 | 版本与 ELF 匹配，无 panic 或栈溢出 |
| 2. 配网绑定 | 首次配网、绑定、解绑重绑 | 页面顺序正确，旧请求不影响新身份 |
| 3. 网络恢复 | 手动断开；路由器掉线后恢复 | 手动断开进入热点，被动掉线自动重连 |
| 4. 音频 | 近远讲话、播放中双讲 | 输入连续、参考有效，无削顶或吞字 |
| 5. AI | 唤醒、字幕、打断、结束 | 首句和尾音完整，结束后可再次进入 |
| 6. 通话 | 分别测试设备、微信、H5，再次连接 | 双向媒体正常，挂断和调音量响应及时 |
| 7. 视频并发 | 开关摄像头、切页 | 停发与画面一致，音频不中断，旧帧不进入新会话 |
| 8. 多人对讲 | 两台设备入房、交替按住讲话、翻页、快速返回再进入、AI 切换与断网恢复 | 名称/备注及本机标记正确，松手停发；返回后断连且不后台续租，再进入恢复默认收听；“退出房间”解除关系 |
| 9. 稳态 | 重复上述操作并持续观察 | 稳定阶段内存、队列和任务数量无持续异常增长 |

测试前确认端口、板卡和对端可用。烧录不擅自擦除 NVS；采样避免记录无关人员和隐私。

启动校时专项应同时覆盖已有绑定和首次绑定：`network clock synchronized` 必须先于 `pre-tirtc-bootstrap` 和设备上报/鉴权请求。SNTP 无响应时保留 `network clock unavailable`，后续上线不能提前执行；网络恢复后应由同一校时服务继续推进，UI 和采集仍可运行。顺序通过不等于 SDK 防重放问题已经闭环，仍需多次断电与复位日志验证。

## 弱网检查

### 先确认实际传输协议

TiRTC 2.3.0 / 2.5.0 的版本信息只能证明包含哪一版库，不能证明每次连接都走 TGMP。
建连时检查 `starter_tirtc` 的以下记录：

- `TP init proto=TGMP transport=tgtrp ...`：SDK 已选中并成功创建 TGMP 传输会话。
- `TP init proto=KCP transport=kcp ...`：SDK 实际初始化了 KCP。
- `TP init-failed proto=TGMP ... rc=...`：TGMP 初始化失败，不能记为已启用。

同一次呼叫还应出现 `connection ready` 或 `connection accepted`，随后有媒体收发。
`TP init` 本身不代表通话成功；没有对应记录时协议仍是未确认，不能用 `mode=4`、
SDK 版本、服务端能力字段或“没有 KCP 日志”代替。若混有多次呼叫、取消或失败重试，
先按时间线分开，不把上一条连接的初始化记录套到下一条连接。

`mtu` 为 SDK 生效的 MTU；TGMP 的 `poll` 是每次 poll 的最大发送分段数，**不是毫秒**；
`sndcap` 是发送缓存上限（字节），不是实际占用。KCP 的 `interval` 是更新间隔，不能当成 RTO。

探针使用当前 SDK 的公开底层日志回调，仅提取三个固定初始化格式的数值；不输出原始
SDK 文本、源码路径、SDP 或凭据。连接建立后 `tp_probe=off` 表示底层诊断关闭，挂断后
重新等待下次建连。媒体阶段继续使用原有 `AP/CP` 指标，不开启逐包或 SDK STAT 日志。
以后更新 SDK 若日志格式变化，需要重新核对过滤器。没有新增任务、PCM 队列或应用堆分配。

`tools/test_transport_diagnostics.py` 检查日志提取、敏感字段过滤、失败语义和日志开关；
它是主机检查，不能证明真机协议协商。实测时保留从建连到挂断的完整日志，再施加丢包、
延迟和抖动，分别记录协议、首次收发、缓冲水位、变速、队列丢弃、供音超时和听感。

按“无整形 → 延时 → 抖动 → 丢包 → 组合条件 → 恢复正常”的顺序测试，先建立同一网络下的基线。

每轮记录双方 Wi-Fi/热点、整形方向与参数，以及播放日志中的到包间隔、目标水位、实际缓冲、变速档和写入耗时。结合双方时间线，判断等待来自网络、SDK 交付还是本地播放。

## 改动时容易遗漏的地方

| 改动 | 相邻检查 |
| --- | --- |
| 页面事件 | 重复进入、退出后是否重复注册或引用已销毁对象 |
| WithCaps 任务 | 创建、正常退出和失败出口是否使用配对接口 |
| 视频流号 | 活动会话和入站媒体是否匹配 |
| Wi-Fi 状态 | 同步 RPC 是否移出了 LVGL 锁 |
| Hosted 响应 | 迟到、重复和类型错误的响应是否交给错误请求 |
| DSP 初始化 | I2S 是否在处理器准备前开始持续输入 |
| I2S 播放 | 等待写入时是否占住网络接收槽 |
| 解绑和挂断 | 旧房间、旧身份结果是否影响下一次会话 |

## 保存结果

一次记录包含：源码及未提交改动、SDK/工具链、有效配置、板卡修订、操作、预期、实际结果和首处异常。分别标明静态检查、主机测试、构建、烧录、真机和长稳结果。

日志、录音和固件放在自己的调试目录。分享前遮盖 token、设备密钥和个人信息。串口开发控制台默认关闭，命令以 `starter_console` 注册表为准；视频采样见[抓流说明](VIDEO_CAPTURE.md)。
