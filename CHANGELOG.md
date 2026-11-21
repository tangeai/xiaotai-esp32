# 版本记录

## 0.3.0

本版支持四款彩屏开发板：立创实战派 S3、M5Stack CoreS3-Lite、微雪 ESP32-P4 3.5 寸（rev3）和 WT99P461-S1。每款都提供 ESP-IDF 6.0 和 5.5 两条线的固件。

### 界面

- 彩屏界面改版：更深的背景和两层卡片，强调色统一为与标志一致的天蓝，接听用绿色、挂断和拒绝用红色，圆角加大。
- 按钮同时显示图标和文字；通话页的按钮都有底色，接听键是电话图标。
- 来电和通话页增加圆形头像（名字的第一个字），名字、状态和通话时长居中排列。
- 菜单每项有图标；能点进去的列表行右侧有 `>`。
- 配网页改为三步说明：连接热点、打开网址、选择家里的 Wi-Fi。
- 1024×600 等大屏上，普通页面集中在屏幕中央，不再拉散到整屏。
- 首页右侧的通话按钮离屏幕边缘留出间距。
- 英文、数字和标点改用思源黑体自己的字宽，`XiaoTai-0001`、`Wi-Fi`、`06:15` 这类文字不再有多余空隙。
- 修正部分页面左上角多出的 “Text” 字样。

### TiRTC 与 ESP-IDF

- 通话库升级到 TiRTC 官方正式版 2.6.3，四款产品用同一种芯片通用包，不再区分立创专用包。
- 仓库不再存放 TiRTC 库文件。`packages.json` 记录官方包的地址和 SHA-256，第一次编译时 `scripts/fetch_tirtc.py` 自动下载、核对并解压；编译机不能上网时可以用 `--from` 指定下载好的包。
- 构建按 `components/tirtc_sdk/contracts/` 的契约核对库、头文件、ESP-IDF 与工具链、libc、FreeRTOS 类型大小和加密依赖。新增 `scripts/tirtc_contract.py`，升级 TiRTC 版本时从官方包直接生成契约。
- 默认版本线统一为官方 ESP-IDF 6.0.3（按官方提交核对），可选 5.5.5。ESP-IDF 6.0 版的官方包自带加密实现，不再约束 Mbed TLS 配置。
- 依赖小版本更新：所有产品、两条版本线统一用 cjson 1.7.19~2 和 esp-dl 3.3.15；P4 两款的 usb 更新到 1.6.1。

### 构建

- 只有一个构建入口：`python scripts/build.py <板>`，产物在 `build/<产品>/sdk-firmware/`，5.5 线在 `sdk-firmware-idf5.5/`。旧的构建入口和 `main/` 目录已删除，`--entry legacy` 会直接报错。
- 版本号只在 `cmake/esp-idf-product/CMakeLists.txt` 的 `PROJECT_VER`。
- `scripts/package_release.py` 直接打包 `sdk-firmware*` 构建目录，同一产品两条版本线的固件可以一起打包：默认线的文件名不变，5.5 线的文件名带 `-idf5.5`，发布说明和烧录说明标出每个镜像的 ESP-IDF 版本。
- 在 Windows 上编译时，编译参数经 Ninja 响应文件传给编译器。P4 两款的头文件路径很多，原来会超出 Windows 命令行长度上限。

### 开发工具

- 新增 `tools/ui_preview.py`：在电脑上用真实的产品、主题和 LVGL 把每一页画成 PNG，自动点遍各页并演示 AI 对讲和来电，输出总览页。主机端的 LVGL 支持旋转安装的屏，微雪 P4 的界面也能在主机测试里画出来。
- 新增 Claude Code 技能 `xiaotai-ui`（改界面的流程和设计规范）；`xiaotai-build` 按新的版本线和 TiRTC 下载流程重写。
- 新增《使用说明》，写给使用设备的人：配网、绑定、AI 聊天、打电话、多人对讲、设置和常见问题。

## 0.2.0

### 功能

