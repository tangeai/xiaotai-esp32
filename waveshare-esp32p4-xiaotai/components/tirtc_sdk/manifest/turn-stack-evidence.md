# P4 TURN 查询栈修复证据

日期：2026-09-22。批次：`20260922-p4-turn-stack1`。目标：小钛微雪工程的 TiRTC 2.5.0，保留现有 HTTPS 和 feedback-stack。

## 根因与修改

历史 P4 弱网日志出现 `socket_addr_compare <- darray_binary_search <- turn_agent_allocation_find_by_relay` 栈保护异常。SP 越过线程栈下界，线程申请栈为 8192 B。
2.5.0 对应 TGWebRTC 源码两个查询函数仍在栈上构造完整 `turn_allocation_t`，本次目标编译确认各占 3792 B。只为了比较地址而携带整个分配对象，是此次移除的栈压力来源。

`client/sdk/libice/src/turn-agent.c` 改为借用调用期间有效的地址指针，通过专用比较函数同步二分查找。指针不保存到列表，无堆分配、新任务或全局 scratch；查找顺序和空结果语义不变。不是通过增大线程栈规避问题。

| 目标编译器 .su 测量 | 原版 | 修复版 |
| --- | ---: | ---: |
| turn_agent_allocation_find_by_relay | 3792 B | 16 B |
| turn_agent_allocation_find_by_address | 3792 B | 32 B |
| 对应 key 比较函数 | 0 / 16 B | 0 / 16 B |
| turn_agent_allocation_insert | 48 B | 64 B |

insert 源码未改，编译产物的固定帧增加 16 B；反汇编一并归档。上表只表示自身固定帧，socket_addr_compare 仍为 112 B。保持 rtc_thread 8192 B，不把函数帧节省等同于系统常驻 SRAM 节省。

## 构建口径

Nano `9088239cc654ec863869d4adc5433ddec3572fad`；TGWebRTC `f72f5d3ce04c2be369d88f499c7720d0ed7d788f`；TgSysAdpt `983a086f2bf472f89655f1d62b228ab4d5241635`。
TgSysAdpt 已从公司仓库 fetch 核对，未修改其源码。baseline 与 fixed 为两个独立工作树，fixed 仅带 `manifest/turn-stack.patch`。

```bash
cd ${SDK_WORKSPACE}/issues/lab/20260922-p4-turn-stack
bash build.sh baseline
bash build.sh fixed
python3 test_turn.py baseline
python3 test_turn.py fixed
python3 abi_probe.py
python3 package_audit.py
```

build.sh 先执行 TgSysAdpt/genenv.sh，然后在每个工作树 `client/webrtc/libwebrtc` 执行 clean / make：

```text
ARCH=esp32p4 RELEASE=y libnamefix=_nosctp
CROSS=${IDF_TOOLS_PATH}/tools/riscv32-esp-elf/esp-14.2.0_20260121/riscv32-esp-elf/bin/riscv32-esp-elf-
TGSA_ROOT=${SDK_WORKSPACE}/source/TgSysAdpt
IDFPATH=${IDF_PATH}
SDKCONFIG_PATH=${SDK_WORKSPACE}/issues/lab/20260922-p4-turn-stack/config
CFLAGS=-DSMALL_MEM_NO_DTLS -DSMALL_MEMORY_WITHOUT_SCTP -fstack-usage
```

实际优化为末尾 `-O2`，`-march=rv32imafc_zicsr_zifencei_xesppie -mabi=ilp32f`。完整命令在 baseline/build.log、fixed/build.log。
配置取自当前微雪工程生成的 sdkconfig.h，SHA-256 `393bf02c0d7cce46ceaa45590f9088d717952ceba381057143859b2760f6ceb2`。
相同目标编译参数下 static_assert 验证 1000 Hz、trace/stats off、StaticSemaphore_t=84 B。
TgSysAdpt 规则中的旧 IDF_VER 宏字符串仍为 v5.5.1，但实际头文件来自上述 5.5.4 路径；此次未顺带改动。修复前 turn-agent.o 与已装库逐字节相等，确认复现了该对象。

## 审计结果

- 两份 clean lower 归档各 88 个成员，只有 turn-agent.o 不同。
- 最终从已装 7cf 库定向回灌一个对象；107 个成员中其余 106 个（包括同名 crc32.o）逐字节不变，全部为 ELF32 RISC-V。
- 定义的全局符号不变、未定义符号集合不变；turn-agent.o 少一个 memcpy 引用，无新增外部依赖。
- 原反馈对象 SHA-256 仍为 `dbc0cdcfaa5f1db6289326bd3592a476109c73c629050a8f42273ecdc065e64e`。Nano、HTTPS、线程及其他下层对象均未替换。
- 所有头文件逐字节不变。CMake 只更新批次注释和库 SHA 门禁。
- 临时 MRX/TRX/TTX/JTL/KG/KR/IRX/NET、SOCK_TX_FAIL、ICE_TX_FAIL、ICE_RX_DIAG、TGTRP_POLL_DIAG 字符串均为 0，正常日志保留。
- libTiRTC.a SHA-256：`5571e6918dd07b78e1df9e76362f427ef90d250075b8203a3bae8c2330972792`。
- 补丁 SHA-256：`c8417a0e40b97a2d6a5a1815d15689226c7c60b241bd3a574825707054a2a2c1`。

详见 manifest/turn-stack-audit.json；历史反馈修复证据保存在 manifest/feedback-stack-evidence.md，不能把其旧 SHA 或测试时间视为本次新库结果。

## 测试边界

采用真实查询函数、真实 socket_addr_compare 与 darray_binary_search 源码，配合主机地址/列表夹具。基线和修复版各在 O2、ASan/UBSan 两种模式通过 512000 次 IPv4 查询检查及空表/越界 miss；两版结果相同，无 sanitizer 报错。该实现的比较函数仅支持 IPv4，不扩展宣称 IPv6。

已证实移除完整查询 key 带来的约 3.7 KB 固定栈帧。主机测试不模拟真实 ICE、FreeRTOS 或整库生命周期，不能据此保证整库无泄漏或所有栈路径安全。
本次未构建/烧录 APP，不触碰串口。需要在原弱网和 TURN 中继条件下复测连接、断开、重连及长稳，记录新 ELF 的栈高水位和崩溃日志，才能完成硬件验收。
