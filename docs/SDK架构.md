# SDK 架构

[文档地图](README.md) · [架构总览](架构总览.md) · [开发指南](开发指南.md)

**给谁看**：正在写或修改组件的开发者。这一篇是规范，条目密，适合对照着查；先想了解思路，读[架构总览](架构总览.md)。

小钛把共享业务做成可组合的 SDK。板包提供硬件和能力，产品清单选择应用、模块与参数。核心在目标芯片上运行，也能在主机上用假后端测试。

本文里的目录和 API 对应 `sdk/`、`apps/`、`boards/` 与 `products/` 的产品图。从 v0.2 升级见[迁移说明](迁移说明.md)。

## 1. 分层

```text
产品配置            板子 + 应用列表 + 主题 + 参数（不写代码）
应用（apps/）       伴侣对讲机、门禁机、语音终端、闹钟……：业务、交互、界面流程
基础模块（sdk/modules/）  AI 对讲、通话、配网、绑定……：提供可复用的业务行为和事件，不负责页面或产品编排
运行框架（sdk/xiaotai_framework，必选）  生命周期、意图、事件和呈现应用、定时器、前台活动和会话扩展、静默、有序重启、设置、指令、诊断、视图
引擎与服务（sdk/xiaotai_*）  事件日志、控制台、身份与偏好、网络、平台核心、声音、媒体、传输、视频、唤醒、界面、反馈、交互
硬件抽象（sdk/xiaotai_hal）+ 零件库（sdk/parts/）+ 板包（boards/）  零件接口和描述符；板子按数量提供零件
端口（sdk/xiaotai_port）+ 通用库（sdk/xiaotai_json）  os mem kv log sys err clock http mqtt console config；不分配内存的 JSON 读写
平台                ESP-IDF（ESP32-S3、P4、S31，按板卡使用锁定工具链）、主机（Linux）
```

依赖只向下。应用用基础模块，基础模块用引擎和框架，引擎用硬件抽象和端口；引擎不依赖基础模块，下层不包含上层的头文件。组件之间的依赖图和各层的接口约定见各组件的头文件；一个产品带哪些组件由清单展开决定（第 6.3 节）。

各层能用哪些层（`scripts/check_sdk_layers.py` 按路径定层，对 `#include "xiaotai/..."` 和 CMake 里出现的每个组件名检查）：

| 层 | 路径 | 能用 |
| --- | --- | --- |
| 端口、通用库 | `sdk/xiaotai_port`、`sdk/xiaotai_json` | 端口（通用库还能用端口） |
| 硬件抽象 | `sdk/xiaotai_hal` | 端口、通用库 |
| 零件库 | `sdk/parts/*` | 端口、通用库、硬件抽象、别的零件 |
| 板包 | `boards/<厂商>/<板>` | 以上全部 |
| 运行框架 | `sdk/xiaotai_framework` | 端口、通用库（不碰板子） |
| 公开 API | `sdk/xiaotai_sdk` | 端口、通用库、硬件抽象、框架 |
| 引擎 | 其余 `sdk/xiaotai_*` | 端口、通用库、硬件抽象、框架、清单里声明的引擎 |
| 主题 | `sdk/themes/*` | 同引擎，加声明的主题 |
| 基础模块 | `sdk/modules/*` | 同引擎，加声明的基础模块 |
| 应用 | `apps/*` | 同基础模块；不用别的应用 |

同层之间（引擎、主题、模块、应用）的边必须写在清单的 `depends`、`optional_depends`（应用是 `modules`、`optional_modules`）里；反过来，清单的 `depends` 必须出现在 CMake 的 `REQUIRES` 里。硬件抽象链接选中的板包（板子通过硬件抽象调用的 `SelectedBoard()` 登记自己），但不包含它的任何头文件。

引擎一览（清单名 → 组件）：

| 清单名 | 组件 | 作用 | 依赖的引擎 |
| --- | --- | --- | --- |
| `eventlog` | `xiaotai_eventlog` | 内存里的事件环和实时上报信箱，取代从日志文本里读状态 | — |
| `at` | `xiaotai_at` | 控制台和统一的 AT 词表 | `eventlog` |
| `identity` | `xiaotai_identity` | 设备身份、绑定代次、偏好存放位置 | — |
| `net` | `xiaotai_net` | 联网状态机、凭据、配网、维护门、重启 | `eventlog`、`at` |
| `platform` | `xiaotai_platform` | 时钟、服务发现、登录、信令按类型分发、请求池、设备档案 | `identity`、`net`、`eventlog`、`at` |
| `sounds` | `xiaotai_sounds` | 提示音、铃声、引导语，按名字找 | — |
| `media` | `xiaotai_media` | 音频管线、下行缓冲、提示音播放、录音、唤醒前回放 | `sounds`、`eventlog`、`at` |
| `transport` | `xiaotai_transport` | 实时链路（音视频、命令）和它下面的 RTC 协议栈 | `identity`、`net`、`platform`、`media`、`eventlog`、`at` |
| `video` | `xiaotai_video` | 相机上行、对端画面解码、码率调节 | `transport`、`eventlog`、`at` |
| `wake_engine` | `xiaotai_wake` | 唤醒词 | `media`、`eventlog`、`at` |
| `ui` | `xiaotai_ui` | 视图、主题选择、触摸和导航键 | `eventlog`、`at` |
| `feedback` | `xiaotai_feedback` | 状态到灯、振动、提示音（表驱动） | `media`、`eventlog`、`at` |
| `interaction` | `xiaotai_interaction` | 按键、旋钮手势到意图（表驱动） | `eventlog`、`at` |
| `theme_lcd320`、`theme_lcd480`、`theme_mono128` | `sdk/themes/` 下同名组件 | 界面主题，按显示描述符匹配 | `ui` |

接入流式声音时，由模块声明编码和采样率，板包只提供音频输出零件。具体调用顺序和失败处理见[流式声音接入](功能/流式声音.md)。

`eventlog`、`at` 和 `identity` 是基础引擎：每个产品都带，按这个顺序最先初始化，其他组件直接登记事件日志和指令；身份引擎在自己的 init 里把偏好服务装成框架的设置存储（第 4 节）。`xiaotai_json` 是通用库，没有清单和生命周期，每个产品都链接。

