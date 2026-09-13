#ifndef STARTER_RUNTIME_H
#define STARTER_RUNTIME_H

/**
 * @file starter_runtime.h
 * @brief H5 与 AI 共用媒体资源的会话状态机。
 *
 * 本模块隐藏 token 获取、WHIP 建连、AI JSON-RPC 握手、超时和迟到回调过滤。
 * 所有状态变化都在一个 FreeRTOS 任务内串行执行；调用者只投递意图并读取快照。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STARTER_RUNTIME_WAITING = 0, /**< 空闲，允许 H5 入站连接。 */
    STARTER_RUNTIME_H5_ACTIVE,   /**< H5 占用媒体路径。 */
    STARTER_RUNTIME_AI_CONNECTING, /**< AI token/建连/握手进行中。 */
    STARTER_RUNTIME_AI_ACTIVE,   /**< AI 已确认 start_session，可发送音频。 */
    STARTER_RUNTIME_CALL_INCOMING, /**< 设备或微信语音来电，等待用户处理。 */
    STARTER_RUNTIME_CALL_CONNECTING, /**< 语音外呼/接听正在建连。 */
    STARTER_RUNTIME_CALL_ACTIVE, /**< 语音通话已收到 0x2000 接通确认。 */
} starter_runtime_state_t;

typedef enum {
    STARTER_CONTACT_DEVICE = 0,
    STARTER_CONTACT_WECHAT,
} starter_contact_source_t;

#define STARTER_PRODUCT_CONTACTS_MAX 8

typedef struct {
    starter_contact_source_t source;
    bool online; /**< 只对 device 有意义；微信联系人不得伪造在线状态。 */
    char id[65];
    char name[65];
    char wx_app_id[65];
    char wx_model_id[65];
} starter_product_contact_t;

typedef struct {
    starter_runtime_state_t state; /**< 当前公开状态。 */
    uint32_t session_generation;   /**< 每次业务会话递增，用于过滤 HTTP 响应。 */
    uint32_t connection_generation; /**< TiRTC 连接代次，0 表示无连接。 */
    int last_error;                /**< 最近一次结束原因，0 表示正常结束。 */
    uint32_t stack_high_water_bytes; /**< 状态任务历史最小剩余栈，单位字节。 */
} starter_runtime_status_t;

#define STARTER_DIAGNOSTIC_EVENTS 8
typedef struct {
    uint32_t at_ms;
    uint32_t session;
    int value;
    char label[32]; /**< Fixed event label; never user text, identifiers or tokens. */
} starter_diagnostic_event_t;
typedef struct {
    unsigned count;
    starter_diagnostic_event_t events[STARTER_DIAGNOSTIC_EVENTS]; /**< Oldest first. */
} starter_runtime_diagnostics_t;
void starter_runtime_diagnostics(starter_runtime_diagnostics_t *out);

typedef enum {
    STARTER_AI_UI_IDLE = 0,
    STARTER_AI_UI_LISTENING,
    STARTER_AI_UI_THINKING,
    STARTER_AI_UI_SPEAKING,
} starter_ai_ui_phase_t;

/** Lightweight status preview. Full captions use the separate PSRAM-backed API. */
typedef struct {
    starter_ai_ui_phase_t ai_phase;
    bool caption_is_ai;
    bool caption_final;
    char subtitle[193];
    char emotion[16];
    bool call_incoming;
    bool call_wechat;
    bool call_microphone_muted;
    bool call_video;
    bool call_camera_enabled;
    uint8_t contact_count;
    char call_peer[65];
    char call_result[33];
    starter_product_contact_t contacts[STARTER_PRODUCT_CONTACTS_MAX];
} starter_runtime_product_snapshot_t;

#define STARTER_CAPTION_BYTES 4096U
typedef struct {
    uint32_t revision;
    uint32_t paragraph;
    int utterance_id; /**< -1 when the server omits it. */
    bool is_ai;
    bool final;
    bool truncated;
    char text[STARTER_CAPTION_BYTES];
} starter_runtime_caption_t;

/** Copy only a changed caption; keep this large caller-owned buffer in PSRAM.
 * false also means the short lock was busy: retry on the next UI tick. */
bool starter_runtime_caption_read(starter_runtime_caption_t *in_out);

/**
 * 创建状态任务并注册平台/TiRTC 回调。必须在 platform_client_start() 和
 * starter_tirtc_start() 前完成；重复调用安全。
 */
esp_err_t starter_runtime_start(const char *device_id);

/**
 * 在 TiRTC 已完成启动、MQTT 尚未创建前保留下一次外连所需的连续内部堆。
 * 连接门禁会在销毁 MQTT 后立即释放该块；调用方仅在启动编排阶段调用一次。
 */
esp_err_t starter_runtime_arm_external_connect_reserve(void);

/** 非阻塞请求启动 AI 对讲；返回值只表示事件是否成功入队。 */
esp_err_t starter_runtime_ai_start(void);
esp_err_t starter_runtime_ai_start_from_wake(uint32_t wake_token);

/** 非阻塞请求结束 AI 对讲；返回值只表示事件是否成功入队。 */
esp_err_t starter_runtime_ai_stop(void);

/** 联系人同步与纯语音呼叫控制，均只把意图投递给统一状态任务。 */
esp_err_t starter_runtime_contacts_refresh(void);
esp_err_t starter_runtime_call_contact(uint8_t index);
#if CONFIG_IDF_TARGET_ESP32P4
esp_err_t starter_runtime_call_contact_video(uint8_t index);
esp_err_t starter_runtime_call_set_camera_enabled(uint32_t session_generation, bool enabled);
#endif
esp_err_t starter_runtime_call_accept(void);
esp_err_t starter_runtime_call_reject(void);
esp_err_t starter_runtime_call_hangup(void);
esp_err_t starter_runtime_call_set_microphone_muted(bool muted);

/** 返回线程安全的瞬时状态快照。 */
starter_runtime_status_t starter_runtime_status(void);

/** 复制当前 AI 字幕/阶段；内部使用互斥量，不返回运行时持有的指针。 */
starter_runtime_product_snapshot_t starter_runtime_product_snapshot(void);

/** Copies the immutable, provisioned device ID for product diagnostics. */
void starter_runtime_copy_device_id(char *out, size_t out_size);

/** 返回用于日志和串口状态输出的静态状态名称。 */
const char *starter_runtime_state_name(starter_runtime_state_t state);

#ifdef __cplusplus
}
#endif

#endif