- 立创 S3、CoreS3-Lite、两款 P4 和 Xmini 的网络音频统一为 PCMA（G.711 A-law）8 kHz 单声道，这些板之间可以直接互相通话。板内的采集、回声消除和播放仍是 16 kHz。
- 六个产品都支持通过串口切换测试环境（`AT+EP SHOW/SET/RESET/CANCEL`），切换后身份和 Wi-Fi 保留。
- 立创 S3 和 CoreS3-Lite 支持声音识别：录音、提交、查询结果、播放结果语音；Xmini 支持读取识别配置。
- Xmini 界面重做：首页改成列表，可选 AI 对话、微信通话、设备通话、多人对讲、设置；AI 页只显示状态和时长，单击打断，长按结束；设置页可以调音量、麦克风增益、语音引导、好友申请。
- MQTT 按服务发现下发的地址连接，同时接受 `mqtt://` 和 `mqtts://`。
- 可选 ESP-IDF 5.5：`python scripts/build.py <板> --idf 5.5`，使用官方 ESP-IDF 5.5.5 和 TiRTC 官方发布的 v2.6.0 原件，产物在 `firmware-idf5.5/`。默认仍是 ESP-IDF 6.0。Xmini 用到的两个接口还不在官方包里，暂不支持 5.5。`python scripts/build.py --list-idf` 列出每块板的支持情况。

### 行为变化

- 第三方组件统一到同一版本：所有产品、两条 ESP-IDF 版本线用同一套组件。esp-sr 2.6.1（连带 esp-dl 3.3.14、dl_fft 0.8.0）、esp_audio_codec 2.6.2、esp-tflite-micro 1.4.1、esp-nn 1.4.2、mqtt 1.1.0、esp_lcd_touch_ft5x06 1.1.1。P4 家族原来放在仓库里的 esp-sr、esp-dl、esp-nn、esp_codec_dev 副本删除，改用组件仓库的发布版；tflite 不再从 GitHub 取。
- CoreS3-Lite 的 MQTT 收发缓冲改用 mqtt 1.1.0 自带的 `CONFIG_MQTT_BUFFERS_ON_EXTERNAL_MEMORY` 放进 PSRAM，删掉构建时替换 mqtt 源码的做法；S3 的 tflite 卷积通道修正已在上游 1.4.1 里，构建时补丁一并删除。
- 立创 S3 的回声消除和双麦降噪（ESP-SR AFE）改用“多用 PSRAM”的内存策略。esp-sr 2.5 起，原来的均衡策略会把双麦分离的工作区放进内部 RAM（约 176 KB），屏幕和通话就没有内存可用了；改后 AFE 只占约 14 KB 内部 RAM。两款 S3 还把 esp-sr 里用不到的降噪模型静态表（约 18 KB）放到 PSRAM。
- 所有产品的 APP 分区都扩展到 Flash 末尾（16 MB 板 16,320 KB，MZ28 8,128 KB），主芯片不再预留 OTA 分区。`nvs` 位置不变，已经配网和绑定的设备只更新分区表和应用时，数据会保留。P4 的 C6/C61 协处理器升级不受影响。
- CoreS3-Lite 通话时加了上行噪声门，没人说话时不再把房间底噪发给对方。Xmini 的回声门会扣除对方稳定的底噪，减少自己说话时被误压的情况。
- Xmini 收到不带载荷的设备挂断信令时会正常结束通话，不再停在“呼叫失败”页。
- 构建入口去掉了开发阶段的门禁，开发者按文档编译不会再被拦下。

### 已知限制

- 声音识别和测试环境切换已经接入代码，但还没有在真实服务上做完端到端验证。
- WT99P461-S1 和 MZ28 只验证到编译通过；微雪 P4 已验证能启动，联网和业务还没有验证。
- MZ28 只支持微信通话，网络音频仍是 Opus 16 kHz。
- ESP-IDF 5.5 线只验证到编译通过。官方 ESP32-S3 包里有 5 条 `s32c1i` 硬件原子指令，它们在 PSRAM 上不保证原子性，S3 产品在 5.5 上构建时会给出警告；需要稳定运行的 S3 产品请用默认的 6.0。
- 官方 ESP32-S3、ESP32-P4 v2.6.0 包是用 ESP-IDF 5.5.4 编的，仓库在 5.5.5 上使用，靠契约里的 ABI 检查保证结构大小一致。
- 立创 S3 升级 esp-sr 后可用 PSRAM 少了约 290 KB（esp-sr 2.6 会带进 esp-dl 和降噪模型的代码，这块板的程序又是在 PSRAM 里运行的），声音识别一次能录的时长从约 24 秒降到约 12–15 秒，默认的 10 秒不受影响。
- 默认 6.0 线的 TiRTC 库是 IDF 6 验证版，不是 TiRTC 正式发布的包。立创 S3 的 6.0.3 只认构建镜像里的源码树，自己安装的 6.0.3 编不了立创，请用 `--idf 5.5`。

### 开发工具

