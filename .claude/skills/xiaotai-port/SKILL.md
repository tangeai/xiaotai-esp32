---
name: xiaotai-port
description: 把小钛移植到新的 ESP-IDF 版本或新芯片：同一版本线升补丁版（例如 6.0.3 到 6.0.4）、加一条新版本线（例如 6.1）、支持一种新芯片（例如 ESP32-S31、C5），或者两件一起来（新芯片只有新 IDF 支持）。先用自带的盘点脚本列出要登记、要审源码、要判断的每一处，再管 IDF 版本登记、TiRTC 官方包和构建契约（缺包时写包需求单）、产品档案和依赖锁、构建入口和代码里按版本线或芯片写的分支、审过源码才放行的替换，最后统一编译、主机测试、代表板上板。用户说“升级 IDF”“适配 IDF 6.1”“支持新芯片”“移植到某款新芯片”“换 TiRTC 包”“加一条版本线”“新 IDF 编不过”时使用。已支持的芯片接新板用 xiaotai-new-board；单纯编译烧录用 xiaotai-build。
---

# 移植到新 ESP-IDF 版本或新芯片

四条原则：

- **官方的东西原样用**：ESP-IDF 和 TiRTC 官方包不改、不重编。差异写进档案、契约和清单，或写成我们自己组件里的适配，不在代码里判断板名。
- **一次改完再编**：登记、档案、锁先改齐，静态预检，只解析依赖生成锁并审过差异，最后一轮编所有受影响的产品，集中修错。不要改一处编一次。
- **新线先当可选线**：默认线不动；新线过了上板验证、用户同意后才改默认。
- **两件一起来时先线后芯片**：先让现有芯片至少一款产品在新线上编过，再加新芯片。新线的问题（组件管理器、IDF 接口、源码审查）和新芯片的问题（芯片表、分支、内存）分开暴露，出错时知道是哪一边。

## 1. 先盘点，再分类

```bash
python .claude/skills/xiaotai-port/scripts/port_inventory.py --line 6.1 --target esp32s31
```

只读，不编译、不下载；只给 `--line` 或只给 `--target` 也行。在 Git Bash 这类管道里中文乱码时，用 `python -X utf8` 运行。每行一个结论：

- `[缺]`：编译前必须补上的登记、包、契约、档案、锁、审过源码的哈希。
- `[查]`：按版本线或芯片写的分支、只列了部分芯片的能力表、审过源码才放行的地方。逐个读代码，决定新线或新芯片走哪边。
- `[有]`：已经登记好。

芯片表、按线写的分支和源码审查门都是扫描出来的，以后新加的也会列出。输出存进工作记录；改完再跑一次，`--strict` 在还有 `[缺]` 时返回 1。

| 情况 | 例子 | 读哪几节 |
| --- | --- | --- |
| 同一条线升补丁版 | 6.0.3 → 6.0.4 | 3、4 |
| 加一条版本线 | 6.1 | 2、3、5 |
| 支持一种新芯片 | 新的 ESP32 系列 | 2、3、6 |
| 新芯片只有新线支持 | 6.1 加 S31 | 2、3、5、6、7 |

## 2. 登记在哪

**版本线**

| 文件 | 内容 |
| --- | --- |
| `scripts/idf_environment.py` 的 `LOCKS` | 每个验证过的 ESP-IDF 版本和官方提交号；构建核对当前 IDF 树就是这一份 |
| `components/tirtc_sdk/packages.json` | `default_line`；每条线的 `idf_versions`（第一个是这条线通用包契约要求的版本）和每种芯片的包（发布站路径、版本、SHA-256、契约文件） |
| `components/tirtc_sdk/contracts/<线>/<芯片>.json` | 由 `scripts/tirtc_contract.py` 从包生成，字段和核对规则见第 3 节 |
| `profiles/native-firmware/<产品>.json`（非默认线放 `idf<线>/`） | 板、芯片、`idf_line`、`idf_version`、`tirtc_package`、配置层；P4 还有 `sdkconfig` 和 `vendor_hashes` |
| `profiles/<产品>-native.yml`（非默认线 `-idf<线>-native.yml`） | 组件清单，版本钉死 |
| `profiles/dependency-locks/`（非默认线放 `idf<线>/`） | 每款产品的依赖锁，裁剪变体另有 `<产品>--<变体>.lock` |
| `boards/<厂商>/<板>/board.json` 的 `idf` | 这块板支持哪几条线 |
| `sdk/xiaotai_port/esp-idf/usb_monitor/usb_monitor.cmake` | 按线记录的、审过的 IDF 源码哈希（见下面“审过源码才放行”） |

