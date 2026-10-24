# 本地 I2S 生命周期补丁

来源：组件仓库 `espressif/esp_codec_dev 1.6.2`，原组件 hash `4779f31a3ba3f9b38aee1afe6ccb9ba8f6108dc3a6e77d39d7d714e9a215371b`。除本说明和 `platform/audio_codec_data_i2s.c` 外，保留组件源码及授权原文；不携带修改后失效的组件校验元数据。

2026-10-15，Xmini-C3-V3 / ESP-IDF 5.5.5 真机复现：

1. `esp_codec_dev_close()` 正常关闭 TX/RX。
2. 下一次 `esp_codec_dev_open()` 调用 `set_fmt()`。
3. `set_fmt()` 无条件调用 `_i2s_drv_enable(..., false)`，对已经关闭的通道再次执行 `i2s_channel_disable()`，每次出现两条 `the channel has not been enabled yet`。
4. 上层没有传播这次停通道失败，后续重配和录放仍完成，因此帧数正常不能单独作为通过条件。

补丁在 I2S 数据适配层使用 IDF 5.5.5 提供的 `i2s_channel_get_info().is_enabled` 查询驱动实际状态，已经处于目标状态时直接完成幂等操作，其他状态继续执行真正的驱动切换并保留失败返回。不能用 codec 的逻辑 `in_enable/out_enable` 代替驱动状态，因为临时重配期间二者不同。

没有降低日志级别、保持 codec 常开、增加重试或延时。补丁仅用于本项目固定的 IDF 5.5.5，双设备原触发条件的复验结果记录在项目回归文档中。后续升级组件时先核对该状态处理，不把本地修改覆盖回组件缓存或其他板型。
