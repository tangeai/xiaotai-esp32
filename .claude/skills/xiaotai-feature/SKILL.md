---
name: xiaotai-feature
description: 给小钛加一个新功能或新应用：闹钟、倒计时、时钟表盘、门禁机、开锁、可视门铃、新的语音或通话业务、新的设置项或 AT 指令。先把需求对到已有的模块和应用，定改哪一层，再按清单约定写组件（module.json / application.json、事件意图设置的命名、登记数量、配置、AT 指令），在产品清单里选上，写主机测试，最后编译上板。用户说“加个闹钟”“做个门禁/门铃”“加一个功能”“这个功能放哪一层”“加个设置”“加条 AT 指令”时使用。只改界面样式用 xiaotai-ui；整理已有代码不改行为用 xiaotai-refactor；接新板用 xiaotai-new-board。
---

# 加一个功能

目标：功能用已有的积木拼出来，差异写在清单和参数里，每个产品选不选它由产品清单决定。架构和清单的完整约定在 `docs/SDK架构.md`（第 1、2、6 节），加功能的分层表在 `docs/开发指南.md` 的“加一个功能”。

## 1. 先找已有的积木

动手前先查 `sdk/modules/` 和 `apps/` 里已经有什么，读对应的公开头文件（`include/xiaotai/modules/<名字>.h`）开头的说明：

| 需求 | 已有的 | 还缺的通常是 |
| --- | --- | --- |
| 闹钟、倒计时 | `clock` 模块：闹钟（每天或选定星期，重启后还在）、倒计时；到点发 `clock.due`；指令 `AT+CLOCK` 的 `ALARM`、`COUNTDOWN`、`REMOVE`、`ENABLE`。`alarm` 应用：时钟页、闹钟列表、设闹钟、倒计时、设置（响铃时长、稍后提醒、时区）、响铃和稍后提醒（`docs/功能/闹钟.md`），CoreS3 选了它，翻页后第一页 | 自定义铃声、闹钟名称 |
| 时钟表盘 | `clock` 模块的墙上时钟：时间是否已知、本地时间、每分钟 `clock.minute`、时区设置 `clock.timezone`；视图种类 `XIAOTAI_VIEW_CLOCK` 由主题画大字时间和日期 | 表盘样式（主题里画） |
| 门禁机、开锁 | `door` 应用的两个角色：室内机（缺省）和门禁机（设置 `door.sender`，`AT+DOOR SENDER`；公司门口那种：「呼叫前台」一键呼叫、「通讯录」呼叫员工微信或设备、门禁机自己拨出的设备和微信通话都由门禁页面显示），见 `docs/功能/门禁.md`；WT99 以它为首页，微雪 P4 也带。`call`、`wechat`、`contacts`。**`lock` 模块现在只有接口（头文件和 Kconfig），没有实现**；台上的板包也都没有 `door` 角色的继电器，产品选上 `lock` 会在选择阶段报缺 `door_lock`。没有门锁时，门口机可以用 `parameters.door.open_on_screen` 在屏幕上演示开门 | 实现 `lock` 模块、板包加继电器零件；开锁的授权规则、门磁状态 |
| 可视门铃 | `door` 应用、`video_call`、`stream_video`、`call` | 实体门铃键呼出、来访画面录像 |
| 刷脸、人脸识别 | 没有：SDK 里没有人脸检测和识别，也还不能把本机摄像头画面显示到屏上（`xiaotai/video.h` 只画对端画面） | 本地预览要新的视频源；识别要官方模型组件或平台接口，先问用户走哪条 |
| 应用自己拨出的通话由自己显示 | `door` 的做法：拨号时记下会话号，`claim()` 只认这个会话的 `call.*`／`wechat.*` 事件，清单 `handles` 列上这些事件，产品里排在 `companion` 前面 | — |
| 两台设备之间传应用消息（开门请求、确认） | `call` 模块的通话消息：已接通的设备通话里 `xiaotai_call_send_message`，对端收到 `call.message`，每条不超过 96 字节 | 应用自己定消息类型和超时 |
| 语音对讲、AI | `ai_talk`、`ai_vision`、`wake`、`recognition` | 新的意图或提示 |