**芯片**

| 文件 | 内容 |
| --- | --- |
| `scripts/xiaotai_build.py` 的 `TARGETS` | 构建入口认的芯片 |
| `scripts/tirtc_contract.py` 的 `TOOLCHAINS` | 芯片对应的工具链前缀（`xtensa-esp-elf` 或 `riscv32-esp-elf`），生成契约时找 `nm`、`objdump` 用 |
| `scripts/package_release.py` 的 `CHIP_IDS` | 打包时核对镜像头里的芯片 ID |
| `tools/xt/partition_plan.py` 的 `BOOT_OFFSETS`、`IMAGE_CHIP_IDS` | 烧录前比对分区用的引导程序地址和芯片 ID。地址看新芯片构建出的 `flasher_args.json`，芯片 ID 看 esptool，不猜 |
| `tools/xt/boot_check.py` 的 `CHIPS`、`tools/xt/partition_capture.py` 的芯片版本检查 | 只对部分芯片生效的能力表，按新芯片的能力决定加不加 |
| `sdk/xiaotai_media/idf_component.yml` 的 `target in [...]` | 回声消除和唤醒用的 ESP-SR、音频编解码按芯片装；不在表里的芯片装不上 |
| `tests/test_build_profiles.py` 的芯片集合、`tools/xt/test_partition_plan.py` | 测试里列举的芯片 |

**审过源码才放行**：构建在几处先比对官方或组件源码，再替换文件或打补丁，对不上就停：IDF 的 USB 串口监视（按线记哈希）、HTTP 和 MQTT 的源码登记，唤醒用的 TFLM 卷积源码，DVP 相机对 esp32-camera 的覆盖，WT 板对 esp_hosted 的 SPI 诊断，TiRTC 的 HTTP TLS 后端。新线、补丁版或组件升级都可能停在这里。做法：读新源码，和登记的那一版逐段比，确认我们依赖的行为（时序、所有权、调用顺序）没变，再登记新哈希；按线记录的加一个新键，不覆盖旧线的。只抄哈希不读源码，等于把审查拆掉。

`python scripts/build.py --list-idf` 列出每条线和每款产品选中的包。TiRTC 包的选法见 `components/tirtc_sdk/README.md`。

## 3. TiRTC 包和契约

构建时 `components/tirtc_sdk/CMakeLists.txt` 逐项核对，任何一项不对就停：

| 核对 | 要求 |
| --- | --- |
| 线和芯片 | 包所属的线等于当前 IDF 的主次版本；契约 `target` 等于 `IDF_TARGET` |
| `idf_version` | 和当前 IDF **完全相同**，包括补丁号 |
| `compiler_version`、`toolchain_release` | 编译器版本相同，编译器横幅里有这个工具链发布号 |
| `libc_config`、`atomic_config` | 每一项和产品配置相同（构建会先按契约补上） |
| `memory_config` | 包声明 `PSRAM=enabled` 时要求 `CONFIG_SPIRAM=y`，任何芯片都一样；只核对、不补，板子没开 PSRAM 就停 |
| `crypto_mode` | `bundled` 时核对库不调 ESP-IDF 的 `mbedtls_*`/`psa_*`；`required` 时把 `crypto_config` 写进配置 |
| FreeRTOS | 1000 Hz，不开 trace 和运行统计 |
| `abi` | 信号量、任务控制块、tick、`time_t` 的大小和对齐，编译时静态断言 |
| `hardware_atomics` | 只在 S3 上数 `s32c1i` 指令 |

