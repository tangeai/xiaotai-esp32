# 内机 V4 本地资源

来源为小钛内机 V4 设计稿（Figma）。各页面节点：首页 `91:144`、来电 `91:189`、通话 `91:211`、查看 `91:250`、确认 `91:463`、
音量 `91:534`、未确认 `91:512`，原始资源于 2026-11-02 导出。

- `wallpaper.png` 是该设计的生成山湖壁纸；不是实景门口视频，也不用于来电预览。
- SVG 保留 Figma 原始导出、尺寸与颜色。线条图标来自 Lucide；ISC 与相关 Feather MIT
  许可保留在[图标许可](../../ICONS_LICENSE.txt)。圆形底板、状态点和
  滑块为设计中的几何资源。
- 原资源生成器（提交 `0f93f4147b2978211f47ce5e80c5739449451892` 的 `scripts/generate_door_assets.py`）将壁纸按居中 object-cover 裁切为 1024x600 RGB565，
  SVG 按原尺寸转为 LVGL 9 RGB565 平面加 Alpha 平面，不手描、拉伸图标。`manifest.json` 保留最初的 LVGL 8 导出记录，迁移到 LVGL 9 后像素不变。
- 壁纸通过 IDF binary-data 嵌入 Flash，图标为只读 C 数据；运行时不联网下载、解压
  或额外分配整屏壁纸缓存。仅待机页使用壁纸，切页会清除；通话复用实际视频呈现层。

生成环境使用 Pillow 和 CairoSVG 2.8.2，需要本机 Cairo。Windows 的 Cairo DLL 不齐时
可在 WSL 的隔离 Python 环境执行生成脚本，不修改固件依赖。生成环境版本记录于清单。
