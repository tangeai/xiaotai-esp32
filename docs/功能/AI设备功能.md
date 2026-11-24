# AI 对讲里的设备功能

AI 对讲进行中，用户可以直接说“声音大一点”“明早七点叫我”“十分钟后提醒我关火”，设备就照做。这件事由两头配合完成：

- **固件**登记一组“设备动作”，每个动作一个名字、一个处理函数；
- **控制台**（`https://xiaotai.chat`）给智能体角色配上同名的“设备插件”，告诉模型什么时候调用、参数怎么填、结果怎么理解。

对话中模型决定调用某个动作时，服务端把动作名和参数下发给设备，设备执行后把结果回给模型，模型再用一句话告诉用户。

本文先列出固件自带的动作，再讲怎么在控制台配置，最后讲怎么加自己的动作。

## 自带的动作

哪些动作存在，取决于产品选了什么、板子有什么，不看产品名：

| 动作 | 作用 | 什么时候有 |
| --- | --- | --- |
| `set_volume` | 调音量、静音 | 板子有喇叭 |
| `set_brightness` | 调屏幕亮度 | 屏幕背光可调（板包声明 `XIAOTAI_DISPLAY_DIMMABLE`） |
| `get_device_status` | 读时间、日期、星期、音量、亮度 | 选了 `ai_talk` 的产品都有 |
| `add_alarm` | 加闹钟 | 产品带 `alarm` 应用（和 `clock` 模块） |
| `list_alarms` | 列出闹钟 | 同上 |
| `change_alarm` | 删除、关闭、打开闹钟 | 同上 |
| `start_timer` | 开始倒计时 | 同上 |
| `cancel_timer` | 取消倒计时 | 同上 |
| `get_timer` | 倒计时还剩多久 | 同上 |
| `call_device`、`query_contact_status` 等 | 呼叫联系人、查联系人状态 | 产品带 `contacts` 模块 |

前三个在 `apps/xiaotai_app_companion/src/ai_device_tools.c`，闹钟和倒计时的六个在 `apps/xiaotai_app_alarm/src/alarm_ai_tools.c`。控制台里没有配置对应插件的动作不会被调用；设备上没有的动作被调用时，固件回“不支持”。

### 回包的共同格式

每个动作都回一个 JSON 对象，前三项固定：

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `ok` | boolean | 这次请求算不算成功 |
| `status` | string | 固定的英文小写词，见各动作的表 |
| `message` | string | 一句中文说明，模型可以照着说 |

后面是各动作自己的数据，都是执行之后设备上的实际值。

### `set_volume`

| 参数 | 类型 | 必填 | 含义 |
| --- | --- | --- | --- |
| `mode` | string | 是 | `set` 设为 `value`；`up` 调大；`down` 调小；`mute` 静音；`unmute` 取消静音 |
| `value` | integer | `set` 时必填 | `set`：目标音量 0–100；`up`、`down`：变化量 1–100，不给就调一档 |

| 返回 | 类型 | 含义 |
| --- | --- | --- |
| `volume` | integer | 调整后的音量 0–100 |
| `muted` | boolean | 是否静音 |

`status`：`ok`、`at_max`（已经最大）、`at_min`（已经最小）、`invalid_params`、`busy`（设备在维护，设置暂时不能改）、`failed`。

音量按产品的音量档位取整（默认 10 一档，和设置页一致），大于 0 的请求至少落在第一档。调大或设为大于 0 的值会顺带取消静音；调小不会。

### `set_brightness`

| 参数 | 类型 | 必填 | 含义 |
| --- | --- | --- | --- |
| `mode` | string | 是 | `set` 设为 `value`；`up` 调亮；`down` 调暗 |
| `value` | integer | `set` 时必填 | `set`：目标亮度 0–100；`up`、`down`：变化量 1–100，不给就调 20 |

返回 `brightness`（integer，调整后的亮度）。`status`：`ok`、`at_max`、`at_min`、`invalid_params`、`busy`、`failed`。

亮度最低到 10，再低也按 10 处理，屏幕不会被调到看不见。亮度是一项设置（`screen.brightness`，键 `bright`），重启后还在。

### `get_device_status`

没有参数。

| 返回 | 类型 | 含义 |
| --- | --- | --- |
| `time_synced` | boolean | 设备时间是否已经同步 |
| `time` | string | 本地时间 `HH:MM`，24 小时制；没同步时没有这一项 |
| `date` | string | `YYYY-MM-DD`；没同步时没有 |
| `weekday` | string | `sunday` … `saturday`；没同步时没有 |
| `volume`、`muted` | integer、boolean | 有喇叭时才有 |
| `brightness` | integer | 屏幕可调光时才有 |

### `add_alarm`

