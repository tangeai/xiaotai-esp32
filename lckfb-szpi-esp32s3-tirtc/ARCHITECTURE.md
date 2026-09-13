# S3 通信与媒体链路

把一通电话拆开看：平台处理呼叫请求，TiRTC 连接双方并传送声音，设备完成采集和播放。还没跑过设备的话，先从 [README](README.md) 开始；听到声音后，再来追它经过了哪些函数。

## TiRTC 连接与收发

| 阶段 | 代码里发生了什么 |
| --- | --- |
| 上线 | 配网、校时、获取身份，设置选项后执行 `TiRtcInit()` 和 `TiRtcStart()`；收到 `TIRTC_EVENT_SYS_STARTED` 才进入就绪状态 |
| 连接 | 设备主动呼叫用 `TiRtcConnect()`，AI/微信用 `TiRtcWhipConnect()`；H5 入站走 `on_conn_accepted`，连接结果由回调通知 |
| 发送 | `starter_media` 采集编码，经 `starter_tirtc_send_alaw()` 调用 `TiRtcSendAudioStream()`；帧描述中的格式、时间戳和长度要对应实际数据 |
| 接收 | `on_audio` 经 runtime 交给 `starter_media_submit_audio()`，复制数据后由播放任务处理 |
| 挂断 | runtime 停止本次媒体，SDK 适配层断开；generation（会话代次）检查挡住旧连接迟到的回调与音频包 |

这张表对应 [starter_tirtc.c](components/starter_tirtc/src/starter_tirtc.c)；参数说明在随包 [tiRTC.h](third_party/tirtc/include/tirtc/tiRTC.h)。留意设置顺序：最大发送缓冲等全局选项放在 Init 前，设备身份放在 Start 前。

业务授权通过后，才进入 RTC 建连。WebRTC 接入以及本版 TGTRP/KCP 传输适配由 SDK 处理，应用统一调用 TiRTC API。接自己的 Web 或手机端时，还需对齐客户端和呼叫信令；“支持 WebRTC”本身不等于已经接通。

## 音频链路

```text
上行：ES7210 -> 双麦 + 播放回采参考 -> AFE -> 单声道 PCM
      -> 8 kHz G.711 A-law -> TiRTC -> 对端
下行：对端 -> TiRTC 回调 -> PSRAM 缓冲 -> 解码与播放控制
      -> I2S -> ES8311 -> 扬声器
```

ES7210 四槽数据重排为 **MMR**：两路麦克风加一路播放参考。参考告诉 AEC“扬声器正在播什么”，用于抑制声音回到麦克风后形成的回声；AGC 再调整增益。双麦处理后的输出是**单声道**，其中 16 kHz PCM 用于唤醒，通信上行转为 8 kHz。

网络到包不总是匀速，播放却需要连续。下行用 **32 × 1500 bytes** 的槽池暂存数据，预缓冲从 **200 ms** 起、在 **60–500 ms** 内调整，播放速度微调约 **±0.625%**。这些数值是本机播放策略，不是端到端延时承诺。

采集和 AFE 常驻，挂断只停止本次传输与播放，下一次唤醒仍需要麦克风。双讲、底噪和唤醒效果要用真实声音检查。

## 代码入口

| 要修改什么 | 入口 |
| --- | --- |
| SDK 初始化、连接与媒体帧 | [starter_tirtc](components/starter_tirtc/src/starter_tirtc.c) |
| 呼叫与会话切换 | [starter_runtime](components/starter_runtime/src/starter_runtime.c) |
| 麦克风、编解码与扬声器 | [starter_media](components/starter_media/src/starter_media.c) |
| 绑定、联系人、平台请求 | [platform_client](components/platform_client/) |
| 启动、配网与身份保存 | [app_main.c](main/app_main.c)、[wifi_manager](components/wifi_manager/)、[runtime_config](components/runtime_config/) |
| 唤醒与界面 | [starter_voice](components/starter_voice/)、[starter_product](components/starter_product/) |

想改通话流程，从 runtime 开始；想改声音，从 media 开始。UI 只读取状态并绘制页面，网络等待和媒体发送留给对应任务，避免一次慢请求卡住屏幕。

## 移植到自己的板卡

- **先换驱动，保留通信接口。** 音频帧格式保持一致；SDK 回调数据需在返回前复制，异步任务才能继续使用。设备/微信会抢占 AI/H5，代次检查也要保留。
- **按用途分配内存。** DMA、驱动和 NVS/Flash 关缓存路径有内部 RAM 要求，不能全部搬进 PSRAM。
- **带齐构建输入。** 保留 SDK 配置、`components/starter_voice/model/` 与根 `common/models/`。1.0.0 的 P4 还引用 S3 业务、字体和提示音。
- **按真实麦克风布置配置 AFE。** S3 的双麦 MMR 与 P4 的单麦加参考 MR 不可互换。接入生产环境前，阅读[排障与安全说明](KNOWN_LIMITATIONS.md)。
