# P4 通信与媒体链路

把一通视频电话拆开看：平台处理呼叫请求，TiRTC 连接双方并传送媒体，P4 采集、编解码和呈现音视频，C6 提供网络。还没跑过设备的话，先从 [README](../README.md) 开始；有了画面，再来追每一帧的去向。

## TiRTC 连接与收发

| 阶段 | 代码里发生了什么 |
| --- | --- |
| 上线 | 配网、校时、获取身份，设置选项后执行 `TiRtcInit()` 和 `TiRtcStart()`；收到 `TIRTC_EVENT_SYS_STARTED` 才进入就绪状态 |
| 连接 | 设备主动呼叫用 `TiRtcConnect()`，AI/微信用 `TiRtcWhipConnect()`；H5 入站走 `on_conn_accepted`，连接结果由回调通知 |
| 发送 | 采集编码后调用 `TiRtcSendAudioStream()` / `TiRtcSendVideoStream()`；每次交付完整帧，关键帧标记要对应实际视频内容 |
| 接收 | `on_audio` / `on_video` 经 runtime 交给媒体模块，复制入队后分别播放声音、解码显示画面 |
| 挂断 | runtime 停止本次媒体，SDK 适配层断开；generation（会话代次）检查挡住旧连接迟到的回调与帧 |

这张表对应同仓 [starter_tirtc.c](../../lckfb-szpi-esp32s3-tirtc/components/starter_tirtc/src/starter_tirtc.c)，P4 链接自己的 [TiRTC SDK](../components/tirtc_sdk/VERSION.md)。留意设置顺序：最大发送缓冲等全局选项放在 Init 前，设备身份放在 Start 前。

业务授权通过后，才进入 RTC 建连。WebRTC 接入由 TiRTC SDK 和平台完成，对端需匹配客户端与呼叫信令。本版 TGTRP 的码率建议由媒体任务应用到编码器，不在 SDK 回调里阻塞处理；这项能力也不代表所有传输模式都支持同样的反馈。

## 音频链路

```text
ES8311 MIC + DAC 回采参考
  -> 16 kHz / 16 bit 两槽采集 -> ESP-SR MR AEC
  -> 约 100 Hz 高通 -> AGC
      -> 16 kHz PCM 给唤醒
      -> 降采样至 8 kHz -> 预录/实时队列
      -> 单次 +6 dB 限幅增益
      -> G.711 A-law，20 ms 分包 -> TiRTC
```

两个槽分别是麦克风与播放参考，构成 **MR**。参考告诉 AEC“扬声器正在播什么”，用于抑制回声；高通和 AGC 继续处理麦克风信号。发送走独立队列，上传增益不重复作用于唤醒输入。

对应代码：[采集与发送](../components/starter_media/src/starter_media.c)、[AEC](../components/starter_media/src/starter_aec.c)、[高通](../components/starter_media/src/p4_capture_highpass.c)。

下行从 TiRTC 回调复制到 RX 槽，解码进 PSRAM PCM 环，再以不超过 20 ms 的小块写入 I2S。RX 为 **32 × 1500 bytes**，PCM 环为 **8000 个 int16**，另有包结束位图。

| 业务 | 初始预缓冲 | 调整范围 |
| --- | --- | --- |
| H5 / 微信 VoIP | 20 ms | 20–100 ms |
| 设备呼叫 | 120 ms | 120–560 ms |
| AI 对话 | 120 ms | 120–320 ms |

网络到包忽快忽慢，播放需要尽量连续。控制器调节缓冲和播放速度，I2S 时钟保持不变；跨块保留重采样相位，结束时限时排空尾音，部分写入失败也不重播已写入的数据。表中数值是本机缓冲策略，通话延时和听感还要在实际对端检查。

对应代码：[PCM 队列](../components/starter_media/src/p4_audio_playout.c)、[播放控制器](../components/starter_media/src/audio_playout_controller.c)。

## 视频链路

从这块板看，上行是发出去的画面，下行是收到的画面。**屏幕的 480×320 只决定本机怎样显示**，传输分辨率要按场景看：

| 场景 | 上行配置 | 下行 |
| --- | --- | --- |
| H5 查看 | OV5647 1280×960，经 PPA 旋转为 H264 960×1280@15fps，目标 2 Mbps | 仅接收对讲音频 |
| 微信视频 | H264 960×1280@15fps，目标 2 Mbps | MJPEG，经 JPEG 硬解、旋转和等比例裁切 |
| 设备视频呼叫 | H264 384×256@12fps，目标 256 kbps | 受限 baseline H264，软件解码 |

微信下行按 JPEG 每帧尺寸解码，上限为最长边 640、总像素 640×480；对端可发送较小画面。视频下行统一适配到 480×320 显示。

AI 和纯语音会话不采集视频。入站池 **4×256 KiB** 与 RGB565 画布放在 PSRAM，编解码池另算。码率反馈由视频模块执行，实际帧率与码率要看运行计数。

对应代码：[视频会话](../components/p4_hardware/p4_video.c)、[媒体参数](../main/media/media_tuning.h)、[下行呈现](../main/services/call_video_renderer.c)。

## 代码入口

| 要修改什么 | 入口 |
| --- | --- |
| SDK 初始化、连接与媒体帧 | [starter_tirtc 组件入口](../components/starter_tirtc/CMakeLists.txt) |
| 呼叫与会话切换 | [starter_runtime 组件入口](../components/starter_runtime/CMakeLists.txt) |
| 麦克风、编解码与扬声器 | [starter_media.c](../components/starter_media/src/starter_media.c) |
| 摄像头、编码与显示 | [p4_hardware](../components/p4_hardware/CMakeLists.txt)，沿引用进入 `main/` 中的 camera_pipeline、media_governor 和 call_video_renderer |
| 绑定、联系人与平台请求 | [platform_client 组件入口](../components/platform_client/CMakeLists.txt) |
| 唤醒与界面 | [starter_voice](../components/starter_voice/CMakeLists.txt)、[starter_product](../components/starter_product/CMakeLists.txt) |

1.0.0 复用 S3 的业务与 UI，点进组件 CMake 可以找到实际源码；P4 的 SDK、驱动和媒体实现在本目录。改通话流程先看 runtime，改画面先看视频模块，网络等待留在 UI 任务之外。

## 移植到自己的板卡

- **先换驱动，保留通信接口。** 媒体帧格式保持一致；SDK 回调数据需在返回前复制，会话代次检查也要保留，避免旧帧进入新通话。
- **按用途分配内存。** 媒体帧、PCM 等大缓冲用 PSRAM；DMA、驱动与关缓存路径保留必要的内部 RAM。排障同时看空闲量和最大连续块。
- **先确认网络硬件。** 本板通过 C6/ESP-Hosted 联网，SDIO 为 **4 线 40 MHz SDR**，不是 S3 的原生 Wi-Fi。适配修改见 [Hosted 说明](../components/espressif__esp_hosted/LOCAL_CHANGES.md)。
- **带齐构建输入。** 保留同级 S3 和根 `common/`，但 SDK 与单麦 MR 配置用 P4 自己的版本。接着看[排障与验证](TESTING.md)和[版本与依赖](../VERSION.md)。
