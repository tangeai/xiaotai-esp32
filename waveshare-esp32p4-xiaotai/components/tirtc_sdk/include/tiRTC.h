/** \file tiRTC.h
 * \brief TiRTC 开发接口
 */

/**
 * \mainpage TiRTC nano 参考手册
 *
 * TiRTC nano 是一个基于 P2P 的轻量双向命令数据/媒体流传输框架，底层实现
 * 异步、单线程，非常适合资源有限的嵌入式系统。
 *
 * **主要功能**
 * - 接收或发起连接（可同时）
 * - 多路流媒体传输（双向）
 * - 基于命令字的数据收发，支持异步请求与乱序应答
 * - 可配置的内部日志输出方式
 *
 * \remark 所有数据和状态通过回调推出，<b>不能</b>在回调里执行阻塞或耗时操作。
 *
 * ## 使用入门
 * - \subpage quickguide
 *
 * ## API 参考
 * - \subpage api_ref_by_catagory 
 */

/**
 * \page api_ref_by_catagory API 分类参考
 *
 * | 模块 | 说明 |
 * |---|---|
 * | \ref tirtc_types    "类型与错误码" | 枚举、结构体、回调类型及错误码 |
 * | \ref tirtc_lifecycle "生命周期"   | 初始化、配置、启动与停止 |
 * | \ref tirtc_conn     "连接管理"    | 发起/断开连接、用户数据绑定 |
 * | \ref tirtc_media    "媒体发送"    | 音视频帧与流内消息的发送 |
 * | \ref tirtc_command  "命令通道"    | 基于命令字的双向数据传输 |
 * | \ref tirtc_whip     "WHIP 接口"  | WHIP 服务端/客户端支持 |
 * | \ref tirtc_log      "日志"       | 日志级别、路径、回调配置 |
 *
 * \sa \ref cmdword_guide
 */

/**
 * \defgroup cmdword_guide 命令字最佳实践
 *
 * ## 概念
 *  * <b>命令字</b>(\c cmdw) - 32位无符号整数. TiRtcSendCommand()的`cmdw`参数.
 *  * <b>命令</b>(\c cmd) - 命令字中的的 bit1~bit15 共15位, 取值范围 0~0x7fff. 也可称为命令标识(<tt>cmd id</tt>)
 *
 * ## 关于命令字
 * 在 TiRtcSendCommand() 中，开发者对命令字（cmdw）参数可以在 SDK 保留范围外任意取值。
 * 这在简单的使用场景下没有问题。
 *
 *   - \ref simple_cmdw "简单命令使用示例"
 *
 * 但如果应用存在以下需求:
 * - 希望以非串行的方式处理/响应命令，先处理完的先响应
 * - 要支持双向命令请求，收发两端要处理的命令有共同的集合
 * - 客户端会发送重复的命令, 但应答需要有对应关系
 *
 * SDK提供了一种扩展的机制来支持上述功能.
 *
 * - \ref extended_cmdw "扩展命令使用示例"
 *
 * ## 命令字(\c cmdw)组成
 *
 * | 位域 | 字段 | 说明 |
 * |---|---|---|
 * | bit 0 | 应答标志(RESPONSE_BIT) | 为 1 时表示对己方先前命令的应答，否则为请求 |
 * | bit 1~15 | 命令(cmd) | 开发者使用从 0x4000 ~ 0x7fff间的值 |
 * | bit 16~31 | 命令序号(sn) | 用于匹配请求与应答 |
 *
 * ### 命令字(\c cmdw)和命令(\c cmd)的保留范围
 *
 * | cmdw 范围 | cmd 范围 | 使用者 |
 * |------------------|------------------|----------------|
 * | 0 ~ 0x1fff      | 0 ~ 0xfff       | sdk 内部使用   |
 * | 0x2000 ~ 0xffff | 0x1000 ~ 0x3fff | 探鸽提供的应用 |
 * | ≥ 0x10000       | 0x4000 ~ 0x7fff | 开发者应用     |
 *
 * ## 使用命令序号(\c sn)
 *
 * 如果命令流程简单，可以全程将 \c sn 设为 0。需要支持乱序应答时，遵守以下规则：
 * 1. 为每个新请求赋予不同的命令序号（循环递增）
 * 2. 接收方应答时，复制发送方的 \c sn 和 \c cmd，并将应答位置 1
 * 3. 不需要应答的命令就是单向消息
 * 4. 请求方用序号匹配应答（同一命令可能连续发送多次）
 *
 * ### 正确的发送步骤
 *
 * 1. 调用 atomic_get_cmd_sn() 获取新序号
 * 2. 将带有该序号的应答处理器挂入等待队列
 * 3. 发送命令
 *
 * \remark 必须先挂队列再发送，否则应答可能在处理器挂入前就已到达。
 *
 * ## 辅助宏
 *
 * | 宏 | 说明 |
 * |---|---|
 * | GET_CMD(cmdw) | 从命令字提取 cmd |
 * | GET_SN(cmdw) | 从命令字提取序号 |
 * | MAKE_CMDW(cmd, sn) | 用 cmd 和 sn 生成命令字 |
 * | TiRtcSendReq() | 发送请求（清除应答位） |
 * | TiRtcSendReqWithSn() | 发送请求（自动组装序号） |
 * | TiRtcSendResp() | 发送应答（置位应答位） |
 *
 * \sa TiRtcSendCommand(), atomic_get_cmd_sn()
 */

/* =========================================================================
 * 分组定义
 * ========================================================================= */

/**
 * \defgroup tirtc_types 类型与错误码
 * \brief 枚举、结构体、回调类型定义及错误码常量。
 */

