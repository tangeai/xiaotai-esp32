# 小钛

面向 ESP32 设备的 TiRTC 语音、通话和实时音视频工程。设备平台、会话、媒体和板级硬件分别维护，通过产品配置组合固件。

## 选哪个分支

| 路线 | 适合谁 | 从哪里开始 |
| --- | --- | --- |
| 现有板型工程（`main`） | 按既有板型体验、维护独立工程 | [ESP32 现有工程与使用引导](https://github.com/tangeai/xiaotai-esp32/tree/main) |
| 多芯片适配（`codex/multi-chip-v2`） | 在统一源码中维护多款板卡，或接入新板 | [选板、构建与适配](docs/MULTI_CHIP.md) |

本分支只做多芯片适配及必要的公共接口、配置和回归验证。每块板分别选配置、编译和验收；固件不跨板通刷。

**多芯片源码预览，基于 V0.1.0 开发基线。** 六个默认产品已纳入统一源码和独立构建入口。立创 S3 接入 TiRTC 2.6.0 / IDF 6.0.3 的 PSRAM 兼容 SDK；其他五板保留原 SDK 和 IDF 6.0.2 环境。各板仍保留原有业务、媒体和交互，部分运行代码与原板绑定，尚未达到“新板只适配硬件”的通用化验收条件。

此前立创 S3 的隔离试验包已有启动、联网和部分 AI 媒体证据，本版源码与固件的验证范围分别记录在 [V0.1.0 说明](docs/releases/v0.1.0.md)。尚未完成六板全功能真机验收，不作为生产稳定版承诺。

## 结构

```text
main/main.cc                     固件入口
main/boards/                     板型选择、物理资源与 C 兼容桥
main/xiaotai/application/        C++ 启动编排、C 启动服务与会话状态机
main/xiaotai/services/           设备平台、绑定、鉴权与业务请求
main/xiaotai/transport/          TiRTC 生命周期、连接代次与回调边界
main/xiaotai/platform/           NVS、配置、Wi-Fi 与配网
main/xiaotai/input/              按键与控制台输入
main/xiaotai/media/              采集、编码、播放、摄像头与唤醒
main/xiaotai/ui/                 各屏幕的现有交互与资源
components/tirtc_sdk/            分芯片 SDK、产品专用变体及 ABI 契约
components/xiaotai_lvgl/         LVGL 9 图像绘制任务适配
components/xiaotai_crypto/       IDF 6 PSA HMAC
vendor/                         保留来源和许可的板型专用依赖
config/                         公共配置与当前开发阶段
tests/                          主机回归、静态约束及 SDK 全符号链接检查
```

按职责混合 C/C++：C++ 管理应用编排和板级抽象，成熟的 C 状态机、驱动、算法和 SDK 接口保持行为。不会为了语言统一而重写媒体时序或协议。

## 当前入口

以下命令只查看配置，不编译、不访问设备：

```sh
python scripts/build.py --list-boards
python scripts/build.py lckfb/szpi-esp32s3 --dry-run
python scripts/build.py xmini/c3-v3 --stage sdk-check --dry-run
python scripts/check_structure.py
```

先查看 `--dry-run` 输出中的 `idf_environment` 和 `sdk_directory`，激活匹配的固定环境后，使用 `python scripts/build.py <board>` 构建。每板配置和产物独立，新构建从 `profiles/dependency-locks/` 装入该产品的依赖锁，已有构建的锁不会被覆盖；共享依赖缓存仍串行使用。该入口不烧录或发布。

功能选项目前只接受对应板型的保留默认集，尚不能任意裁剪。五个后续板的业务按 profile 隔离保留，尚未全部合并为一份通用状态机。

构建前检查 `capabilities.json`、媒体/交互需求和产品契约，缺失端口或不匹配时明确失败。无屏是完整的按键/灯/声音交互组合。契约匹配不代表运行时可移植；现有源图仍标记 `native-bound`，禁止改板名就套用旧硬件实现。

## 文档

- [架构开发指南](docs/架构开发指南.md)：加功能、做适配、选配置与编译，包含流程配图。
- [V0.1.0 说明](docs/releases/v0.1.0.md)：版本范围、环境、验证记录与已知问题。
- [本地诊断与 AT 指令](docs/本地诊断与AT指令.md)：维护入口及独立诊断构建。
- [架构与重构审查](docs/架构与重构审查.md)：本轮发现、职责、六板功能对照和进入调试的条件。
- [新板适配与兼容性契约](docs/新板适配与兼容性契约.md)：分层接口、无屏/多输入、媒体格式、兼容矩阵及尚待完成的运行时解耦。
- [验证状态](docs/VALIDATION.md)：本轮构建、主机证据与当前未验证范围。
- [来源与第三方许可](THIRD_PARTY.md)：必要的来源、版权和资源授权说明。

彩屏依赖 LVGL 9.5.0。立创 S3 专用 SDK 的 HTTP-only 能力只作用于 TiRTC SDK 路径，不改变其他服务的 TLS 配置。SDK 和整机的生产验收仍未完成。本分支提供源码预览，不提供可直接烧录的固件包。
