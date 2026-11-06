# 立创 S3 专用 SDK

本变体来自 2026-11-05 立创实战派 S3 的独立验证包，仅由 `lckfb/szpi-esp32s3` 选择。CoreS3 使用原通用 S3 库，P4、MZ28 和 Xmini 的库不变。

| 项目 | 固定值 |
| --- | --- |
| TiRTC Nano | 2.6.0，源码 `894972fbf2f5a8be921756a611dc68a14c528e25` |
| TGWebRTC | `tag.v1.5.29`，`64b74c0a3be692f03bd42ded3a6ebbcb1162ce24` |
| TgSysAdpt | `37cf0662382d51dd60366f25df0db3f1b0219bd4` |
| IDF / 编译器 | 6.0.3 / GCC 15.2.0，`esp-15.2.0_20251204` |
| `libTiRTC.a` SHA-256 | `a3acbc149d88af766d2cf41b31b20000d52f53941ce9d70618c3eaa5ea36ee5c` |
| HTTP TLS 能力 | `none`，构建时检查实际归档，不继承应用 Mbed TLS 开关 |

`lib/contract.json`、库和 `include/tirtc/` 下两个头文件保持验证包原件。头文件与通用包目前字节相同，仍单独保存和核对，避免后续升级混用。SDK 名称中的 `test`、`production_ready=false` 和 `hardware_tested=false` 保留其交付时的声明；具体应用实测范围见版本文档，不凭一次短测改写这些声明。

此库保留 IDF 的 PSRAM 感知原子操作编译选项和 Picolibc 配置，归档中不含 `s32c1i` 指令。应用继续核对 FreeRTOS、crypto、libc、工具链和原子操作契约。`lib/sdkconfig.defaults` 显式保留生产端及已测应用的证书包配置，避免新配置引入未匹配的默认值；不绕过 ABI 检查，也不统一降级其他 HTTP/MQTT 服务。

IDF 6.0.3 验证环境来自固定构建镜像。由于该环境的 IDF 为源码归档而非 Git checkout，构建入口用 `scripts/idf_environment.py` 核对根 CMake/Kconfig、components、tools 共 20,927 个文件的路径与内容摘要；Python 缓存和 Git 元数据不计入。6.0.2 仍按原 Git 提交核对。镜像与摘要用于识别已验证环境，不表示该镜像已公开发布。

本次仅消费已验证静态库，没有再次改写或复编 Nano/TGWebRTC 业务。原桌面验证包、通用 SDK 和原工程均保留。PSRAM 原子操作的兼容修正不等于 AEC、相机或 AI 长会话问题已修复。
