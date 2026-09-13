# S3 APP 1.0.0

1.0.0 把 TiRTC / WebRTC 双向语音通信带到立创·实战派 ESP32-S3 N16R8：从配网、绑定到拨通电话，都可以直接在板上体验。

- **通话**：设备与微信语音呼叫、H5 音频对讲，连接和媒体收发由 TiRTC 处理；当前不启用摄像头。
- **声音**：双麦 AFE、AEC、AGC，G.711 A-law 上行与自适应下行缓冲；同一音频链路也用于唤醒和 AI。
- **环境**：ESP-IDF 5.5.4、TiRTC SDK 2.3.0。

先下载 [16 MB 完整固件](https://github.com/tangeai/xiaotai-esp32/releases/tag/esp32-s3-app-v1.0.0)，按 [README](README.md) 烧录，再打通第一通电话。

当前为 **Pre-release 体验版**：干净构建与镜像校验已完成，目标板通话及长稳回归仍待验证。使用前留意[已知问题与安全说明](KNOWN_LIMITATIONS.md)。
