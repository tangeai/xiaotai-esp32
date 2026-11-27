# 门禁屏幕资源

这组资源给门禁应用的首页和宽屏模板使用。首页的背景图由 `src/backdrop.c` 画到外部内存里，没有图片素材；监看及通话由视频层提供画面。图标和字库从 Flash 直接读，不联网取图。

| 资源 | 来源与格式 |
| --- | --- |
| `door_v4_assets.c/.h` | 保留旧 WT99 LVGL 9 的 26 个描述符和 18 组去重像素：RGB565 平面在前，A8 平面在后。只整理声明和源码格式，不重绘或拉伸图标。 |
| `door_station_icons.c/.h` | 门禁自己的图标（Lucide 的线形）：人像、门铃、眼睛、设置、时钟、箭头、刷新、切换、静铃、房子、门，24 和 36 px，由 `scripts/generate_door_station_icons.py` 用 Pillow 画出；人像 36 px 沿用 RGB565A8，其余只有 A8 覆盖度，按模板的颜色着色。`--check` 核对 |
| `fonts/door_font_*.c` | 固定文案用 Noto Sans SC 18/24/26/32 px（24 px 有两种字重），大时钟用 Inter 128 px，小屏首页用 72 px。保持原字重、行高和基线，字体编译成 A4 子集。动态联系人等文字使用界面容器提供的完整中文字库，不依赖固定文案的子集。 |

72 px 的圆形底板（`door_asset_circle_*_72`）的像素平面不是 LVGL 9 的 RGB565A8 排列，模板不再绘制它们，只按它们代表的颜色直接画圆（青绿 `#1FAB96`、红 `#E34547`、深色 84% 不透明），并保留 22% 白色描边。

[V4 设计来源与原始 SVG](source/door-v4/README.md)随包保留。该目录的图标清单记录初版 LVGL 8 导出；本包的 LVGL 9 数组和描述符取自旧 WT99 固件的 `door_v4_assets.c`（提交 `0f93f4147b2978211f47ce5e80c5739449451892`）。Lucide ISC 和相关 Feather MIT 许可见 [ICONS_LICENSE.txt](ICONS_LICENSE.txt)。字库随包保留许可全文：[Noto Sans SC](OFL-NotoSansSC.txt)、[Inter](OFL-Inter.txt)。

## 修改文案后怎么验证

在仓库根目录执行 `python -B scripts/generate_door_fonts.py --check`。检查项包括门禁应用 `.c` 和 `.h` 中的固定字符串、产品配置里的 `door.title` 和 `door.door_name`（含变体）、字库清单及文件哈希。主机门禁宽屏测试还会让 LVGL 逐字查找字形和位图，确认能绘制，未落入缺字占位。

修改固定文案后，用本地装有 Pillow（含 FreeType）和 fontTools 的 Python 执行 `python -B scripts/generate_door_fonts.py`，再跑 `--check` 和宽屏测试。自定义产品清单可以追加 `--product 路径`，生成和检查时使用同一参数。[生成清单](fonts/manifest.json)记录源文件哈希和生成工具版本；生成输入是 [Noto Sans SC](../../../components/xiaotai_lvgl/fonts/source/) 和 [Inter](source/inter/) 的原始字库，许可随文件保留。正常编译只读取已生成的资源，不运行生成器或下载字库。
