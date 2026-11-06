#ifndef STARTER_VOICE_H
#define STARTER_VOICE_H

/**
 * @file starter_voice.h
 * @brief 小钛本地唤醒与开发调试语音协议。
 *
 * Voicute TFLite 专用“你好小钛”唤醒。独立任务处理最新连续窗口，
 * 不拥有 I2S、网络会话或 UI；旧意图类型仅供现有手动控制接口使用。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STARTER_VOICE_CALL_TARGET_MAX 64

typedef enum {
    STARTER_VOICE_INTENT_NONE = 0,
    STARTER_VOICE_INTENT_WAKE_ONLY,
    STARTER_VOICE_INTENT_OPEN_SETTINGS,
    STARTER_VOICE_INTENT_VOLUME_UP,
    STARTER_VOICE_INTENT_VOLUME_DOWN,
    STARTER_VOICE_INTENT_MUTE_UPLINK,
    STARTER_VOICE_INTENT_UNMUTE_UPLINK,
    STARTER_VOICE_INTENT_OPEN_CONTACTS,
    STARTER_VOICE_INTENT_REST,
    STARTER_VOICE_INTENT_START_AI_CHAT,
    STARTER_VOICE_INTENT_CALL_CONTACT_REMOTE,
} starter_voice_intent_t;

typedef struct {
    starter_voice_intent_t intent;
    int64_t captured_ms; /**< Acoustic frame end; zero for console intents. */
    uint32_t wake_token; /**< Media-owned preroll request; zero for manual start. */
    bool acoustic_score_valid; /**< Immutable accepted-inference diagnostics. */
    uint16_t probability_milli;
    uint16_t threshold_milli;
    /** 仅 CALL_CONTACT_REMOTE 使用；UTF-8，必须由本地通讯录再次校验。 */
    char call_target[STARTER_VOICE_CALL_TARGET_MAX + 1];
} starter_voice_result_t;

typedef void (*starter_voice_result_handler_t)(
    const starter_voice_result_t *result,
    void *user_data);

/**
 * 解析以“你好小钛”“小钛小钛”或“小钛同学”开头的中文文本。后续内容不改变本地行为：
 * 统一发起远程 AI 对话。
 */
esp_err_t starter_voice_parse(const char *text, starter_voice_result_t *result);

/**
 * 异步启动一次；ESP_OK 表示任务已创建，ready() 表示模型已就绪。
 */
esp_err_t starter_voice_start(starter_voice_result_handler_t handler,
                              void *user_data);

/**
 * 有界、非阻塞复制 AEC 后的单声道 16 kHz PCM；不会在调用者执行推理。
 */
esp_err_t starter_voice_feed_pcm16k(const int16_t *pcm, size_t samples,
                                   int64_t captured_ms);

/** 暂停/恢复；切换时清空窗口并使在途推理结果失效。 */
void starter_voice_set_listening(bool enabled);

typedef struct {
    bool ready;
    int init_error; /**< Most recent initialization error; zero on successful self-test. */
    bool listening;
    uint32_t fed;
    uint32_t dropped;
    uint32_t stale;
    uint32_t resets;
    uint32_t detections;
    uint32_t max_age_ms;
    uint32_t max_detect_us;
    unsigned threshold_milli;
    bool ns_vad;
    /* [DEBUG-wake49] Model invocations/results are distinct from dispatched detections. */
    uint32_t model_frames, model_timeouts, model_detected;
    uint32_t skipped_unarmed, skipped_cooldown;
    uint32_t reject_epoch, reject_age, reject_vad, reject_id;
    uint32_t detect_avg_us, detect_over_budget;
    int last_raw_id;
    uint32_t last_raw_probability_milli, last_raw_at_ms;
} starter_voice_status_t;
starter_voice_status_t starter_voice_status(void);
/* [DEBUG-wake50] Bounded metadata only; no PCM retained. Reason bits may combine. */
#define STARTER_VOICE_TRACE_DEPTH 16U
enum {
    VOICE_TRACE_CONFIG = 1U, VOICE_TRACE_CONTROL = 2U,
    VOICE_TRACE_SAMPLES = 4U, VOICE_TRACE_EPOCH = 8U,
    VOICE_TRACE_DISABLED = 16U, VOICE_TRACE_AGE = 32U,
    VOICE_TRACE_CLOCK = 64U, VOICE_TRACE_SEQUENCE = 128U,
    VOICE_TRACE_GAP = 256U,
};
typedef struct {
    uint32_t serial, at_ms, captured_ms, sequence, reasons;
    int32_t age_ms, gap_ms;
    uint32_t queued, reset;
} starter_voice_trace_event_t;
typedef struct {
    uint32_t total, count, queued, queue_peak, dequeue_age_ms;
    starter_voice_trace_event_t events[STARTER_VOICE_TRACE_DEPTH];
} starter_voice_trace_t;
void starter_voice_trace(starter_voice_trace_t *out);
/** 临时阈值 50..990；ns_vad 必须为 true（固定使用 AEC clean 输入）。 */
esp_err_t starter_voice_configure(unsigned threshold_milli, bool ns_vad);

/** 模型已完成分配和张量验证；不代表已经通过声学准确率测试。 */
bool starter_voice_ready(void);

/** 返回用于日志和开发控制台的静态意图名称。 */
const char *starter_voice_intent_name(starter_voice_intent_t intent);

#ifdef __cplusplus
}
#endif

#endif
