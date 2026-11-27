---
name: xiaotai-ui
description: 改小钛彩屏的界面和交互：在电脑上把每一页画成 PNG 看效果，按设计规范改主题（配色、按钮、列表、通话页、配网页）和应用里的页面内容，核对五款彩屏的三种分辨率（320×240、480×320、1024×600），再跑界面测试。用户说界面难看、改样式、调布局、加图标、改文案、某页显示不对、要截图看效果时使用。只改界面不改业务逻辑；业务行为变化用 xiaotai-refactor。
---

# 改界面

界面由两部分组成，改之前先分清改哪边：

| 层 | 目录 | 管什么 |
| --- | --- | --- |
| 应用 | `apps/xiaotai_app_companion/src/`、`apps/xiaotai_app_alarm/src/`（闹钟）、`apps/xiaotai_app_door/src/`（门禁） | 页面上**有什么**：标题、文字、列表项、按钮、图标名、按钮意图 |
| 主题 | `sdk/themes/xiaotai_theme_lcd_kit/src/`（`lcd320`、`lcd480` 只是变体入口） | 页面**长什么样**：配色、字体、位置、按钮样式、头像、箭头 |
| 应用自带的模板 | `apps/xiaotai_app_door/src/templates_wide.c` | 门禁在 800×480 以上的屏（WT99）上自己画的五种页面，有自己的配色、图标和子集字库 |

应用一般只描述视图（`xiaotai_view_t`：种类、标题、条目、动作、图标名），不画东西；主题按视图种类排版：`lcd_home.c` 首页和时钟页，`lcd_pages.c` 列表、通知、验证码、通话、进度和对话框，`lcd_form.c` 表单和数字键盘，`lcd_widgets.c` 按钮、图标和断行，`lcd_motion.c` 动效，`lcd_kit.c` 图层、提示条、字号选择和大屏版面。

例外是 `CUSTOM` 视图：应用为它支持的屏注册模板（门禁的大屏模板按控件的意图名摆位置），别的屏上按通用的 `NOTICE` 画。所以同一个门禁页在 WT99 上是门禁模板，在微雪 P4 上是主题的通用页面，两块屏都要出图看。门禁模板加新按钮时，在 `templates_wide.c` 按意图名给它一个位置，否则大屏上不显示；它的文字用门禁子集字库，改了文案要跑 `python scripts/generate_door_fonts.py`，再 `--check`。

门禁模板的图标是 RGB565A8 位图（`assets/door_v4_assets.c`）。其中 72 px 的圆形底板像素排列不对，画出来是三个挤在一起的圆和一排点，所以模板改成按颜色直接画圆（`templates_wide.c` 的 `plate()`），不要再用这些底板图。新图标别手写像素：照 `scripts/generate_door_station_icons.py` 用 Pillow 按 Lucide 的线形画，`--check` 核对。看图标对不对，把预览图放大到像素级看，缩略图里看不出花点。

要大字时间就用 `XIAOTAI_VIEW_CLOCK`：主题用数字字体（48 px，大屏 96 px）画时间和日期，`text` 放一行说明（例如下一个闹钟），按钮排在底部，`XIAOTAI_ICON_PREVIOUS`／`NEXT` 的按钮放在左右两侧当翻页箭头。

## 1. 先出图

在 Linux 或 WSL 的仓库根目录：

```bash
python3 tests/fetch_components.py --set all          # 第一次：准备锁定的 LVGL 等主机组件
python3 tools/ui_preview.py                          # 五款彩屏
python3 tools/ui_preview.py --product xiaotai-szpi-s3 --out build/ui-preview
```

它用真实的产品、板包、主题和 LVGL 在主机上运行，自动点遍能点的页面（三层深，长列表会滚动一次），再演示 AI 对讲、来电、拨出、设置各项，把每一帧存成 `build/ui-preview/<产品>/<home|setup|gallery>/NN-<页面>.png`。`gallery` 是合成画廊（`tests/sdk/product/product_screens_gallery.c`）：主机假后端走不到的状态（配网各步、失败、空列表、超长名字、滑块两端、对话框、提示条）直接用视图画出来。总览在 `build/ui-preview/index.html`。`index.tsv` 记着每张图的页面名和控件坐标、文字、意图。场景代码在 `tests/sdk/product/product_screens.c`，要加新场景（微信来电、房间里讲话）就加在那里。

自动探索只在首页应用里点，不进翻页后的其他应用，也不点通话、会话类按钮。新应用、新流程要写成场景：用 `xiaotai_intent_exists("<意图>")` 判断这款产品有没有，再 `xiaotai_intent_post` 一步步走，不写产品名（照 `scene_alarm`、`scene_door` 抄）。到点、来电这类事件尽量走真路径（例如发 `clock.countdown` 让时钟模块自己触发）；要通话服务器才能走通的页面，可以直接发 `call.state` 事件带一份通话状态来显示。

