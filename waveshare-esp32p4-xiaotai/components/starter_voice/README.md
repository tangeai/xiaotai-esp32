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

当前使用 `nihaoxiaotai_v9.3_20260915_nhwc_tflite` 配套模型和分类头。输入仍按时间优先排列为 `[1,98,32]`；模型首层自行转换为 `[1,98,1,32]`，应用不要额外转置。输入和输出的量化参数从张量读取，不沿用旧模型的固定值。

该包只包含模型和 `head.h`，未附特征提取实现或新的归一化说明。本次保持现有 16 kHz PCM、Mel 提取、唤醒阈值和音频处理不变。主机契约检查不能证明训练前处理一致性或实际识别率，替换后仍需实测。

## FFT 与卷积适配

特征提取保留 P4 ANSI FFT 路径。初始化、FFT 或位反转失败时不继续消费错误结果。

当前锁定 TFLM 1.3.5 / ESP-NN 1.3.2。上游 `conv.cc` 在 Prepare 和 Eval 两处将 filter channels 填为 0；模型首个普通卷积实际为 32 通道、3×1 卷积核，P4 因通道不匹配回退到 ANSI C。

[conv_channels.cmake](conv_channels.cmake)执行以下处理：

1. 校验上游文件 SHA-256。
2. 将两处参数替换为 filter 张量的实际通道数。
3. 生成 `build/p4_tflm/conv.cc`，替换原 TFLM target 中的该源文件。

下载缓存、模型和依赖版本保持不变。上游文件变化时配置会停止，需重新审阅适配；不要绕过哈希检查。升级 TFLM/ESP-NN 时，也应确认上游是否已修正、能否移除本地替换。

## 检查方法

构建配置阶段会校验上游卷积源码摘要并生成适配后的 `build/p4_tflm/conv.cc`；摘要或替换点不匹配时直接停止配置。链接后的固件门禁还会检查产品入口和驱动所有权。

模型更换后，先核对模型与分类头摘要，再完成一次干净构建。设备启动时应出现 `KWS nhwc-20260915` 和 `wake self-test=ESP_OK`；随后分别测试快、正常、慢语速，记录命中次数与推理耗时。静态检查和构建均不能代替实际唤醒率验证。

调节效果时分别记录不同语速、距离、环境噪声和扬声器播放中的识别情况。卷积适配不调整采集、AEC、增益、阈值或任务调度；相关音频路径见[系统架构](../../docs/P4_MEDIA_ARCHITECTURE.md#音频采集)。
