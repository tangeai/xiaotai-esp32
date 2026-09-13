# P4 APP 1.0.0

1.0.0 把 TiRTC / WebRTC 音视频通信带到微雪 ESP32-P4-WIFI6-Touch-LCD-3.5：从配网、绑定到看见通话对端，都可以直接在板上体验。

- **通话**：设备与微信音视频通话、H5 查看和对讲；上行 H.264，设备视频下行 H.264，微信下行 MJPEG。
- **声音与画面**：单麦 MR AEC、AGC、G.711 A-law 与自适应播放缓冲；视频随会话启停，AI 和纯语音不启动摄像头。
- **环境**：ESP-IDF 5.5.4、TiRTC 2.3.0 P4 验证补丁库、C6/SDIO 4 线 40 MHz SDR。

先下载 [16 MB 完整固件](https://github.com/tangeai/xiaotai-esp32/releases/tag/esp32-p4-app-v1.0.0)，按 [README](README.md) 烧录，再打通第一通电话。

当前为 **Pre-release 体验版**：干净构建与镜像校验已完成，目标板通话及长稳回归仍待验证。使用前留意[排障与安全说明](docs/TESTING.md)。
