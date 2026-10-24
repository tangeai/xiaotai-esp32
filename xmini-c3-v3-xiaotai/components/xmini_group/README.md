# GROUP 控制模块

此模块只在现有业务控制线程运行，不创建任务、SDK 实例或音频所有者。HTTP、WHIP、信令和实际媒体启停通过回调交给 `xmini_business`；回调返回 `0` 表示受理，负值或 ESP 错误码表示失败。凭证和 JSON 均只在回调期间借用，接收方需要复制。

连接输入与RTC使用同一 `xmini_rtc_limits.h`：peer最多2047字节、token最多1023字节，不包含末尾NUL；原样交接，不解析或截断。GROUP不保存第二份字符串；RTC按实际长度复制并拥有至现有异步完成/取消/关闭释放点，未增加常驻大缓存。

- 每次打开先读取服务器 assignment；create/join/leave 的本地受理不改变持久关系。
- DISCONNECT、页面关闭和业务抢占只释放连接。LEAVE 携带原始房间 ID 和关系版本，收到服务端新 assignment 后才清关系。
- HTTP 票据、业务代次、assignment 版本、租约 session ID 分别保存。迟到回调不能启用新代次媒体；关闭完成必须通知 `on_closed`，即使尚未提交 RTC 连接。
- 每次连接生成带随机熵的新 session ID。仅 `joined` 成功续租，以请求开始时间计算租约下界，网络往返不会人为延长租约。
- `0x2200 join_room` 双向明确请求并验证 `g711a/8000/mono`，成功之后才调用媒体 profile barrier；音频流 ID 为 1。
- PTT 松开先关闭本地发送状态；业务音频桥还必须检查当前代次和 `active && speaking`，不能只依赖 `set_mic_state`。
- 验证并固定当前 RTC 连接的命名空间；快照验证 self 身份和去重人数。成员通知合并后重新读取快照，避免 C3 常驻 100 条完整成员表。当前公开快照提供人数及同步状态，不提供成员明细分页。

协议依据：[平台 device-room.md 固定版本](https://github.com/tangeai/tirtc-server-example/blob/44fb692700831c0ae83dd27b8816b71ab2a53fd9/thing-connect/device-room.md)、[同版本 HTTP 接口](https://github.com/tangeai/tirtc-server-example/blob/44fb692700831c0ae83dd27b8816b71ab2a53fd9/thing-connect/api-reference.md)、[Room 设备音频与信令](https://docs.tange.ai/products/room/guides/device-integration.html)。

`tests/test_group.c` 编译实际模块并使用官方cJSON，覆盖初读后操作、前导零、连接参数上下界、149字节完整peer、异步失败/再次连接、迟到回调、PTT、租约、命名空间和退出语义。RTC测试另覆盖调用方释放输入后的实际所有权。普通与ASan/UBSan主机验证不代替真机混音、双向声音、平台房间操作和弱网验收。
