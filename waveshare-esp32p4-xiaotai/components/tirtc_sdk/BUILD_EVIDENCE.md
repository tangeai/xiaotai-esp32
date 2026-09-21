# P4 TGTRP 反馈栈修复证据

## 现象与修改

TiRTC 2.5.0 HTTPS 包在 IPC 上行、100 ms/5% 丢包下发生 rtc_thread 栈保护异常。SP 比栈下界低 24 B，格式串指向日志时间戳和 Nano 的 `event: %d`。SDK 发送事件正在进入日志，应用未打印码率回调不代表底层从未产生事件。

```text
反馈接收 -> tgtrp_sender_input_feedback -> BWE 决策
-> sender_emit_video_event -> Nano on_video_send_event
-> Logf / vLogf / _getTime / sprintf / _svfprintf_r
-> rtc_thread 栈保护异常
```

原反馈入口同时声明 ACK/NACK/BWE 解码对象，即使收到的只是 BWE，目标编译仍保留 5312 B 栈帧。已识别的入口及日志调用帧合计 7296 B，尚未计入更外层 RTC/ICE 调用；线程申请大小只有 8192 B。完整上层回溯缺失，不推定具体事件一定是降码率、升码率或请求关键帧。

修复仅改 TGWebRTC 的 `client/webrtc/tgtrp-media-transport/src/tgtrp/tgtrp_sender.c`：将 ACK/NACK 原分支移入两个禁止内联的私有函数。校验、解码、计数、日志、处理顺序及返回值保持原样。BWE 不再携带未使用的 ACK/NACK 大栈。

| P4 固定栈帧 | 修改前 | 修改后 |
| --- | ---: | ---: |
| feedback 入口 / BWE 路径 | 5312 B | 288 B |
| ACK 私有函数 | 与入口共用 | 1152 B |
| NACK 私有函数 | 与入口共用 | 5232 B |
| rtc_thread 申请栈 | 8192 B | 8192 B |
| 新增堆/常驻内存 | 0 | 0 |

BWE 路径减少 5024 B。P4 反汇编确认 ACK/NACK 分支先恢复入口 288 B 栈再尾调用，两个栈帧不叠加。NACK 处理本身仍是大栈路径，本补丁不宣称消除了所有栈压力。

没有共享 scratch、新锁或新资源生命周期；解码对象仍属于单次调用。保持 sender 单线程所有权，不宣称支持同一个 sender 跨线程调用或在回调中销毁自身。未改日志等级、格式化实现、重传、ACK/NACK、jitter、heartbeat、transport、线程栈大小。

## 来源

| 来源 | 完整提交 |
| --- | --- |
| Nano 2.5.0 | `9088239cc654ec863869d4adc5433ddec3572fad` |
| TGWebRTC v1.5.18 | `f72f5d3ce04c2be369d88f499c7720d0ed7d788f` |
| TgSysAdpt | `983a086f2bf472f89655f1d62b228ab4d5241635` |

TgSysAdpt 来自 `https://gitlab.tange-ai.com/device/TgSysAdpt.git`，本次使用其构建规则和头文件，未改其源码。Nano 保留上一版 HTTPS 定向修改，没有新增上层源码改动。

```text
基线源码：/home/wty/work/01_TiRTC/worktrees/20260919-p4-feedback-baseline
修复源码：/home/wty/work/01_TiRTC/worktrees/20260919-p4-feedback-stack
证据目录：/home/wty/work/01_TiRTC/issues/lab/20260919-p4-feedback-stack
产物：证据目录/output/tirtc_sdk
备份：证据目录/input/tirtc_sdk
```

## 构建与审计

```bash
cd /home/wty/work/01_TiRTC/issues/lab/20260919-p4-feedback-stack
bash build.sh baseline
bash build.sh fixed
bash test.sh baseline release
bash test.sh fixed release
bash test.sh baseline sanitizer
bash test.sh fixed sanitizer
bash package.sh
```

构建脚本先运行 TgSysAdpt 的 genenv.sh，再清理独立工作树，直接调用 libwebrtc 的 Makefile。参数：`ARCH=esp32p4 RELEASE=y libnamefix=_nosctp`，`SMALL_MEM_NO_DTLS`、`SMALL_MEMORY_WITHOUT_SCTP`、`-O2 -fstack-usage`、`-march=rv32imafc_zicsr_zifencei_xesppie -mabi=ilp32f`。完整参数保存在 `baseline/build.log`、`fixed/build.log`。

实际 IDF 路径为 `/home/wty/tools/esp/esp-idf-5.5.4`；TgSysAdpt 原规则仍带旧 IDF_VER 宏字符串，本次未顺带修改。基线对象与原库逐字节相等，确认该对象可复现。配置头 SHA-256：`10454188804e7c0f8ea82766af1ffc3103a789e940b6950dbb439a384ef86743`。1000 Hz、trace/stats 关闭、StaticSemaphore_t=84 B。原包与 HTTPS 定向重编的 sockets 配置差异沿用原 manifest，不宣称整库重编成16个。

- 全量 lower clean A/B 各 88 个成员，仅 tgtrp_sender.o 变化。
- 基线 sender 对象与原 6265 库对象一致：`67b7f5af28cd6382d75d63693399f6bdf3cd839588fda61ad9f9a10a8abbc9ad`。
- 修复 sender 对象：`dbc0cdcfaa5f1db6289326bd3592a476109c73c629050a8f42273ecdc065e64e`。
- 最终 libTiRTC.a 定向回灌这一个对象，107 个成员中 106 个逐字节保留，包括同名 crc32.o 的两个实例。其他本轮重编对象未混入应用。
- 全局定义符号和未定义依赖符号相同，全部头文件及 CMake 不变，没有新增内存、任务或信号量依赖。
- MRX/TRX/TTX/JTL/KG/KR/IRX/NET、SOCK_TX_FAIL、ICE_TX_FAIL、ICE_RX_DIAG、TGTRP_POLL_DIAG 临时诊断字符串均为零，原反馈日志保留。
- 补丁 SHA-256：`0b45559f5ef26fcc97e74f6ffde7e8eef7baa26565d9ee020a2c7ee0807b55f3`，未提交或推送。

审计见 `manifest/feedback-stack-audit.json`，补丁见 `manifest/feedback-stack.patch`。旧 HTTPS 审计作为前置版本证据保留，不作为本次新 SHA 清单。运行时仍显示 2.5.0/https1，识别本次修复应核对 VERSION 的批次与库 SHA。

## 验证边界

- P4 目标库构建通过；对象架构和符号已审计。
- Release：原有18项+新增1项，基线/修复版各19/19通过；覆盖 ACK/NACK、RTT/RTO、pacing、队列、解析、统计。
- ASan/UBSan：同一组19项在两版均通过，无 sanitizer 报错。
- 新增回归：两个独立 session 并行，总计100轮创建/销毁、1000次 BWE 事件+回调内嵌套 ACK。格式化及事件内容校验通过，两版事件摘要均 ebb3ae82；反馈期间无新分配，销毁后跟踪分配归零。这不是整库无泄漏保证。
- Debug断言模式：两版均有9项触发 tgtrp_frame.c:77 的既有 !seen_sent 断言，失败集合一致；没有删除断言。交付原本采用 Release/NDEBUG，不能宣称 Debug 全过。
- 本次未构建 APP、未烧录。需在新库重链接后，按原100 ms/5%丢包、IPC上行和码率反馈场景复测，才能完成真机闭环。

历史 TURN 查询大栈、连接失败回收、视频卡顿及 APP 内存不足不在本次修复范围。HTTPS 沿用上一版实现与验证边界。
