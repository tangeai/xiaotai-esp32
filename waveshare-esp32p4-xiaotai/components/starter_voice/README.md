# P4 唤醒组件

本目录提供 P4 的组件入口。[CMakeLists.txt](CMakeLists.txt) 从同级 S3 工程读取唤醒源码、特征提取实现和模型，[Kconfig](Kconfig) 保留配置入口。请保留两个应用的相对目录关系。

- 算法入口：[starter_voice.c](../../../lckfb-szpi-esp32s3-tirtc/components/starter_voice/src/starter_voice.c)。
- 模型参数：[head.h](../../../lckfb-szpi-esp32s3-tirtc/components/starter_voice/model/head.h)。模型以 `RENAME_TO "nihaoxiaotai.tflite"` 嵌入固件。
- 特征提取：[mel_extractor.c](../../../lckfb-szpi-esp32s3-tirtc/components/starter_voice/vendor/mel_extractor.c)，其中保留 P4 的 ANSI FFT 分支。
- 依赖版本：[idf_component.yml](idf_component.yml)。
- 供应商来源：[UPSTREAM.md](vendor/UPSTREAM.md)。其中的复制路径是来源记录，当前构建位置以组件 CMake 为准。