已有模块够用，就只写应用或改参数；模块缺一个通用能力，就加在模块里（所有产品都能用），不要在应用里另起一套状态机或连接。

**表里写“有”的，动手前打开源码确认真的实现了**：看组件的 `CMakeLists.txt` 有没有源码列表、`src/` 在不在。只有头文件的模块（目前是 `lock`）选上以后在链接时才报缺符号。

## 2. 定改哪一层

依赖只向下：应用 → 基础模块 → 引擎和框架 → 硬件抽象 → 端口。

| 改的是 | 放在 |
| --- | --- |
| 交互流程、页面跳转、什么时候响什么 | `apps/<应用>/` |
| 可复用的业务行为、事件、状态 | `sdk/modules/xiaotai_mod_<名字>/` |
| 页面长什么样 | `sdk/themes/`（用 `xiaotai-ui` 看效果） |
| 新外设的驱动 | `sdk/parts/`，板包只接线 |
| 灯、振动、提示音怎么对应状态 | `xiaotai_feedback` 的表，声音放 `xiaotai_sounds` |
| 按键、旋钮手势到意图 | `xiaotai_interaction` 的表 |

判断不了时问自己：换一块板、换一个产品，这段代码还对吗？只对一款产品的，写成参数或产品清单里的选择，不写 `if (板名)`。

## 3. 按约定写组件

1. **目录和清单**：新模块 `sdk/modules/xiaotai_mod_<名字>/`，含 `module.json`、`CMakeLists.txt`（ESP-IDF 和主机两个分支，共用一份源码列表）、`include/xiaotai/modules/<名字>.h`、`src/`、`test/`。新应用 `apps/xiaotai_app_<名字>/`，清单是 `application.json`。照一个相近的现有组件抄结构。
2. **清单字段**（未知字段会报错）：
   - `depends`：调用了头文件的每个引擎和模块，和 CMake 的 `REQUIRES` 一致；只在产品选了才用的写 `optional_depends`（应用写 `modules` / `optional_modules`）。
   - `needs`：硬件需求，取值见 `sdk/xiaotai_hal/needs.json`；板子满足不了，选择阶段就会报错。
   - `registrations`：初始化最多登记多少个 `intents`、`subscribers`、`settings`、`commands`，按真正登记的条数算；只在选了某个可选依赖时才登记的，用 `when_selected` 写增量。登记数改了，同一改动里改清单。
   - `configure`：产品要给的配置，由生成的产品胶水在启动前调用 `xiaotai_<名字>_configure`。
   - `commands`：AT 指令，`access` 是 `read`、`action` 或 `guarded`；开发构建专用的再写 `option`。构建把选中单元的指令合成产品的 `generated/commands.json`。
3. **命名**：函数前缀是清单名；事件、意图、设置叫 `<领域>.<事情>`，领域默认是清单名。要用别的领域，先在 `docs/SDK架构.md` 的领域表和 `scripts/check_sdk_layers.py` 的 `DOMAINS` 各加一行。
4. **设置**：用户在使用中会改的值登记成运行期设置，存储交给框架（偏好服务决定放在哪）。不要做成编译开关，也不要自己读写 NVS。设置要不要改成新的含义（例如范围、单位变了），先想清楚已出货设备里存的旧值怎么办。
5. **任务和内存**：模块优先做成事件驱动，不为一个功能单开任务；要定时就用框架定时器。能放 PSRAM 的放 PSRAM；没有 PSRAM 的芯片用静态或开机一次分配，缓冲按实测最大值定。热路径不打日志，用事件日志记短标签加数字。
6. **页面和提示**：应用发状态，主题负责画；声音和灯走反馈表。

