/*
 * H5/AI 会话状态机。
 *
 *                 H5 入站
 *   WAITING ----------------------> H5_ACTIVE
 *      ^                               |
 *      | 断连/AI 结束/失败             | AI start 抢占
 *      |                               v
 *      +------ AI_ACTIVE <------ AI_CONNECTING
 *                    start_session 成功
 *
 * s_task 是状态机唯一写入者。TiRTC、MQTT 和 HTTP 回调只把有界事件投递到
 * s_queue；这样连接互斥、超时、资源释放和迟到回调过滤都在一个任务内顺序执行。
 * 对外状态使用原子快照，供串口或产品 UI 无锁读取。
 */
#include "starter_runtime.h"

#include <ctype.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "platform_client.h"
#include "starter_media.h"
#include "starter_tirtc.h"
#include "wifi_manager.h"

#define RUNTIME_QUEUE_DEPTH 12U
#define RUNTIME_TEXT_MAX 4096U
#define ROOM_COMMAND_MAX (40U * 1024U)
/*
 * AI token/command JSON handling and call signalling share this state-owner
 * task.  Hardware high-water telemetry reached 428 bytes with the former
 * 13 KiB stack during an ordinary AI session, so keep enough PSRAM-backed
 * headroom for the deepest supported callback chain and future SDK changes.
 */
#define RUNTIME_TASK_STACK_BYTES 24576U
#define VOIP_CONNECT_TASK_STACK_BYTES (24U * 1024U)
#define EXTERNAL_CONNECT_RESERVE_BYTES (17U * 1024U)
#define AI_PEER_ID_MAX 1024U
#define AI_TOKEN_MAX 1024U
#define AI_COMMAND 0x2100U
#define AI_REQUEST_TIMEOUT_MS 17000
#define AI_CONNECT_TIMEOUT_MS 12000
#define AI_RESPONSE_TIMEOUT_MS 10000
#define CALL_PENDING_TIMEOUT_MS 45000
#define CALL_CONNECT_TIMEOUT_MS 30000
#define VOIP_CONNECTED_WAIT_TIMEOUT_MS 35000
#define VOIP_PROFILE_RETRY_MS 15000
#define DEVICE_PROFILE_MAX_ATTEMPTS 3U
#define CALL_COMMAND_CONNECT 0x2000U
#define CALL_COMMAND_HANGUP 0x2001U

/* 所有异步来源都归一成事件，由 runtime_task 串行处理。 */
typedef enum {
    EVENT_TIRTC_STATE = 0, /* SDK 启停通知。 */
    EVENT_CONNECTION,      /* H5 入站或 AI 外连的连接结果。 */
    EVENT_COMMAND,         /* TiRTC 控制命令，当前用于 AI JSON-RPC。 */
    EVENT_AI_START,        /* 产品控制意图。 */
    EVENT_AI_STOP,         /* 产品控制意图。 */
    EVENT_AI_TOKEN,        /* /v1/ai/token 的异步响应。 */
    EVENT_PLATFORM_SIGNAL, /* MQTT 设备信令，例如 unbind。 */
    EVENT_PLATFORM_ONLINE, /* MQTT 已订阅且 HTTP worker 就绪。 */
    EVENT_DEVICE_PROFILE,
    EVENT_VOIP_CONNECT,
    EVENT_VOIP_CONNECT_RESULT,
    EVENT_CONTACTS_REFRESH,
    EVENT_CONTACTS_RESULT,
    EVENT_CALL_DIAL,
    EVENT_CALL_ACCEPT,
    EVENT_CALL_REJECT,
    EVENT_CALL_HANGUP,
    EVENT_CALL_MIC_MUTE,
    EVENT_CALL_CAMERA,
    EVENT_CALL_HTTP,
    EVENT_CONTACT_QUERY_RESULT,
    EVENT_ROOM_INTENT,
    EVENT_ROOM_HTTP,
} runtime_event_type_t;

typedef enum {
    CALL_HTTP_DEVICE_DIAL = 1,
    CALL_HTTP_DEVICE_INFO,
    CALL_HTTP_VOIP_DIAL,
    CALL_HTTP_ROOM_QUERY,
    CALL_HTTP_ROOM_RELEASE,
    CALL_HTTP_ROOM_VERIFY,
} call_http_stage_t;

/*
 * 固定大小的事件头进入 FreeRTOS 队列。text 在回调中按需复制到堆上，所有权
 * 随事件转移给 runtime_task，并由 release_event() 统一释放。
 */
typedef struct {
    runtime_event_type_t type;  /* 选择事件解释方式。 */
    starter_tirtc_mode_t mode;  /* 事件所属 H5/AI 模式。 */
    uint32_t generation;        /* TiRTC 连接代次。 */
    uint32_t request_tag;       /* 发起 AI 请求时的业务会话代次。 */
    uint32_t platform_epoch;    /* Reject HTTP/MQTT data from a previous owner. */
    uint32_t command;           /* TiRTC 命令号。 */
    uint32_t length;            /* text 有效字节数，不含结尾 NUL。 */
    bool flag;                  /* started/connected 等布尔结果。 */
    int error;                  /* ESP-IDF 或 TiRTC 错误码。 */
    char *text;                 /* 可选堆内存，消费后必须释放。 */
} runtime_event_t;

/* WHIP URL 和 token 较大，不能与 TiRtcWhipConnect 的大栈帧叠加。 */
typedef struct {
    char peer_id[AI_PEER_ID_MAX];
    char token[AI_TOKEN_MAX];
} ai_credentials_t;

/* TiRtcWhipConnect 的 TLS/SDP 路径需要远大于 session 状态机的栈。
 * 请求副本由工作任务独占，取消或新会话发生时以 generation 丢弃迟到结果。 */
typedef struct {
    uint32_t generation;
    char peer_id[AI_PEER_ID_MAX];
    char token[AI_TOKEN_MAX];
} voip_connect_request_t;

static const char *TAG = "starter_runtime";
static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_product_mutex;
static EXT_RAM_BSS_ATTR starter_runtime_product_snapshot_t s_product_snapshot;
/* Do not enlarge the by-value status snapshot or any task's internal stack. */
static EXT_RAM_BSS_ATTR starter_runtime_caption_t s_caption;
static void *s_external_connect_reserve;

/* 下列字段只由 runtime_task 读写，不需要加锁。 */
static char s_device_id[65];
static char s_ai_role_id[65];
static char s_ai_request_id[24];
static int64_t s_deadline_ms;
static uint32_t s_session_generation;
static uint32_t s_connection_generation;
static int64_t s_connect_reserve_retry_ms;
static char s_call_room_id[129];
/* A reboot loses the local room, not the server's device-room lock. Recovery
 * owns a separate ID so late room_cancel cannot end the new outgoing call.
 * This bounded transaction reuses the HTTP worker and the PSRAM runtime stack. */
static EXT_RAM_BSS_ATTR struct {
    char room_id[129];
    call_http_stage_t stage;
    bool attempted;
} s_call_room_recovery;
static char s_call_peer_id[65];
static char s_call_peer_name[65];
static char s_call_wx_app_id[65];
static char s_call_wx_model_id[65];
static EXT_RAM_BSS_ATTR char s_call_connect_peer[AI_PEER_ID_MAX];
static EXT_RAM_BSS_ATTR char s_call_connect_token[AI_TOKEN_MAX];
static EXT_RAM_BSS_ATTR char s_call_wx_session_token[257];
static EXT_RAM_BSS_ATTR char s_call_wx_payload[513];
static char s_call_id[65];
static bool s_call_wechat;
#if CONFIG_IDF_TARGET_ESP32P4
static bool s_call_video;
static bool s_call_camera_enabled;
#endif
static bool s_call_outgoing;
static bool s_call_waiting_confirm;
static bool s_voip_connect_inflight;
/*
 * 设备互呼的业务接听通知（MQTT callee_answered）与 P2P 入站连接是两条
 * 独立的异步链路。不能把 0x2000 当成唯一的开通条件：该命令只是兼容性
 * 确认，可靠的建立条件是“同一代次的 P2P 已连接 + 对端已业务接听”。
 */
static bool s_call_peer_answered;
static bool s_call_p2p_connected;
/* The unified device report also establishes the server's WeChat profile.
 * Keep its readiness gate separate from AI/H5 and the usable home screen. */
static EXT_RAM_BSS_ATTR bool s_voip_profile_ready;
static EXT_RAM_BSS_ATTR bool s_voip_profile_inflight;
static EXT_RAM_BSS_ATTR int64_t s_voip_profile_retry_at_ms;
static EXT_RAM_BSS_ATTR unsigned s_device_profile_attempts;
/* epoch + 1; zero means no lost result. Only the HTTP worker writes failures. */
static EXT_RAM_BSS_ATTR atomic_uint s_voip_profile_delivery_failed;

/* 供其他任务读取的公开快照，只能由 publish_state() 更新。 */
static atomic_int s_public_state;
static atomic_uint_fast32_t s_public_session_generation;
static atomic_uint_fast32_t s_public_connection_generation;
static atomic_int s_last_error;

static portMUX_TYPE s_diagnostic_lock = portMUX_INITIALIZER_UNLOCKED;
static starter_runtime_diagnostics_t s_diagnostics;
static int64_t s_diagnostic_ai_started_ms;

static void diagnostic_event(const char *label, int value)
{
    starter_diagnostic_event_t event = {
        .at_ms = (uint32_t)(esp_timer_get_time() / 1000),
        .session = s_session_generation, .value = value,
    };
    (void)snprintf(event.label, sizeof(event.label), "%s", label);
    /* Only a fixed, small memory copy inside the critical section; no I/O. */
    portENTER_CRITICAL(&s_diagnostic_lock);
    if (s_diagnostics.count == STARTER_DIAGNOSTIC_EVENTS) {
        memmove(s_diagnostics.events, s_diagnostics.events + 1,
                (STARTER_DIAGNOSTIC_EVENTS - 1U) * sizeof(event));
        --s_diagnostics.count;
    }
    s_diagnostics.events[s_diagnostics.count++] = event;
    portEXIT_CRITICAL(&s_diagnostic_lock);
}

void starter_runtime_diagnostics(starter_runtime_diagnostics_t *out)
{
    if (out == NULL) return;
    portENTER_CRITICAL(&s_diagnostic_lock);
    *out = s_diagnostics;
    portEXIT_CRITICAL(&s_diagnostic_lock);
}

/*
 * SDK/MQTT 回调不能阻塞等待队列。关键事件入队失败时设置恢复标志，由状态
 * 任务执行断连或重新核验平台身份，避免永久停留在错误状态。
 */
static atomic_bool s_transport_recovery_required;
static atomic_bool s_platform_reconcile_required;
static void room_before_finish(int error);
static bool room_owns_media(void);
static void room_reset(void);
static void room_transport_lost(uint32_t generation, uint32_t request_tag);
static void reconcile_platform_binding(bool known_unbound);

