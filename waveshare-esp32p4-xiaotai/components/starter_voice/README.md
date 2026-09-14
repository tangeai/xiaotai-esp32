# P4 唤醒组件

本组件从处理后的 PCM 识别“你好小钛”，识别成功后向应用提交唤醒事件。参数和模型位置如下：

| 内容 | 文件 |
| --- | --- |
| 识别流程 | [starter_voice.c](src/starter_voice.c) |
| 模型与参数 | [head.h](model/head.h)、[nihaoxiaotai.tflite](model/nihaoxiaotai.tflite) |
| 特征提取 | [mel_extractor.c](vendor/mel_extractor.c) |
| 应用配置 | [Kconfig.xiaotai](../../main/Kconfig.xiaotai) |
| 构建和依赖 | [CMakeLists.txt](CMakeLists.txt)、[idf_component.yml](idf_component.yml) |
| 供应商来源 | [UPSTREAM](vendor/UPSTREAM.md) |

模型以 `nihaoxiaotai.tflite` 嵌入，使用本目录资源。文件摘要见[版本与依赖](../../docs/DEPENDENCIES.md)。

## FFT 与卷积适配

特征提取保留 P4 ANSI FFT 路径。初始化、FFT 或位反转失败时不继续消费错误结果。

当前锁定 TFLM 1.3.5 / ESP-NN 1.3.2。上游 `conv.cc` 在 Prepare 和 Eval 两处将 filter channels 填为 0；模型首个普通卷积实际为 32 通道、3×1 卷积核，P4 因通道不匹配回退到 ANSI C。

[conv_channels.cmake](conv_channels.cmake)执行以下处理：

1. 校验上游文件 SHA-256。
2. 将两处参数替换为 filter 张量的实际通道数。
3. 生成 `build/p4_tflm/conv.cc`，替换原 TFLM target 中的该源文件。

下载缓存、模型和依赖版本保持不变。上游文件变化时配置会停止，需重新审阅适配；不要绕过哈希检查。升级 TFLM/ESP-NN 时，也应确认上游是否已修正、能否移除本地替换。

## 检查方法

在 P4 根目录运行，需要 CMake 和 Ninja：

```sh
python -B -X utf8 tools/test_conv_channels.py
```

该检查覆盖源码替换、重复配置、缓存不变及上游变化的拒绝处理。P4 向量计算输出、推理耗时和唤醒率仍需上板验证。

调节效果时分别记录不同语速、距离、环境噪声和扬声器播放中的识别情况。卷积适配不调整采集、AEC、增益、阈值或任务调度；相关音频路径见[系统架构](../../docs/P4_MEDIA_ARCHITECTURE.md#音频采集)。
