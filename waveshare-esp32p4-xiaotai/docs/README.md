# 文档导航

第一次使用先读[开发指南](GETTING_STARTED_CN.md)，按顺序完成编译、配网和绑定。其余文档按需要查阅。

| 你现在要做的事 | 文档 |
| --- | --- |
| 准备环境，烧录并体验功能 | [开发指南](GETTING_STARTED_CN.md) |
| 修改界面、音频、视频或会话逻辑 | [系统与媒体架构](P4_MEDIA_ARCHITECTURE.md) |
| 定位问题，选择回归检查 | [测试与排障](TESTING.md) |
| 核对工具链、SDK、模型和配置 | [版本与依赖](DEPENDENCIES.md) |
| 查看当前版本的功能与改动 | [版本记录](../RELEASE_NOTES.md) |

## 专项资料

- [C6 准备与恢复](C6_PREPARATION.md)：C6 固件缺失、版本不兼容或 SDIO 初始化异常时使用。
- [视频抓流](VIDEO_CAPTURE.md)：导出进入 SDK 前的 H264，检查编码和方向。
- [唤醒组件](../components/starter_voice/README.md)：模型接入、FFT 和卷积适配。
- [TiRTC SDK](../components/tirtc_sdk/VERSION.md)：库版本、补丁与校验值。
- [Hosted 修正](../components/espressif__esp_hosted/LOCAL_CHANGES.md)：主机驱动的并发与资源回收修正。

供应商组件内的 README 和示例说明对应其上游工程。小钛的板型、入口和操作步骤以本目录文档为准。
