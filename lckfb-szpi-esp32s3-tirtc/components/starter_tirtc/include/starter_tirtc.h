#ifndef STARTER_TIRTC_H
#define STARTER_TIRTC_H

/**
 * @file starter_tirtc.h
 * @brief TiRTC SDK 的窄适配接口。
 *
 * 只有本模块的实现文件直接包含 tiRTC.h。其余代码只看见模板自有类型，避免
 * SDK ABI 和回调细节扩散。模块最多持有一个连接，并用 generation 标识连接生命周期。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STARTER_TIRTC_NONE = 0, /**< 当前没有连接。 */
    STARTER_TIRTC_H5,      /**< H5 入站查看/对讲连接；S3 仅音频。 */
    STARTER_TIRTC_AI,      /**< AI WHIP 外连，只有音频。 */
    STARTER_TIRTC_VOIP,    /**< 微信 VoIP WHIP 语音连接。 */
    STARTER_TIRTC_CALL,    /**< 设备互呼 P2P 语音连接。 */
    STARTER_TIRTC_ROOM,    /**< Room WHIP, stream 1, explicit join/PTT. */
} starter_tirtc_mode_t;

/** SDK 帧元数据的稳定副本；payload 不包含在该结构中。 */
typedef struct {
    uint8_t stream_id;     /**< 协议流编号。 */
    uint8_t media;         /**< SDK 媒体编码类型。 */
    uint8_t flags;         /**< 关键帧等 SDK 标志。 */
    uint32_t timestamp_ms; /**< 单调媒体时间戳，单位毫秒。 */
    uint32_t length;       /**< payload 字节数。 */
} starter_tirtc_frame_t;

/**
 * 适配层向会话状态机报告的事件。
 *
 * 所有函数都可能运行在 SDK 回调线程中，必须快速返回。若需保留 data，回调实现
 * 必须复制后再投递到自己的队列，不能执行 HTTP、阻塞 I/O、Stop 或 Uninit。
 */
typedef struct {
    /** SDK 异步启动或停止结果。 */
    void (*on_started)(bool started, int error, void *user_data);

    /** 入站/外连建立或断开；request_tag 只用于关联 AI 外连请求。 */
    void (*on_connection)(starter_tirtc_mode_t mode,
                          uint32_t generation,
                          uint32_t request_tag,
                          bool connected,
                          int error,
                          void *user_data);

    /** 远端控制命令；data 只在回调期间有效。 */
    void (*on_command)(starter_tirtc_mode_t mode,
                       uint32_t generation,
                       uint32_t command,
                       const void *data,
                       uint32_t length,
                       void *user_data);

    /** 下行音频帧；frame 和 data 都只在回调期间有效。 */
    void (*on_audio)(starter_tirtc_mode_t mode,
                     uint32_t generation,
                     const starter_tirtc_frame_t *frame,
                     const void *data,
                     void *user_data);

#if CONFIG_IDF_TARGET_ESP32P4
    void (*on_video)(starter_tirtc_mode_t mode, uint32_t generation,
                     const starter_tirtc_frame_t *frame, const void *data, void *user_data);

    /** H5 请求 stream 11 刷新帧；MJPEG 的每一帧本身都是独立图像。 */
    void (*on_key_frame)(uint32_t generation, void *user_data);
#endif

    /** 原样传给以上回调，生命周期由调用者保证。 */
    void *user_data;
} starter_tirtc_handlers_t;

typedef struct {
    const char *device_id;       /**< 已绑定设备 ID，调用 start 时必须有效。 */
    const char *device_secret;   /**< 设备密钥；模块不会打印其内容。 */
    const char *client_id;       /**< TiRTC 客户端 ID。 */
    uint32_t max_send_buffer_bytes; /**< SDK 最大发送缓冲，0 使用 SDK 默认值。 */
    int log_level;               /**< TiRTC 日志级别，非正数使用 3。 */
} starter_tirtc_config_t;

/** 以下版本字符串由 SDK 持有，调用者不得释放。 */
const char *starter_tirtc_version(void);
const char *starter_tirtc_build_info(void);

