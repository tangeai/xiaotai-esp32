# 界面截图

这个目录里的图片是设备屏幕的截图，用在 [README](../../../README.md)、[快速开始](../../快速开始.md)、[使用说明](../../使用说明.md) 和各[开发板](../../开发板/README.md)页面里。

它们不是手工截的，也不是示意图：`tools/ui_preview.py` 在电脑上运行真实的产品代码、主题和 LVGL，把每一页画成图片；`render.py` 再从中挑出文档要用的那些。

## 重新生成

改了界面之后，在 Linux 或 WSL 的仓库根目录运行：

```bash
python3 tests/fetch_components.py --set all        # 第一次运行前，准备 LVGL 等主机组件
python3 tools/ui_preview.py --out build/ui-preview
python3 docs/assets/screens/render.py build/ui-preview
```

`render.py` 需要 Pillow（`pip install pillow`）。它按页面的名字找图，不按序号，所以预览流程里多一步或少一步，文档里的图仍然对得上。某一页在这次预览里找不到时，它会列出名字并保留原图。

## 文件

| 文件名 | 内容 |
| --- | --- |
| `s3-*.png` | 立创实战派 S3 的页面，320×240 放大到两倍 |
| `cores3-*.png` | M5Stack CoreS3-Lite 的闹钟页面，同样放大到两倍 |
| `p4-*.png` | 微雪 P4 的页面，480×320 |
| `wt99-*.png` | WT99P461-S1 的页面，1024×600 |
| `strip-*.png` | 几个页面并排的拼图，高度统一为 480 像素，背景透明 |

要增加或更换一张图，改 `render.py` 开头的 `FRAMES`（单张）或 `STRIPS`（拼图），再运行一次。