契约怎么来：`python3 scripts/tirtc_contract.py --line <线> --target <芯片> --package components/tirtc_sdk/packages/<包名> --out components/tirtc_sdk/contracts/<线>/<芯片>.json`（`nm`、`objdump` 用该芯片工具链里的）。

- `idf_version` 取 `packages.json` 里这条线 `idf_versions` 的第一个。
- 编包用的 IDF、编译器版本、工具链日期从包目录名读，格式对不上脚本直接拒绝。
- libc 和类型大小从包里的 `manifest/build-contract.env` 读，**缺了就填 S3 的默认值**。新芯片的包必须带齐这些字段，否则契约会是错的。

所以，旧包能不能用到新的补丁版，看工具链：新补丁版自带的工具链和契约的 `toolchain_release` 一样，就能沿用（5.5 线就是拿 5.5.4 编的包用在 5.5.5 上），重新生成契约即可；工具链换了，就要新包。

**缺包时写包需求单**交给 TiRTC 打包方，不要自己重编：

| 项 | 要求 |
| --- | --- |
| 芯片和 IDF | `<芯片>`，用 ESP-IDF `<x.y.z>` 官方发布版编 |
| 工具链 | 这个 IDF 版本自带的那一版，写出 `esp-<编译器版本>_<日期>` |
| C 库 | 和这个 IDF 版本、这款芯片的默认一致（picolibc 或 newlib） |
| FreeRTOS | 1000 Hz，不开 trace 和运行统计 |
| 加密 | 和现有 6.0 线一样自带加密、不调 ESP-IDF 的 `mbedtls_*`/`psa_*`；做不到就列出要用的符号 |
| `manifest/build-contract.env` | `ARCH`、`IDF_VERSION`、`FREERTOS_HZ`、`ABI`、`LIBC`、`STATIC_SEMAPHORE_T_SIZE`、`STATIC_TASK_T_SIZE`、`TICK_TYPE_T_SIZE`、`TIME_T_SIZE`；库从 PSRAM 分配缓冲或任务栈时写 `PSRAM=enabled`，库里调用了 `heap_caps_*` 或 `*WithCaps` 却没写，契约生成会拒绝 |
| 平台适配 | 用不用 PSRAM 分配器、任务栈放不放 PSRAM，按构建配置里的 `CONFIG_SPIRAM` 判断，不按芯片名列名单，新芯片才不会漏掉 |
| 包名 | 沿用现有格式：`tirtc__espressif_<芯片>_idf<x_y_z>__esp-idf<x.y.z>-<架构>-esp<编译器版本>_<日期>__v<TiRTC 版本>__mini` |
| 交付 | 发布站路径和 `.tgz.sha256` |

## 4. 升补丁版

1. 装官方发布版，版本和提交号加进 `LOCKS`。
2. `packages.json` 这条线的 `idf_versions` 把新版本放第一个。
3. 在新版本的环境里看工具链（`xtensa-esp-elf-gcc --version` 或 `riscv32-esp-elf-gcc --version`），和每份契约的 `toolchain_release` 比：一样就重新生成契约；不一样就写包需求单，等新包。
4. 这条线上每款产品的档案改 `idf_version`；依赖锁只解析、不编译地重新生成，审差异。
5. 跑盘点：源码审查门比的是文件内容，补丁版也可能改到登记过的文件。
6. 这条线的产品全部编一次、跑 `verify_build`，每种芯片一块代表板上板。

## 5. 加一条版本线

1. **IDF**：装官方发布版，记下提交号，加进 `LOCKS`。IDF 树不改；用不到的子模块可以不检出。
   - **Python 环境**：新的次版本常多要几个包（6.1 比 6.0 多 `esp-idf-sbom`、`idf_drivers_gdb`），缺了 `idf.py` 直接拒绝运行，没有正规的跳过办法。做法：为新线单独建一个工具目录，工具链相同就链接过去；把上一条线的 Python 环境复制一份，拿 IDF 官方的约束文件（`espidf.constraints.v<线>.txt`）逐个核对已有的包，只装缺的；最后用 `python $IDF_PATH/tools/idf_tools.py check-python-dependencies` 确认。要下载的东西先列给用户。
