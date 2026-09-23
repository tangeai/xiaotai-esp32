# P4 两类问题修复与交付证据

批次：`20260922-p4-fixes2`，2026-09-22。TiRTC 2.5.0 / ESP-IDF 5.5.4 / P4，1000 Hz、trace/stats off、StaticSemaphore_t=84 B，noSCTP。

## 当前库包含什么

| 问题 | 当前库中的处理 | 证据边界 |
| --- | --- | --- |
| 主动连接失败后 hconn=NULL，连接资源未回收 | 本次补齐 Nano 异步销毁与私有完成回调 | 源码、目标编译、主机回归已验证；真实 socket/任务回收待真机 |
| TURN 查询栈过大 | 保留轻量地址 key，3792 B -> 16/32 B | 对象未变，沿用 TURN 目标编译与主机测试 |
| TGTRP 反馈回调栈过大 | 保留 ACK/NACK 私有 noinline 函数，BWE 入口 5312 B -> 288 B | 对象未变，沿用反馈回归；NACK 分支仍为 5232 B |
| HTTPS | 保留上一批次证书 bundle/hostname 校验 | 对象未变，不降级 HTTP |

线程申请栈保持 8192 B，不改变协议、重传、心跳、jitter 或运行时 transport 协商。清除的是临时诊断，不是正常错误日志。

## 本次根因与最小修改

Nano 原 `_on_error()` 在 `connect_cb` 存在时仅回调 `error + NULL` 后返回，没有发起销毁。应用没有拿到句柄，无法执行 `TiRtcDisconnect()`。
返回 NULL 符合失败契约，缺失的是 SDK 的回收动作。

修复只改 `TiRTC/tiRTC.c`：保存结果回调参数；撤销本连接错误、新通道和已打开通道的数据回调；调用已有异步销毁接口；通知原连接回调。销毁完成仅释放 send_lock/hconn，不触发全局 on_disconnected。已建立连接的错误与主动断开流程保持原样。

```text
_on_error（尚未交付 hconn）
  -> 保存 error/cb/user_data，清除 connect_cb
  -> 撤销迟到事件回调
  -> tgtrp_connection_destroy：在连接线程排队
  -> cb(error, NULL, user_data)，不再访问 hconn

连接线程后续循环
  -> 关闭本次 session
  -> detach peer，调用原 peer/DTLS/ICE 清理链
  -> 私有 destroy-done：释放 send_lock/hconn
  -> 下层回收通道、连接对象并异步退出本连接线程
```

不会调用 TiRtcStop/TiRtcUninit，也不会重启设备或把失败返回成成功。没有新增 Nano 堆对象或扩大连接结构；原异步销毁队列仍使用下层已有分配逻辑。

## 为什么能从错误回调发起销毁

以下路径以 TGWebRTC `f72f5d3ce04c2be369d88f499c7720d0ed7d788f` 为准：

- `client/webrtc/api/tgtrp.c:780`：destroy 只标记 closed、保存回调并调用 rt_thread->run_once，不在当前错误回调中直接 free。
- `client/sdk/source/rtc_thread.c:144`：run_once 加入 wait_list；`:257` 处理时先移出队列并解锁，再执行任务，允许回调继续排队。
- `client/webrtc/api/tgtrp.c:763`：关闭 session，调用 destroy_peer_connection_ex，再排队 close-finish。
- `client/webrtc/libwebrtc/peer_connection.c:3864`：peer 的 detach 在同一线程队列中排在 close-finish 前。`:4230` 对传入的线程设置 m_use_global_rtc_thread=1，不走同步等待当前线程退出的分支。
- `client/webrtc/api/tgtrp.c:704`：close-finish 调用 Nano 完成回调，然后 rtc_async_thread_destroy；`rtc_thread.c:793` 只设置停止标志，退出后回收线程对象。

这是已有正常断开链的复用，不新增同步 join、自删 TCB 或跨线程直接 free。以上是源码时序核对；不能替代 P4 的真实线程/socket 计数验证。下层队列分配耗尽场景未纳入本次保证。

## 构建、来源与对象审计

| 来源 | 完整提交 |
| --- | --- |
| Nano | `9088239cc654ec863869d4adc5433ddec3572fad` |
| TGWebRTC | `f72f5d3ce04c2be369d88f499c7720d0ed7d788f` |
| TgSysAdpt | `983a086f2bf472f89655f1d62b228ab4d5241635` |

```bash
cd ${SDK_WORKSPACE}/issues/lab/20260922-p4-failure-cleanup
bash build.sh baseline
bash build.sh fixed
python3 test_cleanup.py baseline
python3 test_cleanup.py fixed
python3 package_audit.py
```

