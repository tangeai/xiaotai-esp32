# 测试指南

这里维护小钛的可复用主机测试。先确定本次改动，再选择用例；SDK 专项排障和个人真机记录不属于默认流程。怎么选测试见 [测试与诊断](../docs/测试与诊断.md)。

## 常用入口

在仓库根目录运行。主机 C/C++ 用例需要 Linux 或 WSL 里的编译器、CMake 和 Ninja；依赖 IDF 源码的测试使用产品锁定的 IDF，不临时下载另一版本代替。

```sh
# 只规划受影响的产品和检查项，不构建、不刷机
python scripts/check_change_impact.py --files <本批改动路径>

# 构建工具、产品清单和发布打包的自测，无网络、无需板卡
python tests/test_change_impact.py
python tests/test_products.py
python tests/test_native_build_entry.py

# 全部 SDK 组件、板包和五款产品的主机测试
python3 tests/sdk/run.py
# 只改一个模块时，先单测该模块及其依赖
python3 tests/sdk/run.py xiaotai_mod_ai_talk

# 整套：上面的工具自测加 SDK 主机测试
python3 tests/fetch_components.py --set all
python3 tests/run_host.py --p4-idf-path <已核对的 IDF 6 源码目录>

# 迁移台账：223 行旧测试都已关闭，旧源码在 tests/migration_inventory.json 记录的提交里
python3 tests/check_migration.py
```

| 目录 | 是什么 |
| --- | --- |
| `tests/sdk/` | SDK 组件、板包和产品图的主机测试；`product/` 是五款产品的生成和启动测试，`integration/` 是跨组件场景 |
| `tests/test_*.py` | 构建入口、产品清单、依赖锁、发布打包、串口工具等的自测 |
| `tests/hosted_spi/` | WT99 协处理器 SPI 诊断补丁的主机夹具 |
| `tests/recognition/` | 声音识别配置的核验工具，按需使用 |
| `tests/migration.json`、`tests/migration_inventory.json` | 旧测试迁移台账和冻结的旧测试清单 |

### 组件源码

不少主机测试直接编译第三方组件源码：cJSON、ESP-DSP 内核、LVGL、WT99 的 esp_hosted 补丁和 esp-mqtt 检查要用的源码。版本和 `component_hash` 取自各产品的 SDK 依赖锁（`profiles/dependency-locks/<产品>-native.lock`）；测试不直接读 `managed_components/`，那里只是最后一次编译那块板的依赖。第一次运行前准备一次：

```sh
python3 tests/fetch_components.py --set all
```

脚本按顺序在本机组件缓存（`IDF_COMPONENT_CACHE_PATH` 和默认缓存，WSL 里还查 Windows 侧缓存）、EIM 离线组件库、仓库根的 `managed_components/`（仅当哈希相符）中查找，校验 `component_hash` 后复制到 `build/host-components/`，并生成 `paths.json`、`paths.cmake`。`XIAOTAI_HOST_COMPONENTS` 可以改放置位置，只能在仓外或 `build/` 下。找不到时退出码为 2，并列出每个组件的下载页、应有的 `component_hash` 和目标目录；手动下载的 zip 用 `--archive <zip>` 导入，加 `--download` 则允许从官方组件库下载。`--isolated` 只查 `IDF_COMPONENT_CACHE_PATH`、默认缓存和命令行给的来源。它和 `run_host.py` 都接受 `--idf <线>`，按那条线的锁取版本，没有该线锁的产品用默认线。

`test_device_console_recognition.py` 执行实际串口脚本参数入口，串口模块禁止访问设备。检查只读、任务操作与录音上传的独立开关、非法编号/版本/时长，以及 dry-run 不打开串口、不创建文件。采集工具不自动判定识别成功；设备侧 `OK`、准入回执和最终任务结果仍要分开核对。

## 每项怎么验

| 项目 | 方法 | 通过条件 |
| --- | --- | --- |
| 协议回包 | 主机注入正常、拒绝、缺字段、重复和迟到回包 | 错误可见，旧回包不污染新会话，失败不伪装成功 |
| 交互与设置 | 走原业务入口，查询实际状态；持久化变化补一次重启 | 显示、业务状态和保存结果一致 |
| 会话生命周期 | 代表板执行进入、退出、再进入及本批失败条件 | 资源归还，可再次使用，未改业务保持原行为 |
| 媒体或硬件差异 | 对受影响板核对格式、计数与实际输入输出 | 正确数据路径及实物效果；数字计数与听感分别记录 |
| 功能配置 | 验证开启、关闭、依赖不足和不支持组合 | 源图、UI、任务和能力上报一致，非法组合明确报错 |

实机前核对设备、产物和授权。优先使用现有本地 AT、参数化脚本和获准的手机操作，保留身份及会话归属检查。普通检查不清配网、绑定或 NVS；真实听感、实体触摸按已有有效验收或人工确认记录。

## 结果怎么记

记录改动范围、源码/产物身份、用例、实际结果和未测原因，分清主机、构建、烧录、运行与实物效果。共享问题记录一次适用证据，不在所有板上重复证明同一结论。协议专项入口如 [识别配置核验](recognition/README.md) 是按需工具，不是所有贡献者都必须访问真实后台的门槛。

公共仓只保留可复现方法、合成测试数据和必要脱敏摘要。运行输出写仓外；不提交个人设备、账号、原始日志、录音、固件或 SDK 实验包。
