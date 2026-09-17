#ifndef __TGTRP_API_HEADER_H__
#define __TGTRP_API_HEADER_H__
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#define TGTRP_INTERFACE_VERSION "tag.v1.5.18"

#define MAX_TGTRP_ADDR_INFO_SIZE 40

typedef void (*tgtrp_log_cb_t)(const char* fmt, va_list args);

#define TGTRP_LOG_LEVEL_MASK 0x00ff
#define TGTRP_LOG_FLAG_STAT  0x0100

/**
 * @brief 监听器句柄，用于服务端监听连接请求
 */
typedef void* tgtrp_listener;

/**
 * @brief 监听器Access Token失效回调
 * @note 回调运行在信令线程，不能阻塞。
 */
typedef void (*tgtrp_listener_token_invalid_cb_t)(tgtrp_listener listener, void* context);

/**
 * @brief 连接句柄，代表一个P2P连接
 */
typedef void* tgtrp_connection;

/**
 * @brief 通道句柄，代表连接中的一个数据传输通道
 */
typedef void* tgtrp_channel;

/**
 * @brief 呼叫端请求的连接链路模式。
 * @note 传给 tgtrp_connection_call() 的 link_param 使用以下常用值。
 */
typedef enum tgtrp_connection_link_mode {
    /** 允许 UDP/P2P 直连，并保留 TURN 中转回退。推荐作为默认值。 */
    TGTRP_CONNECTION_LINK_MODE_DEFAULT = 0,

    /**
     * 仅允许 UDP/P2P 直连，不使用 TURN 中转。
     * @warning 受 NAT、防火墙或网络策略影响，直连失败时连接将无法建立。
     */
    TGTRP_CONNECTION_LINK_MODE_DIRECT_ONLY = 1,

    /**
     * 仅允许 TURN 中转，禁用 P2P 和内网直连。
     * @warning TURN 服务不可用或不可达时，连接将无法建立。
     */
    TGTRP_CONNECTION_LINK_MODE_RELAY_ONLY = 8
} tgtrp_connection_link_mode_t;

/**
 * @brief 数据发送向量结构体，用于scatter/gather IO
 */
struct tgtrp_channel_vec
{
    char* buf; ///< 数据缓冲区指针
    int  len;  ///< 数据长度
};

/**
 * @brief Channel视频帧类型
 */
enum TGTRP_CHANNEL_VIDEO_FRAME_TYPE
{
    TGTRP_CHANNEL_VIDEO_FRAME_I = 0, ///< I帧/关键帧
    TGTRP_CHANNEL_VIDEO_FRAME_P = 1, ///< P帧
    TGTRP_CHANNEL_VIDEO_FRAME_B = 2, ///< B帧
    TGTRP_CHANNEL_VIDEO_MSG = 5,
};

typedef enum tgtrp_video_send_event_type {
    TGTRP_VIDEO_SEND_EVENT_REDUCE_BITRATE = 1,
    TGTRP_VIDEO_SEND_EVENT_REQUEST_KEYFRAME = 2,
    TGTRP_VIDEO_SEND_EVENT_INCREASE_BITRATE = 3
} tgtrp_video_send_event_type_t;

typedef enum tgtrp_video_send_event_reason {
    TGTRP_VIDEO_SEND_EVENT_REASON_SENDER_BACKLOG = 1,
    TGTRP_VIDEO_SEND_EVENT_REASON_SENDER_BACKLOG_EMERGENCY = 2,
    TGTRP_VIDEO_SEND_EVENT_REASON_SENDER_RECOVERY = 3,
    TGTRP_VIDEO_SEND_EVENT_REASON_RECEIVER_BWE = 4,
    TGTRP_VIDEO_SEND_EVENT_REASON_RECEIVER_BWE_RECOVERY = 5
} tgtrp_video_send_event_reason_t;

/*
 * Video send event handling recommendation:
 * - REDUCE_BITRATE means receiver-side stream BWE observed high loss / delay
 *   overuse, or sender-side backlog reached the emergency high-water guard.
 *   The sender may repeat this event while pressure continues, using cooldown.
 *   The upper layer should set the stream encoder bitrate to target_bitrate_bps
 *   when it is non-zero, and clamp by its own minimum bitrate policy.
 *   SENDER_BACKLOG_EMERGENCY is more severe than SENDER_BACKLOG.
 * - INCREASE_BITRATE means backlog and receiver-side BWE have recovered. The
 *   upper layer should set the stream encoder bitrate to target_bitrate_bps,
 *   then continue increasing slowly step by step to avoid rebuilding backlog
 *   immediately after recovery.
 */
typedef struct tgtrp_video_send_event {
    uint32_t struct_size;
    uint32_t event_seq;

    uint8_t event_type;
    uint8_t reason;
    uint8_t stream_id;
    uint8_t awaiting_keyframe;

    uint32_t rtt_ms;
    uint32_t reduce_threshold_ms;
    uint32_t keyframe_threshold_ms;

    uint32_t pending_head_wait_ms;
    uint32_t backlog_gap_ms;
    uint32_t backlog_span_ms;
    uint32_t pending_frames;
    uint32_t history_frames;
    uint32_t unacked_segments;

    uint32_t dropped_pending_frames;

    /* Optional BWE recommendation. 0 means this event does not adjust bitrate.
       For bitrate events, upper layer should apply this absolute target. */
    uint32_t target_bitrate_bps;
    uint32_t receive_bitrate_bps;
    uint32_t loss_rate_ppm;
    uint8_t bwe_state;
    uint8_t bwe_delay_state;
    uint8_t bwe_app_limited;
    uint8_t reserved_bwe;
} tgtrp_video_send_event_t;

typedef void (*tgtrp_channel_video_send_event_cb_t)(
    tgtrp_connection pc,
    tgtrp_channel c,
    void* context,
    const tgtrp_video_send_event_t* event);

typedef struct tgtrp_connection_network_quality_observation {
    /* 1 秒窗口样本数；0 表示还没有数据、非 TGTRP 或对端反馈已过期。 */
    uint32_t observation_sample_count;
    /* 是否检测到延迟排队拥塞；丢包率请看 loss_rate_ppm。 */
    uint8_t congested;
    uint8_t reserved[3];
    /* 1 秒窗口丢包率，1000000 表示 100%。 */
    uint32_t loss_rate_ppm;
    /* 接收端观测到的 bitrate，单位 bps。 */
    uint32_t observed_bitrate_bps;
} tgtrp_connection_network_quality_observation_t;

/**
 * @brief on_data_ex 回调附带的单包/单帧收包统计。
 *
 * @note 所有时间字段单位均为毫秒；字段不适用于当前传输或当前包时填 0。
 * @note media_type/stream_id/frame_type 已作为 on_data_ex 回调参数单独传出，
 *       不在 stats 中重复保存。
 */