有产品配置的引擎、基础模块和应用提供 `xiaotai_<名字>_configure(const xiaotai_<名字>_config_t *)`（标注 `[glue]`），由构建生成的产品胶水在框架启动前调用（例如绑定、联系人、环境、设置、门锁模块，以及时钟模块的 `xiaotai_clock_module_configure`），和引擎的做法一样；清单的 `configure` 条目登记它。基础模块的构建期选项放在 `include/xiaotai/modules/<名字>_config.h` 加组件自己的 `Kconfig`（绑定、门锁），写法同第 3 节。

函数前缀等于清单名；下层组件已经占用这个前缀时换一个，并在头文件里写明原因（唤醒模块用 `xiaotai_local_wake_`，时钟模块用 `xiaotai_wallclock_`、`xiaotai_schedule_`）。事件日志来源和引擎的调参键一律用清单名。事件、意图和设置的名字是 `<领域>.<事情>`：领域默认就是清单名；不用清单名的领域登记在下表里，每个领域只有一个所有者（`scripts/check_sdk_layers.py` 的 `DOMAINS`，对各组件头文件里发布的 `XIAOTAI_EVENT_*`、`XIAOTAI_INTENT_*`、`XIAOTAI_SETTING_*` 检查）：

| 领域 | 所有者 | 原因 |
| --- | --- | --- |
| `system`、`activity`、`app`、`setting`、`view` | 运行框架 | 框架没有清单名，这几个是它自己的事件和意图（`system.started`、`system.restart`、`system.menu`、`activity.changed`、`app.foreground`、`setting.not_saved`、`view.changed`） |
| `talk` | `ai_talk` | 意图名是测试工具和语音指令里已经固定的写法 |
| `vision` | `ai_vision` | 同上 |
| `audio`、`screen`、`guidance` | `settings` | 各产品共用的设置和它们的意图，只由通用设置模块登记 |
| `input` | `interaction` | 手势事件，不管来自哪个零件 |
| `terminal` | `voice_terminal` | 语音终端应用的事件和意图 |

加一个领域就是在这张表和 `DOMAINS` 里各加一行。

## 2. 规则

1. `sdk/`、`apps/` 和 `boards/` 下的代码不包含任何平台头文件（ESP-IDF、FreeRTOS、驱动、`sdkconfig.h`、pthread、套接字、TLS 库），只通过 `xiaotai/port/*.h` 和零件接口用平台。例外是 `sdk/`、`apps/` 里任一组件名为 `esp-idf/`、`host/` 的平台目录，以及 `test/` 测试目录（见第 6 节）；板包只有 `test/` 例外。第三方库的头文件（LVGL、RTC 协议栈、cJSON）只出现在 CMake 里写了这个库的组件里。创建总线、通道和编解码设备的调用（`i2c_new_master_bus`、`i2s_new_channel`、`esp_codec_dev_*`、`esp_lcd_new_panel_*`、`spi_bus_initialize` 等）只出现在 `sdk/parts/*/esp-idf/` 和 `sdk/xiaotai_port/esp-idf/`：引擎的 `esp-idf/` 用零件，不自己建硬件。依赖方向见第 1 节的表。需求表 `sdk/xiaotai_hal/needs.json`、`xiaotai/needs.h`、`sdk_selection.py` 三处一致，词表里的 C 枚举都在 `xiaotai/hal.h` 里。可移植代码（`sdk/`、`apps/` 里 `esp-idf/`、`host/`、`test/` 以外）还要满足三条，因为 ESP-IDF 上 `xiaotai/config.h` 会带进 `sdkconfig.h`、`xiaotai/port/os.h` 会带进 RTOS，只查包含行看不出来：代码里不读 `CONFIG_*`（配置头文件 `*_config.h` 除外）、`ESP_PLATFORM`、`IDF_TARGET*`、`SOC_*`；不直接调 RTOS、ESP-IDF 和驱动函数（`xTaskCreate`、`vTaskDelay`、`heap_caps_*`、`esp_*`、`nvs_*`、`gpio_*` 等）；代码和注释里都不出现板名、芯片名、产品名（`esp32*`，单独成词的 `s3`、`p4`、`c3`、`c6`、`c61`，各板包的厂商和系列名，`boards/` 下的目录名；`port/sys.h` 里的一处示例在白名单里）。处理函数会抹掉身份、忘记网络凭据或带 FORGET 重新配网的意图必须用 `xiaotai_intent_handle_ex(..., XIAOTAI_INTENT_LOCAL)` 登记。事件、意图、设置的名字按第 1 节的领域表。`scripts/check_sdk_layers.py` 检查以上全部，`check_structure.py` 和主机测试都会跑它。
2. 内存按用途要：大块缓冲用 `XIAOTAI_MEM_BULK`（有 PSRAM 就在 PSRAM，没有才用内部 RAM，有 PSRAM 时不悄悄回落到内部 RAM）、实时环路用 `XIAOTAI_MEM_INTERNAL`、DMA 用 `XIAOTAI_MEM_DMA`。代码里不写 `heap_caps_*`。
3. 存储调用都在端口的存储工作线程上执行，闪存写入不会跑在调用者（可能在 PSRAM 上）的栈上。
4. 板子差异只走零件描述符、端口实现和产品配置；`sdk/` 代码不出现板名、芯片名、产品名。
5. 采集、播放、音视频帧回调不分配内存、不打日志，只计数；诊断读计数。控制消息与媒体帧分开。默认常驻预留控制消息载荷；产品可选择 RTC 控制指令的 `command_pool_mode` 或 MQTT 信令的 `signal_payload_on_demand`。按需时只在各自任务的消息入口申请，先查额度，再按实际长度复制；载荷处理或拒收后归还。回调不持临界区分配，不把这套做法用到逐帧路径。
6. 跨组件的媒体和链路调用都带 `uint32_t` 代次（0 无效），过期的代次被丢弃并计数。会话代次全设备只有一个来源：`xiaotai_generation_next()`（`xiaotai/framework.h`）。绑定代次（`xiaotai/identity.h`）和网络地址代次（`xiaotai/net.h`）是各自独立的计数，不和会话代次混用。
7. 结果一律是 `xiaotai_err_t`。组件只为调用者必须区分的情况新增错误码，从自己的号段取（`xiaotai/port/err.h` 里有号段表）。
8. 每个公开函数标注谁可以调，含义只在 `xiaotai/framework.h` 开头定义一次，各头文件同义，端口也用它们（端口只另加 `[isr]`）：`[glue]`（主任务上、框架启动之前：产品胶水和 `xiaotai_start()` 本身；这里不启动任何东西，唯一例外是先打开事件日志、再打开控制台，好让启动失败有记录、有应答）、`[board]`（`xiaotai_hal_init()` 里板子的 `Init()`）、`[init]`（引擎、模块、应用的 init 里；只允许其中一部分时写成“`[init]`，不含引擎”）、`[owner]`（框架所有者任务）、`[any]`（任意任务；会等待的写明等多久，从不等网络）、`[rt]`（含实时任务：不分配、不打日志、只计数；只为自己的输入或节拍阻塞，不等别的组件）；可以组合（`[glue, init]`）。头文件需要别的标注时在文件开头定义，不能重述或换掉这几个的含义。除非写明，任何函数都不能在中断里调。
9. 产品工程只用公开 API（`sdk/xiaotai_sdk/include/xiaotai/xiaotai.h`）、板包和产品配置。