## 4. 让产品选上

在 `products/<产品>/product.json`：应用写进 `applications`，额外的模块写进 `modules`，参数写在 `parameters.<名字>`（节名就是清单名，生成的胶水把它交给 `configure`），构建期选项写 `options`。抢前台的活动（例如闹钟响铃要不要打断通话）用 `priorities` 和 `preemptions` 定。

`variants` 只能去掉模块（`without`）、改编译选项、替换参数字段，**不能加应用或模块**。新功能要么进产品的默认清单（看得见的行为先请用户确认），要么另建一个产品。

**一个产品带几个应用时，`applications` 的顺序有含义**：事件由第一个在 `handles` 里列了它、`claim()` 又接受的应用显示（`xiaotai/framework.h` 的 “Which application presents an event”）；没人认领的才归前台应用。`companion` 会认领它当前的每一通通话，所以只认领一部分通话的应用（`door` 只认领选定门口机的通话）要排在它前面。排到前面后，把这个应用 `handles` 里的每个事件都过一遍，看会不会抢走原来由首页应用显示的东西（`door` 还会认领实时查看）；要收窄就按能力或状态判断（是不是首页应用、在不在前台），不写产品名。页面翻页（`xiaotai_app_next`）按这个顺序循环，首页由 `home` 定。

选用 `door` 的产品要在 `parameters.preferences.bindings` 里写 `door_sender`、`door_peer` 两项，照 WT99 抄。

改完先跑 `python3 -B scripts/sdk_selection.py --check --strict`，再 `python scripts/build.py <板> --dry-run` 看选中的组件和生成的配置。编译后到 `compile_commands.json` 里确认新文件真的编进去了。

## 5. 测试

1. **组件的主机测试**：放在组件的 `test/`，用 `xiaotai_host_test` 登记，测试名以组件短名开头。依赖用假实现隔离（`INCLUDE_ONLY`），覆盖正常流程、拒绝和失败、重启后的保存值、开和关两种构建。应用的测试照 `apps/xiaotai_app_alarm/test/` 抄：用真框架（`xiaotai_framework_start`），被依赖的模块在测试文件里写假实现；收视图的 `xiaotai_view_set_sink` 要在一个夹具模块的 `init` 里装，在启动前装会被框架清掉。改了别的应用会用到的模块接口，给那些应用的测试夹具补上假实现，否则链接失败。
2. **产品主机测试**：`python3 -B tests/sdk/product/run.py --product <产品>`。启动检查会核对登记数量和实际容量；新的端到端场景写在 `tests/sdk/product/`。
3. **静态检查**：`scripts/check_sdk_layers.py`（分层、领域）、`scripts/check_structure.py`。
4. **上板**：编译烧录用 `xiaotai-build`，第一次上板走 `xiaotai-bringup`，回归用 `xiaotai-test`（新指令要加进 `--allow` 才能用 `xt.py` 调）。

## 6. 交付

- 用户看得见、听得见的行为（默认值、响铃方式、什么时候打断别的事）进默认之前，先给用户看效果，请他确认。
- 改了公开头文件、AT 指令、产品参数，同时更新 `docs/产品选项.md`、`docs/功能/` 下的说明（新功能加一页，并在 `README.md` 的“查功能接口”里加链接）和 `CHANGELOG.md`。`docs/AT指令.md` 的词表是生成的：`python -B scripts/generate_at_reference.py --write`，`--check` 会在词表过时时失败，不要手改。
- 改了 `door` 应用里的中文文案，要重新生成它的子集字库：`python scripts/generate_door_fonts.py`（要 Pillow 带 FreeType 和 fontTools），再 `--check`。不重新生成，大屏上新字会显示成方框。

汇报写成：需求对到了哪些已有积木 → 新增或改了哪些组件 → 哪些产品选上了 → 主机测试和上板结果 → 还没验证的项。
