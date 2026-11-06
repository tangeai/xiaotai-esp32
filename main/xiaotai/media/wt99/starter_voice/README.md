# P4 唤醒组件

本组件识别“你好小钛”，保留本板原有模型、分类头、16 kHz PCM、Mel 前处理、阈值及事件提交顺序。

- [识别流程](src/starter_voice.c)与[前处理来源](vendor/UPSTREAM.md)。
- [模型](model/nihaoxiaotai.tflite)与[分类头](model/head.h)保持原文件。
- [应用参数](../../../application/wt99/startup/Kconfig.xiaotai)由当前产品配置选择。
- [依赖声明](idf_component.yml)与[构建文件](CMakeLists.txt)是当前依赖来源，不能照搬旧工程构建结果。

IDF 6 迁移不改采样顺序、AEC、模型输入排列或量化参数读取。当前模型以时间优先的 98 帧输入，模型内部完成 NHWC 转换，应用不额外转置。P4 使用原有 ANSI FFT 路径，计算失败仍保留错误。

构建、唤醒识别率及扬声器播放中识别效果分开验收；详见[六板迁移审查](../../../../../docs/架构与重构审查.md)。