/**
 * \defgroup tirtc_lifecycle 生命周期
 * \brief SDK 初始化、配置项设置、启动与停止。
 *
 * 典型调用顺序：
 * 1. TiRtcInit()
 * 2. TiRtcSetOption() — 设置服务入口等选项（须在 TiRtcStart() 前，部分选项须在 TiRtcInit() 前）
 * 3. TiRtcStart()
 * 4. 使用 SDK（收发媒体、命令等）
 * 5. TiRtcStop()
 * 6. TiRtcUninit()
 */

/**
 * \defgroup tirtc_conn 连接管理
 * \brief 发起/断开 P2P 连接，以及连接上的用户数据绑定与发送缓冲区查询。
 */

/**
 * \defgroup tirtc_media 媒体发送
 * \brief 音视频帧与流内消息的发送接口，线程安全，支持多线程并发调用。
 */

/**
 * \defgroup tirtc_command 命令通道
 * \brief 基于命令字的双向数据传输，支持异步请求与乱序应答。
 * \sa \ref cmdword_guide
 */

/**
 * \defgroup tirtc_whip WHIP 接口
 * \brief WHIP 协议的服务端接入（TiRtcWhipAccept）与客户端发起（TiRtcWhipConnect）。
 */

/**
 * \defgroup tirtc_log 日志
 * \brief SDK 日志的级别、文件路径与回调输出配置。
 */

/* =========================================================================
 * 头文件主体
 * ========================================================================= */

#ifndef __tiRTC_h__
#define __tiRTC_h__

#include "basedef.h"

#define TiRTC_EXPORT TG_PUBLIC