typedef struct tgtrp_channel_data_stats {
    /* 实际承载本次回调数据的传输类型：0=unknown, 1=sctp/dtls, 2=kcp, 3=tgtrp。 */
    uint8_t transport_type;

    /* 保留字段，当前填 0；调用方不要依赖该值表达业务含义。 */
    uint8_t reserved;

    /* TGTRP 发送端背压观测：本帧在 sender pending 队列中的等待时长，最大 65535ms。 */
    uint16_t sender_pending_wait_ms;

    /* 发送端写入 debug header 时观测到的发送缓冲区占用字节数。 */
    uint32_t debug_send_buffer_bytes;

    /* debug header 中发送端记录的 RTT/SRTT。 */
    uint32_t debug_rtt_ms;

    /* 根据 debug header 估算的发送端到接收端单向耗时。 */
    uint32_t debug_oneway_delay_ms;

    /* TGTRP frame id。同一 stream 内递增，用于定位帧、NACK 和重传；非 TGTRP 时为 0。 */
    uint32_t frame_id;

    /* TGTRP 发送端从首个分片发送到最后一次发送/重传之间的时间跨度。 */
    uint32_t send_ts_span_ms;

    /* TGTRP 发送端记录的本帧首次发送时间戳，单位 ms，使用发送端本地 TGTRP 时钟。 */
    uint32_t first_send_ts_ms;

    /* TGTRP 接收端从收到本帧首个分片到帧组装完成之间的时间跨度。 */
    uint32_t receive_ms_span;

    /* TGTRP 从首次收到本帧分片到交付应用回调的耗时；仅 transport_type=3 时有效。 */
    uint32_t delivery_since_first_seen_ms;

    /* TGTRP 本帧交付时采用的 RTT 估计值；优先本端估计，缺失时使用对端通告值。 */
    uint32_t delivery_rtt_ms;

    /* TGTRP 接收端本地 RTT 平滑估计值。 */
    uint32_t local_rtt_ms;

    /* TGTRP 对端在包头/反馈中通告的发送端 RTT 平滑估计值。 */
    uint32_t peer_rtt_ms;

    /* TGTRP 接收端针对本帧发送过的 NACK 次数。 */
    uint32_t nack_send_count;

    /* TGTRP 发送端对本帧执行过的重传分片次数。 */
    uint32_t retransmit_segment_count;

    /* TGTRP 发送端背压观测：本帧出 pending 队列时的队列深度，包含本帧；未排队时为 0。 */
    uint8_t sender_pending_queue_length;

    /* TGTRP 发送端 pacer 队列观测：发送本帧 segment 前 pacer 总队列长度最大值。 */
    uint16_t pacer_queue_count_max;

    /* TGTRP 发送端 pacer 队列观测：本帧 segment 在 pacer 队列中的最大等待时长。 */
    uint16_t pacer_queue_wait_ms_max;

    /* 本端发送方向质量，来自对端 ping 反馈；非 TGTRP 或无反馈时 observation_sample_count=0。 */
    tgtrp_connection_network_quality_observation_t send_quality;

    /* 本端接收方向质量，来自本地 TGTRP 接收统计；非 TGTRP 或无统计时 observation_sample_count=0。 */
    tgtrp_connection_network_quality_observation_t recv_quality;
} tgtrp_channel_data_stats_t;

/**
 * @brief 监听器配置选项枚举
 */
enum TGTRP_LISTENER_OPTION
{
    /**
     * @brief 是否当前listener的所有连接都使用同一个线程
     * @details 默认值为0 (每个连接独立线程)。
     * 取值示例:
     * - 0: 每个连接使用独立线程
     * - 1: 所有连接共享同一个线程
     */
    TGTRP_MULTIPLE_CONNECTION_SHARED_THREAD,

    /**
     * @brief 加密级别
     * @details 取值范围0~7。
     * 取值示例:
     * - 0: 最低加密级别/不加密
     * - 7: 最高加密级别 (算法复杂，CPU代价高)
     */
     TGTRP_CONNECTION_ENCRYPTO_LEVEL,

     /**
      * @brief 是否需要开启唤醒服务
      * @details 默认值为0。
      * 取值示例:
      * - 0: 不开启
      * - 1: 开启唤醒服务
      */
      TGTRP_ENABLE_WAKEUP_PROPERTY,

      /**
       * @brief 是否强制使用域名作为中转服务器地址
       * @details 默认为0。用于定向流量卡等无白名单IP无法连接的场景。
       * 取值示例:
       * - 0: 使用IP或域名 (默认)
       * - 1: 强制服务器返回域名
       */
       TGTRP_TURN_SERVER_ADDR_DOMAIN,
};


/**
 * @brief 连接信息结构体
 */
struct tgtrp_connection_info
{
    char link_mode[MAX_TGTRP_ADDR_INFO_SIZE];       ///< 连接模式，例如host-host host-relay relay-relay等(P2P的连接模式)
    char local_candidate[MAX_TGTRP_ADDR_INFO_SIZE]; ///< 本地候选地址. 示例: "192.168.1.101:60380.host.udplocal"
    char remote_candidate[MAX_TGTRP_ADDR_INFO_SIZE];///< 远端候选地址. 示例: "192.168.1.101:60380.host"
    char app_internal_ip[MAX_TGTRP_ADDR_INFO_SIZE]; ///< app端(呼叫端)外网地址. 示例 "113.106.106.98" 用于网络分析
    char device_internal_ip[MAX_TGTRP_ADDR_INFO_SIZE];///< 设备端(被呼叫端)外网地址. 示例"113.106.106.98" 用于网络分析
};

typedef struct tgtrp_connection_network_quality_stats {
    uint32_t struct_size;
    /* 本端发送方向，来自对端 ping 反馈。 */
    tgtrp_connection_network_quality_observation_t send;
    /* 本端接收方向，来自本地接收统计。 */
    tgtrp_connection_network_quality_observation_t recv;
} tgtrp_connection_network_quality_stats_t;

#define TGTRP_CONNECTION_TIME_STATS_WINDOW_SIZE 10

typedef struct tgtrp_connection_time_metric_stats {
    /* 当前窗口内有效样本数量，最大为 TGTRP_CONNECTION_TIME_STATS_WINDOW_SIZE。 */
    uint32_t sample_count;

    /* 当前窗口中最新一次采样值，单位 ms；无样本时为 0。 */
    uint32_t latest_ms;

    /* 当前窗口内最小采样值，单位 ms；无样本时为 0。 */
    uint32_t min_ms;

    /* 当前窗口内最大采样值，单位 ms；无样本时为 0。 */
    uint32_t max_ms;

    /* 当前窗口内平均采样值，单位 ms，整数除法向下取整；无样本时为 0。 */
    uint32_t avg_ms;
} tgtrp_connection_time_metric_stats_t;