### 无外部内存的工作区

会话内存峰值期间持续使用的工作区，优先静态存放或在开机时一次预留，避免反复拆分内部堆。获取令牌的 HTTP/TLS 缓冲、握手解析暂存等若能在峰值前归还，就继续用完释放；配网门户等低频功能可以按需申请。缓冲仍按用途选内存能力，DMA 不借普通工作区充当；也别把大临时对象挪到任务栈上。

只有生命周期确实互斥的业务才能共用工作区。按已测最大用量和契约边界定容量，交接所有权时先等上一代的回调、队列和写入完成。该规则按能力选取，不在公共代码里按板名分支；实测、兼容验证和故障语义不因内存优化而省略。

没有外部内存的产品按能力选用只含 IPv4 的配置；有外部内存的产品不变。

选用 PCM/I²S 音频零件且无外部内存的产品，默认开启 `AUDIO_RETAIN_I2S_DMA`，产品清单可显式关闭。音频零件开机预留 RX/TX 通道和 DMA，空闲时关闭 codec、功放和通道时钟，保留 DMA，下一次打开前清理 TX 残留。无 I²S 零件的语音协处理器产品和有外部内存产品的缺省策略不变。这会把部分申请提前到开机，并不会增加可用内存；产品验收还要核对配网和会话的内存余量。

## 3. 端口

| 头文件 | 内容 | ESP-IDF 实现 | 主机实现 |
| --- | --- | --- | --- |
| `port/os.h` | 时间、延时、任务和栈类型、互斥锁、信号、队列、事件位、临界区 | FreeRTOS；按需使用内部栈或外部栈 | pthread；模拟任务的实际栈类型 |
| `port/mem.h` | 按用途分配、外部 RAM 判断、统计 | `heap_caps` | `malloc` |
| `port/kv.h` | 字符串、二进制、定宽整数、异步批量读写与刷盘；在存储工作任务上执行 | NVS | 带类型的内存表 |
| `port/log.h` | 分级日志 | ESP-IDF 日志 | 标准错误 |
| `port/sys.h` | 固件信息、随机数、重启、SHA-256、HMAC-SHA256、端口诊断 | ESP 固件信息、平台加密库、芯片诊断 | `/dev/urandom`、软件散列；不伪造芯片诊断 |
| `port/err.h` | 错误码，数值与 `esp_err_t` 相同；SDK 自有号段表 | 直接透传 | 同左 |
| `port/clock.h` | 日历时间和“已同步”闸门、时区 | 网络时间服务 | 操作系统时钟，可由测试控制 |
| `port/http.h` | 一次一个请求的 HTTP/1.1 客户端，连接复用由调用者决定 | 平台 HTTP 栈 | 可编排的假实现 |
| `port/mqtt.h` | 信令连接：连接、订阅、发布、事件回调 | 平台 MQTT 库 | 可注入事件的假实现 |
| `port/console.h` | 主串口控制台的字节流：整条记录写出、不被日志打断 | UART 或 USB 串口 | 标准输入输出 |
| `xiaotai/config.h` | 构建期选项的统一写法 | 取 Kconfig | 取头文件里的默认值 |

网络不是端口：它是网络零件（`xiaotai/hal.h`）加网络引擎（`sdk/xiaotai_net`），网络类型按数量扩展。

维护新平台时先跑公开接口一致性测试：`python3 -B tests/sdk/run.py xiaotai_port --skip-dependency-tests`。其中 `port_conformance` 不使用主机专用测试钩子，不联网，覆盖任务与队列、内存水位、类型化 KV 和完成屏障、散列、时钟、控制台信息及 HTTP/MQTT 的离线入口。其他端口测试才能覆盖任务归属、协议回包和故障注入；真实 TLS、板载资源和串口传输仍须单独留证。

### 3.1 MQTT 的适配边界

同一 ESP-IDF 平台上的板卡共用 MQTT 端口。新板接好网络零件、选好产品参数即可，不要在板包里再放一份 MQTT 客户端或重连状态机。

| 位置 | 负责什么 |
| --- | --- |
| 平台引擎 → `port/mqtt.h` | 登录后的订阅、消息与业务确认；不持有原生库句柄 |
| `xiaotai_port/esp-idf/mqtt.c` | 配置副本、生命周期、事件翻译和统计；回调在客户端任务上串行执行，跨回调保留的数据需要复制 |
| `mqtt_stock.inc` | 默认用锁定 `esp-mqtt` 的公开接口，原生回调把事件转交给端口的派发任务；不替换原生库源码 |
| `mqtt_owner_api.h`、`mqtt_native_api.h` | 可选原生补丁的组件内接口；不向上暴露原生私有结构 |
| `mqtt_native_overlay.py`、`mqtt_idf.cmake` | 补丁启用时核对锁定源码并生成到独立构建目录，不改依赖原件 |

