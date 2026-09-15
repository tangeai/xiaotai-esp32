# 构建与检查工具

正常编译只需在工程根目录执行 `idf.py build`。下面的主机测试和资源生成工具按需单独运行，不需要连接开发板。

## 构建检查

链接后自动调用 `check_build_policies.py`，执行 21 项源码/ELF 检查，兼容 Windows 和 Unix。查看规则或单独检查 AEC：

```sh
python tools/check_build_policies.py --help
python tools/check_build_policies.py --policy aec
```

需要 ELF 的规则同时传入 `--elf build/lckfb_szpi_esp32s3_tirtc.elf`。使用 ESP-IDF 终端，便于找到对应的 `nm`、`objdump`；也可通过 `--nm`、`--objdump` 指定工具。

旧 Bash 转发脚本已归档，统一使用此 Python 入口。

## 主机回归

建议在 Linux/WSL 中准备 Bash、Python 3、`cc`、`c++`、CMake 和 ASan/UBSan。部分测试读取 cJSON、TFLM、LVGL 源码；先在本工程的 ESP-IDF 环境执行 `idf.py reconfigure` 下载锁定依赖。

```sh
bash tools/run_host_tests.sh
```

入口运行 42 项脚本，缺少工具或依赖时提前报错。测试覆盖部分实际 C 函数、模拟平台交互和资源契约；通过不代表设备时序、真实网络或听感通过。仅运行主机测试时，不要在 WSL 重新配置 Windows 的 `build/`。

`test_audio_only.py` 保留原脚本名，现检查 S3 视频仅允许 H5 订阅、其他模式仍为音频；`test_h5_video_send.py` 检查 JPEG 发送的代次、积压、错误返回和本地断连保护，不模拟摄像头 DMA 或证明远端帧率。

`test_device_profile.py` 检查上报三个场景的实际 JSON、编码声明、在线/在途互斥与失败后的相同快照重试。


`test_nvs_worker.py` 检查实际执行器的并发、快照合并和超时所有权；`test_runtime_config.py` 检查绑定凭据读写与失败语义。主机模拟不证明 Flash 擦页、掉电安全或设备栈余量。

`test_room.py` 和 `test_room_ui.py` 检查房间状态机、按住讲话门控、页面生命周期与数字键盘；`test_home_wake_hint.py` 检查首页提示与会话状态的一致性。

`test_ui_routes.py` 执行当前 14 个页面的实际路由和销毁逻辑，检查重复切换、房间退出、待处理意图保留及非法页面报错；具体布局和绘制由房间、表情等测试分别覆盖。

`test_wifi_credentials.py` 检查当前凭据、最近 5 个成功网络的历史和密码复用；`test_wifi_portal.py` 检查异步扫描、连接优先、超时清理、列表 JSON 和密码不外传；`test_wifi_manual_disconnect.py` 检查主动断开、持续重连及缺失 DNS 的配置处理。它们模拟驱动和存储，不证明手机热点兼容、DNS/NTP 可达或射频扫描时序。

`test_face_animation.py` 使用实际表情绘制代码和 LVGL 数学函数，检查 23 类共 44 个可用姿态、动作切换、局部刷新与定时器复用；思考和放松固定第一套，其余类别二选一。检查使用 ASan/UBSan，不连接开发板。`test_clock_fallback.py` 检查本次开机首次校时、备用服务器等待、失败返回与后续复用。

`test_wake_model.py` 编译并执行 `test_voicute_model.cpp`，检查当前模型的结构、NHWC 卷积通道、算子和配套 head 数值边界，不执行神经网络推理或证明识别效果。脚本数量表示检查范围，实际通过项以对应一次运行的完整日志为准。

## 修改图标

`generate_phone_shortcut.py` 使用 Pillow，从保留的 PNG 生成电话图标数组。只有修改该图标时才运行，不属于日常构建步骤。

设备使用与故障排查见[开发指南](../docs/GETTING_STARTED_CN.md)。