typedef struct tgtrp_connection_time_stats_group {
    /* debug header 估算的发送端到接收端单向耗时，对应 debug_oneway_delay_ms。 */
    tgtrp_connection_time_metric_stats_t debug_oneway_delay;

    /* debug header 中发送端记录的 RTT/SRTT，对应 debug_rtt_ms。 */
    tgtrp_connection_time_metric_stats_t debug_rtt;
} tgtrp_connection_time_stats_group_t;

typedef struct tgtrp_connection_time_stats {
    /* 全连接总计：debug 样本覆盖所有传输。 */
    tgtrp_connection_time_stats_group_t total;

    /* TGTRP audio Kind 统计，media_type=1。 */
    tgtrp_connection_time_stats_group_t audio;

    /* TGTRP video Kind 统计，media_type=2。 */
    tgtrp_connection_time_stats_group_t video;

    /* TGTRP data Kind 统计，media_type=3。 */
    tgtrp_connection_time_stats_group_t data;
} tgtrp_connection_time_stats_t;

/**
 * @brief 初始化TIRTC库
 * @note 异步调用: 非阻塞
 *
 * @param max_snd_buff_size 最大发送缓冲区大小 (字节)。示例: 1024 * 1024 (1MB)
 * @note TGTRP模式下音频使用高于video/data的拒绝水位，达到后同样停止入队。
 */
void tgtrp_init(size_t max_snd_buff_size);

/**
 * @brief 反初始化TIRTC库，释放全局资源
 * @note 异步调用: 否 (非阻塞)
 */
void tgtrp_uninit(void);

/**
 * @brief 获取SIGNAL_INTERFACE_VERSION
 * @details 返回的指针指向静态常量，不需要释放，可在 tgtrp_init 前调用。
 *
 * @return SIGNAL_INTERFACE_VERSION 字符串
 */
const char* tgtrp_get_version(void);

/**
 * @brief 设置全局网络socket轮询超时时间
 * @details 对当前运行中及后续创建的监听、连接和共享线程生效。
 *          普通构建下，传入值会被限制到1~50毫秒；Google WebRTC构建仍使用0毫秒非阻塞轮询。
 * @note 同步调用。当前正在进行的轮询仍使用旧值，下一轮开始使用新值。
 *
 * @param timeout_ms 轮询超时时间（毫秒）
 * @return int 实际保存的超时时间，范围为1~50毫秒
 */
int tgtrp_set_net_sock_poll_timeout(int timeout_ms);

/**
 * @brief 创建一个新的监听器对象
 * @note 异步调用: 非阻塞
 *
 * @param max_conn_num 允许的最大并发连接数。示例: 10
 * @return tgtrp_listener 成功返回监听器句柄，失败返回NULL
 */
tgtrp_listener tgtrp_listener_new(int max_conn_num);

/**
 * @brief 设置监听器的配置选项
 * @note 异步调用: 非阻塞
 *
 * @param listener 监听器句柄。示例: 由 tgtrp_listener_new 返回的指针
 * @param opt 选项枚举值，参见 enum TGTRP_LISTENER_OPTION。示例: TGTRP_CONNECTION_ENCRYPTO_LEVEL
 * @param value 选项对应的值。示例: 1
 */
void tgtrp_listener_set_opt(tgtrp_listener listener, enum TGTRP_LISTENER_OPTION opt, int value);

/**
 * @brief 设置监听器Access Token失效回调
 * @details 多个信令服务器同时报告同一Token失效时，同一Token在60秒内只回调一次。
 * @note 必须在 tgtrp_listen() 前调用；回调运行在信令线程，不能阻塞。
 *
 * @param listener 监听器句柄。示例: 由 tgtrp_listener_new 返回的指针
 * @param cb Token失效回调，传NULL表示不注册回调
 * @param context 用户自定义上下文指针，将传递给回调函数
 */
void tgtrp_listener_set_on_token_invalid(tgtrp_listener listener, tgtrp_listener_token_invalid_cb_t cb, void* context);

/**
 * @brief 绑定监听器参数并设置新连接回调
 * @note 异步调用: 非阻塞
 *
 * @param listener 监听器句柄。示例: 由 tgtrp_listener_new 返回的指针
 * @param config_str 配置字符串。示例: "w9bl84KRoLbI1bOi0sD05LDQ4u2FlLi2x9b88YeSrL7A0eI="
 * @param token_str 鉴权Token字符串；允许传空字符串，并通过Token失效回调刷新。示例: "eyJh..."
 * @param device_id 本地设备ID(控制在32字符以内)。示例: "YT4F77V532RR"
 * @param newconn_cb 新连接建立时的回调函数。此回调函数不能阻塞，否则会卡住P2P线程.
 *        - context: 用户传入的上下文
 *        - pconn: 新建立的连接句柄
 * @param context 用户自定义上下文指针，将传递给回调函数。
 */
int tgtrp_listener_bind(tgtrp_listener listener, const char* config_str, const char* token_str, const char* device_id, void (*newconn_cb)(void* context, tgtrp_connection pconn), void* context);

/**
 * @brief 刷新监听器使用的Access Token
 * @details bind后、listen前调用会更新初始Token；listen后调用会异步更新所有信令连接，
 *          新Token将在下一次正常心跳中发送，不会强制立即发送心跳。
 * @note 可以在Token失效回调中调用。关闭监听器开始后不能再调用。
 *
 * @param listener 监听器句柄。示例: 由 tgtrp_listener_new 返回的指针
 * @param token_str 新的非空Access Token，长度不能超过500字节
 * @return int 0代表已接受，-1代表参数、状态或内存分配失败
 */
int tgtrp_listener_refresh_token(tgtrp_listener listener, const char* token_str);

/**
 * @brief 开始监听
 * @note 异步调用: 非阻塞
 *
 * @param listener 监听器句柄。示例: 由 tgtrp_listener_new 返回的指针
 * @return int 0 代表成功，-1 代表失败
 */
int tgtrp_listen(tgtrp_listener listener);

/**
 * @brief 关闭监听句柄
 * @details 在异步回调中清除应用层注册的context相关数据
 * @note异步调用: 非阻塞，上层context对象需要等待close_finish_cb回调后才能释放
 *
 * @param obj 监听器句柄。示例: 由 tgtrp_listener_new 返回的指针
 * @param close_finish_cb 关闭完成时的回调函数。此回调函数不能阻塞，否则会卡住P2P线程。
 *        - context: 用户传入的上下文
 * @param context 用户自定义上下文指针，将传递给回调函数。
 */