/** 设置事件处理器；starter_runtime_start() 会在 SDK 启动前调用。 */
void starter_tirtc_set_handlers(const starter_tirtc_handlers_t *handlers);

/** 初始化并异步启动 SDK。返回 0 只表示启动请求已接受，最终结果见 on_started。 */
int starter_tirtc_start(const starter_tirtc_config_t *config);

/** SDK 是否已经通过 TIRTC_EVENT_SYS_STARTED 确认启动。 */
bool starter_tirtc_started(void);

/** 控制是否接受 H5 入站；AI 占用资源时会暂时关闭。 */
void starter_tirtc_accept_h5(bool accept);

/** 异步发起 AI WHIP 连接；request_tag 会原样带回 on_connection。 */
int starter_tirtc_ai_connect(const char *peer_id,
                             const char *token,
                             uint32_t request_tag);

/** 微信语音使用 WHIP，设备互呼被叫使用 P2P connect。 */
int starter_tirtc_voip_connect(const char *peer_id,
                               const char *token,
                               uint32_t request_tag);
int starter_tirtc_call_connect(const char *remote_id,
                               const char *token,
                               uint32_t request_tag);

/** 主叫建房后等待被叫 P2P 入站；request_tag 用于过滤旧房间。 */
void starter_tirtc_expect_call(uint32_t request_tag);

/** 取消待完成外连并断开当前连接。 */
int starter_tirtc_disconnect(void);

/** 以下查询函数可从任意任务调用，返回瞬时原子快照。 */
bool starter_tirtc_connected(void);
starter_tirtc_mode_t starter_tirtc_mode(void);
uint32_t starter_tirtc_generation(void);

/** Match legacy/raw and SDK-encoded AI signaling words without changing them. */
bool starter_tirtc_is_ai_command(uint32_t command);
bool starter_tirtc_is_room_command(uint32_t command);
int starter_tirtc_room_connect(const char *peer_id, const char *token, uint32_t request_tag);

/** 向当前连接发送控制命令。 */
int starter_tirtc_send_command(uint32_t command,
                               const void *data,
                               uint32_t length);

/** 设备身份调用 TiRTC 服务接口；用于微信语音来电拒接。 */
int starter_tirtc_service_request(const char *path, const char *json_body);

/**
 * 发送 G.711 A-law、8 kHz、16 bit、单声道音频。
 * H5 自动使用 stream 10，AI 自动使用 stream 1。
 */
int starter_tirtc_send_alaw(uint32_t timestamp_ms,
                            const void *data,
                            uint32_t length);

#if CONFIG_IDF_TARGET_ESP32P4
/** 发送一张完整 JPEG 图像；只允许 H5，固定使用 stream 11。 */
int starter_tirtc_send_mjpeg(uint32_t timestamp_ms,
                             const void *data,
                             uint32_t length);
#endif

/**
 * H5 发送前检查远端是否已订阅对应媒体。AI 音频在业务握手完成前由
 * starter_runtime 阻止媒体任务启动；设备互呼、微信 VoIP 在通话确认后
 * 才启动媒体任务，不能依赖可选的订阅回调。
 */
bool starter_tirtc_audio_ready(void);
#if CONFIG_IDF_TARGET_ESP32P4
bool starter_tirtc_video_ready(void);
int starter_tirtc_send_h264(uint32_t timestamp_ms, const void *data, uint32_t length, bool key);
int starter_tirtc_subscribe_call_video(void);
int starter_tirtc_request_remote_key_frame(void);
/** P4 video owner registers bounds, then consumes coalesced SDK feedback.
 * Neither the SDK callback nor this adapter reconfigures the encoder. */
int starter_tirtc_set_video_bitrate(uint32_t generation, uint32_t min_bps,
                                  uint32_t max_bps, uint32_t start_bps);
bool starter_tirtc_take_video_bitrate(uint32_t generation, uint32_t *target_bps);
#endif

/** 返回当前 SDK 发送缓冲占用字节数，无连接时为 0。 */
size_t starter_tirtc_send_buffer_used(void);

#ifdef __cplusplus
}
#endif

#endif