- 新增多设备自动化测试工具 `tools/xt`：通话、AI、微信、逐帧抓包对账、内存趋势测试，以及一条命令跑完的标准回归 `xt.py regress`。
- Xmini 和 CoreS3-Lite 支持 `AT+TRACE`，在通话中逐帧记录校验值和电平，用来判断丢帧出在网络还是两端的处理。
- 每块板只有一个声明文件 `main/boards/<厂商>/<板>/board.json`：芯片、硬件能力、用哪套源码、出哪些产品都写在这里。原来每块板的 `config.json`、`capabilities.json`、`source-profile.json`，以及中心的 `config/product-contracts.json` 和 Kconfig 板型选择都删掉了。板子按目录自动发现，接新板不用再去登记。
- 内存放置按能力统一：板子开了 PSRAM，构建入口就套用 `config/capabilities/psram.defaults`（MQTT 缓冲、任务栈、静态区、AFE 等放进 PSRAM），板子自己的配置文件不再各写一份。
- `python scripts/build.py <板> --resolve-only` 只解析依赖和配置、不编译，几分钟就能检查依赖和锁文件改得对不对。
- 新增发布打包工具 `scripts/package_release.py`：从编好的产品生成完整镜像、应用镜像、分区表、`release-manifest.json`、中文烧录说明、发布说明和 `SHA256SUMS.txt`。打包前核对每个构建的源码都来自发布提交、镜像和 ELF 对得上、固件版本号等于发布版本，只打包验证过的产品。固件版本号统一由根目录 `CMakeLists.txt` 的 `PROJECT_VER` 决定，两个版本之间带 `-dev` 后缀。
- 新增 Claude Code 技能：`xiaotai-build`、`xiaotai-new-board`、`xiaotai-test`、`xiaotai-verify`。
- 文档按开发者视角重写：快速开始、开发指南、新板接入、测试与诊断、AT 指令、功能参考。

### 仓库整理

- 两款 P4 共用 `vendor/esp32p4/` 下的同一份乐鑫组件，H.264 编码器也合成了一份（WT99 用到的颜色描述字段是可选项，微雪 P4 不填时码流不变），仓库少了一整份重复的 vendor。
- 两款 P4 的业务组件开始合并成一个家族：存储（`nvs_store`）、运行配置、按键、开发控制台、平台客户端、语音唤醒、Wi-Fi 管理各只留一份，放在 `p4/` 下，WT99 通过 `board.json` 引用。Wi-Fi 管理里两块板不同的只有协处理器链路（微雪是 SDIO 接 C6，WT99 是 SPI 接 C61），按 Kconfig 选择，每块板编出来的行为和以前一样。应用参数改由各自启动组件的 `Kconfig` 提供，删掉了没有代码读取的旧标记 `XIAOTAI_TIRTC_HTTP`。
- 第三方来源合并成一份 [THIRD_PARTY.md](THIRD_PARTY.md)，链接都指向文件现在的位置；`docs/licenses/` 只放许可原文，补上了 M5Stack 的 MIT 许可。
- 结构检查对 THIRD_PARTY.md 和许可目录也做链接检查。
- 四款彩屏产品共用 `components/xiaotai_lvgl/fonts/` 里同一份 18 px 中文字库，去掉了三份逐字节相同的副本（约 38 MB）。
- TiRTC 包由一张表 `components/tirtc_sdk/packages.json` 选择，产品契约按版本线声明用哪个包；构建入口统一解析，CMake 只做核对。
- SDK 要求的 libc、原子操作和加密配置由包的契约统一注入，不再分散写在板级配置里。
- 组件清单不再逐个钉死 IDF 版本，只在 `main/idf_component.yml` 写一个范围，精确版本由构建入口按官方提交核对。
- `--write-lock` 生成或刷新依赖锁，本地组件统一写成相对路径。
- USB 连接监测的 IDF 源码核对改为先统一换行再比对，Windows 和 Linux 的检出结果一致。
- 删除构建不读取的板级 `sdkconfig.defaults`、WT99 目录里没被使用的微雪 BSP 副本，以及 Xmini 一个无人引用的 cmake 文件。

## 0.1.0

- 建立统一的小钛 C/C++ 固件仓库，接入立创 S3、CoreS3-Lite、微雪 P4、WT99P461-S1、Xmini C3 V3 和 MZ28 六个产品，保留各板原有业务与交互实现。
- 按板卡与产品选择源码、媒体、交互和 SDK，独立生成配置与固件；对不支持的能力组合明确报错。
- 立创 S3 使用 TiRTC 2.6.0 / IDF 6.0.3 的 PSRAM 兼容 HTTP-only SDK 变体，其他产品的 SDK 保持不变。
- 固定各产品依赖锁，记录有效配置、实际构建输入和产物身份。
- 提供架构、新板适配、编译选择及版本验证说明。