`SEND_NOW` 与 `SEND_QUEUED` 分别选择发布和原生待发队列。可选的 `XIAOTAI_PORT_MQTT_NATIVE_OVERLAY` 默认关闭；启用后才有补丁提供的原生 outbox 事务和额度控制。SUBACK/PUBACK 是协议确认，业务确认仍由上层处理，不能把成功入队当成对方已收到。

显式 `stop` 清空待发和待 PUBACK 发布，产品沿用停止、销毁、重建的生命周期；运行中的自动重连保留未确认发布。这两种情况要分开处理。

只有新产品图 `XIAOTAI_PORT_V3_PRODUCT` 注册这套实现，且产品须已选择锁定的 `espressif/mqtt`。这套实现不改变依赖锁，也不构成新的 TiRTC SDK 版本。

### 3.2 设置存储

应用和基础模块通过框架登记设置；`xiaotai_identity` 在登记前安装共享偏好后端。旧设备有专用格式时，在产品的 `parameters.preferences.bindings` 声明旧键、类型、值表或记录字段。没绑定的设置使用 `xt_settings`。板包、界面和业务模块都不用拷贝存储代码。

框架所有者把改动交给偏好服务合并，再由端口的 KV 工作任务写入，不为每次设置新建任务。改动已接纳不等于已落盘：写入失败会发 `setting.not_saved`，维护流程要检查刷盘结果。带版本的旧记录按其规则升级，读不懂的记录原样保留。旧产品的值表下标是存储格式，不能重排。

第一次初始化 NVS 也会映射分区表。KV 工作任务须先在内部栈上完成后端初始化，将原始结果回传给调用方，成功后才接收任务。Wi-Fi 启动和持久化配置由内部栈网络任务负责；Wi-Fi 设成只用 RAM 时，PHY 校准仍可能访问 NVS。

可能关闭外存缓存的端口入口先调用 `xiaotai_require_internal_stack()`。ESP 实现检查实际栈地址，出错时报告任务和操作名；主机端口按板卡能力模拟 `BULK` 是否落在外部内存。不要把已映射的只读数据、嵌入的唤醒模型或视频 DMA 缓冲当成新建 Flash 映射，也不要为了这些正常路径把大栈全部搬回内部 RAM。

有构建期选项的组件各带一对文件：`<组件>/Kconfig`（ESP-IDF 上的选项）和 `<组件>/include/xiaotai/<名字>_config.h`（每个选项一个 `XIAOTAI_<名字>_<选项>` 宏，并给出没有 Kconfig 时用的默认值）。源码只包含配置头文件，不直接读 `CONFIG_*`，也不包含 `sdkconfig.h`。运行时能改的是设置（`xiaotai/framework.h`），每块板不同的是零件描述符（`xiaotai/hal.h`）。

## 4. 公开 API 和框架接口

`xiaotai/xiaotai.h`：

- `xiaotai_start(config)`：先由构建生成的产品胶水把产品配置交给各个被选中的引擎、模块和应用（并打开事件日志和控制台），再初始化板子（`xiaotai_hal_init`，打一行 `board=<型号> revision=<版本>`；失败，或零件给出的需求和板子清单声明的不一致，就停），最后由运行框架按“引擎 → 基础模块 → 应用”的顺序初始化并启动。本地部分起好就返回，上线在后台继续。启动失败时按 `start_failure_restart_ms` 处理：默认 2 秒后重启（各家族现在的做法），`XIAOTAI_START_HALT` 停在原地、控制台照常应答。第二次调用直接返回第一次的结果。
- `xiaotai_config_t`：开机前台应用、前台活动优先级表和抢占规则、主题名、服务发现地址、启动失败策略。主题名和发现地址由产品胶水转交界面系统和平台核心；产品没有选这两个引擎时被忽略。公开 API 组件本身不链接任何可选引擎。
- `xiaotai/selection.h`：构建生成的内容（已选引擎和模块表、已选应用表、产品的 `xiaotai_product_config`、板子清单声明的需求 `xiaotai_board_declared_needs`、产品胶水函数 `xiaotai_selected_configure`），`xiaotai_start` 只认这几样。产品胶水按初始化顺序调用被选单元清单里登记的每个 `configure` 函数。产品的 `main` 是 `xiaotai_start(&xiaotai_product_config)`。

写应用和基础模块用 `xiaotai/framework.h`：

