# 首页与新手指南配图

- [小钛功能概览动图](xiaotai-overview.gif)由项目维护者提供，保留原始 GIF 字节与动画，尺寸 750 × 360，共 60 帧。图示是项目的功能范围，不代表每块板都支持全部功能，也不作为本分支固件的验收依据。
- [立创 S3 实拍](lckfb-s3-device-front.webp)与[微雪 P4 实拍](waveshare-p4-device-front.webp)沿用项目既有的[设备照片](https://github.com/tangeai/xiaotai-esp32/tree/main/docs/images/boards)，保留原始 WebP 字节。照片用于对照外观，板型与固件还需分别核对。
- 首页复用原有[编译流程图](../architecture/07-build.png)，不另画一套流程。架构指南的九张技术图及可编辑源码见[配图说明](../architecture/配图说明.md)，内容正确的图保留，只在事实变化时同步修改。
- [新手指南](../../第一次使用.md)的配网和通话图直接写在 Markdown 的 `mermaid` 代码块中。图中文字、节点和连线可直接编辑，不使用生成式插画，也不模拟真实界面。

## 内容依据

六板默认功能对照 [product-contracts.json](../../../config/product-contracts.json)，屏幕逻辑尺寸与硬件条件分别对照各板 `capabilities.json` 和 `sdkconfig.xiaotai`。默认功能与真机验证状态分开记录。

配网时序对照本分支 `wifi_manager` 与 Xmini/MZ28 的 `wifi_provisioning` 实现：热点为 `XiaoTai-XXXX`，配网页为 `http://192.168.6.1`。手机先连设备热点，设备联网后手机再恢复上网，随后进行账号绑定。

通话图用于分别检查声音的两个方向和挂断结果。箭头表示声音方向，不表示网络直连或当前六板已全部通过验收。通过条件仍需实际听音，不以在线标志或计数代替。

GitHub 支持 [Mermaid 技术图](https://docs.github.com/en/get-started/writing-on-github/working-with-advanced-formatting/creating-diagrams)。其他 Markdown 阅读器需要启用相应支持；不支持时仍可按图旁的文字步骤操作。
