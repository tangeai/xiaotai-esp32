#pragma once

/** tgwxvoip → 设备：呼叫已接通, 开始媒体交换，无 payload。 */
#define TIRTC_VOIP_CALL_CONNECTED 0x2000

/** 双向：会话结束/拒接/忙线/未接 等统一用本命令.
 *  TiRTC VoIP 云端 → 设备：由 TiRTC VoIP 云端挂断会话。
 *  设备 → TiRTC VoIP 云端：设备挂断会话。
 *  **Payload（必填）**： UTF-8 JSON object，`reason` 见 `tirtc_voip_hangup_reason_t`。
 *
 *  接收端收到后，应尽快结束会话。
 *  异常处理：
 *  - Payload 非 UTF-8 编码 / 非 JSON 对象 / 缺少 `reason` 字段 / reason > TIRTC_VOIP_HANGUP_REASON_MAX: 忽略命令
 *  - reason <= TIRTC_VOIP_HANGUP_REASON_MAX 但不在枚举中: 结束会话
 */
#define TIRTC_VOIP_HANGUP 0x2001

// 以下预留，其他 TiRTC X 应用不要占用
#define TIRTC_VOIP_RESERVED_CMD1 0x2002
#define TIRTC_VOIP_RESERVED_CMD2 0x2003
#define TIRTC_VOIP_RESERVED_CMD3 0x2004
#define TIRTC_VOIP_RESERVED_CMD4 0x2005
#define TIRTC_VOIP_RESERVED_CMD5 0x2006
#define TIRTC_VOIP_RESERVED_CMD6 0x2007
#define TIRTC_VOIP_RESERVED_CMD7 0x2008
#define TIRTC_VOIP_RESERVED_CMD8 0x2009
#define TIRTC_VOIP_RESERVED_CMD9 0x200A
#define TIRTC_VOIP_RESERVED_CMD10 0x200B
#define TIRTC_VOIP_RESERVED_CMD11 0x200C
#define TIRTC_VOIP_RESERVED_CMD12 0x200D
#define TIRTC_VOIP_RESERVED_CMD13 0x200E
#define TIRTC_VOIP_RESERVED_CMD14 0x200F

/** `TIRTC_VOIP_HANGUP` 命令体 JSON 字段 `reason` 的取值。 */
typedef enum {
    TIRTC_VOIP_HANGUP_REASON_UNKNOWN = 0,
    TIRTC_VOIP_HANGUP_REASON_MANUAL = 1,   /* 用户手动挂断/取消 */
    TIRTC_VOIP_HANGUP_REASON_SYSTEM = 2,   /* 被系统电话挂断 */
    TIRTC_VOIP_HANGUP_REASON_APP = 3,      /* 被其他应用挂断 */
    TIRTC_VOIP_HANGUP_REASON_DEVICE = 4,   /* 采集播放设备启动失败 */
    TIRTC_VOIP_HANGUP_REASON_BUSY = 5,     /* 忙线 */
    TIRTC_VOIP_HANGUP_REASON_TIMEOUT = 6,  /* 超时未接听  */
    TIRTC_VOIP_HANGUP_REASON_REJECT = 7,   /* 拒绝通话，未进入通话即挂断 */
    TIRTC_VOIP_HANGUP_REASON_EXCEPTION = 8, /* 会话异常中止  */

    TIRTC_VOIP_HANGUP_REASON_MAX = 20,     /*  保留20个枚举值，后续扩展 */
} tirtc_voip_hangup_reason_t;