改之前先出一套“改前”的图，改完再出一套对比。

## 2. 设计规范

| 项目 | 规则 |
| --- | --- |
| 配色 | 只用 `lcd_internal.h` 里的变量：背景 `LCD_BACKGROUND`，卡片和行 `LCD_SURFACE`，按钮 `LCD_SURFACE_HIGH`，边线 `LCD_OUTLINE`，正文 `LCD_TEXT`，次要 `LCD_MUTED`，失效 `LCD_FAINT`，强调 `LCD_ACCENT`（与标志一致的天蓝），接听和拨出 `LCD_POSITIVE`（绿），挂断和拒绝 `LCD_DESTRUCTIVE`（红），警告 `LCD_WARNING`。深色值要是 RGB565 能精确表示的，否则会偏绿 |
| 颜色的含义 | 强调色只给每页一个主动作；绿只用于接听和拨出；红只用于挂断、拒绝和有破坏性的确认；其余都是中性 |
| 圆角 | 按钮和列表行 `LCD_RADIUS`（10），对话框 `LCD_RADIUS_CARD`（16），头像和通话按钮正圆 |
| 字号 | 尺寸写设计像素，用 `lcd_dp()` 换算；正文 18 px，宽高都不小于 800×480 的屏用 26 px 字库（`XIAOTAI_THEME_LCD_LARGE_TEXT`，构建按板子首屏尺寸自动打开，产品可在 `options` 里改）。26 px 字库收 GB 2312 一级字和固定文案用到的字，其余字落回 18 px |
| 图标 | 主题自带的线形图标（`scripts/generate_lcd_theme_assets.py`，24 格、圆头 2.2 格线宽），图标名在 `view.h` 的 `XIAOTAI_ICON_*`。新图标在脚本里加几何，跑 `--write` 再 `--check` |
| 按钮 | 图标和文字并排居中，放不下时只留文字；一排放不下时自动换行。通话类页面用圆形按钮，文字在下；关掉的麦克风、摄像头用强调色点亮 |
| 列表 | 能点进去的行是抬起的卡片，右侧有 `>`；信息行平放，“名称 值”两列对齐 |
| 断行 | 大段文字用 `lcd_prose()`：行首不出现收尾标点，“7:30”“Wi-Fi”不拆开 |
| 动效 | 只用 `lcd_motion.c`：新页面落位、进度条移动、忙碌圆环、等待通话的呼吸圈。界面系统不跑 LVGL 动画，`lv_anim` 和样式过渡不会动；不要给整页加透明度（要整页离屏缓冲，首帧会是空屏） |
| 大屏 | 宽高都超过 2 倍设计尺寸的屏，普通页面按 2 倍单位排在居中的一列里，高度用满；首页、通话、模板页和背景全屏 |
| 文案 | 写给普通用户：说怎么做，不说内部状态；按钮用动词（「接听」「挂断」「断开当前连接」）；不出现英文调试字段 |

新的颜色、尺寸先加成变量，再在需要的地方引用。

## 3. 改的时候注意

- **不改意图和按钮文字，除非确实要改交互。** 主机测试按页面名、控件文字和意图找控件，改了要同步改测试；交互变化要在 CHANGELOG 里写清楚。
- **LVGL 标签必须给初值。** `lcd_label()` 已经把新标签清成空串；自己用 `lv_label_create()` 时也要立刻设文字，否则会显示 LVGL 默认的 “Text”。
- **新文字里的字符要在字库里。** 字库只收了常用汉字和 ASCII，带圈数字之类的符号没有，会显示成方框。查 `components/xiaotai_lvgl/fonts/font_source.json` 的 `ranges`。
- **英文和数字是比例字宽。** 字库生成器（`scripts/generate_lcd_fonts.py`）对 ASCII 用思源黑体自己的字宽；不要按“每个字母 9 像素”去算布局。
- **五款屏都要看。** 立创和 CoreS3 是 320×240，微雪 P4 是 480×320（屏是转过 90° 装的，主机端口也会按同样方式旋转），WT99 和 GUITION S31 是 1024×600。
- **固定文案里的新字要在 26 px 字库里。** 加了文案跑一次 `python scripts/generate_lcd_theme_assets.py --check`，提示缺字就 `--write` 重生成。
- **只在主机上看过的界面，上板前还要在真机看一眼**：颜色在真屏上偏暗或偏亮、触摸热区大小，主机图看不出来。

## 4. 测试

```bash
python3 tests/sdk/run.py -j 6                                  # 主题和应用的单元、集成测试
python3 tests/sdk/product/run.py --product xiaotai-szpi-s3     # 产品级，含 screens 场景
```

`tests/sdk/integration/lcd_*_test.c` 会核对按钮颜色、首页控件位置和字幕排版。改了配色或位置就更新这些断言，写清新的值从哪来（例如 `/* LCD_POSITIVE */`）。
