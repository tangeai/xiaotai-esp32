# 导出发送前的 H264

这项诊断用于检查设备进入 TiRTC SDK 前的视频数据，适合定位编码内容和旋转问题。它不测量对端接收情况，也不是录像功能。

## 准备

- 确认允许采样，只拍测试图，避免录入无关人员。
- 核对源码、固件和板卡身份。
- 启用串口开发控制台，确认注册了 `version` 和 `video-capture` 命令；实现见 [p4_video_capture.c](../components/p4_hardware/p4_video_capture.c)。
- 电脑安装 pyserial；需要分析画面时准备 ffmpeg/ffprobe。
- 关闭 `idf.py monitor`，保证串口只由采样脚本使用。

## 采集并导出

在 P4 根目录运行。将 `PORT` 换成实际串口，`OUTPUT.h264` 换成调试目录中尚不存在的文件路径：

```sh
python tools/capture_uplink.py --port PORT --output OUTPUT.h264
```

1. 按提示建立 H5 查看或微信视频通话，画面出现后按 Enter。
2. 脚本读取固件身份并开始采样，等待关键帧。
3. 按提示关闭查看页或挂断通话，确认断开后按 Enter 导出；只最小化页面不会断开连接。

**预期结果：**脚本检查偏移、长度和 FNV 校验值，保存 H264 及同名记录文件，再释放设备采样缓冲。安装了 ffmpeg 时，还会导出不自动旋转的首帧图像。

## 导出中断

设备仍保留有效样本时，确认通话已断开，换一个未使用的输出文件名：

```sh
python tools/capture_uplink.py --port PORT --output OUTPUT.h264 --dump-only
```

有样本时脚本不会覆盖它；零字节缓冲会释放并报错。记录 `VCAP STATUS` 的停止原因后重新建立视频，再采集。未成功导出的有效样本不要手动 clear。

## 如何判断

H5 与微信分别采样，并记录场景、固件和会话。查看原始像素方向时使用 `ffmpeg -noautorotate`，再与[视频规格](P4_MEDIA_ARCHITECTURE.md#视频)比较。

采样仅在启动后申请最多 512 KiB PSRAM，从完整关键帧开始记录。容量不足、锁竞争或导出不完整都会保留错误。采样会增加短时复制负载，定位性能问题时需与未采样基线比较。

相关主机检查：

- `tools/test_video_capture.py`：采样边界与数据校验。
- `tools/test_capture_terminal.py`：串口分段读取。
- `tools/test_capture_recovery.py`：停止、导出与空缓冲处理。