#ifdef __cplusplus
extern "C" {
#endif

#define TIRTC_VERSION_MAJOR     2
#define TIRTC_VERSION_MINOR     3
#define TIRTC_VERSION_PATCH     0
/* -------------------------------------------------------------------------
 * 错误码
 * ------------------------------------------------------------------------- */

/**
 * \addtogroup tirtc_types
 * @{
 */

/** \name 错误码
 * @{
 */
#define TIRTC_E_NOT_INITIALIZED     -40001  ///< 未初始化，需先调用 TiRtcInit()
#define TIRTC_E_INVALID_HANDLE      -40002  ///< 无效连接句柄
#define TIRTC_E_INVALID_PARAMETER   -40003  ///< 无效参数
#define TIRTC_E_INVALID_LICENSE     -40004  ///< 无效设备 license
#define TIRTC_E_TIMEOUTED           -40005  ///< 操作超时
#define TIRTC_E_BUSY                -40006  ///< 网络忙（发送缓冲区满等）
#define TIRTC_E_CONN_TIMEOUTCLOSE   -40007  ///< 心跳超时关闭
#define TIRTC_E_CONN_REMOTECLOSE    -40008  ///< 远端主动关闭
#define TIRTC_E_CONN_OTHER_ERROR    -40009  ///< 其它连接错误
#define TIRTC_E_LACK_OF_RESOURCE    -40010  ///< 资源不足（含内存）
#define TIRTC_E_CACHE_EXPIRED       -40011  ///< 连接参数缓存过期，需重新传入有效 token
#define TIRTC_E_SERVER_ERROR        -40012  ///< 服务器端错误
#define TIRTC_E_INTERNAL_ERROR      -40013  ///< SDK 内部错误
#define TIRTC_E_NO_SECRET_KEY       -40014  ///< 未设置 secret key
#define TIRTC_E_UNEXPECTED_RESPONSE -40015  ///< 服务器响应格式非预期

/** \name HTTP层错误码
 * @{
 */
#define TIRTC_E_HTTP_TIMEOUT        -2    ///< 连接超时 (CONN_E_TIMEOUT)
#define TIRTC_E_HTTP_RESET          -3    ///< 连接被复位 (CONN_E_RESET)
#define TIRTC_E_HTTP_PEER_CLOSED    -4    ///< 对端关闭连接 (CONN_E_PEER_CLOSED)
#define TIRTC_E_HTTP_RESOLVE        -5    ///< 域名解析失败 (CONN_E_RESOLVE)
#define TIRTC_E_HTTP_CONNECT        -6    ///< 连接失败 (CONN_E_CONNECT)
#define TIRTC_E_HTTP_SSL            -7    ///< SSL握手失败 (CONN_E_SSL)
#define TIRTC_E_HTTP_GENERIC        -8    ///< 通用连接错误 (CONN_E_GENERIC)
#define TIRTC_E_HTTP_WS_HANDSHAKE   -9    ///< WebSocket握手失败 (CONN_E_WS_HANDSHAKE)
#define TIRTC_E_HTTP_INVALID_HANDLE -10   ///< 无效HTTP句柄 (CONN_E_INVALID_HANDLE)
#define TIRTC_E_HTTP_SERVER_DOWN    -11   ///< 服务器不可用 (CONN_E_SERVER_DOWN)
#define TIRTC_E_HTTP_INVALID_PROTO  -12   ///< 无效协议 (CONN_E_INVALID_PROTOCOL)
#define TIRTC_E_HTTP_SOCKET         -13   ///< Socket错误 (CONN_E_SOCKET)
#define TIRTC_E_HTTP_NOMEM          -14   ///< 内存不足 (CONN_E_NOMEM)
#define TIRTC_E_HTTP_HEADER         -101  ///< HTTP头解析错误 (EHTTP_HEADER)
#define TIRTC_E_HTTP_URL            -102  ///< URL格式错误 (EHTTP_URL)
/** @} */

/** @} */

/* -------------------------------------------------------------------------
 * 基础类型
 * ------------------------------------------------------------------------- */

struct _tirtc_conn;
/** 表示一个点对点连接的不透明句柄。
 *
 * 由 \ref TIRTCCALLBACKS::on_conn_accepted 或 \ref TIRTCCONNECTCALLBACK 传出。
 * 生命周期到 \ref TIRTCCALLBACKS::on_disconnected 回调触发后结束，SDK 自动释放。
 */
typedef struct _tirtc_conn *tirtc_conn_t;

/** 当前网络连接方式（缺省路由），用于 \ref TIRTC_OPT_NETWORK_TYPE 选项。 */
typedef enum {
    TIRTC_NETCONN_WIFI = 0,  ///< Wi-Fi（默认）
    TIRTC_NETCONN_4G         ///< 运营商数据网络（4G）
} TIRTCNETCONN;

/** 可配置项，通过 TiRtcSetOption() 设置。
 *
 * 如无特别说明，需在 TiRtcStart() 前设置。
 */
typedef enum TIRTCOPTION {

    /** 服务入口地址（`const char *`）。不设置则使用探鸽默认入口。
     *  @note SDK可能被配置为不支持https, 此时传https地址会返回 TIRTC_E_INVALID_PARAMETER.
     */
    TIRTC_OPT_SERVICE_ENDPOINT = 1,

    /** 设备 secret key（`const char *`）。
     *  可替代在 license 字符串中明文传入 secret_key。
     *  \warning 任何时候不要在日志或代码中明文暴露 secret_key。
     */
    TIRTC_OPT_DEVICE_SECRET_KEY,

    /** 最大连接数（`int *`）。主要用于受限设备上的资源规划。 */
    TIRTC_OPT_MAX_CONNECTIONS,

    /** 联网方式（`int *`，值为 \ref TIRTCNETCONN "TIRTCNETCONN"）。默认 \ref TIRTC_NETCONN_WIFI "TIRTC_NETCONN_WIFI"。 */
    TIRTC_OPT_NETWORK_TYPE,

    /** 4G SIM 卡 ICCID（`const char *`）。联网方式为 \ref TIRTC_NETCONN_4G 时必填。 */
    TIRTC_OPT_ICCID,

    /** 是否支持休眠唤醒（`int *`，0 或 1）。默认 0。 */
    TIRTC_OPT_WAKEUP,

    /** 是否受限网络（`int *`，0 或 1）。默认 0。
     *  受限网络这里指运营商定向物联网卡
     */
    TIRTC_OPT_RESTRICTED_NETWORK,

    /** 发送缓冲区大小（`uint32_t *`，字节）。
     *  \note 须在 TiRtcInit() **之前**设置才能生效。
     *  默认：普通 512 KB；`CONFIG_SMALL_MEMORY` 下 100 KB。
     */
    TIRTC_OPT_MAX_SEND_BUFFER,

    /** 是否开启连接参数缓存（`int *`，0 关闭 / 1 开启）。默认 1。
     *
     * 开启后，TiRtcConnect() 成功时会将服务器返回的连接参数按 remote_id 缓存，
     * TTL 来自服务器响应。再次连接同一设备时 token 可传 NULL 直接复用缓存；
     * 缓存未命中或已过期时返回 \ref TIRTC_E_CACHE_EXPIRED "TIRTC_E_CACHE_EXPIRED"。
     */
    TIRTC_OPT_CONNECT_CACHE,

    /** App id. App必传, 设备可选. */
    TIRTC_OPT_APP_ID,

    /** 开发者自己维护的设备标识, 可以取MAC/ICCID/IMEI/芯片标识/..., 唯一作用是与设备 uuid 一起用来防止生产重号. */
    TIRTC_OPT_CLIENT_ID,

    /** tgtrp轮询超时时间(ms). 取值 1~50. 一般为1, 加大可减少系统负载，但会增加响应时间。慎用!
     * 返回: 实际生效的值
     */
    TIRTC_OPT_TGTRP_POLL_TIMEOUT

    /** 是否在连接关闭时打印统计日志 （`int *`, 0 不打印 / 1 打印). 默认 0. */
    //TIRTC_OPT_PRINT_CONN_STAT

} TIRTCOPTION;

/** 媒体类型，填入 \ref TIRTCFRAMEINFO::media "TIRTCFRAMEINFO::media"。 */
typedef enum TIRTCMEDIA {
    TIRTC_MEDIA_MESSAGE = 0,             ///< 在媒体流内插入的消息帧

    TIRTC_AUDIO_MIN = 1,
    TIRTC_AUDIO_PCM  = TIRTC_AUDIO_MIN, ///< PCM 原始音频
    TIRTC_AUDIO_ALAW = 2,               ///< G.711 A-law
    TIRTC_AUDIO_AAC  = 3,               ///< AAC 压缩音频
    TIRTC_AUDIO_OPUS = 4,               ///< Opus 压缩音频
    TIRTC_AUDIO_AMR  = 5,               ///< AMR. 根据音频采样参数选择 wb/nb
    TIRTC_AUDIO_MAX  = 64,              ///< 1\~64 预留给音频类型

    TIRTC_VIDEO_MIN  = 65,                       ///< 视频类型起始值
    TIRTC_VIDEO_JPEG = TIRTC_VIDEO_MIN,          ///< JPEG 图像帧
    TIRTC_VIDEO_H264 = 66,                       ///< H.264 视频帧
    TIRTC_VIDEO_H265 = 67,                       ///< H.265 视频帧
    TIRTC_VIDEO_MAX  = 127
} TIRTCMEDIA;

/** 判断 media 值是否属于音频范围 [1, 64]。 */
#define TIRTC_IS_AUDIO(mt) ((mt) >= TIRTC_AUDIO_MIN && (mt) <= TIRTC_AUDIO_MAX)
/** 判断 media 值是否属于视频范围 [65, 127]。 */
#define TIRTC_IS_VIDEO(mt) ((mt) >= TIRTC_VIDEO_MIN && (mt) <= TIRTC_VIDEO_MAX)

/** 音频采样规格，填入音频帧的 \ref TIRTCFRAMEINFO::flags "flags"。 */
typedef enum {
    TIRTC_AUDIOSAMPLE_8K16B1C  = 0, ///< 8 kHz，16 bit，单声道
    TIRTC_AUDIOSAMPLE_16K16B1C,     ///< 16 kHz，16 bit，单声道
    TIRTC_AUDIOSAMPLE_8K16B2C,      ///< 8 kHz，16 bit，双声道
    TIRTC_AUDIOSAMPLE_16K16B2C      ///< 16 kHz，16 bit，双声道
} TIRTCAUDIOSAMPLE;

__BEGIN_PACKED__

/** 媒体帧头。
 *
 * 紧随其后的是 payload 数据，长度由 \ref TIRTCFRAMEINFO::length 指定。
 */
typedef struct TIRTCFRAMEINFO {
    uint8_t  stream_id; ///< 流标识，取值 0\~15，全局唯一（音频和视频不能共用同一个 stream_id）
    uint8_t  media;     ///< 媒体类型，取值见 \ref TIRTCMEDIA
    uint8_t  flags;     ///< 视频：最低位为关键帧标志（\ref TIRTC_FRAME_FLAG_KEY_FRAME "TIRTC_FRAME_FLAG_KEY_FRAME"）；音频：取值见 \ref TIRTCAUDIOSAMPLE
    uint8_t  reserved;  ///< 预留扩展字段，当前填 0
    uint32_t ts;        ///< 主机序时间戳，精度 ms
    uint32_t length;    ///< payload 字节长度，不含帧头自身
} __PACKED__ TIRTCFRAMEINFO;

__END_PACKED__

/** 视频关键帧标志，填入 \ref TIRTCFRAMEINFO::flags 的 bit0。 */
#define TIRTC_FRAME_FLAG_KEY_FRAME 0x01
/** 判断 flags 是否为关键帧。 */
#define TIRTC_IS_KEY_FRAME(flags) ((flags) & TIRTC_FRAME_FLAG_KEY_FRAME)

/** SDK 内部系统事件，通过 \ref TIRTCCALLBACKS::on_event 回调通知。 */
typedef enum {
    TIRTC_EVENT_SYS_STARTED,       ///< SDK 成功启动
    TIRTC_EVENT_SYS_STOPPED,       ///< SDK 已停止
    TIRTC_EVENT_ACCESS_HIJACKING,  ///< HTTP 请求被重定向（可能遭受中间人攻击）
} TIRTCSYSEVENT;

/** SDK 回调函数集合，传入 TiRtcStart()。
 *
 * \warning 结构体指针不能指向临时变量，其生命周期须覆盖整个 SDK 运行期间。
 * \warning 所有回调均在 SDK 内部线程中调用，禁止在回调中执行阻塞或耗时操作。
 */
typedef struct TIRTCCALLBACKS {
    /** SDK 内部事件回调。
     *  \param event 事件类型，取值见 \ref TIRTCSYSEVENT
     *  \param data  事件附加数据（可为 NULL）
     *  \param len   附加数据长度
     */
    void (*on_event)(int event, const void *data, int len);

    /** 设备端收到新的入站连接。
     *  \param hconn 新连接句柄，可立即用于发送媒体或命令
     */
    void (*on_conn_accepted)(tirtc_conn_t hconn);

    /** 连接上出现错误。
     *  \param hconn  出错的连接句柄
     *  \param error  错误码（\ref TIRTC_E_CONN_TIMEOUTCLOSE 等）
     *  \note
     *    - 接收或主动发起的连接都可能收到此回调。
     *    - 出现此回调后，在该连接上的收发操作不再有效。
     *    - 应用层需要随后调用 TiRtcDisconnect() 释放资源。
     */
    void (*on_conn_error)(tirtc_conn_t hconn, int error);

    /** TiRtcDisconnect() 内部操作完成通知。
     *  \param hconn 连接句柄，回调返回后由 SDK 自动释放
     *  \note 通常不需要此回调。若需与 SDK 内部释放操作同步（例如在此之后才能
     *        安全销毁关联资源），才需要实现它。可在回调中调用 TiRtcConnGetUserData()。
     */
    void (*on_disconnected)(tirtc_conn_t hconn);

    /** 收到对端音频帧。
     *  \param hconn 连接句柄
     *  \param pFi   帧信息（媒体类型、时间戳、长度等）
     *  \param data  帧 payload 数据指针（回调返回后不再有效）
     */
    void (*on_audio)(tirtc_conn_t hconn, const TIRTCFRAMEINFO *pFi, void *data);

    /** 收到对端视频帧。
     *  \param hconn 连接句柄
     *  \param pFi   帧信息
     *  \param data  帧 payload 数据指针（回调返回后不再有效）
     */
    void (*on_video)(tirtc_conn_t hconn, const TIRTCFRAMEINFO *pFi, void *data);

    /** 收到对端流内消息（media == \ref TIRTC_MEDIA_MESSAGE "TIRTC_MEDIA_MESSAGE"）。
     *  \param hconn 连接句柄
     *  \param pFi   帧信息
     *  \param data  消息数据指针（回调返回后不再有效）
     */
    void (*on_message)(tirtc_conn_t hconn, const TIRTCFRAMEINFO *pFi, void *data);

    /** 收到对端命令通道数据。
     *  \param hconn 连接句柄
     *  \param cmdw  命令字，格式见 \ref cmdword_guide
     *  \param data  命令参数数据指针（回调返回后不再有效）
     *  \param len   参数长度（字节）
     */
    void (*on_command)(tirtc_conn_t hconn, uint32_t cmdw,
                    const void *data, uint32_t len);

    /** 对端请求在指定流上立即发送关键帧。
     *  \param hconn     连接句柄
     *  \param stream_id 目标流 ID
     */
    void (*on_request_key_frame)(tirtc_conn_t hconn, uint8_t stream_id);

    /** 对端请求开始发送视频。
     *  \param hconn     连接句柄
     *  \param stream_id 目标流 ID
     *  \return 0 表示接受；非 0 表示拒绝
     */
    int (*on_subscribe_video)(tirtc_conn_t hconn, uint8_t stream_id);

    /** 对端请求停止发送视频。
     *  \param hconn     连接句柄
     *  \param stream_id 目标流 ID
     */
    void (*on_unsubscribe_video)(tirtc_conn_t hconn, uint8_t stream_id);

    /** 对端请求开始发送音频。
     *  \param hconn     连接句柄
     *  \param stream_id 目标流 ID
     *  \return 0 表示接受；非 0 表示拒绝
     */
    int (*on_subscribe_audio)(tirtc_conn_t hconn, uint8_t stream_id);

    /** 对端请求停止发送音频。
     *  \param hconn     连接句柄
     *  \param stream_id 目标流 ID
     */
    void (*on_unsubscribe_audio)(tirtc_conn_t hconn, uint8_t stream_id);

    /** 回调：SDK 请求调整指定视频流的目标码率。
     *
     * \param hconn              连接句柄
     * \param stream_id          视频流 ID
     * \param target_bitrate_bps SDK 建议应用到编码器的绝对目标码率，单位 bps
     *
     * \note  回调运行在 SDK 内部线程，应用应快速返回；如需重配编码器，建议投递到编码线程处理。
     *    - 先调用TiRtcConnSetVideoBitrateParams()设置
     */
    void (*on_update_bitrate)(tirtc_conn_t hconn,
                                        uint8_t stream_id,
                                        uint32_t target_bitrate_bps);
} TIRTCCALLBACKS;

/** TiRtcConnect() 的结果回调。
 *
 * \param error     0 表示连接成功，否则为错误码
 * \param hconn     连接成功时为有效连接句柄，失败时为 NULL
 * \param user_data TiRtcConnect() 调用时传入的用户上下文指针
 */
typedef void (*TIRTCCONNECTCALLBACK)(int error, tirtc_conn_t hconn, void *user_data);

/** 日志输出回调，通过 TiRtcLogSetCallback() 设置。
 *
 * \param log    日志内容字符串（非 NUL 结尾，以 length 为准）
 * \param length 日志内容字节数
 */
typedef void (*TIRTCLOGCALLBACK)(const char *log, uint32_t length);

/** @} */ /* tirtc_types */

/* =========================================================================
 * 生命周期
 * ========================================================================= */

/**
 * \addtogroup tirtc_lifecycle
 * @{
 */

/** 返回 SDK 版本字符串（静态存储，无需释放）。 */
TiRTC_EXPORT const char *TiRtcGetVersion(void);

/** 返回json格式的版本描述字符串（静态存储，无需释放）。*/
TiRTC_EXPORT const char *TiRtcGetBuildInfo(void);

/** 将错误码转换为可读字符串。
 *
 * \param error TiRTC 错误码（TIRTC_E_* 或 0）
 * \return 静态字符串，格式 `"TIRTC_E_XXX (说明)"`；0 返回 `"OK"`；未知码返回 `"未知错误码"`
 */
TiRTC_EXPORT const char *TiRtcGetErrorStr(int error);

/** 初始化 SDK 运行时。
 *
 * 必须在 TiRtcStart() 之前调用。\ref TIRTC_OPT_MAX_SEND_BUFFER 须在本函数之前设置才能生效。
 *
 * \return 0 成功，否则为错误码
 */
TiRTC_EXPORT int TiRtcInit(void);

/** 释放 SDK 运行时资源。通常在 TiRtcStop() 完成后调用。 */
TiRTC_EXPORT void TiRtcUninit(void);

/** 设置全局配置项。
 *
 * \param opt  配置项名称，取值见 \ref TIRTCOPTION
 * \param data 配置数据指针，类型与具体配置项有关
 * \param len  data 指向的数据长度（字节）
 * \return 0 成功；\ref TIRTC_E_INVALID_PARAMETER 表示 opt 或 data 非法
 * \note 配置调用时机与具体选项有关，详见 \ref TIRTCOPTION 各项说明。
 */
TiRTC_EXPORT int TiRtcSetOption(TIRTCOPTION opt, const void *data, uint32_t len);

/** 启动 SDK。
 *
 * \param deviceId
 *      - \c NULL - 仅启动客户端功能(不支持接收连接)
 *      - \c !NULL - 设备id. 需提前通过TiRtcSetOption(\ref TIRTC_OPT_DEVICE_SECRET_KEY, ...) 设置 secret_key
 * \param cbs 回调结构指针，不能指向临时变量，生命周期须覆盖整个 SDK 运行期
 * \return 0 表示参数通过初步检查；实际启动结果通过 cbs->on_event(\ref TIRTC_EVENT_SYS_STARTED) 通知
 * \warning 任何时候不要在日志或代码中明文暴露 secret_key。
 */
TiRTC_EXPORT int TiRtcStart(const char *deviceId, const TIRTCCALLBACKS *cbs);

/** 停止 SDK。
 *
 * 完成后通过 cbs->on_event(\ref TIRTC_EVENT_SYS_STOPPED) 通知。
 * \return 0 或错误码
 */
TiRTC_EXPORT int TiRtcStop(void);

/** @} */ /* tirtc_lifecycle */

/* =========================================================================
 * 连接管理
 * ========================================================================= */

/**
 * \addtogroup tirtc_conn
 * @{
 */

/** 发起 P2P 连接。
 *
 * \param remote_id 目标设备标识（license 中的 uuid）
 * \param token     (开发者自己的)平台颁发的连接授权凭证（Bearer token），用于从探鸽获取设备的连接参数。 \n
 *                  已成功连接过的 remote_id，再次连接时可传 NULL 复用缓存的连接参数。
 * \param cb        连接结果回调，不能为 NULL
 * \param user_data 透传给回调的用户上下文指针
 * \return 0 表示参数通过初步检查，连接过程异步进行，最终结果通过 cb 通知；
 *         \ref TIRTC_E_CACHE_EXPIRED 表示缓存已过期，需重新获取并传入有效 token
 * \note token 是一次性的，不可重复使用；但 SDK 会缓存服务器返回的连接参数（TTL
 *       由服务器决定），在有效期内可令 token 传 NULL 进行重连, 直到返回 TIRTC_E_CACHE_EXPIRED。
 */
TiRTC_EXPORT int TiRtcConnect(const char *remote_id,
                              const char *token,
                              TIRTCCONNECTCALLBACK cb,
                              void *user_data);

/** 主动断开连接。
 *
 * \param hconn 连接句柄
 * \return 0 成功；\ref TIRTC_E_INVALID_HANDLE 表示句柄无效
 * \note 本操作是异步的，不会阻塞。\n
 *       普通应用不必依赖 on_disconnected() 回调：本调用返回后即认为 hconn
 *       不再有效，不应再对其执行任何操作。\n
 *       若需与 SDK 内部释放操作同步，可在 on_disconnected() 回调中处理。
 */
TiRTC_EXPORT int TiRtcDisconnect(tirtc_conn_t hconn);

/** 返回连接当前发送缓冲区已使用字节数。
 *
 * 可与 \ref TIRTC_OPT_MAX_SEND_BUFFER 设置的上限对比，判断是否需要丢帧或限流。
 *
 * \param hconn 连接句柄
 * \return 已使用字节数；hconn 无效时返回 0
 */
TiRTC_EXPORT size_t TiRtcGetSendBufferUsed(tirtc_conn_t hconn);

/** 将用户数据指针关联到连接。
 *
 * \param hconn     连接句柄
 * \param user_data 用户数据指针，可通过 TiRtcConnGetUserData() 取回
 * \retval 0 成功
 * \retval TIRTC_E_INVALID_HANDLE hconn 无效
 */
TiRTC_EXPORT int TiRtcConnSetUserData(tirtc_conn_t hconn, void *user_data);

/** 取回由 TiRtcConnSetUserData() 关联的用户数据指针。
 *
 * \param hconn 连接句柄
 * \return 用户数据指针；hconn 无效时返回 NULL
 */
TiRTC_EXPORT void *TiRtcConnGetUserData(tirtc_conn_t hconn);

/** 设置指定视频流的码率参数（用于底层动态计算）.
 * \param hconn      连接句柄。
 * \param stream_id  视频流 ID
 * \param min_bps    最小目标码率，单位 bps。
 * \param max_bps    最大目标码率，单位 bps。
 * \param start_bps  当前/初始目标码率，单位 bps。
 * \return 0 或 错误码
 */
TiRTC_EXPORT int TiRtcConnSetVideoBitrateParams( tirtc_conn_t hconn,
        uint8_t stream_id, uint32_t min_bps, uint32_t max_bps, uint32_t start_bps);
/** @} */ /* tirtc_conn */

/* =========================================================================
 * 媒体发送
 * ========================================================================= */

/**
 * \addtogroup tirtc_media
 * @{
 */

/** 通用的基于帧的视频/音频/消息流发送方法.
 *
 * 线程安全，支持多线程并发调用。
 *
 * \param hconn 连接句柄
 * \param pFi   帧信息指针，不能为 NULL
 * \param frame 帧 payload 数据指针
 *
 * \retval >0 发送字节数
 * \retval TIRTC_E_INVALID_HANDLE  hconn 无效
 * \retval TIRTC_E_INVALID_PARAMETER pFi 为 NULL 或 media 类型非法
 * \retval TIRTC_E_BUSY 发送缓冲区已满，视频帧被丢弃
 *
 * \note
 *   - **视频：** 每路视频流的第一帧必须是关键帧（\ref TIRTC_FRAME_FLAG_KEY_FRAME "TIRTC_FRAME_FLAG_KEY_FRAME"）。
 *     缓冲区满时设置跳帧标志，丢弃后续非关键帧直到下一个关键帧到来。
 *   - **音频：** 直接入队，不设跳帧逻辑。
 *   - 发送操作仅根据 pFi->media 选择传输策略，不检查 payload 内容。
 *   - stream_id 全局唯一，同一连接内音频和视频不能使用相同的 stream_id。
 *   - 可在任意 stream 中传入 media == \ref TIRTC_MEDIA_MESSAGE 的帧，
 *     帧头其余成员由开发者自行解释。
 */
TiRTC_EXPORT int TiRtcSendMessageStream(tirtc_conn_t hconn,
                                const TIRTCFRAMEINFO *pFi,
                                const void *frame);

/** 发送视频帧（类型校验封装）。
 *
 * 内部调用 TiRtcSendMessageStream()。若 pFi->media 不是视频类型，
 * 直接返回 \ref TIRTC_E_INVALID_PARAMETER "TIRTC_E_INVALID_PARAMETER"。
 *
 * \param hconn 连接句柄
 * \param pFi   帧信息指针（media 须为视频类型）
 * \param frame 帧 payload 数据指针
 * \return 同 TiRtcSendMessageStream()
 */
TiRTC_EXPORT int TiRtcSendVideoStream(tirtc_conn_t hconn,
                                const TIRTCFRAMEINFO *pFi,
                                const void *frame);

/** 发送音频帧（类型校验封装）。
 *
 * 内部调用 TiRtcSendMessageStream()。若 pFi->media 不是音频类型，
 * 直接返回 \ref TIRTC_E_INVALID_PARAMETER "TIRTC_E_INVALID_PARAMETER"。
 *
 * \param hconn 连接句柄
 * \param pFi   帧信息指针（media 须为音频类型）
 * \param frame 帧 payload 数据指针
 * \return 同 TiRtcSendMessageStream()
 */
TiRTC_EXPORT int TiRtcSendAudioStream(tirtc_conn_t hconn,
                                const TIRTCFRAMEINFO *pFi,
                                const void *frame);

/** @} */ /* tirtc_media */

/* =========================================================================
 * 命令通道
 * ========================================================================= */

/**
 * \addtogroup tirtc_command
 * @{
 */

/** 在命令通道上发送数据。
 *
 * SDK 在独立的 P2P 通道上为应用提供基于命令字的数据传输。
 * 对端通过 \ref TIRTCCALLBACKS::on_command 回调收到数据。
 *
 * \param hconn  连接句柄
 * \param cmdw   命令字，取值须 ≥ 0x10000；格式见 \ref cmdword_guide
 * \param data   命令参数数据指针（可为 NULL）
 * \param length 命令参数长度（字节）
 * \return >0 发送字节数；<0 错误码
 */
TiRTC_EXPORT int TiRtcSendCommand(tirtc_conn_t hconn,
                                  uint32_t cmdw,
                                  const void *data,
                                  uint32_t length);

/** 原子地返回下一个命令序号并自增（16 位循环）。
 *
 * SDK 内部命令与应用共享同一计数器，返回值的低 16 位为有效序号。
 *
 * \note 正确使用步骤（见 \ref cmdword_guide "命令字最佳实践"）：
 *   1. 调用本函数获取序号
 *   2. 将带有该序号的应答处理器挂入等待队列
 *   3. 发送命令
 */
TiRTC_EXPORT uint32_t atomic_get_cmd_sn(void);

/** 从命令字中提取命令标识（cmd）。\sa \ref cmdword_guide */
#define GET_CMD(cmdw) (((cmdw) >> 1) & 0x7fff)
/** 从命令字中提取命令序号（sn）。\sa \ref cmdword_guide */
#define GET_SN(cmdw) ((cmdw) >> 16)
/** 用 cmd 和 sn 生成命令字。\sa \ref cmdword_guide */
#define MAKE_CMDW(cmd, sn) (((uint32_t)(sn)) << 16 | (((cmd) << 1) & 0xffff))

/** 命令字值小于此阈值时为 SDK 内部保留命令字。 */
#define IS_RESERVED_CMDW(cmdw) (cmdw < 0x2000)

/** 应答标志位（bit0）。为 1 时表示该包是对己方命令的应答。 */
#define RESPONSE_BIT 0x0001

/** 发送请求（清除应答位）。需自行在 cmdw 中组装序号。
 *  \sa \ref cmdword_guide
 */
#define TiRtcSendReq(hconn, cmd, data, length) TiRtcSendCommand(hconn, (cmd)&~RESPONSE_BIT, data, length)

/** 发送请求（自动将 sn 填入高 16 位）。
 *  调用前用 atomic_get_cmd_sn() 获取 sn，并提前将应答处理器排队。
 *  \sa \ref cmdword_guide
 */
#define TiRtcSendReqWithSn(hconn, sn, cmd, data, length) \
    TiRtcSendCommand(hconn, MAKE_CMDW(cmd, sn), data, length)

/** 发送应答（置位应答位）。cmdw 直接来自收到的请求命令字。
 *  \sa \ref cmdword_guide
 */
#define TiRtcSendResp(hconn, cmdw, data, length) TiRtcSendCommand(hconn, (cmdw)|RESPONSE_BIT, data, length)

/** 请求对端在指定流上立即发送关键帧。
 *
 * 触发对端 \ref TIRTCCALLBACKS::on_request_key_frame 回调。
 *
 * \param hconn     连接句柄
 * \param stream_id 目标流 ID
 * \return >= 0 成功；否则为错误码
 */
TiRTC_EXPORT int TiRtcRequestKeyFrame(tirtc_conn_t hconn, uint8_t stream_id);

/** 请求对端开始发送指定视频流。
 *
 * 触发对端 \ref TIRTCCALLBACKS::on_subscribe_video 回调。
 *
 * \param hconn     连接句柄
 * \param stream_id 目标流 ID
 * \return >= 0 成功；否则为错误码
 */
TiRTC_EXPORT int TiRtcSubscribeVideo(tirtc_conn_t hconn, uint8_t stream_id);

/** 请求对端停止发送指定视频流。
 *
 * 触发对端 \ref TIRTCCALLBACKS::on_unsubscribe_video 回调。
 *
 * \param hconn     连接句柄
 * \param stream_id 目标流 ID
 * \return >= 0 成功；否则为错误码
 */
TiRTC_EXPORT int TiRtcUnsubscribeVideo(tirtc_conn_t hconn, uint8_t stream_id);

/** 请求对端开始发送指定音频流。
 *
 * 触发对端 \ref TIRTCCALLBACKS::on_subscribe_audio 回调。
 *
 * \param hconn     连接句柄
 * \param stream_id 目标流 ID
 * \return >= 0 成功；否则为错误码
 */
TiRTC_EXPORT int TiRtcSubscribeAudio(tirtc_conn_t hconn, uint8_t stream_id);

/** 请求对端停止发送指定音频流。
 *
 * 触发对端 \ref TIRTCCALLBACKS::on_unsubscribe_audio 回调。
 *
 * \param hconn     连接句柄
 * \param stream_id 目标流 ID
 * \return >= 0 成功；否则为错误码
 */
TiRTC_EXPORT int TiRtcUnsubscribeAudio(tirtc_conn_t hconn, uint8_t stream_id);

/** @} */ /* tirtc_command */

/* =========================================================================
 * 日志
 * ========================================================================= */

/**
 * \addtogroup tirtc_log
 * @{
 */

/** 配置 SDK 内置文件日志（仅 Linux 平台有效）。
 *
 * \param bOutputToConsole 非 0 时同时将日志输出到 stdout
 * \param path 日志文件路径；建议设在 tmpfs 中以减少 Flash 写入。传 NULL 不输出到文件。
 * \param size 日志文件最大字节数，超出后滚动覆盖
 */
TiRTC_EXPORT void TiRtcLogConfig(int bOutputToConsole, const char *path, uint32_t size);

/** 设置日志详细程度。
 *
 * \param level 日志等级. 1~5 对应 error/warn/ok/info/verbose. 大于10会开启WebRTC底层的日志(很多输出，影响性能)。
 */
TiRTC_EXPORT void TiRtcLogSetLevel(int level);

/** 设置日志输出回调。
 *
 * 设置后 SDK 将所有日志内容通过回调传给应用，不再自行输出到文件或控制台。
 * 通常用于无文件系统的小系统。
 *
 * \param cb 日志回调函数指针；传 NULL 可清除回调，恢复默认输出行为
 */
TiRTC_EXPORT void TiRtcLogSetCallback(TIRTCLOGCALLBACK cb);

/** @} */ /* tirtc_log */

/* =========================================================================
 * WHIP 接口
 * ========================================================================= */

/**
 * \addtogroup tirtc_whip
 * @{
 */

/** WHIP 服务端：answer SDP 就绪回调。
 *
 * \param sdp  answer SDP 字符串
 * \param len  SDP 长度（字节）
 * \param user TiRtcWhipAccept() 传入的用户数据指针
 * \warning 禁止在此回调中阻塞。
 */
typedef void (*TIRTCWHIPSERVERONSDPREADYCB)(const char *sdp, int len, void *user);

/** WHIP 服务端接口：接收 WHIP offer SDP，异步生成 answer SDP。
 *
 * \param offer        offer SDP 数据（来自 HTTP 请求 body）
 * \param offer_len    offer SDP 长度（字节）
 * \param candidate    当服务器不直接连到公网时, 本值为外网地址, 否则为NULL.
 * \param whip_sdp_cb  answer SDP 就绪回调（见 \ref TIRTCWHIPSERVERONSDPREADYCB "TIRTCWHIPSERVERONSDPREADYCB"）。
 *                     在此回调中向 WHIP 客户端返回 HTTP 201（body = answer SDP）。
 * \param cb           DataChannel 全部就绪（连接成功）或出错的 \ref TIRTCCONNECTCALLBACK 回调
 * \param user         透传给 whip_sdp_cb 和 cb 的用户数据指针
 * \return 0 或错误码（TIRTC_E_*）
 *
 * **典型用法：**
 * ```
 * HTTP POST /whip → TiRtcWhipAccept()
 *   → whip_sdp_cb：发送 HTTP 201 + answer SDP
 *   → cb：DataChannel 建立完成通知
 * ```
 */
TiRTC_EXPORT int TiRtcWhipAccept(const char *offer, int offer_len,
        const char *candidate,
        TIRTCWHIPSERVERONSDPREADYCB whip_sdp_cb,
        TIRTCCONNECTCALLBACK cb, void *user);

/** WHIP 客户端接口：通过平台信令向目标设备发起连接。
 *
 * \param service_desc 服务描述符（非设备 id）
 * \param token        (开发者自己的)平台颁发的连接授权凭证（Bearer token）
 * \param cb           连接结果回调（\ref TIRTCCONNECTCALLBACK "TIRTCCONNECTCALLBACK"）
 * \param user_data    透传给 cb 的用户数据指针
 * \return 0 或错误码（TIRTC_E_*）
 * \note 连接过程是异步的，返回 0 仅表示参数校验通过，
 *       HTTP 请求及 ICE 协商均在后台完成。
 */
TiRTC_EXPORT int TiRtcWhipConnect(const char *service_desc,
                                   const char *token,
                                   TIRTCCONNECTCALLBACK cb,
                                   void *user_data);

/** @} */ /* tirtc_whip */

/** \addtogroup tirtc_misc
 * @{
 */

/** TiRtcServiceRequest() 的结果回调。
 *
 * \param body      最终响应 body 字符串（回调返回后不再有效）；error 非 0 时可为 NULL
 * \param user_data TiRtcServiceRequest() 调用时传入的用户上下文指针
 */
typedef void (*TIRTCSERVICEREQUESTCALLBACK)(const char *body,
                                            void *user_data);

/** 请求 TiRTC 服务接口。
 *
 * \param path       接口路径，例如 "/v1/wxvoip/reject"
 * \param json_body 请求 JSON 字符串；传 NULL 时使用 GET，否则使用 POST
 * \param token     作为客户端访问时授权 token；作为设备时传 NULL.
 * \param cb        平台返回的数据(如果有的话)回调.
 * \param user_data 透传给 cb 的用户上下文指针
 * \retval 0       成功。数据(如果有的话)通过回调传给应用
 * \retval <0      错误码 TIRTC_E_*
 * \retval 30X~50X 平台返回的http status code
 * \retval >599    平台返回的错误码, 解释查看平台文档
 */
TiRTC_EXPORT int TiRtcServiceRequest(const char *path,
                                     const char *json_body,
                                     const char *token,
                                     TIRTCSERVICEREQUESTCALLBACK cb,
                                     void *user_data);

/** @} */ /* tirtc_misc */

#ifdef __cplusplus
}
#endif

#endif