两个独立 Nano 工作树，fixed 只改 tiRTC.c。TgSysAdpt/genenv.sh 生成 Env.mak；TiCommon/TiRTC clean 后编译，只有修复后的 tiRTC.o 回灌已装 5571 库，未把本次 TiCommon 或旧厂商 lower 库打回去。

```text
ARCH=esp32p4 RELEASE=y p2p=kcp nossl=n
CROSS=${IDF_TOOLS_PATH}/tools/riscv32-esp-elf/esp-14.2.0_20260121/riscv32-esp-elf/bin/riscv32-esp-elf-
TGSA_ROOT=${SDK_WORKSPACE}/source/TgSysAdpt
IDFPATH=${IDF_PATH}
SDKCONFIG_PATH=${SDK_WORKSPACE}/issues/lab/20260922-p4-failure-cleanup/config
CFLAGS=-DCONFIG_SSL_SUPPORT=1 -DTIRTC_COMMIT=9088239c-https1 -DTGTRP_COMMIT=f72f5d3c -UIDF_VER -DIDF_VER=\"v5.5.4\" -fstack-usage
```

同一 SOURCE_DATE_EPOCH 固定本轮 A/B 的真实构建时刻 `2026-09-22T15:27:21Z`，不是伪造旧日期。基线 tiRTC.o 与已装对象仅 __DATE__/__TIME__ 两个 BuildTime 字符串不同；仅在比较副本中归一化这两项后，整个对象逐字节相同。svc.o、tiRTC_stat.o 原始字节即一致。

- A/B 的 Nano 归档只有 tiRTC.o 不同；最终 107 个对象中其余 106 个逐字节不变，全部 ELF32 RISC-V。
- TURN 对象：`efdd5298ffb560df9c587962feb5afdf94b302ad1a71e178ecc8a653b1f3886b`。
- 反馈对象：`dbc0cdcfaa5f1db6289326bd3592a476109c73c629050a8f42273ecdc065e64e`。
- 全局定义符号和未定义依赖均不变，全部头文件不变；目标反汇编确认 _on_error 现在包含 destroy 调用及私有完成回调地址。
- _on_error 自身固定帧 16 B -> 32 B；线程分配栈不变。
- 已知临时 MRX/TRX/TTX/JTL/KG/KR/IRX/NET、SOCK_TX_FAIL、ICE_TX_FAIL、ICE_RX_DIAG、TGTRP_POLL_DIAG 均为零。
- 两版存在相同的原有 LOGINSTANCENAME 重定义、通道标签参数和 my_base64 lastpos 编译警告；未新增或屏蔽警告，也未顺带修改其他模块。
- 新库 SHA-256：`6c5c1543f4a099c2f8fdfdff5b50ef728cf181ce09931c2798d34e84d22473b1`。
- 补丁 SHA-256：`94258a80d322f32d4b95072b71686aec1089f596e77c0f4a621c63a4b3480da6`。

配置 SHA 与上一 TURN 包相同：`393bf02c0d7cce46ceaa45590f9088d717952ceba381057143859b2760f6ceb2`。平台、tick/trace/staticsem 和实际 sockets 配置差异说明沿用 VERSION，不宣称整库重编。

## 回归与待验收

主机夹具直接提取实际 Nano 回调，模拟异步 transport，不模拟真实 socket/ICE。原版在“结果回调前必须已发起销毁”断言稳定失败。修复版 O2、ASan/UBSan 都通过：

- 200 次失败，覆盖 14 个底层错误码及尚未开通、开通 1/2 个通道的情况。
- 延迟销毁与防御性立即完成；每个失败只通知一次，锁和连接对象只释放一次。
- 销毁排队期间重复错误、新通道及数据到达不会暴露未交付句柄。
- 结果回调内发起下一次成功连接；已建立连接仍通知正常 on_conn_error/on_disconnected，被动接入仍可正常通知。
- 全局 callbacks 为空时仍能回收；夹具结束时 wrapper 和待销毁对象计数归零。

不能把夹具计数当作真实 P4 socket 数。本次未构建 APP、未烧录、未推送。真机需验证：超时后 socket/任务恢复基线，连续 50 次失败后重连仍成功，TiRtcStart 监听保持可用，以及原弱网、中继、反馈栈触发场景和长稳测试。

审计：manifest/connect-failure-cleanup-audit.json。既有修复：manifest/turn-stack-evidence.md、manifest/feedback-stack-evidence.md。旧证据中的库 SHA 只代表各自历史批次。
