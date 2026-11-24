# TiRTC SDK

小钛的音视频通话用 TiRTC SDK。仓库里**不放 TiRTC 的库文件**，只放三样东西：

| 文件 | 是什么 |
| --- | --- |
| [packages.json](packages.json) | 每条 ESP-IDF 版本线、每种芯片用哪个官方包：在 TiRTC 发布站上的路径、版本和包的 SHA-256 |
| `contracts/<版本线>/<芯片>.json` | 构建契约：库和头文件的哈希、编译它的 ESP-IDF 与工具链、libc、FreeRTOS 类型大小、加密依赖、S3 上的硬件原子指令数 |
| `include/tirtc/` | 官方 2.6.3 的头文件，四个包的头文件逐字节相同；主机测试用它编译，固件构建用包里自带的那份 |

库文件在第一次编译时由 [`scripts/fetch_tirtc.py`](../../scripts/fetch_tirtc.py) 从 [TiRTC 官方发布站](https://repo-sdk.tange-ai.com/service/rest/repository/browse/tirtc-sdks/releases/)下载，核对 SHA-256 后解压到 `packages/<包名>/`（这个目录不进 Git）。官方包原样使用，不改、不重编。

## 当前用的包

| 版本线 | 芯片 | TiRTC | 编译它的 ESP-IDF | 用在 |
| --- | --- | --- | --- | --- |
| 6.0（默认） | ESP32-S3 | 2.6.3 | 6.0.3 | 立创实战派 S3、CoreS3-Lite |
| 6.0（默认） | ESP32-P4 | 2.6.3 | 6.0.3 | 微雪 P4、WT99 |
| 5.5（可选） | ESP32-S3 | 2.6.3 | 5.5.4（用在 5.5.5 上） | 立创实战派 S3、CoreS3-Lite |
| 5.5（可选） | ESP32-P4 | 2.6.3 | 5.5.4（用在 5.5.5 上） | 微雪 P4、WT99 |
| 6.1（开发） | ESP32-S3 | 2.6.3 | 6.1.0 | 立创实战派 S3 |
| 6.1（开发） | ESP32-P4 | 2.6.3 | 6.1.0 | 微雪 P4 |
| 6.1（开发） | ESP32-S31 | 2.6.3 | 6.1.0 | 还没有板子 |

6.1 线的三份包是用发布工程自己的打包流程在本机编出的（契约的 `origin` 为 `local-build`），还不在发布站上，要先从本地目录安装：`python scripts/fetch_tirtc.py --line 6.1 --from <放 .tgz 的目录>`。

```bash
python scripts/build.py --list-idf      # 每条线、每个产品选中的包和版本
python scripts/fetch_tirtc.py --all     # 预先下载发布站上各线的包
```

产品默认用本芯片的通用包；`product.json` 的 `tirtc` 字段可以为某条线指定别的包名。构建入口按产品和版本线选出包，把 `<版本线>/<包名>` 传给 CMake；CMake 只负责核对，不自己挑选。

## 构建时核对什么

| 契约字段 | 核对内容 |
| --- | --- |
| `sha256`、`headers` | 库和每个头文件是不是官方包里的那一份 |
| `idf_version`、`compiler_version`、`toolchain_release` | 当前 ESP-IDF 版本、编译器版本和工具链发布号必须一致 |
| `libc_config` | 6.0 线 S3 包用 picolibc，P4 包用 newlib；5.5 线都用 newlib。产品配置会按这里补上 |
| `crypto_mode` | `bundled`：库自带加密（6.0 线），构建还会从库的符号确认它不调用 ESP-IDF 的 `mbedtls_*`/`psa_*`；`required`：库要用 ESP-IDF 的 DES 和硬件 SHA（5.5 线），这两项会写进配置 |
| `hardware_atomics` | ESP32-S3 库里 `s32c1i` 指令的条数。5.5 线的官方包有 5 条，构建会打印提示；6.0 线为 0 |
| `memory_config` | 包在 `manifest/build-contract.env` 里声明 `PSRAM=enabled` 时写 `CONFIG_SPIRAM=y`，任何芯片都一样：这样的库从外部 RAM 分配缓冲和任务栈，板子没开 PSRAM 时构建直接停下（只核对，不会替板子打开 PSRAM）。库里调用了 `heap_caps_*` 或 `*WithCaps` 却没写 `PSRAM=` 的包，生成契约时就被拒绝。6.0 线 P4 包的清单没写这一项，它现有的契约照旧 |
| `abi` | `StaticSemaphore_t`、`StaticTask_t`、`TickType_t`、`time_t` 的大小和对齐，编译时用静态断言核对 |
| `http_tls_backend` | 库里的 HTTP 客户端是否带 TLS。当前官方包都只支持 HTTP（服务地址统一用 `http://`） |

任何一项对不上，构建立刻停下并说明是哪一项。

## 升级到新的 TiRTC 版本

1. 在发布站找到新版本两条线、两种芯片的包，记下路径和 `.tgz.sha256` 里的哈希，改 `packages.json` 的 `path`、`version`、`sha256`。
2. 下载并解压：`python scripts/fetch_tirtc.py --all`。这一步会因为契约还没更新而报“libTiRTC.a 与契约不符”，属正常。
3. 激活对应工具链（让 `xtensa-esp-elf-nm`、`riscv32-esp-elf-nm` 在 PATH 上），为每个包重新生成契约：

   ```bash
   python scripts/tirtc_contract.py --line 6.0 --target esp32s3 \
       --package components/tirtc_sdk/packages/<包名> --out components/tirtc_sdk/contracts/6.0/esp32s3.json
   ```

   契约里的每个值都从包里读出：哈希、包名里的 ESP-IDF 和工具链版本、`manifest/build-contract.env` 里的 libc 和类型大小、`nm` 看到的外部加密依赖、`objdump` 数出的原子指令。
4. 如果新包的头文件变了，把它复制到 `include/tirtc/`；对比新旧头文件，确认小钛用到的接口还在。
5. 新包要求的 ESP-IDF 版本变了时，同时改 `scripts/idf_environment.py` 的官方提交、`profiles/native-firmware/` 里各产品的 `idf_version`，以及依赖锁里的 `idf` 版本。
6. 跑 `python scripts/check_structure.py`、`python tests/test_fetch_tirtc.py`、`python tests/test_build_profiles.py`，再每款产品编一次，最后上板回归通话。