void tgtrp_close(tgtrp_listener obj, void (*close_finish_cb)(void* context), void* context);


/**
 * @brief 创建一个新的P2P连接对象
 * @note 异步调用: 非阻塞
 *
 * @return tgtrp_connection 成功返回连接句柄，失败返回NULL
 */
tgtrp_connection tgtrp_connection_new(void);

/**
 * @brief 设置连接超时时间
 * @note 异步调用: 非阻塞
 *
 * @param pconn 连接句柄 (参数名补充)。示例: 由 tgtrp_connection_new 返回的指针
 * @param timeout_ms 超时时间 (毫秒)。示例: 5000
 */
void tgtrp_connection_set_timeout(tgtrp_connection pconn, unsigned int timeout_ms);

/**
 * @brief 销毁连接对象
 * @note 异步调用: 非阻塞，上层context对象需要等待close_finish_cb回调后才能释放
 *
 * @param pconn 连接句柄 (参数名补充)。示例: 由 tgtrp_connection_new 返回的指针
 * @param stat_info_cb 统计信息回调通知，关闭期间可能会被回调多次。此回调函数不能阻塞，否则会卡住P2P线程。
 *        - pconn: 连接句柄
 *        - stat_context: 统计上下文
 *        - stat_str: 统计信息字符串
 *        - length: 字符串长度
 * @param stat_context 统计信息回调通知的context指针。
 * @param close_finish_cb 关闭成功的回调通知。此回调函数不能阻塞，否则会卡住P2P线程。
 *        - close_context: 关闭上下文
 * @param close_context 关闭成功通知的context指针。
 */
void  tgtrp_connection_destroy(tgtrp_connection pconn,
    void (*stat_info_cb)(tgtrp_connection pconn, void* stat_context, const char* stat_str, int length),
    void* stat_context,
    void (*close_finish_cb)(void* close_context),
    void* close_context);

/**
 * @brief 获取连接信息
 * @note 异步调用: 否 (非阻塞)
 *
 * @param pconn 连接句柄 (参数名补充)。示例: 由 tgtrp_connection_new 返回的指针
 * @param pinfo 输出参数，用于存储连接信息的结构体指针。示例: 指向 struct tgtrp_connection_info 的指针
 */
void tgtrp_connection_get_info(tgtrp_connection pconn, struct tgtrp_connection_info* pinfo);

/**
 * @brief 获取连接本端ID
 * @note 异步调用: 否 (非阻塞)
 *
 * @details 返回值指向连接对象内部缓存，连接销毁后失效。主叫端在 tgtrp_connection_call 成功创建 session 后可获取；
 *          被叫端在 newconn_cb 回调内可获取。尚未缓存时返回空字符串。
 *
 * @param pconn 连接句柄。示例: 由 tgtrp_connection_new 返回或 newconn_cb 传入的指针
 * @return const char* 本端ID，参数非法或尚未缓存时返回空字符串
 */
const char* tgtrp_connection_get_local_id(tgtrp_connection pconn);

/**
 * @brief 获取连接远端ID
 * @note 异步调用: 否 (非阻塞)
 *
 * @details 返回值指向连接对象内部缓存，连接销毁后失效。主叫端在 tgtrp_connection_call 成功创建 session 后可获取；
 *          被叫端在 newconn_cb 回调内可获取。尚未缓存时返回空字符串。
 *
 * @param pconn 连接句柄。示例: 由 tgtrp_connection_new 返回或 newconn_cb 传入的指针
 * @return const char* 远端ID，参数非法或尚未缓存时返回空字符串
 */
const char* tgtrp_connection_get_remote_id(tgtrp_connection pconn);

/**
 * @brief 同步获取connection层耗时统计信息
 * @note 每个统计分组只保留最近10条有效样本，返回的是该窗口的汇总值。
 * @param pconn 连接对象
 * @param time_stats 输出统计快照，成功时被完整填充
 * @return int 0表示成功，-1表示参数非法或连接不可用
 */
int tgtrp_connection_get_time_stats(tgtrp_connection pconn, tgtrp_connection_time_stats_t* time_stats);

/**
 * @brief 同步获取connection层最近一次TGTRP网络质量统计信息
 * @note 该快照由 on_data_ex 数据交付路径缓存，只在TGTRP数据交付时更新。
 *       无可用样本时 observation_sample_count 为0。
 * @param pconn 连接对象
 * @param quality_stats 输出统计快照，成功时被完整填充
 * @return int 0表示成功，-1表示参数非法或连接不可用
 */
int tgtrp_connection_get_network_quality_stats(tgtrp_connection pconn, tgtrp_connection_network_quality_stats_t* quality_stats);

/**
 * @brief 注册连接错误回调通知
 * @note  异步调用: 非阻塞
 *
 * @param pconn 连接句柄 (参数名补充)。示例: 由 tgtrp_connection_new 返回的指针
 * @param error_notify_cb 错误发生时的回调函数。此回调函数不能阻塞，否则会卡住P2P线程。
 *        - pconn: 连接句柄
 *        - context: 用户上下文
 *        - ev: 错误码/事件ID
 * @param context 用户自定义上下文指针。
 */
void tgtrp_connection_set_on_error(tgtrp_connection pconn, void (*error_notify_cb)(tgtrp_connection pconn, void* context, int ev), void* context);

/**
 * @brief 注册新Channel创建成功的回调通知
 * @details 通知一个新的tgtrp_channel创建成功，此时可以开始收发数据
 * @note  异步调用: 非阻塞
 *
 * @param pconn 连接句柄 (参数名补充)。示例: 由 tgtrp_connection_new 返回的指针
 * @param on_newchannel_cb 新Channel就绪时的回调函数。此回调函数不能阻塞，否则会卡住P2P线程。
 *        - pconn: 连接句柄
 *        - c: 新创建的Channel句柄
 *        - context: 用户上下文
 * @param context 用户自定义上下文指针。
 */
void tgtrp_connection_set_on_channel(tgtrp_connection pconn, void (*on_newchannel_cb)(tgtrp_connection pconn, tgtrp_channel c, void* context), void* context);

/**
 * @brief 请求创建一个新的Channel
 * @details 创建后不会立即返回channel句柄，需要底层握手成功后，通过 `tgtrp_connection_set_on_channel` 设置的回调通知上层。
 * @note 异步调用: 否 (非阻塞)
 *
 * @param pconn 连接句柄 (参数名补充)。示例: 由 tgtrp_connection_new 返回的指针
 * @param label_name Channel的标签名称，长度不超过12字节。示例: "data_ch_1"
 */
void tgtrp_channel_new(tgtrp_connection pconn, const char label_name[12]);