2. **TiRTC 包**：要上新线的每种芯片各一份用这个 IDF 编的包。发布站上有，就写进 `packages.json` 的新线，`python scripts/fetch_tirtc.py --line <线>` 下载，生成契约（第 3 节）。拿新契约和上一条线的逐字段比，每一处不同都要知道构建怎么处理（上表）。没有包就写需求单，这一步停下，其他步骤照做。
   - 用发布工程在本机自编、还没上发布站的包：这条线的 `origin` 写 `local-build`，契约会照抄；用 `fetch_tirtc.py --line <线> --from <放 .tgz 的目录>` 安装。还没有契约时，先把包解到 `components/tirtc_sdk/packages/`，生成契约后再用 `--from` 核对一遍。
3. **产品档案**：要上新线的每款产品，从现有线复制档案和组件清单，改 `idf_line`、`idf_version`、`tirtc_package`；`board.json` 的 `idf` 加上新线。
4. **构建入口里按线写的分支**（盘点会列出）：现在主要是 5.5 线的特例：重补官方包要的加密配置、组件管理器 2.x 的版本要求、产物别名 `xiaotai.bin`、esptool 4 的 `image_info` 写法，以及按组件管理器大版本决定锁里路径的读法。新线比现有线都新时，这些通常走非 5.5 的那边，但要逐个确认。能按能力判断的（组件管理器版本、esptool 版本、契约字段）改成按能力，不要再复制一个只为新线的分支。
5. **代码里的版本分支**（盘点会列出）：IDF 改了接口的地方按版本窗口处理（例如 LVGL 的 DPI 刷新回调名、CSI 相机从 IDF 6 起依赖的 DMA 组件改名为 `esp_driver_dma`），放在端口层或零件的 `esp-idf/` 目录下。钉死某个补丁版的策略（例如只对审过的那份 TiRTC 二进制启用的空闲栈策略）在新线上不生效，走普通路径，这是有意的。Kconfig 里写着“每条线核对一次”的默认值（例如 P4 唤醒用的便携 FFT），在新线上台架验证后再决定。主机测试里的 IDF 桩（`sdk/xiaotai_ui/test/idf_stubs/esp_idf_version.h`）按需加新版本。
6. **审过源码才放行**：按第 2 节的做法逐个过，按线记录的哈希加新键。
7. **依赖锁**：只解析、不编译，生成每款产品在新线的锁。审间接依赖的差异：新出现的组件、版本跳变、内容哈希变化。共享组件保持和现有线同一版本，除非新 IDF 不支持。原生构建发现锁和解析结果不一致时会停下，报出两份文件的路径，不会自动更新锁；审过差异，再把解析出的锁放进 `profiles/dependency-locks/idf<线>/`。变体锁的离线解析写法见 `docs/开发指南.md` 代码约定里讲更新依赖的那一条。
   - 具体做法：把上一条线的锁复制到 `idf<线>/` 当种子，`python scripts/build.py <板> --idf <线> --resolve-only`；它报出的差异审过以后，把解析出的锁（报错里给的路径）复制回去，再跑一次 `--resolve-only`，应当一致。
   - 正常的差异：`idf` 的版本；用了本地组件的产品（档案里有 `vendor_hashes`）`manifest_hash` 每条线本来就不同，比较锁时也不看它；组件管理器大版本不同的线，锁文件格式版本不同。组件的版本和内容哈希一个都不该变。