| 接口 | 说明 |
| --- | --- |
| 意图 `xiaotai_intent_handle` / `_handle_ex` / `_post` / `_post_external` | 按名字，一个意图一个处理者；`XIAOTAI_INTENT_LOCAL` 的意图从控制台等开放通道来时得到 `XIAOTAI_ERR_NOT_ALLOWED` |
| 事件 `xiaotai_event_subscribe` / `_emit` | 按名字（`<组件>.<事情>`），可带一份拷贝的数据 |
| 呈现应用（`xiaotai_application_t.handles` / `claim`） | 多应用时，应用声明自己呈现哪些模块事件；派发前框架按产品里的应用顺序找第一个声明并认领的应用，没有就给前台应用；不在前台的先切到前台，事件只交给它（引擎和模块的订阅照常）。呈现完由它调 `xiaotai_app_return()` 回到原来的应用；`xiaotai_app_next` / `_previous` 按顺序翻页。被切到前台的应用在 `foreground(true)` 里用 `xiaotai_app_entry()` 知道原因（开机、切换、下一个、上一个、呈现、返回），`xiaotai_app_home()` 是产品的首页应用 |
| 视图归前台应用 | 框架知道正在运行哪个应用的代码（init、start、foreground、事件和意图处理、它起的定时器和 `xiaotai_run` 任务、它的指令、claim）；后台应用调 `xiaotai_view_show` 被丢弃并计数（诊断段 `framework` 的 `view_refused=`）。引擎和模块不调它 |
| 共用意图 `system.menu`、`system.network` | `XIAOTAI_INTENT_SYSTEM_MENU` / `_NETWORK`：最多一个应用处理，没有参数；提供入口的一方先用 `xiaotai_intent_exists` 判断再显示 |
| `xiaotai_run` | 让一个函数在所有者任务上执行；任何任务（含实时任务）调都零等待、不分配，队列满返回 `XIAOTAI_ERR_NO_MEM` |
| `xiaotai_generation_next` | 全设备统一的会话代次 |
| 定时器 `xiaotai_timer_start` / `_cancel` | 所有者任务上的一次性定时器 |
| 前台活动 `xiaotai_activity_begin` / `_end` / `_attach` / `_set_generation` | 产品的抢占规则（成对）优先，其次比优先级：高的抢占低的，同级或更低得到“忙”。优先级来自产品表，其次是引擎、模块和应用清单声明的默认值（同名活动的默认值必须一致），再其次是 1。应用可以开始自己的活动（例如闹钟响铃）。宿主在附着者可以用会话时把活动代次设成会话代次，不能用时设回 0；附着者由 `session()` 知道，`session(id, 0)` 表示停 |
| 会话扩展 `xiaotai_attachment_t.contributions`、`xiaotai_activity_contributions` | 一套契约对所有宿主和扩展：扩展按活动名附着（生命周期 `begin` / `end` / `session`，撤销时附着者的 `end` 先于活动的 `revoked`，不另行仲裁资源），并按“槽”登记要加进宿主会话的记录（链路描述的视频部分 `XIAOTAI_LINK_SLOT_VIDEO`、设备档案场景的成员 `XIAOTAI_PLATFORM_SLOT_SCENE`），记录可以限定场景。槽和记录类型由填它的引擎头文件定义，框架只存指针。宿主建链路或写场景时按“活动、槽、场景”取出全部记录，没有记录就没有视频 |
| 静默 `xiaotai_quiesce_begin` / `_end`、守卫 `xiaotai_quiesce_guard_register` | 维护操作开始前收回当前活动、拒绝新活动和设置修改；登记的守卫可以否决（例如房间还在释放预约）。原因词有宏：`XIAOTAI_QUIESCE_COPROCESSOR`、`_ENVIRONMENT`、`_PROVISIONING`、`_BINDING`，守卫和它们比较 |
| 有序重启 `xiaotai_restart_request` / `_hold` / `_release`，意图 `system.restart` | 静默（守卫最多等 10 秒）→ 等重启保持全部放开 → 发 `system.restarting` → 延时 → 在所有者任务上冲刷设置（框架自己的存储用 `xiaotai_kv_flush`）→ 在一个栈放内部内存的短任务上调 `xiaotai_restart()`（有协处理器的板子不在外部内存的栈上走重启路径；建不了任务时退回所有者任务并计数）。调用者可以把自己的静默票交给重启，中间不留空隙 |
| 设置 `xiaotai_setting_register` 等 | 自动持久化，自动进设置界面和 `AT+SET`。存放位置由设置存储决定：身份引擎在 init 里装上偏好服务（`xiaotai/preferences.h`，旧固件的存放位置照用）；没有时框架自己存在 `xt_settings`。静默期间修改返回 `XIAOTAI_ERR_BUSY` |
| 指令 `xiaotai_command_register`、诊断 `xiaotai_diag_register` | 指令行只按空格和制表符分词，最长 767 个字符、12 个词。框架自己登记两个诊断段：`framework`（一行所有者栈、队列峰值和溢出、前台应用、活动、被拒的视图；一条 `+FRAMEWORK-RESTART:` 记录静默和重启；空值写 `-`）和 `tasks`（`supported= count=`，再每个任务一条 `+TASK: name= stack_min_free= priority= core=`，取自系统端口的 `xiaotai_task_list()`） |

所有者任务的栈和队列是构建期选项（`xiaotai/framework_config.h`）：有外部内存 24576 字节、没有 6144 字节（BULK），队列 32 条；每个处理函数连同它调的东西最多用 3072 字节栈，更大的缓冲从 BULK 申请。各登记表的容量也是选项，超出时 init 返回 `XIAOTAI_ERR_NO_MEM`，启动失败而不是悄悄丢掉。

引擎的 `start` 不在别的任务上开始会占内部或 DMA 内存、并且不等它做完就返回的硬件启动（在后台启动的射频、协处理器驱动）；这类启动等 `system.started`，所以每个 `start` 里的预留都在它之前。例外：`start` 在引擎自己的任务上做、并且同步等它做完的准备，算 `start` 里的预留（视频引擎让相机任务调相机零件的 `prepare()`，等它完成，再预留编码器）。

平台信令的订阅不在框架里：用 `xiaotai/platform.h` 的 `xiaotai_platform_signal_subscribe`（按类型和通道）。

启动顺序的保证：全部 `init`、全部 `start` 在所有者任务上连续执行完之后，所有者任务才开始派发事件、意图、定时器和 `xiaotai_run` 的任务。所以引擎可以在 `start` 里预留必须早于任何链路的内存；相应地，`init` 和 `start` 里不等网络、不启动协议栈。

## 5. 构建