/**
 * @brief 呼叫对方建立连接
 * @note 异步调用: 否 (非阻塞)
 *
 * @param pconn 连接句柄 (参数名补充)。示例: 由 tgtrp_connection_new 返回的指针
 * @param config_str 配置字符串。示例: "w9bl84KRoLbI1bOi0sD05LDQ4u2FlLi2x9b88YeSrL7A0eI="
 * @param token_str 鉴权Token字符串。示例: "eyJh..."
 * @param appid 应用ID(16字符以内)。示例: "my_app_001"
 * @param remote_device 远端设备ID。示例: "remote_dev_999"
 * @param link_param 链路模式，常用值参见 tgtrp_connection_link_mode_t。
 *                   推荐传 TGTRP_CONNECTION_LINK_MODE_DEFAULT。
 * @warning TGTRP_CONNECTION_LINK_MODE_DIRECT_ONLY 不使用中转，P2P 直连失败时
 *          连接将无法建立。
 * @warning TGTRP_CONNECTION_LINK_MODE_RELAY_ONLY 会禁用 P2P 和内网直连，
 *          强制使用 TURN 中转；TURN 不可用时连接将无法建立。
 * @param timeout_ms 呼叫超时时间 (毫秒)。示例: 10000
 */
void tgtrp_connection_call(tgtrp_connection pconn, const char* config_str, const char* token_str, const char* appid, const char* remote_device, int link_param, uint32_t timeout_ms);

/**
 * @brief 获取Channel的标签名称
 * @note  异步调用: 非阻塞
 *
 * @param c Channel句柄。示例: 回调函数中传入的 tgtrp_channel 指针
 * @return const char* 返回Channel的标签字符串
 */
const char* tgtrp_channel_get_label(tgtrp_channel c);

/**
 * @brief 设置Channel的数据接收回调
 * @note  异步调用: 非阻塞
 *
 * @param pc 该channel所属的p2p连接句柄。示例: 由 tgtrp_connection_new 返回的指针
 * @param c Channel句柄。示例: 回调函数中传入的 tgtrp_channel 指针
 * @param ondata 收到数据时的回调函数。此回调函数不能阻塞，否则会卡住P2P线程。
 *        - pc: 连接句柄
 *        - c: Channel句柄
 *        - context: 用户上下文
 *        - buffer: 数据缓冲区指针
 *        - size: 数据长度
 * @param context 用户自定义上下文指针。
 */
void  tgtrp_channel_set_on_data(
    tgtrp_connection pc,
    tgtrp_channel c,
    void (*ondata)(tgtrp_connection pc, tgtrp_channel c, void* context, char* buffer, int size),
    void* context);

/**
 * @brief 设置Channel的带统计信息数据接收回调
 * @note  异步调用: 非阻塞。stats指针只在回调期间有效，若需保存请自行拷贝。
 *
 * @param pc 该channel所属的p2p连接句柄
 * @param c Channel句柄
 * @param ondata 收到数据时的回调函数。此回调函数不能阻塞，否则会卡住P2P线程。
 *        - pc: 连接句柄
 *        - c: Channel句柄
 *        - context: 用户上下文
 *        - media_type: TGTRP媒体类型，非TGTRP传输模式下为0
 *        - stream_id: TGTRP媒体流ID，非TGTRP传输模式下为0
 *        - frame_type: TGTRP帧类型，非TGTRP传输模式下为0
 *        - buffer: 数据缓冲区指针
 *        - size: 数据长度
 *        - stats: 本次数据的轻量统计信息
 * @param context 用户自定义上下文指针。
 */
void tgtrp_channel_set_on_data_ex(
    tgtrp_connection pc,
    tgtrp_channel c,
    void (*ondata)(tgtrp_connection pc, tgtrp_channel c, void* context, uint8_t media_type, uint8_t stream_id, int frame_type, char* buffer, int size, const tgtrp_channel_data_stats_t* stats),
    void* context);

/**
 * @brief 注册Channel视频发送侧控制事件回调
 * @note 回调执行在 WebRTC 内部线程，必须快速返回。
 * @note 同一个 channel 的同一个 video stream 只允许注册一次。
 * @note 注册会异步投递到 peer/session 线程执行；返回0表示投递成功，不表示底层已立即生效。
 * @note 注册后不支持运行期注销；回调与 channel 同生命周期，channel/connection 销毁时统一清理。
 *
 * @param pc 该channel所属的p2p连接句柄
 * @param c Channel句柄
 * @param stream_id video流ID；0xe0~0xff为TGTRP内部保留值
 * @param cb 事件回调；应为非NULL，并在channel生命周期内保持有效
 * @param context 用户自定义上下文指针
 * @return int 0表示投递成功，-1表示参数非法、连接正在关闭或投递失败
 */
int tgtrp_channel_set_on_video_send_event(
    tgtrp_connection pc,
    tgtrp_channel c,
    uint8_t stream_id,
    tgtrp_channel_video_send_event_cb_t cb,
    void* context);

/**
 * @brief 设置Channel内指定video流的码率目标范围。
 * @note min_bps/max_bps/start_bps 单位均为 bps，即 bits per second，不是 bytes/s。
 *       必须满足 min_bps > 0 && min_bps <= start_bps && start_bps <= max_bps。
 * @note 该配置只影响指定stream_id，对其他video流不生效。
 * @note 该接口异步投递到peer worker执行；返回0不表示配置已经应用。
 * @return int 0表示任务已投递，-1表示参数非法、连接/channel无效或投递失败。
 */
int tgtrp_channel_set_video_bitrate_config(
    tgtrp_connection pc,
    tgtrp_channel c,
    uint8_t stream_id,
    uint32_t min_bps,
    uint32_t max_bps,
    uint32_t start_bps);