8. **测试和工具**：`tests/test_native_build_entry.py`、`tests/test_dependency_lock_gate.py`、`tests/test_change_impact.py`、`tests/host_components.py`、`tests/verify_build.py` 里有按线列举的地方，一起补上。测试里拿新线的版本号当“不存在的线”或“没登记的 IDF 版本”做例子的地方（`grep -rn '"<线>"\|<线>\.0' tests`），换成真不存在的版本号。
9. **统一编译**：每款产品在新线上编一次，在那条线的 IDF 环境里跑 `python tests/verify_build.py --entry sdk --require-flash-layout <构建目录>` 核对产物、锁和烧录布局（它调用当前环境的 esptool，各线的 esptool 大版本不同，换错环境会报 `image_info` 失败）；记下 ELF 大小和内部内存静态占用，和旧线比。
10. **文档**：`docs/开发指南.md` 的版本线说明、`components/tirtc_sdk/README.md` 的包表、`CHANGELOG.md`。

## 6. 支持一种新芯片

1. **先确认，缺一样就停下告诉用户**：
   - 官方 ESP-IDF 哪个版本支持这款芯片，是正式目标还是预览目标：`idf.py --list-targets` 只列正式目标，`idf.py --preview --list-targets` 才加上预览目标（IDF 树里 idf_py_actions 的 `PREVIEW_TARGETS` 表）。IDF 只在 set-target 这一步拦预览目标，我们的构建入口直接传 `-D IDF_TARGET=<芯片>`，不会被拦，所以“预览目标只做开发线、不发布”要我们自己守，发布前核对；第一次先确认这样能配置成功。
   - 从技术规格书和 IDF 查清芯片事实：CPU 架构（决定工具链）、核数、PSRAM 有无和类型、Wi-Fi 是自带还是走协处理器（像 P4 经 esp_hosted）、USB 串口 JTAG、硬件 H.264/JPEG/ISP/PPA、芯片版本（ECO）和 IDF 要求的最低版本。
   - 每条要上的线，它的 IDF 环境里的 esptool 认不认这款芯片：`verify_build`、打包和烧录都用它。
   - TiRTC 有没有这款芯片在这条线上的包，没有就写需求单。
   - 用到的组件支不支持它：ESP-SR（回声消除、唤醒）、音频编解码、LVGL 端口和屏驱、相机、H.264、esp_hosted。不支持的功能，这款芯片的产品就不选。
2. **芯片表**：盘点列出的每张登记表加一行，值从官方 IDF、esptool 和第一次构建的 `flasher_args.json` 查，不猜。
3. **代码里的芯片分支**（盘点会列出）。现有分支有：端口 `diag.c` 的缓存计数（只 S3）、USB 监视的 C3 分支、`xiaotai_transport` 里 C3 单核的 TiRTC mini 路径、`xiaotai_video` 的 P4 硬件 H.264、唤醒 `Kconfig.platform` 的验证过的默认值、TiRTC 组件在 S3 上的原子指令核对和连接任务栈策略、板的配置核对；工具里还有契约生成的 S3 原子指令、`verify_build` 的 S3 构建选项和 P4 芯片版本。按能力判断新芯片走哪边：有没有 PSRAM、单核还是双核、有没有硬件 H.264。能写成能力判断的就不要加芯片名。Kconfig 里“只在某芯片上验证过”的快速路径，新芯片先用保守的默认值，台架验证后再开。
4. **能力层**：板配置里 `CONFIG_SPIRAM=y` 时自动加 `config/capabilities/psram.defaults`；`board.json` 的 `memory.external_ram` 为 `false` 时加 `no-external-ram.defaults`。PSRAM 的模式和速率写在板的 sdkconfig 里。
5. **内存和任务**：没有 PSRAM 的芯片按 `docs/SDK架构.md` 的“无外部内存的工作区”和产品的内存上限来，不能套有 PSRAM 的板的配置；单核芯片留意高优先级任务在句柄交回之前就跑起来。
6. **芯片版本**：像 P4 那样不同版本要不同配置的，在板的 `sdkconfig.board` 里钉最低版本（P4 板写的是 `CONFIG_ESP32P4_REV_MIN_301=y`），烧录工具核对实际版本（`partition_capture.py` 对 P4 的做法）。
7. 第一块板用 `xiaotai-new-board` 接入，上板用 `xiaotai-bringup`。

## 7. 新芯片只有新线支持

