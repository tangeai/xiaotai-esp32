# 唤醒常量表来源

| 文件 | 来源 | SHA-256 |
| --- | --- | --- |
| `head.h` | 同包唤醒模型的输出层参数，原件来自 `nihaoxiaotai_v9.3_20260915_nhwc_tflite.zip`。见[原模型记录](../../esp-idf/model/README.md)。 | `cc0364fa603a43c90d9de0b0fc39587c7c4d3350c3d1537b7018cf3a923d1a1e` |
| `mel_filterbank.h` | `voicute/onnx-wakeword` 的 `esp32/sdk/voicute/mel_filterbank.h`，提交 `61e76c5a12ac33da0def7bceba2e07228ad03e63`。见[原前处理来源记录](PREPROCESS_UPSTREAM.md)。 | `43ef33d029766e64888757437fd0e76391b50570fa75c82b0e4e1723fb050ca6` |

项目所有者确认以上随附资产可按 MIT 再分发，授权范围见[第三方清单](../../../../THIRD_PARTY.md)。这不改变上游仓库的许可，也不自动覆盖新文件或未来模型。

这两个文件与旧 S3/P4 工程的副本逐字节一致。保留原有版权信息、不补写 SPDX 到常量表内；`mel_filterbank.h` 还保留原有 CRLF。表中哈希用于检查搬迁完整性；本目录不包含配套的 `.tflite` 文件，不能据此判断真机推理或识别率。
