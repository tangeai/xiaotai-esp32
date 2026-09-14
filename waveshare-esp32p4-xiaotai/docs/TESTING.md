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

## 业务与媒体

| 现象 | 先找哪段数据 |
| --- | --- |
| 有 IP，没有六位码 | 校时、DNS、服务发现、HTTP 请求及临时 MQTT 订阅 |
| 平台绑定后设备仍等待 | 平台结果、绑定消息接收和确认 |
| AI 唤醒后不回答 | 角色配置、token 请求、TiRTC 建连和 `start_session` 结果 |
| AI 查询联系人没有回包 | `AI tool rx` 是否包含 `query_contact_status` 和有效 `id`，随后是否有 `AI query tx` |
| 微信联系人为空或手机未响铃 | 小程序授权、绑定关系、通讯录同步及实际被呼联系人 |
| H5 有画面，对讲无声 | 浏览器麦克风权限和输入、对讲开关、设备接收计数与播放 |
| 声音小或回声大 | MIC、DAC 参考与 AEC 输出录音，检查增益和限幅 |
| 音频卡顿 | 到包时间、缓冲水位、变速、I2S 写入耗时与错误 |
| 视频延迟或缺帧 | 入站、解析、解码、转换、显示各级计数与耗时 |

接收槽丢弃、I2S 写失败和网络未到包要分别记录。每次只改变一个主要变量，保留故障前后时间线。

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
| 启动校时 | `tools/test_startup_clock.py` | 已绑定/首次绑定、冷启动/保留日期复位；阻断 SNTP 后恢复，确认同步日志先于 SDK 初始化及平台请求 |
| AI 握手和 HTTP | `tools/test_ai_start_contract.py`、`tools/test_platform_http_requests.py`、`tools/test_platform_http_trace.py` | 重复建连、完整应答后开放音频、DNS/连接/响应等待时序 |
| NVS 与建连资源 | `tools/test_nvs_store.py`、`tools/test_runtime_resources.py` | 快速改设置后复位、配网/绑定保存、连续呼叫时 MQTT 在线及内存水位 |
| 多人房间与 UI 快照 | `tools/test_room_release.py`、`tools/test_room_navigation.py` | 创建/加入/退出、返回断连再进入、PTT 松手停止、AI 切换、断网释放后重连、断电后的租约恢复 |
| 任务内存与热点生命周期 | `tools/test_memory_placement.py` | 热点反复启停、DNS/页面访问、配网保存重启、提示音及铃声；记录内部/PSRAM 最大连续块和栈余量 |
| 音量控制 | `tools/test_speaker_controls.py`、`tools/test_playback_ownership.py` | 验证码/铃声播放期间连续调音量、静音与恢复；检查请求值、硬件已应用值、NVS 读回及 UI 耗时 |
| 采集与高通 | `tools/test_audio_pipeline.py`、`tools/test_capture_highpass.py` | 录音、连续性、处理耗时 |
| 播放缓冲与变速 | `tools/test_audio_playout.py`、`tools/test_playback_ownership.py` | 欠载、积压、尾音和听感 |
| 唤醒 FFT | `tools/test_wake_fft.py` | 不同语速、距离及播放中唤醒 |
| 卷积适配 | `tools/test_conv_channels.py` | P4 向量计算输出和推理耗时 |
| 界面 | `tools/test_product_layout.py` | 快速点击、字幕、表情与视频并发 |
| 表情与切换 | `tools/test_face_animation.py` | 23 种表情的两套姿态、快速切换、眨眼、聆听/思考、隐藏后恢复；观察轮廓、装饰、残影、触摸和绘制耗时 |
| 联系人与微信 | `tools/test_contact_query.py`、`tools/test_voip_incoming_media.py`、`tools/test_voip_profile.py` | 查询回包、双向语音/视频呼叫 |
| 视频 | `tools/test_p4_video.py`、`tools/test_full_frame_uplink.py`、`tools/test_video_ingress.py`、`tools/test_p4_profile_switch.py` | 首帧、方向、摄像头开关与连续显示 |
| 码率 | `tools/test_video_bitrate.py`、`tools/test_bitrate_governor.py` | SDK 反馈及真实发送码率 |
| Hosted | `tools/test_hosted_rpc_routing.py`、`tools/test_hosted_init_lifecycle.py` | 并发请求、掉线和资源回收 |
| 手机热点配网 | `tools/test_wifi_portal.py`、`tools/test_nvs_store.py` | 中文名称、开放/隐藏网络、已保存密码复用和修改、超过 4 个网络的替换、失败密码不入历史；零条/多条扫描、反复刷新、扫描中重连及退出；手机 320/390/480 像素宽度 |
| 依赖 | `tools/test_dependency_lock.py` | 固件构建 |

运行前阅读脚本的编译器要求。需要 gcc/g++ 或 sanitizer 的检查使用 Linux/WSL 主机环境；IDF 交叉编译器不能直接替代主机编译器。卷积适配检查需要 CMake 和 Ninja。

表情检查读取本工程 `managed_components` 中的 LVGL 源码，不自动下载依赖；覆盖 46 套姿态与局部重绘一致性，不能替代屏幕目视检查与运行时耗时验证。启动校时检查使用 SNTP stub，不能证明真实服务器可达或 SDK 防重放错误已消失。

例如：

```sh
python tools/test_audio_playout.py
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