1. 盘点 `--line <新线> --target <芯片>`。包需求单一次写齐：新芯片在新线的包，加上要上新线的现有芯片的包。
2. 新线先落在现有芯片上：登记新线，至少一款 S3 或 P4 产品迁过去编通，把新线的问题修完。现有芯片在新线没有包时，先做完登记、档案和源码审查，等包。
3. 再加芯片：第 6 节的登记和分支。
4. 第一块板：`board.json` 的 `idf` 只写新线。档案放 `profiles/native-firmware/<产品>.json`（不放 `idf<线>/`），`idf_line` 写新线；组件清单和锁仍按新线命名（`profiles/<产品>-idf<线>-native.yml`、`profiles/dependency-locks/idf<线>/`）。构建入口、`tests/verify_build.py` 和变更影响检查都按顶层档案里的 `idf_line` 认它（`tests/test_native_build_entry.py` 有这种产品的用例）：`python scripts/build.py <板>` 不带 `--idf` 就编新线，构建会打印选了哪条线，带 `--idf <线>` 结果一样；产物在 `sdk-firmware-idf<线>/`。
5. 测试里有几处默认“每个产品都在默认线上”，第一个只在新线上的产品会碰到，按能力改，不要只为它开特例：
   - `tests/test_dependency_lock_gate.py`：跨线比锁只比两条线都有锁的产品；
   - `tests/test_native_build_entry.py`：产品个数，以及构建目录名按档案的线加后缀；
   - `tests/test_sdk_variants.py`：默认线断言只对档案写默认线的产品；
   - `tests/test_product_feature_contract.py`：旧固件功能表只核对旧固件里有的产品；
   - 新产品自己的两张夹具表见 `xiaotai-new-board` 第 4 节。
6. 平台限制：某芯片在某条线上用不了能力层的某项策略时（例如 IDF 6.1.0 上 S31 程序从 PSRAM 运行时，外部内存里的 BSS 链接不过，报 `cannot move location counter backwards`），只在这条线的档案 `sdkconfig` 里关掉那一项，并登记到 `tests/test_psram_policy.py` 的 `PLATFORM_LIMITS`，写明原因；换到修好的线时随档案一起去掉。先看厂家在同一 IDF 版本上的示例工程开了哪些选项，能少走弯路。
7. 仓库的默认线不变。

## 8. 验证顺序

1. 盘点：`port_inventory.py --strict` 没有 `[缺]`，每个 `[查]` 有结论。
2. 静态：`python3 -B scripts/sdk_selection.py --check --strict`、`scripts/check_sdk_layers.py`、`scripts/check_structure.py`。
3. 主机：`python3 -B tests/sdk/run.py`；每款产品 `tests/sdk/product/run.py --product <产品>`；改了构建入口、契约或登记，在有 cmake 的环境（WSL）里把 `tests/` 下的 `test_*.py` 全部跑一遍，不要只挑看起来相关的。缺主机组件、ninja 或 IDF 路径而跑不了的，单独列出来；`tests/test_port_usb_monitor.py --idf-path <IDF 树>` 每条线的 IDF 都跑一次。
4. 构建：每款受影响的产品在每条线上编一次，不只编新线。
5. 上板：每种芯片、每条新线至少一块代表板走 `xiaotai-bringup` 的十项。同芯片的其他板编过并有代表板验证可以先发，验证等级照实写。
6. 回归：通话、AI、多轮内存（`xiaotai-test`）；和旧线比内部内存最低水位和各任务栈水位，缩栈前先量全部会话类型。

## 规矩

- 官方 IDF 和 TiRTC 包有问题，不改它们：在我们自己的组件里适配，或者整理成清单、需求单交给供应方。
- 源码审查门不绕：不改成只警告，不只抄哈希。
- 下载不通时不改网络配置（代理、TLS 校验）：列出地址、期望哈希和目标目录给用户。
- 改默认线、删旧线、改烧录布局要用户同意。

汇报写成：盘点结果（缺几项、查几项、每项结论）→ 改了哪些登记 → 包需求单（如有）→ 每款产品在哪几条线编过 → 哪些上过板、结果 → 和旧线的内存差 → 还没验证的项。
