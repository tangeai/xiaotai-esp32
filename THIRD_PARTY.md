# 来源与第三方许可

产品实现以小钛的设备平台、TiRTC 会话和现有交互为中心。来源与版权记录独立于产品代码，保留必要的署名，不把参考实现冒充原创。

## 架构与硬件参考

参考基线为 `78/xiaozhi-esp32@a78ca19d5bc9a12ee0853b9f509530276d36aa03`（小智）。借鉴其单工程多板配置、板级选择和硬件组织方式。当前 Board 和 Application 接口已按小钛职责重新编写，不包含原产品云服务、激活、MCP 或 OTA 实现。

原始参考快照、全量路径/哈希映射、未适配板级代码已归档在仓外，便于以后对照成熟驱动；不将未适配驱动标记为本产品已支持。根 [LICENSE](LICENSE) 保留原版权和 MIT 文本，未改署名。新的实现表达不抹去参考来源。

## 小钛业务与资源

现有六个小钛工程是功能基线。六板的业务、媒体、UI、专用驱动及资源均已按 profile 迁入，源码接入不代表功能验收通过。各板差异不能据共同的芯片型号互相替代。

Xmini 接线定义的历史来源包括 `78/xiaozhi-esp32 v2.5.2 main/boards/xmini/c3-v3/config.h`；接线事实与版权归属保留，产品代码不依赖其服务。板级 `bounded_lcd_i2c.c` 派生自固定 IDF 6 快照的 `esp_lcd/i2c/esp_lcd_panel_io_i2c.c`，保留 Espressif Apache-2.0 署名，增加每实例传输超时以承接原工程的 100 ms OLED 边界。

既有业务许可与资源说明保存在 [原业务许可](docs/licenses/xiaotai-s3-LICENSE) 和 [原资源授权记录](docs/licenses/xiaotai-s3-THIRD_PARTY.md)。这两份文件保留历史原文，其中目录引用按原工程理解。

唤醒前处理与模型分别记录在 [前处理来源](main/xiaotai/media/voice/vendor/UPSTREAM.md) 和 [模型说明](main/xiaotai/media/voice/model/README.md)。延续项目所有者对已打包资源的授权范围，不将其扩展到其他版本或未来资源。

## SDK 与依赖

TiRTC 通用 S3/P4/C3 隔离验证包保持原样；Xmini 另有原 Mini 源码的 IDF 6 重编变体，见 [Mini 来源与边界](components/tirtc_sdk/variants/xmini/README.md)。立创 S3 独立选择 [2.6.0 / IDF 6.0.3 HTTP-only 变体](components/tirtc_sdk/variants/szpi-http/README.md)，不替换 CoreS3 的通用 S3 库。均尚未获得生产验收；公开分发条件按实际交付范围核对。

原通用兼容层使用的 [Mbed TLS 3.6.5 许可](docs/licenses/mbedtls-3.6.5-LICENSE) 保留。新增立创 S3 变体的 DES 实现来自锁定 TGWebRTC 中的 Mbed TLS 3.2.1（Apache-2.0），不将其标成小钛原创或统一改为 MIT；对应许可随变体保留。

IDF、LVGL、音频 codec、模型运行时等各依赖沿用原始 LICENSE/NOTICE，不改成产品自有版权。`vendor/` 中保留上游未启用模型的目录项、历史说明和署名；它们不代表产品启用这些模型。构建检查拒绝参考产品的唤醒词配置。供应商适配补丁与依赖身份必须单独记录。