- ESP-IDF：当前公开产品仍用 `python scripts/build.py <板>`。新选型在 `products/<名字>/product.json`（板包、应用、额外模块、首页、抢占规则、参数、选项、TiRTC 包、内存预算、变体），预览方式和隔离目标构建见[产品选型与主机验证](开发指南.md#产品选型与主机验证)。生成选择文件不会改变公开入口。
- 主机（Linux 或 WSL）：`python3 tests/sdk/run.py` 编译全部组件和板包并跑它们的测试；`tests/test_sdk_host.py` 在此之外还跑层次检查和构建辅助函数的自测，主机测试套件（`tests/run_host.py`）里会跑它。细节见第 6 节。

## 6. 组件约定

`sdk/` 和 `apps/` 下每个目录是一个组件。加一个组件只在它自己的目录里写文件，不用改任何公共文件：主机构建按目录发现组件，依赖由组件自己声明、按需拉入。

### 6.1 放在哪、叫什么

| 位置 | 放什么 | 清单 |
| --- | --- | --- |
| `sdk/xiaotai_<名字>/` | 端口、运行框架、硬件抽象、公开 API、引擎与服务 | 引擎有 `engine.json` |
| `sdk/modules/<组件>/` | 基础模块（例如 `xiaotai_mod_call`） | `module.json` |
| `sdk/themes/<组件>/` | 界面主题 | `theme.json`（字段同 `engine.json`，按引擎处理，依赖 `ui`） |
| `sdk/parts/<组件>/` | 零件库（芯片驱动） | 无 |
| `apps/<组件>/` | 应用（例如 `xiaotai_app_companion`） | `application.json` |
| `boards/<厂商>/<板>/` | 板包：板类和板子的硬件事实 | `board.json` |

目录名就是组件名，同时是主机构建的 CMake 目标名和 ESP-IDF 的组件名。组件名在前五个位置里全局唯一，重名在配置阶段报错。板包例外：所有板包的组件名都是 `xiaotai_board`，因为一个产品只链接一块板；一次构建只带一个板包。

板包把引脚、电源顺序和零件参数写在 `board.cc`，能力和身份写在 `board.json`。主机和 ESP-IDF 使用同一份板类，并从清单生成私有身份常量。ESP-IDF 构建入口可用 `sdk/cmake/xiaotai_board.cmake` 的 `xiaotai_board_idf_component` 取得构建目录中的 `xiaotai_board` 组件入口；它引用原板包，不复制板源码。板包以 `WHOLE_ARCHIVE` 注册。新增板后，先跑 `python3 tests/sdk/run.py --board <厂商>/<板>`，检查实际零件表是否与清单一致，再用相应产品做集成验证。

### 6.2 目录结构

```text
<组件>/
├── CMakeLists.txt      两个分支：ESP-IDF 和主机
├── engine.json         或 module.json / application.json（只有引擎、基础模块、应用有）
├── include/xiaotai/    公开头文件：C 用 .h，C++ 用 .hpp
├── src/                可移植源码和内部头文件
├── esp-idf/            只在 ESP-IDF 上编译的实现（可选）
├── host/               只在主机上编译的实现（可选）
└── test/               主机测试：源码和一个 CMakeLists.txt（可选）
```

- 可移植源码列在组件的 `CMakeLists.txt` 里，两个分支共用一份列表。
- 平台实现放 `esp-idf/` 和 `host/`：ESP-IDF 分支只编 `esp-idf/`，主机分支只编 `host/`。平台专用的头文件放在 `esp-idf/include/`、`host/include/`。
- 公开头文件的路径（例如 `xiaotai/media.h`）在所有组件里唯一。
- 只有头文件的组件可以不写 `CMakeLists.txt`：主机构建把它当成只导出 `include/` 的接口库。它要进 ESP-IDF 构建或依赖别的组件时，再写 `CMakeLists.txt`。

### 6.3 清单

名字、依赖和需求只写在清单里，`scripts/sdk_selection.py` 按清单展开产品选中的组件、对照板子能力，并生成运行框架用的表。清单里出现未知字段会报错。

| 清单 | 字段 |
| --- | --- |
| `engine.json` | `name`；`depends`（只能是别的引擎）；`optional_depends`；`one_of`；`needs`；`start`；`configure`；`options`；`activities`；`network_kinds`；`commands`；`registrations` |
| `module.json` | 同 `engine.json`；`depends` 可以是引擎和别的基础模块 |
| `theme.json` | 同 `engine.json`，没有 `activities`，多一个 `display`（主题画得了的显示：`pixel`、`width`、`height` 范围） |
| `application.json` | `name`；`modules`（必需的基础模块）；`optional_modules`（产品选了才带）；`needs`；`start`；`foreground`；`configure`；`options`；`activities`；`handles`；`claims`；`commands`；`registrations` |
| `product.json` | `schema`；`name`；`board`；`applications`；`home`；`modules`（额外的基础模块）；`priorities`（按活动名覆盖清单里的默认优先级）；`preemptions`（`[{activity, over, preempts}]`）；`start_failure_restart_ms`；`parameters`；`options`；`tirtc`；`sdkconfig`；`variants`；`budget`（`internal_bytes`、`baseline_bytes`） |

- `name`：小写字母开头，只含小写字母、数字、下划线，最长 31 个字符。引擎和基础模块共用一个名字空间，应用另算。
- `depends`：列出本组件调用其头文件的每一个引擎（基础模块还可以列别的基础模块）。组件的 `CMakeLists.txt` 里的依赖和它一致。`optional_depends`：产品选了才用；`one_of`：几组里至少选中一个。
- `needs`：硬件需求，取值和 `sdk/xiaotai_hal/needs.json` 的 `needs` 一致（`audio_in`、`pcm_in`、`audio_out`、`display`、`touch`、`key`、`light`、`relay`、`camera`、`network`、`external_ram`、`encoder`、`card`、`voice_command`、`contact`、`vibrator`、`door_lock`），和 `xiaotai/needs.h` 里的 `XIAOTAI_NEED_*` 一一对应。`door_lock` 是角色族 `door` 里的继电器（角色 `door` 或 `door.<任意>`，不含 `doorbell`）。板子满足哪些需求由它的零件描述符推出（`xiaotai_hal_needs()`）。
- `configure`：本单元的 `xiaotai_<名字>_configure` 由产品胶水按什么形式调用（`form`、`section`、`always`、`arrays`、`pointers`、`pairs`、`overrides`、`absent_with`，写法见 `scripts/sdk_selection.py` 开头的说明；`absent_with` 列出选了另一个单元时产品不许给的字段）；`options`：本单元需要打开的构建期选项。
- `activities`：本单元开始的前台活动和默认优先级，例如 `{"call": 3}`，取值 1 到 255；同一个活动在所有单元里的默认值必须相同。引擎、基础模块和应用都可以写（应用的生成到 `xiaotai_application_t.activities`，D47）；产品的 `priorities` 和 `preemptions` 按名字覆盖。`--check` 还要求公开头文件里的 `XIAOTAI_*_ACTIVITY` 都在清单里。
- `network_kinds`：本单元驱动的网络零件种类（`board.json` 的 `parts.network[].kind`，拼写取自 `needs.json` 的 `vocabulary`）。选中的单元需要 `network` 时，板子列出的每一种都要有选中的驱动；整个目录里只有一个单元驱动这种时自动选它，没有或有几个时报错。
- `handles`：应用呈现的模块事件名（前缀是某个单元名）；`claims` 为真时生成的表引用 `xiaotai_app_<名字>_claim`。
- `registrations`：本单元初始化最多会登记多少个 `intents` 和 `subscribers`。按真正登记的条目计数，例如循环订阅四个事件就填 4；应用的 `handles` 只是呈现元数据，不算订阅。若一段实现只在选了可选依赖时编译，用 `"when_selected": [{"any_of": ["call", "wechat"], "subscribers": 2}]` 声明增量；两个依赖都选中仍只加 2。运行时开关或硬件分支按可达到的上限预留。生成器再加框架内建的 `system.restart` 意图，按 Kconfig 范围生成 `FRAMEWORK_MAX_INTENTS` 和 `FRAMEWORK_MAX_SUBSCRIBERS`，产品和变体无需填写，也不能覆盖这两项。增删登记后，在同一改动里更新所属清单，用生成产品的主机启动检查实际容量。
- `commands`：本单元登记的 AT 指令（`command`、`verb`、`usage`、`access` 为 `read` / `action` / `guarded`，开发构建专用的再写 `option` 和 `symbol`）。一种写法是 `command` + `verb` + `usage` 三者合起来；几个单元写同一种写法时 `access` 和 `option` 必须一致。构建按选中的单元合并成产品的词表 `commands.json`（裸 `AT` 总在里面），测试工具的方言、控制台脚本的权限表和公开指令表都从它生成。

构建前的硬件检查（`sdk_selection.py`，`plan()` 和 `--check` 都做）：

- `check_board`：`board.json` 里的拼写都要在 `needs.json` 的 `vocabulary` 里（`input.kind`、`output.kind`、`network.kind`、`measurement.kind`、`display.pixel`、`audio_input.encoding`）；网络零件必须写 `kind`，`wifi` 必须是对象，`role` 是小写点分名字。
- 主题：选中的主题必须画得了板子的某块显示，板子的每块显示也必须有选中的主题能画；`theme.json` 的 `display.pixel` 拼写同样对照 `vocabulary`。
- 主题参数：参数里点名的主题（覆盖 `xiaotai_config_t.theme` 的字段）必须是选中的 `theme_<名字>`，并且画得了板子的第一块显示。
- 基础引擎是 `eventlog`、`at`、`identity`，每个产品都带，按这个顺序最先初始化；基础组件（不论选什么都链接）还有 `xiaotai_port`、`xiaotai_json`、`xiaotai_framework`、`xiaotai_hal`、`xiaotai_sdk`。
- `start`、`foreground` 为真时，生成的表会引用对应的函数。函数按名字约定，由组件实现：

| 种类 | 必有 | `start` 为真 | `foreground` 为真 |
| --- | --- | --- | --- |
| 引擎 | `xiaotai_engine_<name>_init` | `xiaotai_engine_<name>_start` | — |
| 基础模块 | `xiaotai_module_<name>_init` | `xiaotai_module_<name>_start` | — |
| 应用 | `xiaotai_app_<name>_init` | `xiaotai_app_<name>_start` | `xiaotai_app_<name>_foreground` |

### 6.4 CMakeLists.txt

```cmake
set(sources src/call.c src/call_state.c)          # 可移植源码
if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} "esp-idf/call_port.c"
                           INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "src"
                           REQUIRES xiaotai_port xiaotai_framework xiaotai_media)
    return()
endif()

xiaotai_host_require(xiaotai_port xiaotai_framework xiaotai_media)
add_library(xiaotai_mod_call STATIC ${sources} host/call_port.c)
target_include_directories(xiaotai_mod_call PUBLIC include PRIVATE src)
target_link_libraries(xiaotai_mod_call PUBLIC xiaotai_port xiaotai_framework xiaotai_media)
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
```

主机分支用的辅助函数在 `sdk/cmake/xiaotai_host.cmake`，由主机工程（`tests/sdk/CMakeLists.txt`）引入一次，组件里直接调用：

| 函数 | 作用 |
| --- | --- |
| `xiaotai_host_require(<组件>...)` | 目标还不存在时，把这个组件加进构建。在 `sdk/`、`sdk/modules/`、`sdk/themes/`、`sdk/parts/`、`apps/` 下按目录名找；被加进来的组件再拉它自己的依赖。写在 `add_library` 之前，列出本组件链接的所有组件。循环依赖在配置阶段报错 |
| `xiaotai_host_test(<名字> SOURCES ... [LINK ...] [INCLUDE_ONLY ...] [INCLUDE ...] [TIMEOUT 秒])` | 编一个测试程序并登记到 CTest。带 `-Wall -Wextra -Werror` 和地址、未定义行为两个检查器。`LINK` 写要链接的组件或库；`INCLUDE_ONLY` 只加某个组件（连同它公开依赖的组件）的头文件目录、不链接它，测试自己提供这个组件函数的假实现；`INCLUDE` 加普通目录（例如 `../src`）；超时默认 120 秒 |

主机分支必须定义一个和目录同名的目标。主机上没有可编的源码时，写 `add_library(<组件> INTERFACE)` 并导出 `include`，这样它的公开头文件仍然参加头文件检查。

### 6.5 测试

- 测试放在组件自己的 `test/` 目录，由 `test/CMakeLists.txt` 调 `xiaotai_host_test` 登记。组件的 `CMakeLists.txt` 只在 `XIAOTAI_HOST_TESTS` 打开时加这个目录；有 `test/CMakeLists.txt` 却没加，配置阶段报错。
- 测试名全局唯一，以组件的短名开头，例如 `port_kv`、`media_downlink`、`mod_call_dial`。
- 测试程序返回 0 表示通过，失败时把原因打到标准错误。
- 要隔离依赖时用假实现：把被测源码直接编进测试，依赖的组件写在 `INCLUDE_ONLY` 里。被假实现替换的组件不要再经 `LINK` 带进来：`LINK` 的组件公开依赖它时，真实现也会被链接。

```cmake
# test/CMakeLists.txt：通话模块用假的传输和媒体引擎测
xiaotai_host_test(mod_call_dial
    SOURCES call_dial_test.c fake_transport.c fake_media.c ../src/call.c ../src/call_state.c
    INCLUDE ../src
    INCLUDE_ONLY xiaotai_mod_call xiaotai_transport xiaotai_media
    LINK xiaotai_framework)
```

单组件测试放在组件自己的 `test/`；跨组件用例、生成产品的主机用例和全部头文件检查放在 `tests/sdk/`。头文件检查会为每个公开头文件生成编译单元：`.h` 用 C 和 C++ 各编一次，`.hpp` 用 C++ 编，分别检查能否独立、重复及一起包含。

### 6.6 怎么跑

在 Linux 或 WSL 里，从仓库根目录：

| 命令 | 作用 |
| --- | --- |
| `python3 tests/sdk/run.py` | 全部组件：编译、各组件测试、头文件检查；然后逐个板包各编一次、跑板包自己的测试 |
| `python3 tests/sdk/run.py xiaotai_media` | 只编这个组件和它依赖的组件，跑它们的测试；可以写多个组件名 |
| `python3 tests/sdk/run.py --board lckfb/szpi-esp32s3` | 只编这个板包（`boards/<厂商>/<板>`）和它依赖的组件，跑板包的测试；`--board` 可以写多次，也可以和组件名一起写 |
| `python3 tests/sdk/run.py --all-boards` | 逐个编全部板包，不跑组件那一轮 |
| `python3 tests/sdk/run.py --skip-dependency-tests xiaotai_media` | 依赖的组件只编不测 |
| `python3 tests/sdk/run.py -v xiaotai_media` | 显示全部编译和测试输出，主机端口的日志也打出来 |
| `python3 tests/sdk/run.py --keep xiaotai_media` | 保留构建目录并打印路径，便于单独重跑某个测试程序 |
| `python3 scripts/check_sdk_layers.py` | 层次检查 |
| `python3 tests/test_sdk_host.py` | 层次检查 + 全部组件 + 辅助函数自测（提交前跑这个） |

`run.py` 每次在 `/tmp` 下新建一个构建目录，有 Ninja 就用 Ninja，跑完删除；不往源码目录写任何东西，多个 `run.py` 可以同时跑。测试带 `ASAN_OPTIONS=detect_leaks=0` 和 `XIAOTAI_HOST_LOG_QUIET=1` 运行。配置、编译或任何一个测试失败时打印失败步骤的输出（包括失败测试自己的输出），退出码非零。

板包的测试名在报告里写成 `<厂商>/<板>:<测试名>`。板包那一轮只跑板包自己的测试，它依赖的组件的测试属于组件那一轮。

直接用 CMake 时：`cmake -S tests/sdk -B <构建目录> -DXIAOTAI_COMPONENTS="xiaotai_media;xiaotai_hal"`，不写 `XIAOTAI_COMPONENTS` 就是全部组件；`-DXIAOTAI_HOST_DEPENDENCY_TESTS=OFF` 对应 `--skip-dependency-tests`；`-DXIAOTAI_BOARD=<厂商>/<板>` 把这个板包作为组件 `xiaotai_board` 加进来（只写它、不写 `XIAOTAI_COMPONENTS` 时，只编板包和它依赖的组件）。

### 6.7 平台目录规则

- `sdk/` 和 `apps/` 下，平台头文件（ESP-IDF、FreeRTOS、驱动、`sdkconfig.h`、pthread、`unistd.h`）只能出现在名为 `esp-idf` 或 `host` 的目录里，以及 `test` 目录里。其他位置（`include/`、`src/`、组件根目录等）一律不含平台头文件。
- 平台差异因此只有两种写法：通过端口和零件接口调用；或者把实现分成 `esp-idf/` 和 `host/` 两份，接口放在可移植的头文件里。
- 板包（`boards/<厂商>/<板>/`）没有平台目录：引脚是整数，所有平台调用都在零件库的 `esp-idf/` 目录里。板包里只有 `test/` 可以包含平台头文件；别处出现平台头文件，说明这段代码应该是零件库里的一个零件。
- `scripts/check_sdk_layers.py` 按目录名检查，`check_structure.py` 和 `tests/test_sdk_host.py` 都会跑它。

### 6.8 板包

- 板包是 `boards/<厂商>/<板>/`：`board.json`、`board.cc`（板类，唯一的源文件）、`CMakeLists.txt`（组件 `xiaotai_board`，一个 ESP-IDF 分支加一个主机分支）、板子的 `sdkconfig`、分区表和资源，可选的 `test/`。
- 板类不含平台头文件，所以每块板都能在主机上和零件库的 `host/` 假实现链接，测试放在板包的 `test/` 目录，写法和组件的测试一样（`xiaotai_host_test(... LINK xiaotai_board)`）。
- 接新板先把真实零件、引脚、总线、供电和复位顺序写清楚。同款零件尽量复用；如果其中一块板要求先关背光、换复位时机或按另一种顺序回收，就给零件增加通用选项，由板包填值，已有默认值保持不变。测试至少验证上电顺序、故障后的回收和对外报出的能力。无屏板不用虚构显示零件，直接选已有的按键、提示音或灯光交互。
- 产品该开哪些功能，在 `product.json` 选应用、模块与参数；板包只提供硬件事实。用 `python3 -B tests/test_products.py --require-products` 核对全部产品的选型和生成配置能否编译，再挑受影响的产品做集成检查。这一步不替代按目标芯片重新构建固件，也不证明实体设备已跑通。
- 组件里写 `xiaotai_host_require(xiaotai_board)` 得到的是本次构建选中的那块板；没选板时配置阶段报错并提示怎么选。

### 6.9 引擎的无任务形式

没有外部 RAM 的板子上，周期性干活的引擎可以不建自己的任务，改在所有者任务上用定时器跑（界面、交互、反馈三个引擎已经这样定义）：

- 由引擎自己的 `XIAOTAI_<名字>_TASK` 选项选择，Kconfig 写 `default y if SPIRAM`：有外部 RAM 时建任务，没有时走无任务形式；产品可以按现状改写。
- 定时器按绝对时间表排（下一次 = 上一次计划时刻 + 周期），落后超过一个周期才从“现在”重新排，并计数。
- 每一步都是所有者任务上的一个处理函数，栈用量不超过框架给处理函数的预算（`XIAOTAI_FRAMEWORK_HANDLER_STACK_BYTES`，3072 字节）；要在别的任务上跑的工作（例如截屏流）不能落到所有者任务上。
- 两种形式的对外行为相同，诊断里说明当前是哪一种。