/**
 * @brief Channel发送接口的统一返回值及错误码说明。
 * @details 适用于 tgtrp_channel_sendv、tgtrp_channel_video_sndv、
 *          tgtrp_channel_audio_sndv 和 tgtrp_channel_data_sndv。
 *
 * 成功和失败的统一判断：
 * - 返回值 > 0：底层已接受本次数据，返回值等于所有vec的应用载荷长度之和；
 *   TGTRP/KCP/SCTP协议头和内部调试头均不计入。只表示本地接受或入队，不表示对端已收到或确认。
 * - 返回值 == 0：本次没有数据被接受，按发送失败处理。
 * - 返回值 < 0：发送失败。调用方应按当前连接实际使用的传输模式解释错误。
 * @note vec_cnt必须大于0，各vec长度不能为负，且所有vec的应用载荷总长度必须大于0；
 *       不满足时返回-1，任何内部协议头或调试头都不会被发送。
 *
 * TGTRP模式：
 * - -1：参数非法、Channel/连接状态无效、stream_id冲突、视频frame_type非法或通用失败。
 * - -2：内存分配失败。
 * - -3：video/data达到普通发送缓冲区拒绝水位，或音频达到更高的拒绝水位，
 *   当前帧未入队；调用方应稍后重试或执行应用层流控。
 * - -4：TGTRP数据包非法。
 * - -5：当前操作或能力不支持。
 * - -6：TGTRP会话状态不允许当前操作。
 * - -7：调用方或内部缓冲区过小。
 * - -8：帧超过分片数量限制；视频还可能表示压力丢帧后正在等待新的I帧，
 *   此时P/B/MSG帧不会入队，应先发送I帧。
 * - -9：底层网络发送失败。
 * @note 当前异步入队阶段常见返回值为-1、-2、-3和-8；其余TGTRP错误码为底层透传值，
 *       调用方仍应将所有负值统一视为失败。
 *
 * KCP模式：
 * - -1：参数/状态非法，或KCP待发送字节数加本次数据超过发送缓冲区上限。
 * - 其它负值：底层KCP错误透传，统一按失败处理；KCP分支不保证为每次失败更新errno。
 *
 * SCTP模式：
 * - 0：DataChannel未处于可发送状态、SCTP socket不可用/正在销毁，或没有数据被接受。
 * - -1：参数非法或usrsctp发送失败。底层发送失败时应立即读取errno：
 *   EWOULDBLOCK/EAGAIN表示发送缓冲区暂时无空间，可稍后重试；EINVAL表示参数或状态非法。
 *   其它errno值按当前平台和usrsctp的错误定义处理。
 * @note 并非所有公共参数校验失败都会更新errno；只有确认进入SCTP底层发送后才能依赖errno。
 */

/**
 * @brief 通过Channel发送数据 (Scatter/Gather I/O)
 * @note  异步调用: 非阻塞
 *
 * @param pc 连接句柄。示例: 由 tgtrp_connection_new 返回的指针
 * @param c Channel句柄。示例: 回调函数中传入的 tgtrp_channel 指针
 * @param vec 数据向量数组。示例: struct tgtrp_channel_vec my_vecs[2];
 * @param vec_cnt 向量数组的元素个数。示例: 2
 * @return >0 表示底层已接受本次数据，返回所有vec的应用载荷长度之和；内部协议头和调试头不计入。
 *         该返回值不表示对端已经收到或确认。
 * @return 0 表示本次未发送，可能是非TGTRP回退路径的Channel尚未就绪或正在关闭。
 * @return <0 表示失败，按上方统一说明中的当前传输模式解释错误码。
 * @note 调用方应以返回值 > 0 判断本次数据已被本地发送路径接受；接受不等于送达。
 */
int tgtrp_channel_sendv(tgtrp_connection pc, tgtrp_channel c, struct tgtrp_channel_vec vec[], int vec_cnt);

/**
 * @brief 通过Channel发送视频帧 (Scatter/Gather I/O)
 * @details 当底层peer_connection使用TGTRP传输时，按视频流进入TGTRP media transport；
 *          其它传输模式下兼容回退到原 tgtrp_channel_sendv 逻辑。
 *
 * @param pc 连接句柄
 * @param c Channel句柄
 * @param stream_id TGTRP媒体流ID，非TGTRP传输模式下忽略；TGTRP模式下0xe0~0xff为内部保留值
 * @param frame_type 视频帧类型，参见 enum TGTRP_CHANNEL_VIDEO_FRAME_TYPE
 * @param vec 数据向量数组
 * @param vec_cnt 向量数组的元素个数
 * @return >0 表示底层已接受本次视频帧，返回所有vec的应用载荷长度之和；内部协议头和调试头不计入。
 *         该返回值不表示对端已经收到或确认。
 * @return 0 表示本次未发送，可能是非TGTRP回退路径的Channel尚未就绪或正在关闭。
 * @return <0 表示失败，按上方统一说明中的当前传输模式解释错误码。
 * @note 调用方应以返回值 > 0 判断本次帧已被本地发送路径接受；接受不等于送达。
 */
int tgtrp_channel_video_sndv(tgtrp_connection pc, tgtrp_channel c, uint8_t stream_id, int frame_type, struct tgtrp_channel_vec vec[], int vec_cnt);

/**
 * @brief 通过Channel发送音频帧 (Scatter/Gather I/O)
 * @details 当底层peer_connection使用TGTRP传输时，按音频流进入TGTRP media transport；
 *          其它传输模式下兼容回退到原 tgtrp_channel_sendv 逻辑。
 *
 * @param pc 连接句柄
 * @param c Channel句柄
 * @param stream_id TGTRP媒体流ID，非TGTRP传输模式下忽略；TGTRP模式下0xe0~0xff为内部保留值
 * @param vec 数据向量数组
 * @param vec_cnt 向量数组的元素个数
 * @return >0 表示底层已接受本次音频帧，返回所有vec的应用载荷长度之和；内部协议头和调试头不计入。
 *         该返回值不表示对端已经收到或确认。
 * @return 0 表示本次未发送，可能是非TGTRP回退路径的Channel尚未就绪或正在关闭。
 * @return <0 表示失败，按上方统一说明中的当前传输模式解释错误码。
 * @note 调用方应以返回值 > 0 判断本次帧已被本地发送路径接受；接受不等于送达。
 */
int tgtrp_channel_audio_sndv(tgtrp_connection pc, tgtrp_channel c, uint8_t stream_id, struct tgtrp_channel_vec vec[], int vec_cnt);

/**
 * @brief 通过Channel发送数据帧 (Scatter/Gather I/O)
 * @details 当底层peer_connection使用TGTRP传输时，按数据流进入TGTRP media transport；
 *          其它传输模式下兼容回退到原 tgtrp_channel_sendv 逻辑。
 *
 * @param pc 连接句柄
 * @param c Channel句柄
 * @param stream_id TGTRP媒体流ID，非TGTRP传输模式下忽略；TGTRP模式下0xe0~0xff为内部保留值
 * @param vec 数据向量数组
 * @param vec_cnt 向量数组的元素个数
 * @return >0 表示底层已接受本次数据帧，返回所有vec的应用载荷长度之和；内部协议头和调试头不计入。
 *         该返回值不表示对端已经收到或确认。
 * @return 0 表示本次未发送，可能是非TGTRP回退路径的Channel尚未就绪或正在关闭。
 * @return <0 表示失败，按上方统一说明中的当前传输模式解释错误码。
 * @note 调用方应以返回值 > 0 判断本次帧已被本地发送路径接受；接受不等于送达。
 */
