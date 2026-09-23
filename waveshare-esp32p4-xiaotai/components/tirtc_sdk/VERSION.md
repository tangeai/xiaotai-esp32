# TiRTC 2.5.0 / ESP32-P4 本地修复批次

批次：`20260922-p4-fixes2`。同时包含失败连接回收、TURN 查询栈修复和反馈处理栈修复，保留 HTTPS；未升级协议或公开 API。
运行时版本仍为 `v2.5.0-9088239c-https1`，BuildTime 更新为本次构建时间；通过本批次与库 SHA-256 区分，不冒充原厂新版本。

| 项目 | 内容 |
| --- | --- |
| 平台 | ESP32-P4 / ESP-IDF 5.5.4 |
| 工具链 | riscv32-esp-elf-gcc 14.2.0 / esp-14.2.0_20260121 |
| FreeRTOS | 1000 Hz，trace/stats 关闭，StaticSemaphore_t=84 B |
| 构建 | RELEASE=y / noSCTP；不改变运行时 transport 协商 |
| libTiRTC.a 大小 | 2,047,340 bytes |
| libTiRTC.a MD5 | `003ac4a68176ec3592f73f1fa75d7474` |
| libTiRTC.a SHA-256 | `6c5c1543f4a099c2f8fdfdff5b50ef728cf181ce09931c2798d34e84d22473b1` |
| 上一批次 SHA-256 | `5571e6918dd07b78e1df9e76362f427ef90d250075b8203a3bae8c2330972792` |
| tiRTC.h SHA-256（include/tirtc/tiRTC.h） | `b1315121abf843c43c669f754382cab302c3b0b35bdfe41f4c4756ef95636293` |
| 日期 / 状态 | 2026-09-22 / 本地测试包，未推送 |

## 本次修改

**新增失败连接回收**：修复 Nano `TiRTC/tiRTC.c::_on_error()` 在连接未建立时仅回调 `error + NULL`、没有销毁底层连接的问题。先撤销迟到回调、排队异步销毁，再按原契约通知连接失败；销毁完成后释放 send_lock/hconn，不对未交付的句柄触发全局 on_disconnected。正常连接的断开流程不变。

**保留 TURN 查询栈修复**：TGWebRTC `client/sdk/libice/src/turn-agent.c` 用临时地址引用作为二分查找 key，不再在查询函数栈上创建整个 `turn_allocation_t`。

| P4 固定栈帧 | 修改前 | 修改后 |
| --- | ---: | ---: |
| find_by_relay | 3792 B | 16 B |
| find_by_address | 3792 B | 32 B |
| rtc_thread 申请栈 | 8192 B | 8192 B |

保持查找顺序、返回值和地址比较规则；不新增堆分配，不调重传、心跳、jitter 或 transport 参数。上表是函数自身栈帧，不是线程总高水位。

## 保留的修复与来源

- **feedback-stack**：保留 ACK/NACK 私有 noinline 函数；BWE 入口固定栈帧 5312 B -> 288 B。NACK 本身仍有 5232 B 栈帧，不宣称全部栈风险消失。
- **HTTPS**：保留证书 bundle、hostname 校验与原握手失败 socket 归属处理。需要 `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y`，不降级到 HTTP。
- 本次从已带 TURN 修复的库定向替换 `tiRTC.o`，107 个成员中 106 个逐字节不变，包含 HTTPS、TURN、反馈处理与线程对象。全部头文件及本工程的转发头布局保留。

| 来源 | 完整提交 |
| --- | --- |
| Nano 2.5.0 | `9088239cc654ec863869d4adc5433ddec3572fad` |
| TGWebRTC v1.5.18 | `f72f5d3ce04c2be369d88f499c7720d0ed7d788f` |
| TgSysAdpt 构建规则与头文件 | `983a086f2bf472f89655f1d62b228ab4d5241635` |

补丁与审计放在 `manifest/`。本工程仅携带预编译库及必要来源记录，完整构建源码在独立 SDK 工作区。
原厂库的下层 socket 配置为 10；此前 HTTPS/反馈及本次 TURN 定向编译使用的配置为 16，应用保持 16。没有全量替换平台对象，不能把整库描述为全部按 16 重编。

## 验证边界

目标库 clean A/B 编译、对象/架构/符号比对通过；200 次失败的主机回归、迟到回调、失败回调内重试以及正常/被动连接关闭用例，在 O2 和 ASan/UBSan 均通过。TURN 与反馈对象及配套配置未变，沿用其已有证据。详情见 `BUILD_EVIDENCE.md`。
旧批次的应用链接结果不适用于本次新库；本次仅替换 SDK，未构建 APP、未烧录。真实 socket 数量恢复、连续 50 次超时后重连、弱网/中继和长稳复测仍需在 P4 完成。
