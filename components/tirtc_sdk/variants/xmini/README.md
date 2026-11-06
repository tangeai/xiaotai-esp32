# Xmini Mini SDK / IDF 6

仅由 `xmini/c3-v3` 选择。MZ28 仍使用通用 C3 包，S3/P4 的通用包也未覆盖。
这是准备阶段的隔离验证产物，不是正式 SDK 发布或设备验收结果。

## 身份

- 原包：TiRTC 2.5.0 Mini，20261016 command-publication private test-1。
- 原包 SHA-256：`f95b97601c9ef6d541c1dd039196f272e589fb4139773c09586a77fb08b65ff5`。
- 新包、头文件、下层库和配置哈希：[contract.json](lib/contract.json)。
- IDF 6.0.2，提交 `7101770dc6db2667b3c477cc31365dd1acd6db4e`；GCC 15.2.0 / `esp-15.2.0_20251204`。
- 107 个归档成员均为 ELF32 RISC-V / RVC / soft-float ABI；无重复强全局符号。

Nano 的 `TiRTC/` 连接、命令和 WHIP 源码未改；下层仅修改编译标准选择，媒体、信令、生命周期源码保持原样。
适配修改限于 Make 构建入口、IDF 6 FreeRTOS 头文件、HTTP TLS 的 PSA 初始化和 legacy crypto 私有对象接入。
legacy crypto 包装源码与原 Mini 一致，使用已验证且带私有符号前缀的 Mbed TLS 3.6.5 对象，避免与 IDF 6 的库混用上下文。

## 保留的约束

- `TIRTC_PENDING_CLOSE_CONTRACT=1`，保留取消未发布连接及退休期间忙状态语义。
- 连接通知返回之后按 FIFO 发布命令，等待/在途节点的申请字节预算为 16384，保持原溢出失败语义。
- 低内存结构、延迟分配、完整流键空间、每流 64 帧历史及诊断 v2 保持。
- Xmini TLS 动态缓冲开启、TLS 1.3 关闭、对端证书保留关闭；新库与消费者逐项核对完整 crypto 配置。
- 头文件、库哈希、工具链、FreeRTOS 对象尺寸均在构建时检查；不能静默回退到通用 C3 包。

## 本轮检查

- 交叉重编与 ABI 探针通过；全部导出函数强制保留的 SDK 链接检查通过。
- 实际 Nano 源码的 pending lifecycle、WHIP offer ownership、命令发布顺序与预算三项测试，普通及 ASan/UBSan 共六次运行通过。
- 新仓 Xmini 实际业务、RTC、UI、音频和 USB monitor 的主机测试另见仓库验证文档。

这些结果不证明 C3 实机堆余量、声音、弱网或长稳通过。
原 Mini 已知的 TLS `VERIFY_NONE`、socket 重复 close、下层多分段断言和低堆接收保留/停滞风险仍在，未在本次兼容移植中修复。
不能把“保留原行为”写成“原缺陷已经消失”。正式安全和设备验收继续独立处理。