int tgtrp_channel_data_sndv(tgtrp_connection pc, tgtrp_channel c, uint8_t stream_id, struct tgtrp_channel_vec vec[], int vec_cnt);

/**
 * @brief 获取当前发送缓冲区的已使用量
 * @note  异步调用: 非阻塞
 *
 * @param pc 连接句柄。示例: 由 tgtrp_connection_new 返回的指针
 * @return size_t 已使用的字节数。示例: 1024，应用层根据返回值判定缓存大小来决定是否要丢帧。
 */
size_t tgtrp_channel_get_used_send_buffer_size(tgtrp_connection pc);

/**
 * @brief 开启连接的调试模式
 * @details 仅呼叫端有效，用于输出调试信息
 * @note  异步调用: 非阻塞
 *
 * @param pc 连接句柄。示例: 由 tgtrp_connection_new 返回的指针
 */
void tgtrp_connection_enable_debug(tgtrp_connection pc);

/**
 * @brief 设置TIRTC库的日志级别
 * @note  异步属性: 同步调用
 * @note  默认普通日志关闭；如果在 tgtrp_init 前调用过本接口，初始化时会保留用户设置的级别。
 *
 * @param log_level 低8位为原有日志级别，取值范围：
 *        - 0: 关闭普通日志输出
 *        - 1: 预留最高优先级日志
 *        - 2: 预留/底层 LOG_CRIT 级别
 *        - 3: 错误日志（APP_LOG_ERROR）
 *        - 4: 警告日志（APP_LOG_WARNING）
 *        - 5: 重要流程日志（APP_LOG_NOTICE）
 *        - 6: 普通信息日志（APP_LOG_INFO）
 *        - 7: 最低级别日志（输出所有普通日志，包括调试信息）
 *
 *        bit8 (TGTRP_LOG_FLAG_STAT): 打开统计日志（APP_LOG_STAT），可与低8位组合。
 *        示例：tgtrp_set_log_level(5 | TGTRP_LOG_FLAG_STAT) 同时打开 NOTICE 和 STAT。
 *
 *        说明：低8位级别越高（数字越大），输出的普通日志越详细
 *        建议：开发调试时使用7，生产环境使用1~3
 */
void tgtrp_set_log_level(int log_level);

/**
 * @brief 设置日志回调函数
 * @note  异步属性: 同步调用
 *
 * @param cb 日志回调函数指针，类型为 tgtrp_log_cb_t
 *           函数签名: void (*)(const char* fmt, va_list args)
 *           可用于将日志重定向到自定义处理函数
 */
void tgtrp_set_log_callback(tgtrp_log_cb_t cb);


/**
 * @brief 创建一个新的P2P连接对象
 * @note 异步调用: 非阻塞
 * @param use_dtls, 如果主叫方为第三方(例如chrome)，则需要传参数为1
 * use_dtls特别说明：为什么会导出这样的设置，原因在于如果对于自研库之间不想走dtls(消耗性能)，则传0，但是如果对方是web端，此时库并不知道对方是web，需要上层传use_dtls为1. 如果业务后续全部走dtls，则可以去掉该参数，强制所有通信都走dtls加密
 * @return tgtrp_connection 成功返回连接句柄，失败返回NULL
 */
tgtrp_connection tgtrp_connection_whip_new(int use_dtls);

/**
 * @brief [WHIP 主叫] 异步生成 Offer SDP
 * @details 内部触发 ICE gather，收集所有候选后将其拼接到 SDP 末尾，
 *          通过 on_sdp_ready 回调返回含所有候选的完整 offer SDP。
 *          回调执行在 WebRTC 内部线程，不得阻塞；sdp 指针在回调返回后失效，需自行拷贝。
 *
 *          调用顺序：
 *            create_offer_sdp(pc, cb, ctx)
 *            → on_sdp_ready 回调: HTTP POST 完整 offer SDP
 *            → set_remote_sdp(answer) → add_candidate×N → start_connect
 *
 * @param pc           连接句柄，由 tgtrp_connection_new 创建
 * @param on_sdp_ready gather 完成后的回调，参数含完整 SDP 字符串和长度
 * @param context      用户自定义上下文指针
 * @return 0 表示成功，-1 表示失败
 */
int tgtrp_connection_whip_create_offer_sdp(
    tgtrp_connection pc,
    void (*on_sdp_ready)(tgtrp_connection pc, void* context, const char* sdp, int len),
    void* context);

/**
 * @brief [WHIP 主叫] 设置远端 Answer SDP
 * @details 将 WHIP 服务端返回的 answer SDP 设置为远端描述，并自动解析其中内嵌的候选。
 *
 * @param pc                连接句柄
 * @param sdp_buffer        远端 answer SDP 字符串
 * @param sdp_buffer_length SDP 字符串长度
 * @return 0 表示成功，-1 表示失败
 */
int tgtrp_connection_whip_set_remote_sdp(tgtrp_connection pc, const char* sdp_buffer, int sdp_buffer_length);

/**
 * @brief [WHIP 被叫] 异步生成 Answer SDP
 * @details 在 set_remote_sdp 之后调用。内部触发 ICE gather，收集所有候选后将其拼接到 SDP
 *          末尾，通过 on_sdp_ready 回调返回含所有候选的完整 answer SDP。
 *          回调执行在 WebRTC 内部线程，不得阻塞；sdp 指针在回调返回后失效，需自行拷贝。
 *
 *          调用顺序：
 *            set_remote_sdp(offer) → create_answer_sdp(pc, cb, ctx)
 *            → on_sdp_ready 回调: 发送 201（含完整 answer SDP）
 *            → add_candidate×N → start_connect
 *
 * @param pc           连接句柄，由 tgtrp_connection_new 创建
 * @param on_sdp_ready gather 完成后的回调，参数含完整 SDP 字符串和长度
 * @param context      用户自定义上下文指针
 * @return 0 表示成功，-1 表示失败
 */
int tgtrp_connection_whip_create_answer_sdp(
    tgtrp_connection pc,
    void (*on_sdp_ready)(tgtrp_connection pc, void* context, const char* sdp, int len),
    void* context);

/**
 * @brief 添加远端 ICE Candidate
 * @details 主叫和被叫均可调用，将对端通过信令发来的 candidate 字符串加入 ICE 候选列表。
 *          可在 start_connect 之前或之后调用。
 *
 * @param pc                连接句柄
 * @param sdp_buffer        candidate SDP 字符串（"candidate:xxx ..."）
 * @param sdp_buffer_length 字符串长度
 * @return 0 或正数表示成功，-1 表示失败
 */
int tgtrp_connection_whip_add_candidate(tgtrp_connection pc, const char* sdp_buffer, int sdp_buffer_length);