| 参数 | 类型 | 必填 | 含义 |
| --- | --- | --- | --- |
| `hour` | integer | 是 | 时，24 小时制 0–23 |
| `minute` | integer | 否 | 分 0–59，不给按 0 |
| `repeat` | string | 否 | `once`（默认，下一次到这个时刻响一次）、`daily`、`weekdays`（周一到周五）、`weekends`、`custom` |
| `days` | string | `custom` 时必填 | 星期几，英文缩写用逗号隔开：`mon,wed,fri`；也接受 1（周一）到 7（周日） |
| `day_offset` | integer | 否 | 一次性闹钟是哪一天：今天 0、明天 1、后天 2。给了就会核对，见下面；重复闹钟忽略它 |
| `label` | string | 否 | 响铃页上显示的名字，最长 23 字节（约 7 个汉字），超出的部分截掉 |

| 返回 | 类型 | 含义 |
| --- | --- | --- |
| `id` | integer | 闹钟编号 |
| `time` | string | `HH:MM` |
| `repeat`、`days` | string | 重复方式和实际响铃的星期 |
| `enabled` | boolean | 是否开着 |
| `label` | string | 名字 |
| `rings_in_minutes` | integer | 多少分钟后响；设备时间没同步时没有这一项 |

`status`：`ok`、`exists`（时间和重复方式都相同的闹钟已经有了，不重复添加；它关着的话会被打开，这时回 `ok`）、`full`（闹钟和倒计时一共 16 个，满了）、`unsupported_date`、`invalid_params`、`failed`。

一次性闹钟在下一次到这个时刻时响，只能是今天或明天的 24 小时之内。`day_offset` 和实际会响的那一天对不上时（说的是后天，或者说明天下午三点、而现在还没到今天的下午三点），不添加闹钟，回 `unsupported_date`，`next_ring_day` 给出它会在 `today` 还是 `tomorrow` 响。建议在控制台把这个参数设成必填，避免模型把“后天”当成明天。

### `list_alarms`

没有参数。返回 `count`、`time_synced` 和 `alarms` 数组，数组每项的字段和 `add_alarm` 的返回相同。`status`：`ok`、`none`（没有闹钟）。

### `change_alarm`

| 参数 | 类型 | 必填 | 含义 |
| --- | --- | --- | --- |
| `mode` | string | 是 | `remove` 删除；`disable` 关闭但保留；`enable` 打开 |
| `hour`、`minute` | integer | 三选一 | 这个时刻的闹钟都处理；`minute` 不给按 0 |
| `id` | integer | 三选一 | `list_alarms` 返回的编号 |
| `all` | boolean | 三选一 | `true`：全部闹钟 |

返回 `count`（处理了几个）和 `remaining`（还剩几个闹钟）。`status`：`ok`、`not_found`、`invalid_params`、`failed`。

### `start_timer`

| 参数 | 类型 | 必填 | 含义 |
| --- | --- | --- | --- |
| `minutes` | integer | 至少给一个 | 分钟数 |
| `seconds` | integer | 至少给一个 | 秒数；总时长 1 秒到 24 小时 |
| `label` | string | 否 | 响铃页上显示的名字，最长 23 字节 |

返回 `id`、`seconds`（总秒数）、`label`。`status`：`ok`、`full`、`invalid_params`、`failed`。可以同时有多个倒计时。到点时如果还在对讲，不打断，对讲结束后补响；不到两分钟的倒计时，`message` 会让 AI 提醒用户说完就结束对话。

### `cancel_timer`

可选参数 `id`（integer）；不给就取消全部倒计时。返回 `count`。`status`：`ok`、`not_found`（没有倒计时）、`invalid_params`。

### `get_timer`

没有参数。返回 `count`、最先到的那个倒计时的 `id`、`remaining_seconds`、`label`，以及 `timers` 数组（每项 `id`、`remaining_seconds`、`label`）。`status`：`ok`、`none`。

### 参数的宽容度

参数是模型填的，固件按“意思对就接受”来读：整数也接受 `7.0`、`"60"`、`"60%"`；布尔值也接受 `"true"`、`1`、`0`；枚举词不分大小写、前后空格不算。类型不对、超出范围或缺必填项时回 `invalid_params`，`message` 里写明正确的填法，模型通常会改正后重试。

## 在控制台配置

1. 登录控制台，进「设备」，点设备的「AI 角色」，打开要用的角色（系统预设角色只读，先新建一个自己的角色）。
2. 切到「扩展能力」，在「工具与插件」的「设备插件」下点「新建设备插件」，逐项填写：

   | 表单项 | 填什么 |
   | --- | --- |
   | 名称 | 给人看的中文名，例如“设置音量” |
   | 描述 | 写给模型看：什么话该调用它，每个参数怎么填，每种 `status` 该怎么对用户说 |
   | 功能标识（Action） | 固件里的动作名，例如 `set_volume`，必须完全一致 |
   | 输入参数 | 上面各表的参数：名称、类型、是否必填、说明 |
   | 返回参数 | 上面各表的返回字段，至少写 `ok`、`status` 和主要数据 |

