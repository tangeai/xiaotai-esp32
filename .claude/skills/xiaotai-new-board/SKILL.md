---
name: xiaotai-new-board
description: 把一块新的 ESP32 开发板接入小钛：先说明能选哪些功能和交互，只问还缺的硬件资料，然后建板目录、实现板级端口、登记产品、编译、上板验证。用户说“接一块新板”“适配我的板子”“换屏幕/codec/按键方式”时使用。已经支持的板出了故障用 xiaotai-verify。
---

# 新板接入

目标是交付一块能跑起来的产品，不是在编译菜单里多一个名字。完整说明在 `docs/新板接入.md`，下面是执行步骤和完成标准。

## 1. 先讲清楚能做什么，再收资料

先用一两句话告诉用户可以选的功能：AI 聊天、设备对讲、微信通话、多人对讲、网页实时查看。再说交互方式：触摸屏、OLED 加按键、无屏（按键、灯、提示音）。

然后只问还缺的资料，用户能给原理图、厂商例程或引脚表最好：

- 完整板型和修订号；芯片、Flash、有没有 PSRAM；
- 音频：codec 或麦克风/功放型号，有没有回采参考；
- 屏幕和触摸型号、分辨率、方向；按键、灯、摄像头；
- 联网方式：芯片自带 Wi-Fi，还是外挂协处理器；
- 要哪些功能。

已经知道的不要再问。资料没有证实的能力，不写进声明。

## 2. 判断类型，找最近的参照

| 情况 | 参照 |
| --- | --- |
| S3 + 触摸屏 + ES8311/ES7210 | `boards/lckfb/szpi-esp32s3`（产品 `products/xiaotai-szpi-s3`） |
| S3 + 不同的电源或 IO 扩展 | `boards/m5stack/core-s3`（产品 `products/xiaotai-cores3-lite`） |
| P4 | `boards/waveshare/esp32-p4-wifi6-touch-lcd-3.5`、`boards/wt/wt99p461-s1` |
| 新芯片 | 先确认 TiRTC 官方发布站有对应芯片的包；没有就停下来告诉用户 |

## 3. 实现

按 `docs/新板接入.md` 第 3、4 节。一款新产品由四处文件组成：

| 位置 | 写什么 |
| --- | --- |
| `boards/<厂商>/<板>/` | `board.json`（芯片、身份、内存、零件能力、组件和分区）、`board.cc`（用零件接口创建屏、音频、按键、相机等）、`CMakeLists.txt`、`sdkconfig.board`（只写硬件事实）、`partitions.csv`、`test/` |
| `products/<产品名>/product.json` | 选板包、应用、模块、参数、选项和变体 |
| `profiles/native-firmware/<产品名>.json`（5.5 线放 `idf5.5/`） | 板卡参数、芯片、版本线、ESP-IDF 版本、TiRTC 包、配置层 |
| `profiles/<产品名>-native.yml`（5.5 线 `-idf5.5-native.yml`）和 `profiles/dependency-locks/` | 组件清单和依赖锁 |

1. 复制参照板和参照产品的这几处文件，逐个改成新板的事实。`sdkconfig.board` 只写 Flash、PSRAM、缓存、引脚相关的驱动选项；PSRAM 等能力带来的策略由 `config/capabilities/` 自动套用，不要从参照板抄过来。
2. 板包按目录被自动发现，产品清单指向板包后，`python scripts/build.py --list-boards` 里出现新板就对了。
3. 硬件代码只写在板包里：同款外设复用 `sdk/parts/` 的零件；新外设写成零件放进 `sdk/parts/`（平台相关部分在零件的 `esp-idf/` 下），`board.cc` 只负责接线。
4. 引脚只出现在板包里。用 `grep -rn "GPIO_NUM_" sdk/ apps/` 检查有没有残留参照板的引脚。
5. 分区：`nvs` 放在 `0x9000`，`factory` 从 `0x10000` 开到 Flash 末尾，不做 OTA 分区。
6. 能力只写已经实现的。缺零件时先实现零件，再写能力，不要只登记一个名字。

不要为了新板改业务代码或加板名判断。如果确实需要改，说明那一层还没抽象好：在那一层做最小扩展，把差异写成 `product.json` 的参数，并检查对其他板的影响（`python scripts/check_change_impact.py --files ...`）。

## 4. 主机检查和编译

```bash
python3 -B scripts/sdk_selection.py --check --strict
python3 -B tests/sdk/run.py --board <厂商>/<板>
python3 -B tests/sdk/product/run.py --product <产品名>
python3 tools/ui_preview.py --product <产品名>        # 有屏的板：看每一页在新屏上的样子
python scripts/build.py <新板> --dry-run
python scripts/build.py <新板>
python scripts/check_structure.py
```

新产品第一次编译前还没有自己的依赖锁：把芯片相同、模块相近的参照产品的锁复制成 `profiles/dependency-locks/<产品名>-native.lock`，再编译。解析结果和这份锁不同时，构建会停下并给出两个文件的路径；逐项看过差异，确认是新板需要的组件后，把构建目录里的 `dependencies.lock` 复制回去，再编一次。锁和板包一起提交。

编译通过后，到 `compile_commands.json` 里确认新板的零件参与了编译，并且没有参照板的驱动。

新板必须先通过默认的 ESP-IDF 6.0 线。要加 5.5 支持，前提是该芯片有官方 5.5 的 TiRTC 包（见 `components/tirtc_sdk/README.md`），再补 `idf5.5/` 下的档案、清单和锁。这一步不是接入新板的门槛。

## 5. 上板验证

烧录按 `xiaotai-build` 的步骤做。之后按顺序验证，能自动化的用 `tools/xt/xt.py`：

1. 启动：`AT+VERSION` 是新板身份；屏幕、按键、灯正常。
2. 音频：放提示音，麦克风和参考通道都有电平，没有削顶。
3. 联网和绑定。
4. 业务：`xt.py call <新板> <已有板>` 双向都通过；有 AI 的话跑 `xt.py ai <新板>`。
5. 退出和恢复：重复进出、挂断、断网重连之后，都能回到待机。
6. 内存：`xt.py soak <新板> --peer <已有板> --rounds 10`，`pass` 为 true。

听感和实体按键的手感，请用户确认。

## 完成标准

- 能力声明和实际硬件一致。每个功能在所选交互上都能操作，包括无屏时的配网、绑定、接听、退出和失败提示。
- 业务源码没有新板的板名分支。
- 第 5 步全部通过，或者明确列出没测的项目和原因。

最后输出：新增和修改的文件、构建命令、验证结果表、没测的项目。