static void product_snapshot_reset(void)
{
    if (s_product_mutex == NULL ||
        xSemaphoreTake(s_product_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    starter_product_contact_t contacts[STARTER_PRODUCT_CONTACTS_MAX];
    uint8_t contact_count = s_product_snapshot.contact_count;
    s_caption.text[0] = '\0';
    s_caption.utterance_id = -1;
    s_caption.truncated = false;
    s_caption.is_ai = false;
    s_caption.final = false;
    ++s_caption.revision;
    ++s_caption.paragraph;
    memcpy(contacts, s_product_snapshot.contacts, sizeof(contacts));
    s_product_snapshot = (starter_runtime_product_snapshot_t) {
        .ai_phase = STARTER_AI_UI_IDLE,
        .contact_count = contact_count,
    };
    memcpy(s_product_snapshot.contacts, contacts, sizeof(contacts));
    (void)snprintf(s_product_snapshot.emotion,
                   sizeof(s_product_snapshot.emotion),
                   "calm");
    xSemaphoreGive(s_product_mutex);
}

static void product_set_call(bool incoming,
                             bool wechat,
                             const char *peer,
                             bool microphone_muted)
{
    if (s_product_mutex != NULL &&
        xSemaphoreTake(s_product_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_product_snapshot.call_incoming = incoming;
        s_product_snapshot.call_wechat = wechat;
        s_product_snapshot.call_microphone_muted = microphone_muted;
#if CONFIG_IDF_TARGET_ESP32P4
        s_product_snapshot.call_video = s_call_video;
        s_product_snapshot.call_camera_enabled = s_call_camera_enabled;
#endif
        (void)snprintf(s_product_snapshot.call_peer,
                       sizeof(s_product_snapshot.call_peer),
                       "%s",
                       peer == NULL ? "" : peer);
        xSemaphoreGive(s_product_mutex);
    }
}

static void product_set_call_result(const char *result)
{
    if (result != NULL && s_product_mutex != NULL &&
        xSemaphoreTake(s_product_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        (void)snprintf(s_product_snapshot.call_result,
                       sizeof(s_product_snapshot.call_result), "%s", result);
        xSemaphoreGive(s_product_mutex);
    }
}

static void product_set_phase(starter_ai_ui_phase_t phase)
{
    if (s_product_mutex != NULL &&
        xSemaphoreTake(s_product_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_product_snapshot.ai_phase = phase;
        xSemaphoreGive(s_product_mutex);
    }
}

static bool supported_emotion(const char *emotion)
{
    static const char *const allowed[] = {
        "neutral", "happy", "laughing", "funny", "sad", "angry", "crying",
        "loving", "embarrassed", "surprised", "shocked", "thinking",
        "winking", "cool", "relaxed", "delicious", "kissy", "confident",
        "sleepy", "silly", "confused", "listening", "ambient", "speech",
        "calm", "excited", "curious", "proud", "moved",
    };
    if (emotion == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); ++i) {
        if (strcmp(emotion, allowed[i]) == 0) {
            return true;
        }
    }
    return false;
}

static const char *normalize_emotion(const char *emotion)
{
    /* caption.emotion is a Chinese protocol value, not a renderer pose name.
     * Keep legacy English tags compatible and reuse the existing face family. */
    static const struct { const char *wire; const char *pose; } names[] = {
        {"中性", "neutral"}, {"开心", "happy"}, {"兴奋", "excited"},
        {"温和", "calm"}, {"安慰", "loving"}, {"思考", "thinking"},
        {"惊讶", "surprised"}, {"严肃", "confident"},
        {"困倦", "sleepy"}, {"困惑", "confused"},
    };
    if (emotion == NULL) return NULL;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (strcmp(emotion, names[i].wire) == 0) return names[i].pose;
    }
    return supported_emotion(emotion) ? emotion : NULL;
}

/* Copy whole UTF-8 code points, including when a bounded preview fills up. */
static bool caption_copy(char *destination, size_t capacity, const char *source)
{
    size_t length = strlen(source);
    size_t used = length < capacity ? length : capacity - 1U;
    if (used < length) {
        while (used > 0 && ((unsigned char)source[used] & 0xC0U) == 0x80U) --used;
    }
    memcpy(destination, source, used);
    destination[used] = '\0';
    return used < length;
}

bool starter_runtime_caption_read(starter_runtime_caption_t *in_out)
{
    if (in_out == NULL || s_product_mutex == NULL ||
        xSemaphoreTake(s_product_mutex, 0) != pdTRUE) return false;
    bool changed = in_out->revision != s_caption.revision;
    if (changed) *in_out = s_caption;
    xSemaphoreGive(s_product_mutex);
    return changed;
}

static void product_set_caption(bool is_ai,
                                bool final,
                                const char *text,
                                bool append,
                                const char *emotion,
                                int utterance_id)
{
    if (text == NULL || s_product_mutex == NULL ||
        xSemaphoreTake(s_product_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    const bool starts_ai_reply = is_ai && !s_product_snapshot.caption_is_ai;
    bool new_group = s_caption.text[0] == '\0' || is_ai != s_caption.is_ai ||
                     utterance_id != s_caption.utterance_id;
    s_product_snapshot.caption_is_ai = is_ai;
    s_product_snapshot.caption_final = final;
    /* The deployed ASR sends the current whole hypothesis even with mode=1,
     * including revisions of earlier words. Replace it, as in the legacy ASR
     * consumer; prefix/overlap dedup cannot handle those revisions correctly.
     * TTS still follows mode (full/delta). Never remove intentional repeats. */
    if (!is_ai || !append || new_group) {
        (void)snprintf(s_caption.text, sizeof(s_caption.text), "%s", is_ai ? "AI：" : "你：");
        s_caption.truncated = false;
    }
    if (new_group || (!append && utterance_id < 0)) ++s_caption.paragraph;
    size_t used = strlen(s_caption.text);
    bool overflow = caption_copy(s_caption.text + used, sizeof(s_caption.text) - used, text);
    if (overflow && !s_caption.truncated) {
        ESP_LOGW(TAG, "caption capacity exceeded: utterance=%d ai=%d capacity=%u",
                 utterance_id, is_ai, (unsigned)sizeof(s_caption.text));
    }
    s_caption.truncated |= overflow;
    s_caption.utterance_id = utterance_id;
    s_caption.is_ai = is_ai;
    s_caption.final = final;
    ++s_caption.revision;
    (void)caption_copy(s_product_snapshot.subtitle, sizeof(s_product_snapshot.subtitle), s_caption.text);
    /* Only TTS owns the reply's mood. Missing/invalid metadata cannot discard
     * a caption or let an ASR hypothesis repaint the previous AI response. */
    const char *pose = is_ai ? normalize_emotion(emotion) : NULL;
    if (pose != NULL) {
        (void)snprintf(s_product_snapshot.emotion,
                       sizeof(s_product_snapshot.emotion),
                       "%s",
                       pose);
    } else if (starts_ai_reply) {
        /* Missing emotion is not a new emotion. Start an untagged reply calmly,
         * then retain its last explicit mood across sentence-final captions. */
        (void)snprintf(s_product_snapshot.emotion,
                       sizeof(s_product_snapshot.emotion),
                       "%s",
                       "neutral");
    }
    xSemaphoreGive(s_product_mutex);
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void publish_state(starter_runtime_state_t state)
{
    starter_media_set_wake_allowed(state == STARTER_RUNTIME_WAITING);
    /* 这些原子值用于诊断快照；业务判断始终留在 runtime_task 内。 */
    starter_runtime_state_t previous = (starter_runtime_state_t)atomic_exchange_explicit(
        &s_public_state, state, memory_order_acq_rel);
    if (previous != state) {
        if (state == STARTER_RUNTIME_CALL_ACTIVE) diagnostic_event("call active", 0);
        /* 通话 UI 只消费该快照；保留边沿日志可直接定位是谁提前结束通话。 */
        ESP_LOGI(TAG, "runtime state %d -> %d session=%lu connection=%lu",
                 (int)previous, (int)state,
                 (unsigned long)s_session_generation,
                 (unsigned long)s_connection_generation);
    }
    atomic_store_explicit(&s_public_session_generation,
                          s_session_generation,
                          memory_order_release);
    atomic_store_explicit(&s_public_connection_generation,
                          s_connection_generation,
                          memory_order_release);
    ESP_LOGI(TAG,
             "state=%s session=%lu connection=%lu",
             starter_runtime_state_name(state),
             (unsigned long)s_session_generation,
             (unsigned long)s_connection_generation);
}

static bool queue_event(const runtime_event_t *event)
{
    /* 回调不能等待；队列满时由事件生产者记录并丢弃。 */
    return s_queue != NULL && event != NULL &&
           xQueueSend(s_queue, event, 0) == pdTRUE;
}

static bool copy_event_text(runtime_event_t *event,
                            const void *text,
                            size_t length)
{
    /* 加结尾 NUL 便于 JSON 解析，但 length 仍保留协议原始长度。 */
    if (event == NULL || (length > 0U && text == NULL) ||
        length > (event != NULL && event->mode == STARTER_TIRTC_ROOM
                      ? ROOM_COMMAND_MAX : RUNTIME_TEXT_MAX)) {
        return false;
    }
    event->text = heap_caps_malloc(length + 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (event->text == NULL) {
        return false;
    }
    if (length > 0U) {
        memcpy(event->text, text, length);
    }
    event->text[length] = '\0';
    event->length = (uint32_t)length;
    return true;
}

static void release_event(runtime_event_t *event)
{
    if (event != NULL) {
        free(event->text);
        event->text = NULL;
    }
}

static bool prepare_external_connect(void)
{
    if (s_external_connect_reserve == NULL) {
        ESP_LOGE(TAG, "external connect reserve is not armed");
        return false;
    }
    /* MQTT carries callee_answered, hangup, unbind and room membership even
     * while connecting. Release only the SDK's reserved contiguous block;
     * never destroy the signalling transport to make a media connection. */
    heap_caps_free(s_external_connect_reserve);
    s_external_connect_reserve = NULL;
    s_connect_reserve_retry_ms = 0;
    ESP_LOGI(TAG,
             "realtime connect gate opened: internal-free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                                MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                        MALLOC_CAP_8BIT));
    return true;
}

static void restore_external_connect_reserve(void)
{
    if (s_external_connect_reserve != NULL) {
        s_connect_reserve_retry_ms = 0;
        return;
    }
    /* This prepares a future call, not the current signalling/media path.
     * Allocation failure stays visible, without postponing MQTT recovery. */
    s_connect_reserve_retry_ms = starter_runtime_arm_external_connect_reserve() == ESP_OK
                                    ? 0 : now_ms() + 5000;
}

esp_err_t starter_runtime_arm_external_connect_reserve(void)
{
    if (s_external_connect_reserve != NULL) {
        return ESP_OK;
    }
    s_external_connect_reserve = heap_caps_malloc(
        EXTERNAL_CONNECT_RESERVE_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_external_connect_reserve == NULL) {
        ESP_LOGW(TAG,
                 "cannot reserve %u-byte contiguous internal heap for external connect",
                 EXTERNAL_CONNECT_RESERVE_BYTES);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG,
             "reserved %u internal bytes for next TiRTC external connect",
             EXTERNAL_CONNECT_RESERVE_BYTES);
    return ESP_OK;
}

static void finish_session(int error)
{
    room_before_finish(error);
    diagnostic_event("session end/error", error);
    /* 所有退出路径汇聚到这里，确保媒体、连接、超时和 H5 门禁一起复位。 */
    starter_media_stop();
    /* Also invalidates pending external requests / inbound call expectations.
     * No established handle is normal while cancelling a connecting session. */
    (void)starter_tirtc_disconnect();
    starter_tirtc_accept_h5(platform_client_ready());
    s_connection_generation = 0;
    s_deadline_ms = 0;
    s_ai_role_id[0] = '\0';
    s_ai_request_id[0] = '\0';
    s_call_room_id[0] = '\0';
    memset(&s_call_room_recovery, 0, sizeof(s_call_room_recovery));
    s_call_peer_id[0] = '\0';
    s_call_peer_name[0] = '\0';
    s_call_wx_app_id[0] = '\0';
    s_call_wx_model_id[0] = '\0';
    s_call_connect_peer[0] = '\0';
    s_call_connect_token[0] = '\0';
    s_call_wx_session_token[0] = '\0';
    s_call_wx_payload[0] = '\0';
    s_call_id[0] = '\0';
    s_call_wechat = false;
#if CONFIG_IDF_TARGET_ESP32P4
    s_call_video = false;
    s_call_camera_enabled = false;
#endif
    s_call_outgoing = false;
    s_call_waiting_confirm = false;
    s_voip_connect_inflight = false;
    s_call_peer_answered = false;
    s_call_p2p_connected = false;
    atomic_store_explicit(&s_last_error, error, memory_order_release);
    product_snapshot_reset();
    product_set_call(false, false, "", false);
    publish_state(STARTER_RUNTIME_WAITING);
    restore_external_connect_reserve();
}

static void finish_call_session(int error, const char *result)
{
    ESP_LOGW(TAG,
             "[DEBUG-call] finish generation=%lu error=%d result=%s",
             (unsigned long)s_session_generation,
             error,
             result == NULL ? "" : result);
    finish_session(error);
    product_set_call_result(result);
}

static bool copy_json_string(const cJSON *object,
                             const char *name,
                             char *destination,
                             size_t capacity,
                             bool required)
{
    const cJSON *item = cJSON_IsObject(object)
                            ? cJSON_GetObjectItemCaseSensitive(object, name)
                            : NULL;
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        if (!required && destination != NULL && capacity > 0U) {
            destination[0] = '\0';
            return true;
        }
        return false;
    }
    size_t length = strlen(item->valuestring);
    if ((required && length == 0U) || length >= capacity) {
        return false;
    }
    memcpy(destination, item->valuestring, length + 1U);
    return true;
}

static void request_ai_token_response(const char *body, void *user_data)
{
    /* HTTP 回调运行在 platform_client 请求任务中，只复制响应后立即返回。 */
    runtime_event_t event = {
        .type = EVENT_AI_TOKEN,
        .platform_epoch = platform_client_response_epoch(),
        .request_tag = (uint32_t)(uintptr_t)user_data,
    };
    size_t length = body == NULL ? 0U : strnlen(body, RUNTIME_TEXT_MAX + 1U);
    if (length > RUNTIME_TEXT_MAX || !copy_event_text(&event, body, length)) {
        ESP_LOGE(TAG, "AI token response is too large or cannot be copied");
        return;
    }
    if (!queue_event(&event)) {
        ESP_LOGE(TAG, "AI token response dropped: runtime queue is full");
        release_event(&event);
    }
}

static bool queue_http_result(runtime_event_type_t type,
                              uint32_t request_tag,
                              uint32_t stage,
                              const char *body)
{
    runtime_event_t event = {
        .type = type,
        .platform_epoch = platform_client_response_epoch(),
        .request_tag = request_tag,
        .command = stage,
    };
    size_t length = body == NULL ? 0U : strnlen(body, RUNTIME_TEXT_MAX + 1U);
    if (type == EVENT_CALL_HTTP) {
        ESP_LOGI(TAG,
                 "[DEBUG-call] HTTP callback stage=%lu generation=%lu bytes=%u",
                 (unsigned long)stage,
                 (unsigned long)request_tag,
                 (unsigned)length);
    }
    if (length > RUNTIME_TEXT_MAX || !copy_event_text(&event, body, length) ||
        !queue_event(&event)) {
        ESP_LOGE(TAG, "product HTTP response dropped type=%d stage=%lu",
                 (int)type, (unsigned long)stage);
        release_event(&event);
        return false;
    }
    return true;
}

static void contacts_response(const char *body, void *user_data)
{
    (void)user_data;
    queue_http_result(EVENT_CONTACTS_RESULT, 0, 0, body);
}

static void device_profile_response(const char *body, void *user_data)
{
    (void)user_data;
    if (platform_client_response_epoch() != platform_client_epoch()) return;
    if (!queue_http_result(EVENT_DEVICE_PROFILE, 0,
                           (uint32_t)platform_client_response_status(), body)) {
        atomic_store_explicit(&s_voip_profile_delivery_failed,
                              platform_client_response_epoch() + 1U,
                              memory_order_release);
    }
}

static void call_http_response(const char *body, void *user_data)
{
    uintptr_t encoded = (uintptr_t)user_data;
    (void)queue_http_result(EVENT_CALL_HTTP,
                            (uint32_t)(encoded >> 8),
                            (uint32_t)(encoded & 0xffU),
                            body);
}

static bool call_signal_matches_room(bool got_room, const char *room,
                                     const char *current_room)
{
    return got_room && room != NULL && current_room != NULL && room[0] != '\0' &&
           current_room[0] != '\0' && strcmp(room, current_room) == 0;
}

static bool recover_voip_profile_delivery_failure(int64_t current_ms)
{
    unsigned failed_epoch = atomic_exchange_explicit(&s_voip_profile_delivery_failed, 0,
                                                     memory_order_acq_rel);
    if (failed_epoch == 0 || failed_epoch != platform_client_epoch() + 1U) {
        return false;
    }
    s_voip_profile_inflight = false;
    s_voip_profile_ready = false;
    s_voip_profile_retry_at_ms = s_device_profile_attempts < DEVICE_PROFILE_MAX_ATTEMPTS
                                    ? current_ms + VOIP_PROFILE_RETRY_MS : 0;
    return true;
}

static void *call_http_tag(uint32_t generation, call_http_stage_t stage)
{
    return (void *)(uintptr_t)(((uintptr_t)generation << 8) | (uintptr_t)stage);
}

static void on_tirtc_started(bool started, int error, void *user_data)
{
    /* SDK 回调：只投递标量事件。 */
    (void)user_data;
    const runtime_event_t event = {
        .type = EVENT_TIRTC_STATE,
        .flag = started,
        .error = error,
    };
    if (!queue_event(&event)) {
        ESP_LOGE(TAG, "TiRTC state event dropped; scheduling session recovery");
        atomic_store_explicit(&s_transport_recovery_required,
                              true,
                              memory_order_release);
    }
}

static void on_tirtc_connection(starter_tirtc_mode_t mode,
                                uint32_t generation,
                                uint32_t request_tag,
                                bool connected,
                                int error,
                                void *user_data)
{
    /* generation 过滤连接迟到回调，request_tag 过滤 AI 请求迟到回调。 */
    (void)user_data;
    const runtime_event_t event = {
        .type = EVENT_CONNECTION,
        .mode = mode,
        .generation = generation,
        .request_tag = request_tag,
        .flag = connected,
        .error = error,
    };
    if (!queue_event(&event)) {
        ESP_LOGE(TAG, "connection event dropped; scheduling session recovery");
        if (mode == STARTER_TIRTC_ROOM) room_transport_lost(generation, request_tag);
        else atomic_store_explicit(&s_transport_recovery_required, true, memory_order_release);
    }
}

static void on_tirtc_command(starter_tirtc_mode_t mode,
                             uint32_t generation,
                             uint32_t command,
                             const void *data,
                             uint32_t length,
                             void *user_data)
{
    /* 命令 payload 的生命周期只到回调返回，因此需要有界复制。 */
    (void)user_data;
    if ((length > 0U && data == NULL) ||
        length > (mode == STARTER_TIRTC_ROOM ? ROOM_COMMAND_MAX : RUNTIME_TEXT_MAX)) {
        ESP_LOGW(TAG, "command 0x%lx is too large", (unsigned long)command);
        if (mode == STARTER_TIRTC_ROOM)
            room_transport_lost(generation, 0);
        return;
    }
    runtime_event_t event = {
        .type = EVENT_COMMAND,
        .mode = mode,
        .generation = generation,
        .command = command,
    };
    if (!copy_event_text(&event, data, length)) {
        ESP_LOGE(TAG, "command 0x%lx cannot be copied", (unsigned long)command);
        if (mode == STARTER_TIRTC_ROOM) room_transport_lost(generation, 0);
        else atomic_store_explicit(&s_transport_recovery_required, true, memory_order_release);
        return;
    }
    if (!queue_event(&event)) {
        ESP_LOGE(TAG, "command event dropped: runtime queue is full");
        release_event(&event);
        if (mode == STARTER_TIRTC_ROOM) room_transport_lost(generation, 0);
        else atomic_store_explicit(&s_transport_recovery_required, true, memory_order_release);
    }
}

static void on_tirtc_audio(starter_tirtc_mode_t mode,
                           uint32_t generation,
                           const starter_tirtc_frame_t *frame,
                           const void *data,
                           void *user_data)
{
    (void)user_data;
    /* 媒体模块使用独立固定队列，不占用会话事件队列的有限容量。 */
    starter_media_submit_audio(mode, generation, frame, data);
}

#if CONFIG_IDF_TARGET_ESP32P4
static void on_tirtc_key_frame(uint32_t generation, void *user_data)
{
    (void)user_data;
    starter_media_request_key_frame(generation);
}

static void on_tirtc_video(starter_tirtc_mode_t mode, uint32_t generation,
                           const starter_tirtc_frame_t *frame, const void *data, void *ctx)
{
    (void)ctx;
    starter_media_submit_video(mode, generation, frame, data);
}
#endif

static void on_platform_signal(const char *json, size_t length, void *user_data)
{
    /* MQTT 回调和 TiRTC 回调遵守相同规则：复制、入队、立即返回。 */
    (void)user_data;
    if (json == NULL || length == 0U) {
        return;
    }
    if (length > RUNTIME_TEXT_MAX) {
        ESP_LOGE(TAG, "platform signal is oversized; scheduling binding check");
        atomic_store_explicit(&s_platform_reconcile_required,
                              true,
                              memory_order_release);
        return;
    }
    runtime_event_t event = {
        .type = EVENT_PLATFORM_SIGNAL,
        .platform_epoch = platform_client_epoch(),
    };
    if (!copy_event_text(&event, json, length)) {
        ESP_LOGE(TAG, "platform signal copy failed; scheduling binding check");
        atomic_store_explicit(&s_platform_reconcile_required,
                              true,
                              memory_order_release);
        return;
    }
    if (!queue_event(&event)) {
        ESP_LOGE(TAG, "platform signal dropped; scheduling binding check");
        release_event(&event);
        atomic_store_explicit(&s_platform_reconcile_required,
                              true,
                              memory_order_release);
    }
}

static void on_platform_online(void *user_data)
{
    (void)user_data;
    const runtime_event_t event = {.type = EVENT_PLATFORM_ONLINE,
                                   .platform_epoch = platform_client_epoch()};
    if (!queue_event(&event)) {
        ESP_LOGE(TAG, "platform online event dropped");
    }
}

static bool response_ok(const cJSON *root)
{
    const cJSON *code = cJSON_IsObject(root)
                            ? cJSON_GetObjectItemCaseSensitive(root, "code")
                            : NULL;
    return cJSON_IsNumber(code) && (code->valueint == 0 || code->valueint == 200);
}

/* ROOM uses the same state owner, platform worker and media pipeline. */
#include "starter_room.inc"

static void refresh_contacts(void)
{
    if (!platform_client_ready()) {
        return;
    }
    esp_err_t err = platform_client_request(PLATFORM_SERVICE_CALL,
                                             "/v1/call/device/contacts",
                                             NULL,
                                             contacts_response,
                                             NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "contacts refresh submission failed: %s", esp_err_to_name(err));
    }
}

static void request_device_profile(void)
{
    if (s_voip_profile_ready || s_voip_profile_inflight || !platform_client_ready() ||
        !platform_client_mqtt_connected() ||
        s_device_profile_attempts >= DEVICE_PROFILE_MAX_ATTEMPTS ||
        (s_voip_profile_retry_at_ms != 0 && now_ms() < s_voip_profile_retry_at_ms)) {
        return;
    }
    /* One immutable, complete snapshot per scenario. These are product paths,
     * not the SDK's codec list: all audio is A-law/8k/mono; P4 H5 and CALL
     * receive H264, while VOIP receives MJPEG (p4_video_submit).
     * Stream IDs are device-relative: up=send 10/11, down=receive 14/15.
     * Constants stay in rodata; the existing PSRAM HTTP pool copies the body. */
#if CONFIG_IDF_TARGET_ESP32P4
    /* H5 already encodes portrait 960x1280; CALL encodes landscape 384x256.
     * Report those ratios without adding a receiver-side rotation or mirror.
     * WeChat alone retains its validated additional CCW90 UI correction.
     * down_video_rotation=0 and video_res_mode=auto explicitly preserve the
     * upstream defaults; local MJPEG rotation/scaling remains unchanged. */
    static const char profile[] =
        "{\"profiles\":{"
        "\"stream\":{\"up_audio_mt\":[\"alaw\"],\"down_audio_mt\":[\"alaw\"],"
        "\"up_audio_streamid\":10,\"up_video_streamid\":11,"
        "\"down_audio_streamid\":14,\"down_video_streamid\":15,"
        "\"up_video_mt\":[\"h264\"],\"down_video_mt\":[\"h264\"],"
        "\"camera_rotation\":0,\"aspect_ratio\":0.75,"
        "\"hor_mirror\":false,\"vert_mirror\":false,\"object_fit\":\"contain\","
        "\"audio_rate\":8000,\"audio_channels\":1,\"no_video\":false},"
        "\"call\":{\"up_audio_mt\":[\"alaw\"],\"down_audio_mt\":[\"alaw\"],"
        "\"up_video_mt\":[\"h264\"],\"down_video_mt\":[\"h264\"],"
        "\"camera_rotation\":0,\"aspect_ratio\":1.5,"
        "\"hor_mirror\":false,\"vert_mirror\":false,\"object_fit\":\"contain\","
        "\"audio_rate\":8000,\"audio_channels\":1,\"no_video\":false},"
        "\"voip\":{\"screen_width\":480,\"screen_height\":320,"
        "\"camera_rotation\":270,\"aspect_ratio\":0.75,"
        "\"down_video_rotation\":0,\"video_res_mode\":\"auto\","
        "\"hor_mirror\":false,\"vert_mirror\":false,\"object_fit\":\"contain\","
        "\"audio_rate\":8000,\"audio_channels\":1,"
        "\"up_video_mt\":\"h264\",\"down_video_mt\":\"mjpeg\","
        "\"down_audio_mt\":\"alaw\",\"no_video\":false,\"calling_timeout_sec\":30}}}";
#else
    static const char profile[] =
        "{\"profiles\":{"
        "\"stream\":{\"up_audio_mt\":[\"alaw\"],\"down_audio_mt\":[\"alaw\"],"
        "\"up_audio_streamid\":10,\"up_video_streamid\":11,"
        "\"down_audio_streamid\":14,\"down_video_streamid\":15,"
        "\"up_video_mt\":[],\"down_video_mt\":[],"
        "\"audio_rate\":8000,\"audio_channels\":1,\"no_video\":true},"
        "\"call\":{\"up_audio_mt\":[\"alaw\"],\"down_audio_mt\":[\"alaw\"],"
        "\"up_video_mt\":[],\"down_video_mt\":[],"
        "\"audio_rate\":8000,\"audio_channels\":1,\"no_video\":true},"
        "\"voip\":{\"screen_width\":1,\"screen_height\":1,"
        "\"audio_rate\":8000,\"audio_channels\":1,"
        "\"up_video_mt\":\"none\",\"down_video_mt\":\"none\","
        "\"down_audio_mt\":\"alaw\",\"no_video\":true,"
        "\"calling_timeout_sec\":30}}}";
#endif
    /* Keep below the existing 2048-byte request slot, including its NUL. */
    _Static_assert(sizeof(profile) <= 2048U, "device profile exceeds HTTP request slot");
    ++s_device_profile_attempts;
    esp_err_t err = platform_client_request_timeout(
        PLATFORM_SERVICE_DEVICE, "/v1/device/profile", profile,
        10000U, device_profile_response, NULL);
    if (err == ESP_OK) {
        s_voip_profile_inflight = true;
        s_voip_profile_retry_at_ms = 0;
        ESP_LOGI(TAG, "device profile queued: scenes=stream,call,voip bytes=%u attempt=%u",
                 (unsigned)(sizeof(profile) - 1U), s_device_profile_attempts);
    } else {
        s_voip_profile_retry_at_ms = s_device_profile_attempts < DEVICE_PROFILE_MAX_ATTEMPTS
                                        ? now_ms() + VOIP_PROFILE_RETRY_MS : 0;
        ESP_LOGW(TAG, "device profile enqueue failed: attempt=%u error=%s",
                 s_device_profile_attempts, esp_err_to_name(err));
    }
}

static void handle_device_profile(const runtime_event_t *event)
{
    s_voip_profile_inflight = false;
    cJSON *root = event->text == NULL ? NULL :
        cJSON_ParseWithLength(event->text, event->length);
    const cJSON *item = root == NULL ? NULL : cJSON_GetObjectItem(root, "code");
    int code = cJSON_IsNumber(item) ? item->valueint : -1;
    if (event->command == 200U && code == 200) {
        s_voip_profile_ready = true;
        s_voip_profile_retry_at_ms = 0;
        ESP_LOGI(TAG, "device profile reported: stream,call,voip; WeChat calling is ready");
        cJSON_Delete(root);
        return;
    }
    cJSON_Delete(root);
    s_voip_profile_ready = false;
    /* Invalid fields/auth are not a transient network failure. Never fall back
     * to the deprecated route or accept its code=0 as a successful report. */
    bool retryable = event->command == 0U || event->command == 429U ||
                     event->command >= 500U ||
                     (event->command == 200U && (code == -1 || code == 50000));
    if (!retryable) s_device_profile_attempts = DEVICE_PROFILE_MAX_ATTEMPTS;
    s_voip_profile_retry_at_ms = retryable && s_device_profile_attempts < DEVICE_PROFILE_MAX_ATTEMPTS
                                    ? now_ms() + VOIP_PROFILE_RETRY_MS : 0;
    ESP_LOGW(TAG, "device profile failed: http=%lu code=%d retry_ms=%u",
             (unsigned long)event->command, code,
             s_voip_profile_retry_at_ms == 0 ? 0U : (unsigned)VOIP_PROFILE_RETRY_MS);
    if (event->command == 410U && code == 6006) reconcile_platform_binding(true);
}

static bool contact_query_current(uint32_t ticket);
static void contact_query_tick(void);
static void complete_contact_query(const runtime_event_t *event, const cJSON *root);

static void handle_contacts_result(const runtime_event_t *event)
{
    if (event->type == EVENT_CONTACT_QUERY_RESULT) {
        contact_query_tick();
        if (!contact_query_current(event->request_tag)) {
            ESP_LOGI(TAG, "AI query HTTP discarded: stale q=%lu", (unsigned long)event->request_tag);
            return;
        }
    }
    cJSON *root = event->text == NULL ? NULL :
        cJSON_ParseWithLength(event->text, event->length);
    if (event->type == EVENT_CONTACT_QUERY_RESULT) complete_contact_query(event, root);
    const cJSON *data = response_ok(root)
                            ? cJSON_GetObjectItemCaseSensitive(root, "data")
                            : NULL;
    const cJSON *contacts = cJSON_IsObject(data)
                                ? cJSON_GetObjectItemCaseSensitive(data, "contacts")
                                : NULL;
    if (!cJSON_IsArray(contacts) || s_product_mutex == NULL ||
        xSemaphoreTake(s_product_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        cJSON_Delete(root);
        return;
    }
    memset(s_product_snapshot.contacts, 0, sizeof(s_product_snapshot.contacts));
    s_product_snapshot.contact_count = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, contacts) {
        if (s_product_snapshot.contact_count >= STARTER_PRODUCT_CONTACTS_MAX) {
            break;
        }
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
        const cJSON *device_id = cJSON_GetObjectItemCaseSensitive(item, "device_id");
        const cJSON *remark = cJSON_GetObjectItemCaseSensitive(item, "remark");
        if (!cJSON_IsString(type) || !cJSON_IsString(device_id) ||
            device_id->valuestring == NULL || device_id->valuestring[0] == '\0') {
            continue;
        }
        starter_product_contact_t *contact =
            &s_product_snapshot.contacts[s_product_snapshot.contact_count];
        contact->source = strcmp(type->valuestring, "voip") == 0
                              ? STARTER_CONTACT_WECHAT
                              : STARTER_CONTACT_DEVICE;
        const cJSON *online = cJSON_GetObjectItemCaseSensitive(item, "online");
        contact->online = contact->source == STARTER_CONTACT_DEVICE &&
                          cJSON_IsBool(online) && cJSON_IsTrue(online);
        (void)snprintf(contact->id, sizeof(contact->id), "%s", device_id->valuestring);
        (void)snprintf(contact->name,
                       sizeof(contact->name),
                       "%s",
                       cJSON_IsString(remark) && remark->valuestring != NULL &&
                               remark->valuestring[0] != '\0'
                           ? remark->valuestring
                           : (contact->source == STARTER_CONTACT_WECHAT
                                  ? "微信联系人" : device_id->valuestring));
        const cJSON *app = cJSON_GetObjectItemCaseSensitive(item, "wx_app_id");
        const cJSON *model = cJSON_GetObjectItemCaseSensitive(item, "wx_model_id");
        if (cJSON_IsString(app) && app->valuestring != NULL) {
            (void)snprintf(contact->wx_app_id, sizeof(contact->wx_app_id),
                           "%s", app->valuestring);
        }
        if (cJSON_IsString(model) && model->valuestring != NULL) {
            (void)snprintf(contact->wx_model_id, sizeof(contact->wx_model_id),
                           "%s", model->valuestring);
        }
        s_product_snapshot.contact_count++;
    }
    xSemaphoreGive(s_product_mutex);
    cJSON_Delete(root);
}

static bool preempt_for_call(bool incoming)
{
    starter_runtime_state_t state = (starter_runtime_state_t)atomic_load_explicit(
        &s_public_state, memory_order_acquire);
    /* Calls are the foreground owner: a locally initiated call preempts AI
     * and H5 just as an incoming call already does below. */
    if (state == STARTER_RUNTIME_AI_CONNECTING ||
        state == STARTER_RUNTIME_AI_ACTIVE ||
        state == STARTER_RUNTIME_H5_ACTIVE || room_owns_media()) {
        ESP_LOGI(TAG, "call preempts foreground owner=%s",
                 starter_runtime_state_name(state));
        diagnostic_event(incoming ? "incoming preempts" : "outgoing preempts", state);
        finish_session(0);
        state = STARTER_RUNTIME_WAITING;
    }
    return state == STARTER_RUNTIME_WAITING;
}

static bool begin_call_common(const starter_product_contact_t *contact)
{
    if (contact == NULL ||
        !platform_client_ready() || !starter_tirtc_started()) return false;
    if (contact->source == STARTER_CONTACT_WECHAT && !s_voip_profile_ready) {
        ESP_LOGW(TAG, "WeChat call blocked until VoIP profile is reported");
        return false;
    }
    if (!preempt_for_call(false)) return false;
    starter_tirtc_accept_h5(false);
    starter_media_stop();
    if (starter_tirtc_connected()) {
        (void)starter_tirtc_disconnect();
    }
    s_session_generation++;
    if (s_session_generation == 0U) {
        s_session_generation = 1U;
    }
    s_connection_generation = 0;
    s_call_wechat = contact->source == STARTER_CONTACT_WECHAT;
    s_call_outgoing = true;
    (void)snprintf(s_call_peer_id, sizeof(s_call_peer_id), "%s", contact->id);
    (void)snprintf(s_call_peer_name, sizeof(s_call_peer_name), "%s", contact->name);
    (void)snprintf(s_call_wx_app_id, sizeof(s_call_wx_app_id), "%s", contact->wx_app_id);
    (void)snprintf(s_call_wx_model_id, sizeof(s_call_wx_model_id), "%s", contact->wx_model_id);
    s_deadline_ms = now_ms() + CALL_CONNECT_TIMEOUT_MS;
    product_snapshot_reset();
    product_set_call(false, s_call_wechat, s_call_peer_name, false);
    publish_state(STARTER_RUNTIME_CALL_CONNECTING);
    diagnostic_event("outgoing call", s_call_wechat);
    return true;
}

static esp_err_t submit_device_call(void)
{
    bool video = false;
#if CONFIG_IDF_TARGET_ESP32P4
    video = s_call_video;
#endif
    char body[256];
    starter_tirtc_expect_call(s_session_generation);
    (void)snprintf(body, sizeof(body),
                   "{\"targets\":[\"%s\"],\"call_type\":\"%s\"}",
                   s_call_peer_id, video ? "video" : "audio");
    s_deadline_ms = now_ms() + CALL_CONNECT_TIMEOUT_MS;
    esp_err_t err = platform_client_request_timeout(
        PLATFORM_SERVICE_CALL, "/v1/call/request", body,
        CALL_CONNECT_TIMEOUT_MS - 2000U, call_http_response,
        call_http_tag(s_session_generation, CALL_HTTP_DEVICE_DIAL));
    ESP_LOGI(TAG, "[DEBUG-call] dial submitted target=%s type=device stage=1 ret=%s",
             s_call_peer_id, esp_err_to_name(err));
    return err;
}

static void dial_contact(uint8_t index, bool video)
{
#if !CONFIG_IDF_TARGET_ESP32P4
    video = false;
#endif
    starter_product_contact_t contact = {0};
    if (s_product_mutex == NULL ||
        xSemaphoreTake(s_product_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    bool valid = index < s_product_snapshot.contact_count;
    if (valid) {
        contact = s_product_snapshot.contacts[index];
    }
    xSemaphoreGive(s_product_mutex);
    if (!valid || !begin_call_common(&contact)) {
        ESP_LOGW(TAG, "[DEBUG-call] dial rejected index=%u valid=%d", (unsigned)index,
                 valid ? 1 : 0);
        return;
    }
#if CONFIG_IDF_TARGET_ESP32P4
    starter_media_set_call_video(video);
    s_call_video = video;
    s_call_camera_enabled = video;
    product_set_call(false, s_call_wechat, s_call_peer_name, false);
#endif

    char body[512];
    platform_service_t service;
    const char *path;
    call_http_stage_t stage;
    if (s_call_wechat) {
        (void)snprintf(body, sizeof(body),
                       "{\"device_id\":\"%s\",\"wx_app_id\":\"%s\","
                       "\"wx_user_openid\":\"%s\",\"wx_model_id\":\"%s\","
                       "\"wx_room_type\":\"%s\",\"wx_version_type\":0}",
                       s_device_id, s_call_wx_app_id, s_call_peer_id,
                       s_call_wx_model_id, video ? "video" : "voice");
        service = PLATFORM_SERVICE_VOIP;
        path = "/v1/voip/device/call";
        stage = CALL_HTTP_VOIP_DIAL;
    } else {
        esp_err_t err = submit_device_call();
        if (err != ESP_OK) {
            finish_call_session(err, "网络中断");
        }
        return;
    }
    esp_err_t err = platform_client_request_timeout(
        service, path, body, CALL_CONNECT_TIMEOUT_MS - 2000U,
        call_http_response, call_http_tag(s_session_generation, stage));
    ESP_LOGI(TAG, "[DEBUG-call] dial submitted target=%s type=%s stage=%d ret=%s",
             s_call_peer_id, s_call_wechat ? "wechat" : "device", (int)stage,
             esp_err_to_name(err));
    if (err != ESP_OK) {
        finish_call_session(err, "网络中断");
    }
}

static void connect_voip(void);

static void accept_call(void)
{
    if (atomic_load_explicit(&s_public_state, memory_order_acquire) !=
            STARTER_RUNTIME_CALL_INCOMING) {
        return;
    }
    publish_state(STARTER_RUNTIME_CALL_CONNECTING);
    s_deadline_ms = now_ms() + CALL_CONNECT_TIMEOUT_MS;
    if (s_call_wechat) {
        connect_voip();
        return;
    }
    char body[384];
    (void)snprintf(body, sizeof(body),
                   "{\"device_id\":\"%s\",\"room_id\":\"%s\","
                   "\"purpose\":\"call\"}",
                   s_call_peer_id, s_call_room_id);
    esp_err_t err = platform_client_request_timeout(
        PLATFORM_SERVICE_CALL, "/v1/call/device/info", body,
        CALL_CONNECT_TIMEOUT_MS - 2000U, call_http_response,
        call_http_tag(s_session_generation, CALL_HTTP_DEVICE_INFO));
    if (err != ESP_OK) {
        finish_call_session(err, "网络中断");
    }
}

/*
 * MQTT call_incoming 的解析帧里有多组 1 KiB peer/token 临时缓冲。不能在
 * 该深栈函数中再进入 TiRtcWhipConnect，否则 SDK 的握手栈会压穿
 * starter_session。下一轮事件在解析帧已退出后才提交 WHIP。
 */
static void voip_connect_task(void *argument)
{
    voip_connect_request_t *request = argument;
    runtime_event_t event = {
        .type = EVENT_VOIP_CONNECT_RESULT,
        .generation = request == NULL ? 0U : request->generation,
        .error = ESP_ERR_INVALID_ARG,
    };
    if (request != NULL) {
        event.error = starter_tirtc_voip_connect(request->peer_id,
                                                 request->token,
                                                 request->generation);
        ESP_LOGI(TAG, "[DEBUG-voip] WHIP worker completed generation=%lu rc=%d",
                 (unsigned long)request->generation, event.error);
        free(request);
    }
    if (!queue_event(&event)) {
        ESP_LOGE(TAG, "[DEBUG-voip] WHIP result queue full; session will time out");
    }
    /* WithCaps uses a statically registered stack/TCB. Ordinary deletion
     * removes the task but leaks both allocations on every WHIP attempt. */
    vTaskDeleteWithCaps(NULL);
}

static void connect_voip(void)
{
    starter_runtime_state_t state = (starter_runtime_state_t)atomic_load_explicit(
        &s_public_state, memory_order_acquire);
    if (!s_call_wechat || state != STARTER_RUNTIME_CALL_CONNECTING ||
        s_call_connect_peer[0] == '\0' || s_call_connect_token[0] == '\0') {
        return;
    }
    if (s_voip_connect_inflight) {
        return;
    }
    /* 先完成小对象分配，不能在释放 17 KiB 连续块后把它切碎。 */
    voip_connect_request_t *request = heap_caps_calloc(1, sizeof(*request),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (request == NULL) {
        finish_call_session(ESP_ERR_NO_MEM, "内存不足");
        return;
    }
    request->generation = s_session_generation;
    (void)snprintf(request->peer_id, sizeof(request->peer_id), "%s",
                   s_call_connect_peer);
    (void)snprintf(request->token, sizeof(request->token), "%s",
                   s_call_connect_token);
    if (!prepare_external_connect()) {
        free(request);
        finish_call_session(ESP_ERR_NO_MEM, "内存不足");
        return;
    }
    s_voip_connect_inflight = true;
    if (xTaskCreateWithCaps(voip_connect_task, "voip_whip",
                            VOIP_CONNECT_TASK_STACK_BYTES, request, 5, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_voip_connect_inflight = false;
        free(request);
        restore_external_connect_reserve();
        finish_call_session(ESP_ERR_NO_MEM, "内存不足");
        return;
    }
    ESP_LOGI(TAG, "[DEBUG-voip] WHIP worker started outgoing=%d generation=%lu",
             s_call_outgoing ? 1 : 0, (unsigned long)s_session_generation);
    s_deadline_ms = now_ms() + CALL_CONNECT_TIMEOUT_MS;
}

static void handle_voip_connect_result(const runtime_event_t *event)
{
    starter_runtime_state_t state = (starter_runtime_state_t)atomic_load_explicit(
        &s_public_state, memory_order_acquire);
    if (event->generation != s_session_generation || !s_call_wechat ||
        state != STARTER_RUNTIME_CALL_CONNECTING) {
        return;
    }
    s_voip_connect_inflight = false;
    if (event->error != 0) {
        restore_external_connect_reserve();
        finish_call_session(event->error, "网络中断");
        return;
    }
    /* 成功只说明 WHIP 已提交；真正媒体启动由 0x2000 处理。 */
    s_deadline_ms = now_ms() + CALL_CONNECT_TIMEOUT_MS;
}

static void reject_wechat_values(const char *app_id,
                                 const char *model_id,
                                 const char *session_token,
                                 const char *room_id,
                                 const char *payload,
                                 int reason)
{
    if (app_id == NULL || app_id[0] == '\0' || model_id == NULL ||
        model_id[0] == '\0' || room_id == NULL || room_id[0] == '\0') {
        return;
    }
    cJSON *root = cJSON_CreateObject();
    bool ok = root != NULL &&
              cJSON_AddStringToObject(root, "wx_app_id", app_id) &&
              cJSON_AddStringToObject(root, "wx_model_id", model_id) &&
              cJSON_AddStringToObject(root, "wx_session_token",
                                     session_token == NULL ? "" : session_token) &&
              cJSON_AddStringToObject(root, "wx_room_id", room_id) &&
              cJSON_AddStringToObject(root, "wx_payload",
                                     payload == NULL ? "" : payload) &&
              cJSON_AddNumberToObject(root, "hangup_reason", reason);
    char *body = ok ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (body != NULL) {
        (void)starter_tirtc_service_request("/v1/wxvoip/reject", body);
        cJSON_free(body);
    }
}

static void reject_or_hangup_call(bool reject)
{
    starter_runtime_state_t state = (starter_runtime_state_t)atomic_load_explicit(
        &s_public_state, memory_order_acquire);
    if (state != STARTER_RUNTIME_CALL_INCOMING &&
        state != STARTER_RUNTIME_CALL_CONNECTING &&
        state != STARTER_RUNTIME_CALL_ACTIVE) {
        return;
    }
    if (starter_tirtc_connected()) {
        (void)starter_tirtc_send_command(CALL_COMMAND_HANGUP, NULL, 0);
    }
    char body[1200];
    if (s_call_wechat) {
        if (reject && !s_call_outgoing) {
            reject_wechat_values(s_call_wx_app_id,
                                 s_call_wx_model_id,
                                 s_call_wx_session_token,
                                 s_call_room_id,
                                 s_call_wx_payload,
                                 7);
        }
    } else if (s_call_room_id[0] != '\0') {
        const char *path = state == STARTER_RUNTIME_CALL_INCOMING
                               ? "/v1/call/reject"
                               : (state == STARTER_RUNTIME_CALL_CONNECTING && s_call_outgoing
                                      ? "/v1/call/cancel" : "/v1/call/hangup");
        (void)snprintf(body, sizeof(body),
                       strcmp(path, "/v1/call/cancel") == 0
                           ? "{\"room_id\":\"%s\"}"
                           : "{\"room_id\":\"%s\",\"reason\":\"%s\"}",
                       s_call_room_id,
                       reject ? "decline" : "hangup");
        (void)platform_client_request(PLATFORM_SERVICE_CALL, path, body, NULL, NULL);
    }
    finish_session(0);
}

static bool ai_network_ready(void)
{
    return wifi_manager_connected();
}

static void begin_ai_session(uint32_t wake_token)
{
    /*
     * AI 优先于 H5：先关闭 H5 入站门禁并结束当前媒体/连接，再开启新会话代次。
     * HTTP 响应携带该代次，迟到响应不能推动后续新会话。
     */
    starter_runtime_state_t state = (starter_runtime_state_t)atomic_load_explicit(
        &s_public_state, memory_order_acquire);
    if ((state != STARTER_RUNTIME_WAITING &&
         state != STARTER_RUNTIME_H5_ACTIVE && !room_owns_media()) ||
        !ai_network_ready() || !platform_client_ready() || !starter_tirtc_started() ||
        starter_media_status().microphone_muted) {
        ESP_LOGW(TAG, "AI start ignored: network, platform or TiRTC is not ready");
        starter_media_cancel_ai_preroll(wake_token);
        return;
    }

    if (room_owns_media()) finish_session(0);
    starter_tirtc_accept_h5(false);
    starter_media_set_wake_allowed(false);
    if (starter_media_stop_for_ai(wake_token) != ESP_OK) {
        ESP_LOGW(TAG, "AI start ignored: wake audio expired or cancelled");
        finish_session(ESP_ERR_INVALID_STATE);
        return;
    }
    if (starter_tirtc_connected()) {
        (void)starter_tirtc_disconnect();
    }
    s_connection_generation = 0;
    s_session_generation++;
    if (s_session_generation == 0U) {
        s_session_generation = 1U;
    }
    s_deadline_ms = now_ms() + AI_REQUEST_TIMEOUT_MS;
    atomic_store_explicit(&s_last_error, 0, memory_order_release);
    product_snapshot_reset();
    product_set_phase(STARTER_AI_UI_LISTENING);
    publish_state(STARTER_RUNTIME_AI_CONNECTING);

    /* MQTT bearer 由 platform_client 内部添加，状态机不接触设备密钥。 */
    s_diagnostic_ai_started_ms = now_ms();
    diagnostic_event(wake_token != 0 ? "wake accepted" : "manual AI start", 0);
    if (wake_token != 0) {
        ESP_LOGI(TAG, "AI wake request accepted token=%lu session=%lu",
                 (unsigned long)wake_token, (unsigned long)s_session_generation);
    }
    esp_err_t err = platform_client_request_timeout(
        PLATFORM_SERVICE_AI,
        "/v1/ai/token",
        NULL,
        AI_REQUEST_TIMEOUT_MS - 2000U,
        request_ai_token_response,
        (void *)(uintptr_t)s_session_generation);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AI token request submission failed: %s", esp_err_to_name(err));
        finish_session(err);
    }
}

static void handle_ai_token(const runtime_event_t *event)
{
    /* 只接受当前 AI_CONNECTING 会话对应的 token 响应。 */
    if (event->request_tag != s_session_generation ||
        atomic_load_explicit(&s_public_state, memory_order_acquire) !=
            STARTER_RUNTIME_AI_CONNECTING) {
        return;
    }
    cJSON *root = event->length == 0U || event->text == NULL
                      ? NULL
                      : cJSON_Parse(event->text);
    const cJSON *code = root == NULL
                            ? NULL
                            : cJSON_GetObjectItemCaseSensitive(root, "code");
    const cJSON *data = root == NULL
                            ? NULL
                            : cJSON_GetObjectItemCaseSensitive(root, "data");
    ai_credentials_t *credentials = heap_caps_calloc(1, sizeof(*credentials),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (credentials == NULL) {
        cJSON_Delete(root);
        finish_session(ESP_ERR_NO_MEM);
        return;
    }
    bool ok = cJSON_IsNumber(code) &&
              (code->valueint == 0 || code->valueint == 200) &&
              cJSON_IsObject(data) &&
              copy_json_string(data,
                               "peer_id",
                               credentials->peer_id,
                               sizeof(credentials->peer_id),
                               true) &&
              copy_json_string(data,
                               "token",
                               credentials->token,
                               sizeof(credentials->token),
                               true) &&
              copy_json_string(data,
                               "role_id",
                               s_ai_role_id,
                               sizeof(s_ai_role_id),
                               false);
    cJSON_Delete(root);
    if (!ok) {
        free(credentials);
        ESP_LOGE(TAG, "AI token response is invalid");
        finish_session(ESP_ERR_INVALID_RESPONSE);
        return;
    }
    /*
     * MQTT/TLS 的常驻分配会切碎内部堆；WHIP 提交前短暂停止 MQTT，给 SDK
     * 的 16+ KiB 连续分配让路，连接回调到达后立即恢复。
     */
    if (!prepare_external_connect()) {
        free(credentials);
        finish_session(ESP_ERR_NO_MEM);
        return;
    }
    int rc = starter_tirtc_ai_connect(credentials->peer_id,
                                      credentials->token,
                                      s_session_generation);
    free(credentials);
    if (rc != 0) {
        restore_external_connect_reserve();
        ESP_LOGE(TAG, "AI connection submission failed rc=%d", rc);
        finish_session(rc);
        return;
    }
    s_deadline_ms = now_ms() + AI_CONNECT_TIMEOUT_MS;
}

static bool valid_device_room_id(const char *room)
{
    if (room == NULL || strncmp(room, "d_", 2) != 0 || room[2] == '\0') return false;
    size_t i = 0;
    for (; room[i] != '\0'; ++i) {
        unsigned char c = (unsigned char)room[i];
        if (i >= 128 || !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}

static void request_call_room(call_http_stage_t stage, const char *path, const char *body)
{
    s_call_room_recovery.stage = stage;
    esp_err_t err = platform_client_request_timeout(
        PLATFORM_SERVICE_CALL, path, body, 4000U, call_http_response,
        call_http_tag(s_session_generation, stage));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "call room recovery submission failed: stage=%d err=%s",
                 (int)stage, esp_err_to_name(err));
        finish_call_session(err, "网络中断");
    }
}

static void handle_call_room_response(const runtime_event_t *event, const cJSON *root)
{
    if (s_call_wechat || event->command != s_call_room_recovery.stage) return;
    if (event->command == CALL_HTTP_ROOM_RELEASE) {
        /* Even a lost release response needs an authoritative GET, not another
         * release or blind redial. The room may have ended concurrently. */
        const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
        ESP_LOGI(TAG, "call room release response: code=%d; verifying",
                 cJSON_IsNumber(code) ? code->valueint : -1);
        request_call_room(CALL_HTTP_ROOM_VERIFY, "/v1/call/room", NULL);
        return;
    }
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!response_ok(root)) {
        finish_call_session(ESP_ERR_INVALID_RESPONSE, "呼叫失败");
        return;
    }
    /* The deployed room endpoint omits data for an empty room; the public
     * example uses data:null. Accept either only on a successful room GET. */
    if (data == NULL || cJSON_IsNull(data)) {
        ESP_LOGI(TAG, "call room recovery confirmed empty; resuming requested call");
        s_call_room_recovery.stage = 0;
        esp_err_t err = submit_device_call();
        if (err != ESP_OK) finish_call_session(err, "网络中断");
        return;
    }
    /* 40202 names the old room. Never release a different room that appeared
     * while this request was in flight, nor accept a nonempty verify as success. */
    const cJSON *room = cJSON_GetObjectItemCaseSensitive(data, "room_id");
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(data, "status");
    const cJSON *role = cJSON_GetObjectItemCaseSensitive(data, "role");
    if (event->command != CALL_HTTP_ROOM_QUERY || !cJSON_IsString(room) ||
        strcmp(room->valuestring, s_call_room_recovery.room_id) != 0 ||
        !cJSON_IsString(status) || !cJSON_IsString(role)) {
        ESP_LOGW(TAG, "call room recovery not confirmed; no new call submitted");
        finish_call_session(ESP_ERR_INVALID_STATE, "呼叫繁忙");
        return;
    }
    bool caller = strcmp(role->valuestring, "caller") == 0;
    bool callee = strcmp(role->valuestring, "callee") == 0;
    const char *path = NULL;
    if ((caller || callee) && strcmp(status->valuestring, "answered") == 0) {
        path = "/v1/call/hangup";
    } else if (strcmp(status->valuestring, "active") == 0) {
        if (caller) path = "/v1/call/cancel";
        if (callee) path = "/v1/call/reject";
    }
    if (path == NULL) {
        finish_call_session(ESP_ERR_INVALID_RESPONSE, "呼叫失败");
        return;
    }
    char body[192];
    (void)snprintf(body, sizeof(body),
                   strcmp(path, "/v1/call/cancel") == 0
                       ? "{\"room_id\":\"%s\"}"
                       : "{\"room_id\":\"%s\",\"reason\":\"%s\"}",
                   s_call_room_recovery.room_id,
                   strcmp(path, "/v1/call/reject") == 0 ? "decline" : "hangup");
    ESP_LOGI(TAG, "call room recovery: role=%s status=%s action=%s",
             role->valuestring, status->valuestring, path);
    request_call_room(CALL_HTTP_ROOM_RELEASE, path, body);
}

static void handle_call_http(const runtime_event_t *event)
{
    if (event->request_tag != s_session_generation ||
        atomic_load_explicit(&s_public_state, memory_order_acquire) !=
            STARTER_RUNTIME_CALL_CONNECTING) {
        return;
    }
    cJSON *root = event->text == NULL ? NULL :
        cJSON_ParseWithLength(event->text, event->length);
    if (event->command >= CALL_HTTP_ROOM_QUERY && event->command <= CALL_HTTP_ROOM_VERIFY) {
        handle_call_room_response(event, root);
        cJSON_Delete(root);
        return;
    }
    const cJSON *code = cJSON_IsObject(root)
                            ? cJSON_GetObjectItemCaseSensitive(root, "code")
                            : NULL;
    const cJSON *message = cJSON_IsObject(root)
                               ? cJSON_GetObjectItemCaseSensitive(root, "message")
                               : NULL;
    if (!cJSON_IsString(message)) message = cJSON_GetObjectItemCaseSensitive(root, "msg");
    ESP_LOGI(TAG,
             "[DEBUG-call] HTTP result stage=%lu bytes=%u parse=%d code=%d message=%s",
             (unsigned long)event->command,
             (unsigned)event->length,
             root != NULL,
             cJSON_IsNumber(code) ? code->valueint : -1,
             cJSON_IsString(message) && message->valuestring != NULL
                 ? message->valuestring : "");
    if (event->command == CALL_HTTP_DEVICE_DIAL && !s_call_wechat &&
        cJSON_IsNumber(code) && code->valueint == 40202 &&
        !s_call_room_recovery.attempted) {
        const cJSON *old = cJSON_GetObjectItemCaseSensitive(root, "data");
        bool valid = copy_json_string(old, "room_id", s_call_room_recovery.room_id,
                                      sizeof(s_call_room_recovery.room_id), true) &&
                     valid_device_room_id(s_call_room_recovery.room_id);
        s_call_room_recovery.attempted = true;
        cJSON_Delete(root);
        if (!valid) {
            finish_call_session(ESP_ERR_INVALID_RESPONSE, "呼叫失败");
            return;
        }
        /* One bounded reconciliation per user intent, only after the server
         * proves no new room was created. Cancel/new generations invalidate it. */
        s_deadline_ms = now_ms() + 15000;
        ESP_LOGW(TAG, "call room conflict 40202: reconciling old room=%s",
                 s_call_room_recovery.room_id);
        request_call_room(CALL_HTTP_ROOM_QUERY, "/v1/call/room", NULL);
        return;
    }
    const cJSON *data = response_ok(root)
                            ? cJSON_GetObjectItemCaseSensitive(root, "data")
                            : NULL;
    if (!cJSON_IsObject(data)) {
        cJSON_Delete(root);
        finish_call_session(ESP_ERR_INVALID_RESPONSE, "呼叫失败");
        return;
    }
    if (event->command == CALL_HTTP_DEVICE_DIAL) {
        bool ok = copy_json_string(data, "room_id", s_call_room_id,
                                   sizeof(s_call_room_id), true);
        cJSON_Delete(root);
        if (!ok) {
            finish_call_session(ESP_ERR_INVALID_RESPONSE, "呼叫失败");
            return;
        }
        starter_tirtc_expect_call(s_session_generation);
        s_deadline_ms = now_ms() + CALL_CONNECT_TIMEOUT_MS;
        ESP_LOGI(TAG, "[DEBUG-call] room created room=%s; awaiting callee/p2p",
                 s_call_room_id);
        return;
    }
    if (event->command == CALL_HTTP_VOIP_DIAL) {
        (void)copy_json_string(data, "call_id", s_call_id,
                               sizeof(s_call_id), false);
        cJSON_Delete(root);
        /* 真正 peer/token/room 来自匹配的 MQTT call_incoming。 */
        s_deadline_ms = now_ms() + CALL_CONNECT_TIMEOUT_MS;
        return;
    }
    if (event->command == CALL_HTTP_DEVICE_INFO) {
        char token[1024];
        char remote_id[65];
        bool ok = copy_json_string(data, "token", token, sizeof(token), true) &&
                  copy_json_string(data, "device_id", remote_id,
                                   sizeof(remote_id), true);
        cJSON_Delete(root);
        if (!ok) {
            finish_call_session(ESP_ERR_INVALID_RESPONSE, "呼叫失败");
            return;
        }
        if (!prepare_external_connect()) {
            finish_call_session(ESP_ERR_NO_MEM, "内存不足");
            return;
        }
        int rc = starter_tirtc_call_connect(remote_id, token,
                                            s_session_generation);
        ESP_LOGI(TAG, "[DEBUG-call] callee p2p submitted room=%s peer=%s rc=%d",
                 s_call_room_id, remote_id, rc);
        if (rc != 0) {
            restore_external_connect_reserve();
            finish_call_session(rc, "网络中断");
        }
        return;
    }
    cJSON_Delete(root);
}

static void send_ai_start(void)
{
    /*
     * WHIP 连接成功后发送业务层 start_session。媒体仍保持关闭，直到
     * handle_ai_command() 收到匹配 request id 的 result。
     */
    if (!starter_tirtc_connected() || s_connection_generation == 0U) {
        finish_session(ESP_ERR_INVALID_STATE);
        return;
    }
    (void)snprintf(s_ai_request_id,
                   sizeof(s_ai_request_id),
                   "%08lx%08lx",
                   (unsigned long)esp_random(),
                   (unsigned long)esp_random());
    cJSON *root = cJSON_CreateObject();
    cJSON *params = cJSON_CreateObject();
    cJSON *input = cJSON_CreateObject();
    cJSON *output = cJSON_CreateObject();
    bool ok = root != NULL && params != NULL && input != NULL && output != NULL &&
              cJSON_AddStringToObject(root, "jsonrpc", "2.0") &&
              cJSON_AddStringToObject(root, "id", s_ai_request_id) &&
              cJSON_AddStringToObject(root, "method", "start_session") &&
              cJSON_AddStringToObject(params, "device_id", s_device_id) &&
              cJSON_AddStringToObject(params, "role_id", s_ai_role_id) &&
              cJSON_AddStringToObject(input, "codec", "alaw") &&
              cJSON_AddNumberToObject(input, "sample_rate", 8000) &&
              cJSON_AddNumberToObject(input, "channels", 1) &&
              cJSON_AddStringToObject(output, "codec", "alaw") &&
              cJSON_AddNumberToObject(output, "sample_rate", 8000) &&
              cJSON_AddNumberToObject(output, "channels", 1);
    if (ok) {
        /* AddItem allocates the key; ownership moves only on success. */
        ok = cJSON_AddItemToObject(params, "input_audio", input);
        if (ok) input = NULL;
    }
    if (ok) {
        ok = cJSON_AddItemToObject(params, "output_audio", output);
        if (ok) output = NULL;
    }
    if (ok) {
        ok = cJSON_AddItemToObject(root, "params", params);
        if (ok) params = NULL;
    }
    char *json = ok ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(input);
    cJSON_Delete(output);
    cJSON_Delete(params);
    cJSON_Delete(root);
    if (json == NULL) {
        finish_session(ESP_ERR_NO_MEM);
        return;
    }
    int rc = starter_tirtc_send_command(AI_COMMAND, json, (uint32_t)strlen(json));
    cJSON_free(json);
    if (rc < 0) {
        ESP_LOGE(TAG, "AI start_session send failed rc=%d", rc);
        finish_session(rc);
        return;
    }
    s_deadline_ms = now_ms() + AI_RESPONSE_TIMEOUT_MS;
    ESP_LOGI(TAG, "AI start_session sent; audio remains stopped until accepted");
}

static bool ai_audio_profile_valid(const cJSON *profile)
{
    const cJSON *codec = cJSON_IsObject(profile)
                             ? cJSON_GetObjectItemCaseSensitive(profile, "codec")
                             : NULL;
    const cJSON *rate = cJSON_IsObject(profile)
                            ? cJSON_GetObjectItemCaseSensitive(profile, "sample_rate")
                            : NULL;
    const cJSON *channels = cJSON_IsObject(profile)
                                ? cJSON_GetObjectItemCaseSensitive(profile, "channels")
                                : NULL;
    bool codec_ok = cJSON_IsString(codec) && codec->valuestring != NULL &&
                    (strcmp(codec->valuestring, "alaw") == 0 ||
                     strcmp(codec->valuestring, "g711a") == 0);
    return codec_ok && cJSON_IsNumber(rate) && rate->valueint == 8000 &&
           cJSON_IsNumber(channels) && channels->valueint == 1;
}

static void maybe_activate_outgoing_device_call(const char *source)
{
    starter_runtime_state_t state = (starter_runtime_state_t)atomic_load_explicit(
        &s_public_state, memory_order_acquire);
    if (s_call_wechat || !s_call_outgoing ||
        state != STARTER_RUNTIME_CALL_CONNECTING ||
        !s_call_peer_answered || !s_call_p2p_connected ||
        s_connection_generation == 0U) {
        return;
    }
    if (starter_media_start(STARTER_TIRTC_CALL,
                            s_connection_generation) != ESP_OK) {
        ESP_LOGE(TAG, "[DEBUG-call] caller media start failed source=%s room=%s",
                 source, s_call_room_id);
        finish_call_session(ESP_ERR_INVALID_STATE, "音频启动失败");
        return;
    }
    s_call_waiting_confirm = false;
    s_deadline_ms = 0;
    ESP_LOGI(TAG, "[DEBUG-call] caller active source=%s room=%s generation=%lu",
             source, s_call_room_id, (unsigned long)s_connection_generation);
    publish_state(STARTER_RUNTIME_CALL_ACTIVE);
}

static void handle_connection(const runtime_event_t *event)
{
    if (event->mode == STARTER_TIRTC_ROOM) { room_connection(event); return; }
    if (event->flag && !platform_client_ready()) {
        if (event->generation != 0 && event->generation == starter_tirtc_generation())
            (void)starter_tirtc_disconnect();
        return;
    }
    /*
     * 连接回调可能在 disconnect 后迟到。先按 request_tag/generation 过滤，
     * 再根据当前业务状态决定接纳或关闭，避免旧连接复活。
     */
    starter_runtime_state_t state = (starter_runtime_state_t)atomic_load_explicit(
        &s_public_state, memory_order_acquire);
    /* Validate ownership BEFORE touching MQTT reserves or the foreground owner.
     * SDK callbacks may already be queued when finish_session cancels a handle. */
    bool ai = state == STARTER_RUNTIME_AI_CONNECTING || state == STARTER_RUNTIME_AI_ACTIVE;
    bool call = state == STARTER_RUNTIME_CALL_CONNECTING || state == STARTER_RUNTIME_CALL_ACTIVE;
    bool current = false;
    if (event->mode == STARTER_TIRTC_H5) {
        current = event->generation != 0 &&
            (event->flag ? state == STARTER_RUNTIME_WAITING :
             (state == STARTER_RUNTIME_H5_ACTIVE && event->generation == s_connection_generation));
    } else if ((ai && event->mode == STARTER_TIRTC_AI) ||
               (call && event->mode == (s_call_wechat ? STARTER_TIRTC_VOIP : STARTER_TIRTC_CALL))) {
        if (event->flag) {
            current = event->request_tag == s_session_generation &&
                      event->generation != 0 && s_connection_generation == 0 &&
                      (state == STARTER_RUNTIME_AI_CONNECTING || state == STARTER_RUNTIME_CALL_CONNECTING);
        } else if (event->generation != 0) {
            current = event->generation == s_connection_generation;
        } else {
            current = event->request_tag != 0 && event->request_tag == s_session_generation &&
                      s_connection_generation == 0;
        }
    }
    if (!current) {
        ESP_LOGI(TAG, "ignored stale connection event mode=%d generation=%lu request=%lu",
                 (int)event->mode, (unsigned long)event->generation,
                 (unsigned long)event->request_tag);
        return;
    }
    if (event->mode == STARTER_TIRTC_AI ||
        event->mode == STARTER_TIRTC_CALL ||
        event->mode == STARTER_TIRTC_VOIP) {
        restore_external_connect_reserve();
    }
    if (!event->flag) {
        /* AI 意图转呼叫时 finish_session() 会主动断开旧 AI 连接。该断开
         * 回调可能晚于新呼叫的 HTTP 房间创建；它属于旧媒体所有者，绝不能
         * 终止正在等待被叫接入的 CALL/VOIP 会话。 */
        if ((state == STARTER_RUNTIME_CALL_INCOMING ||
             state == STARTER_RUNTIME_CALL_CONNECTING ||
             state == STARTER_RUNTIME_CALL_ACTIVE) &&
            event->mode != STARTER_TIRTC_CALL &&
            event->mode != STARTER_TIRTC_VOIP) {
            ESP_LOGI(TAG,
                     "ignore stale connection close mode=%d while call state=%s",
                     (int)event->mode, starter_runtime_state_name(state));
            return;
        }
        if (event->mode == STARTER_TIRTC_AI && event->request_tag != 0U &&
            event->request_tag != s_session_generation) {
            return;
        }
        if (event->generation != 0U && s_connection_generation != 0U &&
            event->generation != s_connection_generation) {
            return;
        }
        ESP_LOGW(TAG, "connection ended mode=%d error=%d", (int)event->mode, event->error);
        if (state == STARTER_RUNTIME_CALL_INCOMING ||
            state == STARTER_RUNTIME_CALL_CONNECTING ||
            state == STARTER_RUNTIME_CALL_ACTIVE) {
            finish_call_session(event->error,
                                event->error != 0 ? "网络中断"
                                                  : "对方已挂断");
        } else {
            if (state == STARTER_RUNTIME_AI_CONNECTING ||
                state == STARTER_RUNTIME_AI_ACTIVE) {
                ESP_LOGI(TAG,
                         "AI session closed by remote/server idle timeout error=%d generation=%lu",
                         event->error, (unsigned long)event->generation);
            }
            finish_session(event->error);
        }
        return;
    }

    if (event->mode == STARTER_TIRTC_H5 && state == STARTER_RUNTIME_WAITING) {
        /* H5 建连即允许媒体模块启动；实际发送还要等待远端订阅。 */
        s_connection_generation = event->generation;
        s_session_generation++;
        if (s_session_generation == 0U) {
            s_session_generation = 1U;
        }
        atomic_store_explicit(&s_last_error, 0, memory_order_release);
        if (starter_media_start(STARTER_TIRTC_H5, event->generation) != ESP_OK) {
            finish_session(ESP_ERR_INVALID_STATE);
            return;
        }
        /* Playback is ready before asking the peer to send talkback. This
         * subscribes the peer's stream 14; it never enables our stream 10 TX.
         * Run on the session owner, not inside an SDK connection callback. */
        int subscribe_ret = starter_tirtc_subscribe_h5_audio(event->generation);
        if (subscribe_ret < 0) {
            ESP_LOGW(TAG, "H5 talkback subscribe failed: ret=%d", subscribe_ret);
            finish_session(subscribe_ret);
            return;
        }
        publish_state(STARTER_RUNTIME_H5_ACTIVE);
        return;
    }
    if (event->mode == STARTER_TIRTC_AI &&
        state == STARTER_RUNTIME_AI_CONNECTING &&
        event->request_tag == s_session_generation) {
        s_connection_generation = event->generation;
        publish_state(STARTER_RUNTIME_AI_CONNECTING);
        /* The SDK has delivered a valid connected handle. Send on this owner
         * task after the generation checks above, not inside the SDK callback.
         * Audio still waits for the matching start_session result. */
        send_ai_start();
        return;
    }
    if ((event->mode == STARTER_TIRTC_CALL ||
         event->mode == STARTER_TIRTC_VOIP) &&
        state == STARTER_RUNTIME_CALL_CONNECTING &&
        event->request_tag == s_session_generation) {
        s_connection_generation = event->generation;
        publish_state(STARTER_RUNTIME_CALL_CONNECTING);
        /*
         * WHIP 成功仅表示传输连接建立。微信 VoIP 与参考工程一致，必须
         * 再等待服务端的 0x2000 / CALL_CONNECTED，收到后才允许启动 A-law
         * 媒体；过早发音频会被服务端关闭，表现为无声后自动回首页。
         */
        if (event->mode == STARTER_TIRTC_VOIP) {
            s_call_waiting_confirm = true;
            s_deadline_ms = now_ms() + VOIP_CONNECTED_WAIT_TIMEOUT_MS;
            ESP_LOGI(TAG, "VoIP WHIP connected; awaiting CALL_CONNECTED generation=%lu",
                     (unsigned long)s_connection_generation);
            return;
        }
        s_call_waiting_confirm = true;
        if (event->mode == STARTER_TIRTC_CALL && !s_call_outgoing) {
            char confirm[192];
            int length = snprintf(confirm, sizeof(confirm),
                                  "{\"room_id\":\"%s\"}", s_call_room_id);
            if (length <= 0 || (size_t)length >= sizeof(confirm) ||
                starter_tirtc_send_command(CALL_COMMAND_CONNECT,
                                           confirm,
                                           (uint32_t)length) < 0 ||
                starter_media_start(STARTER_TIRTC_CALL,
                                    s_connection_generation) != ESP_OK) {
                finish_session(ESP_FAIL);
                return;
            }
            s_call_waiting_confirm = false;
            s_deadline_ms = 0;
            publish_state(STARTER_RUNTIME_CALL_ACTIVE);
        } else {
            s_deadline_ms = now_ms() + AI_RESPONSE_TIMEOUT_MS;
            if (event->mode == STARTER_TIRTC_CALL && s_call_outgoing) {
                s_call_p2p_connected = true;
                ESP_LOGI(TAG,
                         "[DEBUG-call] caller p2p accepted room=%s answered=%d",
                         s_call_room_id, s_call_peer_answered ? 1 : 0);
                maybe_activate_outgoing_device_call("p2p");
            }
        }
        return;
    }

    ESP_LOGW(TAG, "unexpected connection mode=%d; closing", (int)event->mode);
    finish_session(ESP_ERR_INVALID_STATE);
}

static void handle_call_command(const runtime_event_t *event)
{
    starter_runtime_state_t state = (starter_runtime_state_t)atomic_load_explicit(
        &s_public_state, memory_order_acquire);
    if ((event->mode != STARTER_TIRTC_CALL &&
         event->mode != STARTER_TIRTC_VOIP) ||
        event->generation != s_connection_generation) {
        return;
    }
    if (event->command == CALL_COMMAND_HANGUP) {
        finish_call_session(0, "对方已挂断");
        return;
    }
    if (event->command != CALL_COMMAND_CONNECT ||
        state != STARTER_RUNTIME_CALL_CONNECTING || !s_call_waiting_confirm) {
        return;
    }
    if (!s_call_wechat && event->length > 0U) {
        cJSON *root = cJSON_ParseWithLength(event->text, event->length);
        const cJSON *room = root == NULL ? NULL :
            cJSON_GetObjectItemCaseSensitive(root, "room_id");
        bool matches = cJSON_IsString(room) && room->valuestring != NULL &&
                       strcmp(room->valuestring, s_call_room_id) == 0;
        cJSON_Delete(root);
        if (!matches) {
            return;
        }
    }
    if (!s_call_wechat && s_call_outgoing) {
        /* 0x2000 保持兼容，但不再成为唯一的通话开通条件。 */
        s_call_peer_answered = true;
        maybe_activate_outgoing_device_call("command");
        return;
    }
    starter_tirtc_mode_t mode = s_call_wechat ? STARTER_TIRTC_VOIP
                                               : STARTER_TIRTC_CALL;
    if (starter_media_start(mode, s_connection_generation) != ESP_OK) {
        finish_session(ESP_ERR_INVALID_STATE);
        return;
    }
    s_call_waiting_confirm = false;
    s_deadline_ms = 0;
    publish_state(STARTER_RUNTIME_CALL_ACTIVE);
}

typedef enum {
    CONTACT_MATCH_NOT_FOUND = 0,
    CONTACT_MATCH_UNIQUE,
    CONTACT_MATCH_AMBIGUOUS,
} contact_match_t;

static bool contact_channel_matches(const starter_product_contact_t *contact,
                                    const char *channel)
{
    if (channel == NULL || channel[0] == '\0') {
        return true;
    }
    if (strcmp(channel, "wx") == 0 || strcmp(channel, "wechat") == 0 ||
        strcmp(channel, "voip") == 0) {
        return contact->source == STARTER_CONTACT_WECHAT;
    }
    if (strcmp(channel, "device") == 0 || strcmp(channel, "call") == 0 ||
        strcmp(channel, "tirtc") == 0) {
        return contact->source == STARTER_CONTACT_DEVICE;
    }
    return false;
}

static bool normalize_contact_name(const char *input,
                                   char *output,
                                   size_t capacity)
{
    if (input == NULL || output == NULL || capacity == 0U) {
        return false;
    }
    size_t used = 0;
    for (const unsigned char *cursor = (const unsigned char *)input;
         *cursor != '\0'; ++cursor) {
        if (*cursor < 0x80U && isspace(*cursor)) {
            continue;
        }
        if (used + 1U >= capacity) {
            output[0] = '\0';
            return false;
        }
        output[used++] = *cursor < 0x80U
                             ? (char)tolower(*cursor) : (char)*cursor;
    }
    output[used] = '\0';
    return used > 0U;
}

/*
 * AI 只能提出呼叫意图。最终目标必须在设备当前通讯录里唯一命中；禁止把
 * 大模型返回的任意 ID 直接交给拨号接口。
 */
static contact_match_t find_contact(const char *target_id,
                                    const char *target_name,
                                    const char *channel,
                                    uint8_t *index)
{
    if (index == NULL || s_product_mutex == NULL ||
        xSemaphoreTake(s_product_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return CONTACT_MATCH_NOT_FOUND;
    }
    char normalized_target[65] = "";
    bool match_by_id = target_id != NULL && target_id[0] != '\0';
    bool target_valid = normalize_contact_name(target_name,
                                               normalized_target,
                                               sizeof(normalized_target));
    uint8_t matches = 0;
    bool id_consistent = !match_by_id;
    for (uint8_t i = 0; i < s_product_snapshot.contact_count; ++i) {
        const starter_product_contact_t *contact = &s_product_snapshot.contacts[i];
        if (!target_valid || !contact_channel_matches(contact, channel)) {
            continue;
        }
        char normalized_contact[65];
        bool matched = normalize_contact_name(contact->name,
                                             normalized_contact,
                                             sizeof(normalized_contact)) &&
                      strcmp(normalized_contact, normalized_target) == 0;
        if (matched) {
            *index = i;
            ++matches;
            if (match_by_id && strcmp(contact->id, target_id) == 0) {
                id_consistent = true;
            }
        }
    }
    xSemaphoreGive(s_product_mutex);
    return matches == 1U && id_consistent ? CONTACT_MATCH_UNIQUE
           : matches > 1U ? CONTACT_MATCH_AMBIGUOUS
                          : CONTACT_MATCH_NOT_FOUND;
}

/* AI 服务的 device_action 是一个可扩展的工具调用协议。不同角色版本会把
 * 参数放在 data/arguments 等包装对象中；在这里归一化，后面的拨号策略始终
 * 只面对本机通讯录中的联系人，绝不相信模型给出的任意设备 ID。 */
static const cJSON *action_payload(const cJSON *params)
{
    if (!cJSON_IsObject(params)) {
        return NULL;
    }
    static const char *const wrappers[] = {
        "data", "arguments", "args", "input", "payload", "parameters",
    };
    for (size_t i = 0; i < sizeof(wrappers) / sizeof(wrappers[0]); ++i) {
        const cJSON *nested = cJSON_GetObjectItemCaseSensitive(params, wrappers[i]);
        if (cJSON_IsObject(nested)) {
            return nested;
        }
    }
    return params;
}

static bool copy_first_json_string(const cJSON *object,
                                   const char *const *names,
                                   size_t count,
                                   char *destination,
                                   size_t capacity)
{
    if (destination == NULL || capacity == 0U) {
        return false;
    }
    destination[0] = '\0';
    if (!cJSON_IsObject(object)) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, names[i]);
        if (!cJSON_IsString(item) || item->valuestring == NULL ||
            item->valuestring[0] == '\0' || strlen(item->valuestring) >= capacity) {
            continue;
        }
        (void)snprintf(destination, capacity, "%s", item->valuestring);
        return true;
    }
    return false;
}

static bool ai_call_action_name(const char *action)
{
    return action != NULL &&
           (strcmp(action, "call") == 0 || strcmp(action, "call_contact") == 0 ||
            strcmp(action, "call_device") == 0 || strcmp(action, "device_call") == 0 ||
            strcmp(action, "start_device_call") == 0 ||
            strcmp(action, "call_wechat") == 0 || strcmp(action, "wechat_call") == 0 ||
            strcmp(action, "start_voip_call") == 0);
}

#include "starter_contact_query.inc"

static void send_device_action_result(const cJSON *request_id,
                                      bool ok,
                                      const char *status,
                                      const char *message)
{
    /* JSON-RPC notification 没有 id，无需也不能回复。 */
    if (request_id == NULL || (!cJSON_IsString(request_id) && !cJSON_IsNumber(request_id))) {
        return;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *result = cJSON_CreateObject();
    cJSON *id = cJSON_Duplicate(request_id, true);
    bool valid = root != NULL && result != NULL && id != NULL &&
                 cJSON_AddStringToObject(root, "jsonrpc", "2.0") &&
                 cJSON_AddBoolToObject(result, "ok", ok) &&
                 cJSON_AddStringToObject(result, "status", status) &&
                 cJSON_AddStringToObject(result, "message", message);
    if (valid) {
        valid = cJSON_AddItemToObject(root, "id", id);
        if (valid) id = NULL;
    }
    if (valid) {
        valid = cJSON_AddItemToObject(root, "result", result);
        if (valid) result = NULL;
    }
    char *json = valid ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(id);
    cJSON_Delete(result);
    cJSON_Delete(root);
    if (json == NULL) {
        ESP_LOGE(TAG, "AI action reply allocation failed");
        return;
    }
    int rc = starter_tirtc_send_command(AI_COMMAND, json, (uint32_t)strlen(json));
    cJSON_free(json);
    if (rc < 0) ESP_LOGE(TAG, "AI action reply send failed rc=%d", rc);
}

static contact_match_t parse_ai_call_action(const cJSON *params,
                                            bool legacy_call_intent,
                                            char *target_id,
                                            size_t target_id_capacity,
                                            char *target_name,
                                            size_t target_name_capacity,
                                            char *channel,
                                            size_t channel_capacity,
                                            char *media,
                                            size_t media_capacity,
                                            uint8_t *contact_index)
{
    const cJSON *payload = action_payload(params);
    static const char *const action_names[] = {"action", "name", "tool", "function"};
    static const char *const target_id_names[] = {
        "target_id", "target_device_id", "device_id", "callee_device_id", "peer_id",
    };
    static const char *const target_name_names[] = {
        "target_name", "target", "contact_name", "contact", "device_name",
        "device_alias", "nickname", "alias", "remark", "callee", "name",
    };
    static const char *const channel_names[] = {"channel", "source", "type"};
    static const char *const media_names[] = {"media", "call_type", "room_type"};
    char action[32] = "";
    if (!legacy_call_intent &&
        (!(copy_first_json_string(params, action_names,
                                  sizeof(action_names) / sizeof(action_names[0]),
                                  action, sizeof(action)) ||
            copy_first_json_string(payload, action_names,
                                   sizeof(action_names) / sizeof(action_names[0]),
                                   action, sizeof(action))) ||
         !ai_call_action_name(action))) {
        return CONTACT_MATCH_NOT_FOUND;
    }
    (void)copy_first_json_string(payload, target_id_names,
                                 sizeof(target_id_names) / sizeof(target_id_names[0]),
                                 target_id, target_id_capacity);
    (void)copy_first_json_string(payload, target_name_names,
                                 sizeof(target_name_names) / sizeof(target_name_names[0]),
                                 target_name, target_name_capacity);
    (void)copy_first_json_string(payload, channel_names,
                                 sizeof(channel_names) / sizeof(channel_names[0]),
                                 channel, channel_capacity);
    (void)copy_first_json_string(payload, media_names,
                                 sizeof(media_names) / sizeof(media_names[0]),
                                 media, media_capacity);
    bool audio_only = media[0] == '\0' || strcmp(media, "audio") == 0 ||
                      strcmp(media, "voice") == 0;
    return audio_only ? find_contact(target_id, target_name, channel, contact_index)
                      : CONTACT_MATCH_NOT_FOUND;
}

static void handle_ai_command(const runtime_event_t *event)
{
    /* 当前 AI 连接上的 start_session 响应与 UI 通知都在状态任务串行处理。 */
    if (event->mode != STARTER_TIRTC_AI ||
        event->generation != s_connection_generation) {
        diagnostic_event("AI stale command", (int)event->generation);
        return;
    }
    if (!starter_tirtc_is_ai_command(event->command)) {
        diagnostic_event("AI unknown command", (int)event->command);
        ESP_LOGW(TAG, "AI command ignored: cmd=0x%08lx gen=%lu bytes=%lu",
                 (unsigned long)event->command, (unsigned long)event->generation,
                 (unsigned long)event->length);
        return;
    }
    cJSON *root = event->text == NULL
                      ? NULL
                      : cJSON_ParseWithLength(event->text, event->length);
    if (root == NULL) {
        diagnostic_event("AI invalid JSON", 0);
        ESP_LOGW(TAG, "AI invalid JSON: cmd=0x%08lx bytes=%lu",
                 (unsigned long)event->command, (unsigned long)event->length);
        return;
    }
    const cJSON *id = root == NULL
                          ? NULL
                          : cJSON_GetObjectItemCaseSensitive(root, "id");
    /* result/error 都必须属于当前 start_session，不能让无 ID 的旁路命令改状态。 */
    bool id_matches = cJSON_IsString(id) && id->valuestring != NULL &&
                      strcmp(id->valuestring, s_ai_request_id) == 0;
    bool rejected = root != NULL && id_matches &&
                    cJSON_GetObjectItemCaseSensitive(root, "error") != NULL;
    const cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    const cJSON *session_id = cJSON_IsObject(result)
                                  ? cJSON_GetObjectItemCaseSensitive(result, "session_id")
                                  : NULL;
    const cJSON *input_audio = cJSON_IsObject(result)
                                   ? cJSON_GetObjectItemCaseSensitive(result, "input_audio")
                                   : NULL;
    const cJSON *output_audio = cJSON_IsObject(result)
                                    ? cJSON_GetObjectItemCaseSensitive(result, "output_audio")
                                    : NULL;
    bool accepted = id_matches && cJSON_IsString(session_id) &&
                    session_id->valuestring != NULL && session_id->valuestring[0] != '\0' &&
                    ai_audio_profile_valid(input_audio) &&
                    ai_audio_profile_valid(output_audio);
    starter_runtime_state_t state = (starter_runtime_state_t)atomic_load_explicit(
        &s_public_state, memory_order_acquire);
    if (rejected) {
        ESP_LOGE(TAG, "AI start_session was rejected");
        cJSON_Delete(root);
        finish_session(ESP_FAIL);
    } else if (state == STARTER_RUNTIME_AI_CONNECTING && id_matches && !accepted) {
        ESP_LOGE(TAG, "AI start_session returned an unsupported audio profile");
        cJSON_Delete(root);
        finish_session(ESP_ERR_NOT_SUPPORTED);
    } else if (state == STARTER_RUNTIME_AI_CONNECTING && accepted) {
        /* 服务端明确接受后才开放麦克风，避免把音频发到未建立的 AI 会话。 */
        cJSON_Delete(root);
        if (starter_media_start(STARTER_TIRTC_AI,
                                s_connection_generation) != ESP_OK) {
            finish_session(ESP_ERR_INVALID_STATE);
            return;
        }
        s_deadline_ms = 0;
        product_set_phase(STARTER_AI_UI_LISTENING);
        publish_state(STARTER_RUNTIME_AI_ACTIVE);
        diagnostic_event("AI ready ms", (int)(now_ms() - s_diagnostic_ai_started_ms));
    } else if (state == STARTER_RUNTIME_AI_ACTIVE) {
        const cJSON *method = cJSON_GetObjectItemCaseSensitive(root, "method");
        const cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
        if (cJSON_IsString(method) && !strcmp(method->valuestring, "device_action")) {
            static const char *const names[] = {"action", "name", "tool", "function"};
            char action[64] = "";
            bool named = copy_first_json_string(params, names, 4, action, sizeof(action)) ||
                copy_first_json_string(action_payload(params), names, 4, action, sizeof(action));
            ESP_LOGI(TAG, "AI tool rx: action=%.63s id=%u gen=%lu cmd=0x%08lx",
                     named ? action : "(invalid)", (unsigned)(id != NULL),
                     (unsigned long)s_session_generation, (unsigned long)event->command);
            if (named && !strcmp(action, "query_contact_status")) {
                start_contact_query(id, params);
                cJSON_Delete(root);
                return;
            }
            if (!named || !ai_call_action_name(action)) {
                contact_query_reply(id, false, named ? "unsupported_action" : "invalid_params",
                                    "设备未实现该动作或参数无效。", NULL, 0, NULL);
                cJSON_Delete(root);
                return;
            }
        }
        if (cJSON_IsString(method) && method->valuestring != NULL) {
            if (strcmp(method->valuestring, "caption") == 0 &&
                cJSON_IsObject(params)) {
                const cJSON *caption_type = cJSON_GetObjectItemCaseSensitive(
                    params, "caption_type");
                const cJSON *text = cJSON_GetObjectItemCaseSensitive(params, "text");
                const cJSON *mode = cJSON_GetObjectItemCaseSensitive(params, "mode");
                const cJSON *final = cJSON_GetObjectItemCaseSensitive(params, "is_final");
                const cJSON *emotion = cJSON_GetObjectItemCaseSensitive(params, "emotion");
                const cJSON *utterance = cJSON_GetObjectItemCaseSensitive(params, "utterance_id");
                if (cJSON_IsNumber(caption_type) && cJSON_IsString(text) &&
                    text->valuestring != NULL) {
                    bool is_ai = caption_type->valueint == 1;
                    bool is_final = cJSON_IsBool(final) && cJSON_IsTrue(final);
                    bool append = cJSON_IsNumber(mode) && mode->valueint == 1;
                    if (is_final) diagnostic_event(is_ai ? "AI caption final" : "ASR caption final", 0);
                    product_set_caption(is_ai,
                                        is_final,
                                        text->valuestring,
                                        append,
                                        cJSON_IsString(emotion) ? emotion->valuestring : NULL,
                                        cJSON_IsNumber(utterance) ? utterance->valueint : -1);
                    product_set_phase(is_ai ? STARTER_AI_UI_SPEAKING :
                                      (is_final ? STARTER_AI_UI_THINKING :
                                                  STARTER_AI_UI_LISTENING));
                }
            } else if (strcmp(method->valuestring, "round_start") == 0) {
                product_set_phase(STARTER_AI_UI_SPEAKING);
            } else if (strcmp(method->valuestring, "round_end") == 0) {
                product_set_phase(STARTER_AI_UI_LISTENING);
            } else if ((strcmp(method->valuestring, "call_intent") == 0 ||
                        strcmp(method->valuestring, "ai_call_intent") == 0 ||
                        strcmp(method->valuestring, "device_action") == 0) &&
                       cJSON_IsObject(params)) {
                char target_id[65] = "";
                char target_name[65] = "";
                char channel[16] = "";
                char media[16] = "";
                uint8_t contact_index = 0;
                bool legacy_call_intent = strcmp(method->valuestring, "device_action") != 0;
                diagnostic_event("call tool received", legacy_call_intent ? 0 : 1);
                contact_match_t match = parse_ai_call_action(
                    params, legacy_call_intent, target_id, sizeof(target_id),
                    target_name, sizeof(target_name), channel, sizeof(channel),
                    media, sizeof(media), &contact_index);
                bool is_device_action = !legacy_call_intent;
                if (match == CONTACT_MATCH_UNIQUE) {
                    diagnostic_event("call tool accepted", 0);
                    if (is_device_action) {
                        send_device_action_result(id, true, "accepted", "正在发起语音呼叫。");
                    }
                    const char end[] =
                        "{\"jsonrpc\":\"2.0\",\"method\":\"end_session\"}";
                    (void)starter_tirtc_send_command(AI_COMMAND,
                                                     end,
                                                     sizeof(end) - 1U);
                    ESP_LOGI(TAG,
                             "AI call accepted: target=%s channel=%s contact-index=%u",
                             target_name[0] != '\0' ? target_name : target_id,
                             channel[0] != '\0' ? channel : "auto",
                             (unsigned)contact_index);
                    cJSON_Delete(root);
                    finish_session(0);
                    dial_contact(contact_index, false);
                } else {
                    diagnostic_event("call tool rejected", (int)match);
                    const char *feedback = media[0] != '\0' &&
                                           strcmp(media, "audio") != 0 &&
                                           strcmp(media, "voice") != 0
                        ? "小钛目前只支持语音呼叫。"
                        : (match == CONTACT_MATCH_AMBIGUOUS
                               ? "找到多个同名联系人，请说得更具体。"
                               : "通讯录里没有找到这个联系人。");
                    if (is_device_action) {
                        send_device_action_result(id, false,
                                                  match == CONTACT_MATCH_AMBIGUOUS
                                                      ? "ambiguous" : "not_found",
                                                  feedback);
                    }
                    product_set_caption(true, true, feedback, false, "confused", -1);
                    product_set_phase(STARTER_AI_UI_LISTENING);
                    ESP_LOGW(TAG,
                             "AI call intent rejected by local contacts: match=%d",
                             (int)match);
                    cJSON_Delete(root);
                }
                return;
            } else if (strcmp(method->valuestring, "end_session") == 0) {
                /* The AI service can end an idle conversation autonomously.
                 * Keep a distinct record so an expected remote timeout is not
                 * mistaken for a local UI or media failure. */
                ESP_LOGI(TAG,
                         "AI session ended by remote end_session/idle policy generation=%lu",
                         (unsigned long)s_connection_generation);
                cJSON_Delete(root);
                finish_session(0);
                return;
            } else {
                diagnostic_event("AI unknown method/params", 0);
            }
        }
        cJSON_Delete(root);
    } else {
        cJSON_Delete(root);
    }
}

static void end_ai_session(void)
{
    /* end_session 是尽力通知；本地资源释放不等待远端响应。 */
    starter_runtime_state_t state = (starter_runtime_state_t)atomic_load_explicit(
        &s_public_state, memory_order_acquire);
    if (state != STARTER_RUNTIME_AI_CONNECTING &&
        state != STARTER_RUNTIME_AI_ACTIVE) {
        return;
    }
    if (starter_tirtc_connected()) {
        const char end[] = "{\"jsonrpc\":\"2.0\",\"method\":\"end_session\"}";
        (void)starter_tirtc_send_command(AI_COMMAND, end, sizeof(end) - 1U);
    }
    finish_session(0);
}

static const char *wechat_incoming_room_type(const cJSON *payload, bool *inferred)
{
    static const char *const keys[] = {
        "wx_room_type", "wxa_room_type", "room_type", "media_type"
    };
    *inferred = false;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(payload, keys[i]);
        if (value == NULL || cJSON_IsNull(value)) continue;
        if (!cJSON_IsString(value)) return "invalid";
        if (value->valuestring[0] != '\0') return value->valuestring;
    }
    /* WeChat may omit the optional room type. Match the published P4's
     * advertised video capability; explicit voice always takes precedence. */
    *inferred = true;
#if CONFIG_IDF_TARGET_ESP32P4
    return "video";
#else
    return "voice";
#endif
}

static void reconcile_platform_binding(bool known_unbound)
{
    if (!platform_client_request_rebind(known_unbound)) return;
    room_reset();
    finish_session(0);
    s_voip_profile_ready = false;
    s_voip_profile_inflight = false;
    s_voip_profile_retry_at_ms = 0;
    s_device_profile_attempts = 0;
    atomic_store(&s_voip_profile_delivery_failed, 0);
    if (s_product_mutex != NULL && xSemaphoreTake(s_product_mutex, portMAX_DELAY) == pdTRUE) {
        memset(s_product_snapshot.contacts, 0, sizeof(s_product_snapshot.contacts));
        s_product_snapshot.contact_count = 0;
        xSemaphoreGive(s_product_mutex);
    }
    platform_client_rebind_quiesced();
    ESP_LOGW(TAG, "binding transition requested: unbound=%d; identity retained", known_unbound);
}

static void handle_platform_signal(const runtime_event_t *event)
{
    cJSON *root = event->text == NULL
                      ? NULL
                      : cJSON_ParseWithLength(event->text, event->length);
    const cJSON *type = root == NULL
                            ? NULL
                            : cJSON_GetObjectItemCaseSensitive(root, "type");
    const char *signal = cJSON_IsString(type) && type->valuestring != NULL
                             ? type->valuestring : "";
    if (strcmp(signal, "unbind") == 0) {
        cJSON_Delete(root);
        /* The PSRAM session stack must not erase NVS or reset the chip.
         * Ownership changes retain the physical identity for signed Report. */
        reconcile_platform_binding(true);
        return;
    }
    if (!platform_client_ready()) { cJSON_Delete(root); return; }
    if (strcmp(signal, "room_assignment_changed") == 0) {
        /* MQTT is only a hint; no credentials or mic commands are accepted here. */
        ++s_room.change_revision;
        s_room.blocked = false;
        s_room.failures = 0;
        s_room.retry_at = 0;
        s_room.dirty = true;
        s_room.poll_at = now_ms();
        cJSON_Delete(root);
        return;
    }
    if (strcmp(signal, "callers_update") == 0 ||
        strcmp(signal, "contacts_update") == 0) {
        cJSON_Delete(root);
        refresh_contacts();
        return;
    }
    const cJSON *payload = cJSON_IsObject(root)
                               ? cJSON_GetObjectItemCaseSensitive(root, "payload")
                               : NULL;
    const cJSON *channel = cJSON_IsObject(root)
                               ? cJSON_GetObjectItemCaseSensitive(root, "channel")
                               : NULL;
    bool wechat = cJSON_IsString(channel) && channel->valuestring != NULL &&
                  strcmp(channel->valuestring, "wx") == 0;
    if (wechat && strcmp(signal, "call_incoming") == 0 && !s_voip_profile_ready) {
        ESP_LOGW(TAG, "ignoring WeChat call before VoIP profile readiness");
        cJSON_Delete(root);
        request_device_profile();
        return;
    }
    if (strcmp(signal, "room_cancel") == 0 ||
        strcmp(signal, "call_cancel") == 0 ||
        strcmp(signal, "call_reject") == 0) {
        bool rejected = strcmp(signal, "call_reject") == 0;
        char room[129];
        bool got_room = copy_json_string(payload,
                                         wechat ? "wx_room_id" : "room_id",
                                         room, sizeof(room), false);
        cJSON_Delete(root);
        if (call_signal_matches_room(got_room, room, s_call_room_id)) {
            starter_runtime_state_t state =
                (starter_runtime_state_t)atomic_load_explicit(
                    &s_public_state, memory_order_acquire);
            if (state == STARTER_RUNTIME_CALL_INCOMING ||
                state == STARTER_RUNTIME_CALL_CONNECTING ||
                state == STARTER_RUNTIME_CALL_ACTIVE) {
                finish_call_session(0,
                                    rejected ? "对方拒接"
                                             : (state == STARTER_RUNTIME_CALL_INCOMING
                                                    ? "来电已取消"
                                                    : "对方已挂断"));
            }
        }
        return;
    }
    if (strcmp(signal, "callee_answered") == 0 && cJSON_IsObject(payload) &&
        !wechat) {
        char room[129] = "";
        char callee[65] = "";
        (void)copy_json_string(payload, "room_id", room, sizeof(room), false);
        (void)copy_json_string(payload, "callee_id", callee, sizeof(callee), false);
        cJSON_Delete(root);
        starter_runtime_state_t state = (starter_runtime_state_t)atomic_load_explicit(
            &s_public_state, memory_order_acquire);
        bool matched = state == STARTER_RUNTIME_CALL_CONNECTING &&
                       !s_call_wechat && s_call_outgoing &&
                       room[0] != '\0' && strcmp(room, s_call_room_id) == 0;
        ESP_LOGI(TAG,
                 "[DEBUG-call] callee_answered room=%s matched=%d p2p=%d",
                 room[0] != '\0' ? room : "-", matched ? 1 : 0,
                 s_call_p2p_connected ? 1 : 0);
        if (matched) {
            s_call_peer_answered = true;
            if (callee[0] != '\0') {
                (void)snprintf(s_call_peer_id, sizeof(s_call_peer_id), "%s", callee);
            }
            s_deadline_ms = now_ms() + CALL_CONNECT_TIMEOUT_MS;
            maybe_activate_outgoing_device_call("cloud-answer");
        }
        return;
    }
    if (strcmp(signal, "call_incoming") != 0 || !cJSON_IsObject(payload)) {
        cJSON_Delete(root);
        return;
    }

    starter_runtime_state_t state = (starter_runtime_state_t)atomic_load_explicit(
        &s_public_state, memory_order_acquire);
    char room[129] = "";
    char peer[1024] = "";
    char token[1024] = "";
    char peer_name[65] = "";
    char wx_open_id[65] = "";
    char call_id[65] = "";
    char from_device[65] = "";
    char room_type[16] = "";
    bool fields_ok;
    if (wechat) {
        fields_ok = copy_json_string(payload, "wx_room_id", room, sizeof(room), true) &&
                    copy_json_string(payload, "peer_id", peer, sizeof(peer), true) &&
                    copy_json_string(payload, "token", token, sizeof(token), true);
        (void)copy_json_string(payload, "wx_user_openid", wx_open_id,
                               sizeof(wx_open_id), false);
        (void)copy_json_string(payload, "wx_user_remark", peer_name,
                               sizeof(peer_name), false);
        (void)copy_json_string(payload, "wx_call_id", call_id,
                               sizeof(call_id), false);
        (void)copy_json_string(payload, "wx_from", from_device,
                               sizeof(from_device), false);
        bool inferred_media = false;
        const char *media = wechat_incoming_room_type(payload, &inferred_media);
        (void)snprintf(room_type, sizeof(room_type), "%s", media);
        ESP_LOGI(TAG, "VoIP incoming media: type=%.15s source=%s", room_type,
                 inferred_media ? "profile" : "signal");
    } else {
        fields_ok = copy_json_string(payload, "room_id", room, sizeof(room), true) &&
                    copy_json_string(payload, "caller_id", peer, sizeof(peer), true);
        (void)copy_json_string(payload, "caller_name", peer_name,
                               sizeof(peer_name), false);
        (void)copy_json_string(payload, "call_type", room_type,
                               sizeof(room_type), false);
    }
    if (!fields_ok) {
        cJSON_Delete(root);
        return;
    }

    /* 微信外呼的 MQTT 回铃是同一流程，不再弹一次“来电”。 */
    bool outgoing_wechat = wechat && state == STARTER_RUNTIME_CALL_CONNECTING &&
                           s_call_wechat && s_call_outgoing &&
                           (from_device[0] == '\0' ||
                            strcmp(from_device, s_device_id) == 0) &&
                           (s_call_id[0] == '\0' || call_id[0] == '\0' ||
                            strcmp(s_call_id, call_id) == 0);

    /* 已取消外呼的迟到回推绝不能重新显示为用户来电。 */
    bool orphaned_outgoing_wechat = wechat && !outgoing_wechat &&
                                    from_device[0] != '\0' &&
                                    strcmp(from_device, s_device_id) == 0;
    if (orphaned_outgoing_wechat) {
        char app_id[65] = "";
        char model_id[65] = "";
        char server_token[257] = "";
        char wx_payload[513] = "";
        (void)copy_json_string(payload, "wx_app_id", app_id,
                               sizeof(app_id), false);
        (void)copy_json_string(payload, "wx_model_id", model_id,
                               sizeof(model_id), false);
        (void)copy_json_string(payload, "wx_server_token", server_token,
                               sizeof(server_token), false);
        (void)copy_json_string(payload, "wx_payload", wx_payload,
                               sizeof(wx_payload), false);
        reject_wechat_values(app_id, model_id, server_token, room,
                             wx_payload, 7);
        cJSON_Delete(root);
        return;
    }

    /* S3 产品只实现语音；明确拒绝视频房间，避免出现“能接听却无视频”的假能力。 */
    bool unsupported_video = room_type[0] != '\0' &&
                             strcmp(room_type, "voice") != 0 &&
                             strcmp(room_type, "audio") != 0;
#if CONFIG_IDF_TARGET_ESP32P4
    unsupported_video = unsupported_video && strcmp(room_type, "video") != 0;
#endif
    if (unsupported_video) {
        if (wechat) {
            char app_id[65] = "";
            char model_id[65] = "";
            char server_token[257] = "";
            char wx_payload[513] = "";
            (void)copy_json_string(payload, "wx_app_id", app_id,
                                   sizeof(app_id), false);
            (void)copy_json_string(payload, "wx_model_id", model_id,
                                   sizeof(model_id), false);
            (void)copy_json_string(payload, "wx_server_token", server_token,
                                   sizeof(server_token), false);
            (void)copy_json_string(payload, "wx_payload", wx_payload,
                                   sizeof(wx_payload), false);
            reject_wechat_values(app_id, model_id, server_token, room,
                                 wx_payload, 7);
        } else {
            char body[256];
            (void)snprintf(body, sizeof(body),
                           "{\"room_id\":\"%s\",\"reason\":\"unsupported_media\"}",
                           room);
            (void)platform_client_request(PLATFORM_SERVICE_CALL,
                                          "/v1/call/reject", body, NULL, NULL);
        }
        cJSON_Delete(root);
        return;
    }

    bool duplicate = state >= STARTER_RUNTIME_CALL_INCOMING &&
                     state <= STARTER_RUNTIME_CALL_ACTIVE &&
                     s_call_room_id[0] != '\0' &&
                     strcmp(s_call_room_id, room) == 0;
    if (duplicate) {
        cJSON_Delete(root);
        return;
    }
    if (!outgoing_wechat && state != STARTER_RUNTIME_WAITING &&
        state != STARTER_RUNTIME_AI_ACTIVE &&
        state != STARTER_RUNTIME_AI_CONNECTING &&
        state != STARTER_RUNTIME_H5_ACTIVE && !room_owns_media()) {
        if (wechat) {
            char app_id[65] = "";
            char model_id[65] = "";
            char server_token[257] = "";
            char wx_payload[513] = "";
            (void)copy_json_string(payload, "wx_app_id", app_id,
                                   sizeof(app_id), false);
            (void)copy_json_string(payload, "wx_model_id", model_id,
                                   sizeof(model_id), false);
            (void)copy_json_string(payload, "wx_server_token", server_token,
                                   sizeof(server_token), false);
            (void)copy_json_string(payload, "wx_payload", wx_payload,
                                   sizeof(wx_payload), false);
            reject_wechat_values(app_id, model_id, server_token, room,
                                 wx_payload, 8);
        } else {
            char body[224];
            (void)snprintf(body, sizeof(body),
                           "{\"room_id\":\"%s\",\"reason\":\"busy\"}", room);
            (void)platform_client_request(PLATFORM_SERVICE_CALL,
                                          "/v1/call/reject", body, NULL, NULL);
        }
        cJSON_Delete(root);
        return;
    }
    if (!outgoing_wechat) {
        /* 产品优先级：来电高于 AI/H5，先完整释放旧 owner 再登记 pending。 */
        if (!preempt_for_call(true)) {
            cJSON_Delete(root);
            return;
        }
        s_session_generation++;
        if (s_session_generation == 0U) {
            s_session_generation = 1U;
        }
        starter_tirtc_accept_h5(false);
        s_call_outgoing = false;
    }
    s_call_wechat = wechat;
#if CONFIG_IDF_TARGET_ESP32P4
    if (!s_call_outgoing) {
        s_call_video = strcmp(room_type, "video") == 0;
        s_call_camera_enabled = s_call_video;
        starter_media_set_call_video(s_call_video);
    }
#endif
    (void)snprintf(s_call_room_id, sizeof(s_call_room_id), "%s", room);
    (void)snprintf(s_call_peer_id, sizeof(s_call_peer_id), "%.64s",
                   wechat && wx_open_id[0] != '\0' ? wx_open_id : peer);
    (void)snprintf(s_call_peer_name, sizeof(s_call_peer_name), "%.64s",
                   peer_name[0] == '\0'
                       ? (wechat && wx_open_id[0] != '\0'
                              ? wx_open_id
                              : (wechat ? "微信联系人" : peer))
                       : peer_name);
    if (wechat) {
        (void)snprintf(s_call_connect_peer,
                       sizeof(s_call_connect_peer), "%s", peer);
        (void)snprintf(s_call_connect_token,
                       sizeof(s_call_connect_token), "%s", token);
        (void)copy_json_string(payload, "wx_app_id", s_call_wx_app_id,
                               sizeof(s_call_wx_app_id), false);
        (void)copy_json_string(payload, "wx_model_id", s_call_wx_model_id,
                               sizeof(s_call_wx_model_id), false);
        (void)copy_json_string(payload, "wx_server_token", s_call_wx_session_token,
                               sizeof(s_call_wx_session_token), false);
        (void)copy_json_string(payload, "wx_payload", s_call_wx_payload,
                               sizeof(s_call_wx_payload), false);
    }
    cJSON_Delete(root);
    product_snapshot_reset();
    product_set_call(!outgoing_wechat, wechat, s_call_peer_name, false);
    if (outgoing_wechat) {
        publish_state(STARTER_RUNTIME_CALL_CONNECTING);
        if (!queue_event(&(runtime_event_t){.type = EVENT_VOIP_CONNECT})) {
            finish_call_session(ESP_ERR_TIMEOUT, "呼叫繁忙");
        }
    } else {
        s_deadline_ms = now_ms() + CALL_PENDING_TIMEOUT_MS;
        publish_state(STARTER_RUNTIME_CALL_INCOMING);
        diagnostic_event("incoming call", wechat);
    }
}

static void runtime_task(void *argument)
{
    (void)argument;
    publish_state(STARTER_RUNTIME_WAITING);
    for (;;) {
        /* A lost platform signal requires a server-side binding check, not a
         * reboot. Only a confirmed 6006 enters signed rebind in the HTTP owner. */
        if (atomic_exchange_explicit(&s_platform_reconcile_required,
                                     false,
                                     memory_order_acq_rel)) {
            ESP_LOGE(TAG, "reconciling binding after platform signal queue overflow");
            reconcile_platform_binding(false);
        }
        if (atomic_exchange_explicit(&s_transport_recovery_required,
                                     false,
                                     memory_order_acq_rel)) {
            ESP_LOGE(TAG, "resetting session after transport event queue overflow");
            finish_session(ESP_ERR_TIMEOUT);
        }
        if (recover_voip_profile_delivery_failure(now_ms())) {
            ESP_LOGW(TAG, "device profile response lost: retry_ms=%u",
                     s_voip_profile_retry_at_ms == 0 ? 0U : (unsigned)VOIP_PROFILE_RETRY_MS);
        }
        contact_query_tick();
        room_tick();
        if (s_connect_reserve_retry_ms != 0 &&
            now_ms() >= s_connect_reserve_retry_ms) {
            restore_external_connect_reserve();
        }

        /* 50 ms 轮询间隔同时用于驱动 AI 延迟发送和各阶段超时。 */
        runtime_event_t event;
        if (xQueueReceive(s_queue, &event, pdMS_TO_TICKS(50)) == pdTRUE) {
            room_apply_navigation();
            bool platform_event = event.type == EVENT_AI_TOKEN || event.type == EVENT_PLATFORM_SIGNAL ||
                event.type == EVENT_PLATFORM_ONLINE || event.type == EVENT_CONTACTS_RESULT ||
                event.type == EVENT_CONTACT_QUERY_RESULT || event.type == EVENT_DEVICE_PROFILE ||
                event.type == EVENT_CALL_HTTP || event.type == EVENT_ROOM_HTTP ||
                event.type == EVENT_ROOM_INTENT;
            if (platform_event && event.platform_epoch != platform_client_epoch()) {
                release_event(&event);
                continue;
            }
            switch (event.type) {
            case EVENT_TIRTC_STATE:
                if (!event.flag) {
                    finish_session(event.error);
                }
                break;
            case EVENT_CONNECTION:
                handle_connection(&event);
                break;
            case EVENT_COMMAND:
                if (event.mode == STARTER_TIRTC_ROOM) {
                    room_command(&event);
                } else if (event.mode == STARTER_TIRTC_AI) {
                    handle_ai_command(&event);
                } else {
                    handle_call_command(&event);
                }
                break;
            case EVENT_AI_START:
                begin_ai_session(event.generation);
                break;
            case EVENT_AI_STOP:
                end_ai_session();
                break;
            case EVENT_AI_TOKEN:
                handle_ai_token(&event);
                break;
            case EVENT_PLATFORM_SIGNAL:
                handle_platform_signal(&event);
                break;
            case EVENT_PLATFORM_ONLINE:
                if (!platform_client_ready()) break;
                s_room.dirty = true;
                s_room.poll_at = now_ms();
                if (atomic_load(&s_public_state) == STARTER_RUNTIME_WAITING)
                    starter_tirtc_accept_h5(true);
                /* MQTT token/会话换代后，服务端 profile 必须重新建立。 */
                s_voip_profile_ready = false;
                if (!s_voip_profile_inflight) {
                    s_device_profile_attempts = 0;
                    s_voip_profile_retry_at_ms = 0;
                }
                request_device_profile();
                /* Contacts/AI/H5 do not depend on the capability endpoint.
                 * Keep WeChat gated, but never hide contacts on report failure. */
                refresh_contacts();
                break;
            case EVENT_DEVICE_PROFILE:
                handle_device_profile(&event);
                break;
            case EVENT_VOIP_CONNECT:
                connect_voip();
                break;
            case EVENT_VOIP_CONNECT_RESULT:
                handle_voip_connect_result(&event);
                break;
            case EVENT_CONTACTS_REFRESH:
                refresh_contacts();
                break;
            case EVENT_CONTACTS_RESULT:
            case EVENT_CONTACT_QUERY_RESULT:
                handle_contacts_result(&event);
                break;
            case EVENT_CALL_DIAL:
                dial_contact((uint8_t)event.command, event.flag);
                break;
            case EVENT_CALL_ACCEPT:
                accept_call();
                break;
            case EVENT_CALL_REJECT:
                reject_or_hangup_call(true);
                break;
            case EVENT_CALL_HANGUP:
                reject_or_hangup_call(false);
                break;
            case EVENT_CALL_MIC_MUTE:
                if (atomic_load_explicit(&s_public_state, memory_order_acquire) >=
                        STARTER_RUNTIME_CALL_INCOMING &&
                    atomic_load(&s_public_state) <= STARTER_RUNTIME_CALL_ACTIVE) {
                    starter_media_set_microphone_muted(event.flag);
                    product_set_call(
                        atomic_load_explicit(&s_public_state, memory_order_acquire) ==
                            STARTER_RUNTIME_CALL_INCOMING,
                        s_call_wechat,
                        s_call_peer_name,
                        event.flag);
                }
                break;
            case EVENT_CALL_HTTP:
                handle_call_http(&event);
                break;
            case EVENT_ROOM_INTENT:
                room_handle_intent(&event);
                break;
            case EVENT_ROOM_HTTP:
                room_http_result(&event);
                break;
#if CONFIG_IDF_TARGET_ESP32P4
            case EVENT_CALL_CAMERA:
                if (event.generation == s_session_generation && s_call_video &&
                    atomic_load(&s_public_state) == STARTER_RUNTIME_CALL_ACTIVE &&
                    starter_media_set_camera_enabled(s_connection_generation, event.flag) == ESP_OK) {
                    s_call_camera_enabled = event.flag;
                    product_set_call(false, s_call_wechat, s_call_peer_name,
                                     starter_media_status().microphone_muted);
                }
                break;
#endif
            default:
                break;
            }
            release_event(&event);
        }
        int64_t current_ms = now_ms();
        if (!s_voip_profile_ready && !s_voip_profile_inflight &&
            s_voip_profile_retry_at_ms != 0 &&
            current_ms >= s_voip_profile_retry_at_ms) {
            request_device_profile();
        }
        if (starter_media_ai_preroll_failed()) {
            ESP_LOGW(TAG, "AI wake audio discontinuity/overflow/timeout; retry required");
            finish_session(ESP_ERR_TIMEOUT);
        }
        if (s_deadline_ms != 0 && current_ms >= s_deadline_ms) {
            ESP_LOGW(TAG, "session setup timed out");
            starter_runtime_state_t state =
                (starter_runtime_state_t)atomic_load_explicit(
                    &s_public_state, memory_order_acquire);
            if (state == STARTER_RUNTIME_CALL_INCOMING) {
                reject_or_hangup_call(true);
                product_set_call_result("来电已结束");
            } else if (state == STARTER_RUNTIME_CALL_CONNECTING) {
                reject_or_hangup_call(false);
                product_set_call_result("无人接听");
            } else if (room_owns_media()) {
                room_fail(ESP_ERR_TIMEOUT, "音频入房超时", true);
            } else {
                finish_session(ESP_ERR_TIMEOUT);
            }
        }
    }
}

esp_err_t starter_runtime_start(const char *device_id)
{
    if (s_task != NULL) {
        return ESP_OK;
    }
    if (device_id == NULL || device_id[0] == '\0' ||
        strlen(device_id) >= sizeof(s_device_id)) {
        return ESP_ERR_INVALID_ARG;
    }
    (void)snprintf(s_device_id, sizeof(s_device_id), "%s", device_id);
    s_queue = xQueueCreate(RUNTIME_QUEUE_DEPTH, sizeof(runtime_event_t));
    if (s_queue == NULL) return ESP_ERR_NO_MEM;
    s_product_mutex = xSemaphoreCreateMutex();
    if (s_product_mutex == NULL) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    product_snapshot_reset();
    if (xTaskCreateWithCaps(runtime_task,
                          "starter_session",
                          RUNTIME_TASK_STACK_BYTES,
                          NULL, 6, &s_task,
                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        vSemaphoreDelete(s_product_mutex);
        s_product_mutex = NULL;
        vQueueDelete(s_queue);
        s_queue = NULL;
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    /* Publish callbacks only after the consumer exists. The composition root
     * starts TiRTC/platform afterwards; failed initialization exposes no sink. */
    const starter_tirtc_handlers_t handlers = {
        .on_started = on_tirtc_started,
        .on_connection = on_tirtc_connection,
        .on_command = on_tirtc_command,
        .on_audio = on_tirtc_audio,
#if CONFIG_IDF_TARGET_ESP32P4
        .on_video = on_tirtc_video,
        .on_key_frame = on_tirtc_key_frame,
#endif
    };
    starter_tirtc_set_handlers(&handlers);
    platform_client_set_signal_handler(on_platform_signal, NULL);
    platform_client_set_online_handler(on_platform_online, NULL);
    return ESP_OK;
}

static esp_err_t enqueue_simple(runtime_event_type_t type)
{
    /* 返回成功只代表控制意图已入队，状态转换结果通过 status 查询。 */
    /* Before clock/binding complete, no consumer exists. This is not a full queue. */
    if (s_queue == NULL) return ESP_ERR_INVALID_STATE;
    const runtime_event_t event = {.type = type};
    return queue_event(&event) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t starter_runtime_ai_start(void)
{
    /* BOOT and console share the same admission rule as touch/voice. The
     * owner checks again when consuming the event to cover disconnect races. */
    if (!ai_network_ready()) return ESP_ERR_INVALID_STATE;
    return enqueue_simple(EVENT_AI_START);
}

esp_err_t starter_runtime_ai_start_from_wake(uint32_t wake_token)
{
    if (wake_token == 0) return ESP_ERR_INVALID_ARG;
    if (!ai_network_ready() || s_queue == NULL) return ESP_ERR_INVALID_STATE;
    const runtime_event_t event = {.type = EVENT_AI_START, .generation = wake_token};
    return queue_event(&event) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t starter_runtime_ai_stop(void)
{
    return enqueue_simple(EVENT_AI_STOP);
}

esp_err_t starter_runtime_contacts_refresh(void)
{
    return enqueue_simple(EVENT_CONTACTS_REFRESH);
}

esp_err_t starter_runtime_call_contact(uint8_t index)
{
    if (index >= STARTER_PRODUCT_CONTACTS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    const runtime_event_t event = {
        .type = EVENT_CALL_DIAL,
        .command = index,
    };
    return queue_event(&event) ? ESP_OK : ESP_ERR_TIMEOUT;
}

#if CONFIG_IDF_TARGET_ESP32P4
esp_err_t starter_runtime_call_set_camera_enabled(uint32_t session_generation, bool enabled)
{
    const runtime_event_t event = {.type = EVENT_CALL_CAMERA,
        .generation = session_generation, .flag = enabled};
    return queue_event(&event) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t starter_runtime_call_contact_video(uint8_t index)
{
    if (index >= STARTER_PRODUCT_CONTACTS_MAX) return ESP_ERR_INVALID_ARG;
    const runtime_event_t event = {.type = EVENT_CALL_DIAL, .command = index, .flag = true};
    return queue_event(&event) ? ESP_OK : ESP_ERR_TIMEOUT;
}
#endif
esp_err_t starter_runtime_call_accept(void)
{
    return enqueue_simple(EVENT_CALL_ACCEPT);
}

esp_err_t starter_runtime_call_reject(void)
{
    return enqueue_simple(EVENT_CALL_REJECT);
}

esp_err_t starter_runtime_call_hangup(void)
{
    return enqueue_simple(EVENT_CALL_HANGUP);
}

esp_err_t starter_runtime_call_set_microphone_muted(bool muted)
{
    const runtime_event_t event = {
        .type = EVENT_CALL_MIC_MUTE,
        .flag = muted,
    };
    return queue_event(&event) ? ESP_OK : ESP_ERR_TIMEOUT;
}

starter_runtime_status_t starter_runtime_status(void)
{
    return (starter_runtime_status_t) {
        .state = (starter_runtime_state_t)atomic_load_explicit(
            &s_public_state, memory_order_acquire),
        .session_generation = (uint32_t)atomic_load_explicit(
            &s_public_session_generation, memory_order_acquire),
        .connection_generation = (uint32_t)atomic_load_explicit(
            &s_public_connection_generation, memory_order_acquire),
        .last_error = atomic_load_explicit(&s_last_error, memory_order_acquire),
        .stack_high_water_bytes = s_task == NULL
                                      ? 0U
                                      : (uint32_t)uxTaskGetStackHighWaterMark(s_task),
    };
}

void starter_runtime_copy_device_id(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0U) {
        return;
    }
    (void)snprintf(out, out_size, "%s", s_device_id[0] == '\0' ? "未绑定" : s_device_id);
}

bool starter_runtime_try_product_snapshot(starter_runtime_product_snapshot_t *out)
{
    if (out == NULL || s_product_mutex == NULL || xSemaphoreTake(s_product_mutex, 0) != pdTRUE)
        return false;
    *out = s_product_snapshot;
    xSemaphoreGive(s_product_mutex);
    return true;
}

starter_runtime_product_snapshot_t starter_runtime_product_snapshot(void)
{
    starter_runtime_product_snapshot_t snapshot = {0};
    if (s_product_mutex != NULL &&
        xSemaphoreTake(s_product_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        snapshot = s_product_snapshot;
        xSemaphoreGive(s_product_mutex);
    }
    return snapshot;
}

const char *starter_runtime_state_name(starter_runtime_state_t state)
{
    switch (state) {
    case STARTER_RUNTIME_WAITING: return "waiting";
    case STARTER_RUNTIME_H5_ACTIVE: return "h5-active";
    case STARTER_RUNTIME_AI_CONNECTING: return "ai-connecting";
    case STARTER_RUNTIME_AI_ACTIVE: return "ai-active";
    case STARTER_RUNTIME_CALL_INCOMING: return "call-incoming";
    case STARTER_RUNTIME_CALL_CONNECTING: return "call-connecting";
    case STARTER_RUNTIME_CALL_ACTIVE: return "call-active";
    case STARTER_RUNTIME_ROOM_CONNECTING: return "room-connecting";
    case STARTER_RUNTIME_ROOM_ACTIVE: return "room-active";
    default: return "unknown";
    }
}