3. 回到角色页，勾上要给这个角色用的插件，点「保存角色」。保存会影响所有使用这个角色的设备。
4. 在角色列表里对设备点「使用」，把它切到这个角色。

描述决定模型用得对不对，几条经验：

- 把用户的原话写进去（“声音大一点”“调到一半”“别出声”），模型按这些话对号入座。
- 枚举值逐个解释，并给换算的例子（“一半”是 50，“一个半小时”是 90 分钟）。
- 写清楚和相邻插件的分工（“几分钟后提醒用倒计时，不要用闹钟”）。
- 写清楚失败时该说什么（`full` 请用户先删一个，`not_found` 如实告诉用户）。
- 设备做不到的事也写上（一次性闹钟只能定在 24 小时内），免得模型硬凑参数。

## 加一个自己的动作

以“打开夜灯”为例，动作名 `set_night_light`，参数 `on`（boolean）。

1. **在拥有这项功能的应用或模块里登记。** 初始化时调用 `xiaotai_ai_talk_tool_register`，名字用静态字符串：

   ```c
   #include "xiaotai/json.h"
   #include "xiaotai/modules/ai_talk.h"

   static void night_light(const xiaotai_ai_talk_tool_call_t *call, void *ctx)
   {
       (void)ctx;
       if (!call->device_action) return;
       bool on = false;
       const xiaotai_err_t read = xiaotai_ai_talk_tool_arg_bool(call, "on", &on);
       const bool done = read == XIAOTAI_OK && light_set(on) == XIAOTAI_OK;
       char text[128];
       xiaotai_json_writer_t writer;
       xiaotai_json_writer_init(&writer, text, sizeof(text));
       xiaotai_json_object_begin(&writer, NULL);
       xiaotai_json_put_bool(&writer, "ok", done);
       xiaotai_json_put_string(&writer, "status", done ? "ok" : read != XIAOTAI_OK ? "invalid_params" : "failed");
       xiaotai_json_put_string(&writer, "message", done ? "夜灯已设置。" : "夜灯没有设置成功。");
       xiaotai_json_put_bool(&writer, "on", light_is_on());
       xiaotai_json_object_end(&writer);
       if (xiaotai_json_writer_end(&writer, NULL) == XIAOTAI_OK)
           (void)xiaotai_ai_talk_tool_reply(call->session, call->request, text);
   }

   /* 应用或模块的 init 里 */
   err = xiaotai_ai_talk_tool_register("set_night_light", night_light, NULL);
   ```

2. **按能力决定登不登记。** 板子没有这个零件、产品没选这个模块时不要登记，不要写产品名判断。应用把 `ai_talk` 写进清单的 `optional_modules`，在 `CMakeLists.txt` 里只在选了它时编进这部分源码（参照 `apps/xiaotai_app_alarm/CMakeLists.txt`）。
3. **处理函数的规矩。** 它在框架的所有者任务里运行：不能阻塞、不能等网络；要异步完成的事先记下 `call->session` 和 `call->request`，做完再调用 `xiaotai_ai_talk_tool_reply`。保存设置走各模块已有的接口（`xiaotai_setting_set_int`、`xiaotai_schedule_add` 等）。没有外部内存的产品不要在处理函数里申请内存，回包用栈上的小缓冲。
4. **读参数用模块提供的函数。** `xiaotai_ai_talk_tool_arg_int`、`_arg_bool`、`_arg_word`（枚举词，自动转小写）、`_arg_text`（显示用的文字，按字符边界截断），含义见 `xiaotai/modules/ai_talk.h`。
5. **回包带上执行后的实际值和固定的 `status` 词。** 模型靠它们接话；数值以设备为准，不要回请求里的值。
6. **留意动作表的容量。** 一个产品能登记的动作数是构建选项 `AI_TALK_TOOLS_MAX`（默认 32）。没有外部内存的产品按实际用量设小，每个名字占 20 字节静态内存。
7. **写主机测试。** 把处理函数接在假的登记表上，覆盖正常调用、缺参数、越界和功能不可用几种回包（参照 `apps/xiaotai_app_alarm/test/alarm_ai_tools_test.c`）。
8. **在控制台建同名插件**，勾到角色上，上板说几句试试。

## 台架上怎么验证

台架版固件可以用指令读回结果，不用盯着屏幕：

| 指令 | 看什么 |
| --- | --- |
| `AT+STATUS settings` | `volume=`、`speaker_muted=` |
| `AT+SET screen.brightness` | 当前亮度 |
| `AT+CLOCK` | 当前时间和每个闹钟、倒计时（`+CLOCK-ITEM:`） |
| `AT+STATUS ai_talk` | `tool_calls=`（收到的动作数）、`tool_unhandled=`（没有登记的动作数）、`+AI-TOOLS: recent=`（最近执行的四个动作名，新的在前） |

`tool_unhandled` 在增加，说明控制台插件的功能标识和固件的动作名对不上，或者这个产品没有这项功能。