/**
 * @brief 开始 ICE 连接
 * @details 在 on_sdp_ready 回调之后、完成 SDP 交换后调用。
 *          ICE gather 已在 create_offer/answer_sdp 内部触发，此函数只启动连接协商。
 *
 * @param pc  连接句柄
 */
void tgtrp_connection_whip_start_connect(tgtrp_connection pc);

/**
 * @brief [WHIP] 设置 candidate IP 覆盖地址
 * @details 设置后，gather 阶段生成的所有本地 candidate 的 IP 地址均替换为指定值。
 *          适用于设备位于 NAT 后、需对外暴露固定公网 IP 的场景。
 *          传入 NULL 或空字符串则清除覆盖，恢复使用实际 IP。
 *          须在 create_offer_sdp / create_answer_sdp 之前调用。
 *
 * @param pc  连接句柄
 * @param ip  要覆盖的 IP 字符串（如 "203.0.113.1"），或 NULL/空串取消覆盖
 */
void tgtrp_connection_whip_set_candidate_ip(tgtrp_connection pc, const char* ip);


/*
if define SMALL_MEM_NO_DTLS:device_property |= 0x1;  device cpu low performance, no dtls.
if define SMALL_MEMORY_WITHOUT_SCTP:device_property |= 0x2;  only support kcp
if define  TGRTC_SECURITY_ENHANCEMENT device_property |= 0x04; force use dtls encrypt.
*/
uint8_t tgtrp_get_library_property(void);

#endif

/***************************示例程序


// Test configuration
//#define TEST_CONFIG "w9bl84KRoLbI1bOi0sD05LDV5e2FlaGpwdjj7YCWrLXA0eLz"
#define TEST_CONFIG "w9bl84KRoLbI1bOi0sD05LDQ4u2FlLi2x9b88YeSrL7A0eI="
#define TEST_DEVICE "test_device_001"
#define TEST_APPID "test_app_001"
#define TEST_REMOTE_DEVICE "remote_device_001"
#define TEST_CHANNEL_LABEL "test_channel"
#define TEST_MAX_CONNECTIONS 5
#define TEST_TIMEOUT_MS 10000

// Test data structure
typedef struct test_context {
    tgtrp_listener obj;
    tgtrp_connection conn;
    tgtrp_channel channel;
    int test_completed;
    char test_name[64];
} test_context_t;


#define MAX_CHANNEL_NUM 10
typedef struct _my_context
{
    tgtrp_connection conn;
    tgtrp_channel chs[MAX_CHANNEL_NUM];
    int channel_num;
    int is_error;
    int is_app;
    SA_HTHREAD th;
}my_context;


void stat_info_cb(tgtrp_connection conn, void* stat_context, const char* stat_str, int length)
{
    printf("%s\n", stat_str);
}


void close_finish_cb(void* close_context)
{
    Mx_free(close_context);
}

// 业务方定义的发送数据到连接的线程.
void conn_thread(void* arg)
{
    my_context* ctx = (my_context*)arg;
    struct tgtrp_channel_vec vec[1];
    char buffer[128];
    memset(buffer, '0', sizeof(buffer));
    vec[0].buf = buffer;
    struct tgtrp_connection_info info;
    vec[0].len = sizeof(buffer);
    while (1)
    {
        SA_Sleep(400);
        if (ctx->is_error)
        {
            tgtrp_connection_destroy(ctx->conn, stat_info_cb, ctx, close_finish_cb, ctx);
            break;
        }
        tgtrp_connection_get_info(ctx->conn, &info);
        printf("type %s----------------------%s %s\n", info.link_mode, info.local_candidate, info.remote_candidate);
        if (ctx->channel_num > 0)
        {
           tgtrp_channel_sendv(ctx -> conn, ctx->chs[0], vec, 1);
        }
    }


}

void ondata(tgtrp_connection pc, tgtrp_channel c, void* context, char* buffer, int size)
{
    my_context* ctx = (my_context*)context;
    assert(ctx->conn == pc);
    printf("ondata size=%d\n", size);
}

void on_newchannel_cb111(tgtrp_connection conn, tgtrp_channel c, void* context)
{
    my_context* ctx = (my_context*)context;
    ctx->chs[ctx->channel_num++] = c;
    assert(ctx->conn == conn);
    tgtrp_channel_set_on_data(ctx->conn, c, ondata, ctx);
}
void error_notiry_cb(tgtrp_connection conn, void* context, int ev)
{
    my_context* ctx = (my_context*)context;
    assert(conn == ctx->conn);
    APP_LOG_NOTICE("error_notiry_cb conn=%p ev=%d", conn, ev);
    ctx->is_error = 1;
}
void new_conn_cb(void* context, tgtrp_connection conn)
{
    my_context* ctx = Mx_calloc(1, sizeof(my_context));
    ctx->conn = conn;
    tgtrp_connection_set_on_channel(conn, on_newchannel_cb111, ctx);
    tgtrp_connection_set_on_error(conn, error_notiry_cb, ctx);
    SA_ThreadCreate(ctx->th, conn_thread, ctx, "xxxx"); //业务创建一个线程处理该连接.
}

//模拟device设备端
void simulate_device()
{
    tgtrp_init(512 * 1024);

    tgtrp_listener l = tgtrp_listener_new(5);

    tgtrp_listener_set_opt(l, TGTRP_MULTIPLE_CONNECTION_SHARED_THREAD, 1);
    tgtrp_listener_set_opt(l, TGTRP_CONNECTION_ENCRYPTO_LEVEL, 2);
    tgtrp_listener_bind(l, TEST_CONFIG,"ajc.c.....", "server", new_conn_cb, NULL);
    tgtrp_listen(l);
}

/// 模拟APP呼叫端
void simulate_app()
{
    tgtrp_init(512 * 1024);
    my_context* ctx = Mx_calloc(1, sizeof(my_context));
    ctx->conn =  tgtrp_connection_new();

    tgtrp_connection_set_on_channel(ctx->conn, on_newchannel_cb111, ctx);
    tgtrp_connection_set_on_error(ctx->conn, error_notiry_cb, ctx);
    SA_ThreadCreate(ctx->th, conn_thread, ctx, "xxxx");

    tgtrp_channel_new(ctx->conn, "1");
    tgtrp_channel_new(ctx->conn, "2");
    tgtrp_channel_new(ctx->conn, "3");

    tgtrp_connection_call(ctx->conn, TEST_CONFIG, "TOKEN", "client", "server",
                          TGTRP_CONNECTION_LINK_MODE_DEFAULT, 10000);
}

int main(int argc, char* argv[]) {

    if (argc > 1)
        simulate_app();
    else simulate_device();

    while (1)
        SA_Sleep(100);

}

**********************************/
