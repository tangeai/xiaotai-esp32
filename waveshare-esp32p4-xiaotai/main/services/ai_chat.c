#include "ai_chat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "ai_chat_token.h"
#include "ai_chat_video.h"
#include "app_memory_policy.h"
#include "app_task_affinity.h"
#include "audio_device.h"
#include "system_time.h"
#include "tirtc_session.h"
#include "tiRTC.h"

static const char *TAG = "ai_chat";
static const char *DIALOG_TAG = "ai_dialog";

#define AI_CHAT_SIGNALING_CMD         0x2100U
#define AI_CHAT_AUDIO_STREAM_ID       1U
#define AI_CHAT_AUDIO_CODEC_NAME      "pcm"
#define AI_CHAT_START_SESSION_INCLUDE_AUDIO 1
#define AI_CHAT_AUDIO_SAMPLE_RATE     16000U
#define AI_CHAT_AUDIO_CHANNELS        1U
#define AI_CHAT_AUDIO_FRAME_MS        20U
#define AI_CHAT_AUDIO_FRAME_SAMPLES   (AI_CHAT_AUDIO_SAMPLE_RATE / 50U)
#define AI_CHAT_AUDIO_FRAME_BYTES     (AI_CHAT_AUDIO_FRAME_SAMPLES * sizeof(int16_t))
#define AI_CHAT_MEDIA_QUEUE_LEN       10
#define AI_CHAT_MEDIA_TASK_STACK      (5 * 1024)
#define AI_CHAT_MEDIA_TASK_PRIORITY   10
#define AI_CHAT_MEDIA_ALLOC_CAPS      (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define AI_CHAT_TASK_ALLOC_CAPS       (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define AI_CHAT_MEDIA_TX_LOG_INTERVAL_MS 10000U
#define AI_CHAT_VIDEO_START_TASK_STACK (4 * 1024)
#define AI_CHAT_VIDEO_START_TASK_PRIORITY 5
#define AI_CHAT_START_TASK_STACK      (16 * 1024)
#define AI_CHAT_START_TASK_PRIORITY   5
#define AI_CHAT_START_RETRY_TASK_STACK (3 * 1024)
#define AI_CHAT_START_RETRY_TASK_PRIORITY 5
#define AI_CHAT_START_RETRY_POLL_MS   100U
#define AI_CHAT_START_RETRY_TIMEOUT_MS 30000U
#define AI_CHAT_SESSION_TASK_STACK    (6 * 1024)
#define AI_CHAT_SESSION_TASK_PRIORITY 5
#define AI_CHAT_START_SESSION_SETTLE_MS 80U
#define AI_CHAT_HEARTBEAT_TASK_STACK  (4 * 1024)
#define AI_CHAT_HEARTBEAT_TASK_PRIORITY 5
#define AI_CHAT_DEVICE_ACTION_TASK_STACK (8 * 1024)
#define AI_CHAT_DEVICE_ACTION_TASK_PRIORITY 5
#define AI_CHAT_DEVICE_ACTION_ERROR_UNSUPPORTED (-32010)
#define AI_CHAT_DEVICE_ACTION_ERROR_BUSY        (-32011)
#define AI_CHAT_DEVICE_ACTION_ERROR_TARGET      (-32012)
#define AI_CHAT_DEVICE_ACTION_ERROR_INVALID     (-32013)
#define AI_CHAT_DEVICE_ACTION_ERROR_LOADING     (-32014)
#define AI_CHAT_DEVICE_ACTION_ERROR_INTERNAL    (-32000)
#define AI_CHAT_RTC_READY_WAIT_MS     30000U
#define AI_CHAT_RTC_READY_POLL_MS     100U
#define AI_CHAT_CONNECT_TIMEOUT_MS    20000U
#define AI_CHAT_CONNECT_TIMEOUT_TASK_STACK (3 * 1024)
#define AI_CHAT_CONNECT_TIMEOUT_TASK_PRIORITY 5
#define AI_CHAT_START_SESSION_TIMEOUT_MS    30000U
#define AI_CHAT_START_SESSION_TIMEOUT_TASK_STACK (3 * 1024)
#define AI_CHAT_START_SESSION_TIMEOUT_TASK_PRIORITY 5
#define AI_CHAT_HEARTBEAT_INTERVAL_MS 30000U
#define AI_CHAT_LIFECYCLE_WAIT_POLL_MS 100U
#define AI_CHAT_START_SESSION_RPC_ID  "start-session-001"

#if AI_CHAT_VIDEO_STREAM_ID == AI_CHAT_AUDIO_STREAM_ID
#error "AI Chat video and audio stream IDs must not collide"
#endif

typedef struct {
    tirtc_conn_t conn;
    TIRTCFRAMEINFO frame;
    uint8_t data[AI_CHAT_AUDIO_FRAME_BYTES];
} ai_chat_media_packet_t;

typedef struct {
    tirtc_conn_t conn;
    QueueHandle_t queue;
    TaskHandle_t task;
    bool initialized;
    bool running;
    bool uplink_enabled;
    uint32_t next_ts_ms;
    uint32_t tx_frames;
    uint32_t tx_failures;
    uint32_t dropped_frames;
    TickType_t last_backpressure_log_tick;
    TickType_t last_format_drop_log_tick;
    TickType_t last_tx_window_log_tick;
    uint32_t tx_window_frames;
    uint32_t tx_window_payload_bytes;
    uint32_t tx_window_peak_percent;
    bool tx_started_logged;
    bool audio_prepared;
} ai_chat_media_state_t;

typedef struct {
    int caption_type;
    int64_t utterance_id;
    bool final;
    char text[AI_CHAT_CAPTION_TEXT_MAX];
} ai_chat_caption_group_t;

typedef struct {
    bool initialized;
    bool observer_registered;
    uint32_t generation;
    ai_chat_config_t config;
    ai_chat_state_t state;
    tirtc_conn_t conn;
    bool listening;
    bool cloud_speaking;
    char role_id[AI_CHAT_ROLE_ID_MAX];
    char session_id[AI_CHAT_SESSION_ID_MAX];
    char status[AI_CHAT_STATUS_TEXT_MAX];
    ai_chat_caption_group_t captions[2];
    uint8_t message_count;
    ai_chat_message_t *messages;
    bool device_action_pending;
    uint8_t start_retries;
    uint32_t rx_commands;
    int last_error;
} ai_chat_state_data_t;

typedef struct {
    uint32_t generation;
} ai_chat_start_context_t;

typedef struct {
    uint32_t generation;
} ai_chat_start_retry_context_t;

static void *ai_chat_calloc_psram(size_t count, size_t size)
{
    return app_memory_calloc_psram(count, size);
}

typedef struct {
    uint32_t generation;
} ai_chat_whip_context_t;

typedef struct {
    uint32_t generation;
} ai_chat_connect_timeout_context_t;

typedef struct {
    uint32_t generation;
} ai_chat_start_session_timeout_context_t;

typedef struct {
    uint32_t generation;
    tirtc_conn_t conn;
} ai_chat_session_context_t;

typedef struct {
    uint32_t generation;
    tirtc_conn_t conn;
} ai_chat_video_start_context_t;

typedef struct {
    uint32_t generation;
} ai_chat_heartbeat_context_t;

typedef struct {
    uint32_t generation;
    tirtc_conn_t conn;
    char jsonrpc_id_json[AI_CHAT_JSONRPC_ID_MAX];
    ai_chat_device_action_t action;
} ai_chat_device_action_context_t;

static SemaphoreHandle_t s_lock;
static ai_chat_state_data_t s_ai = {
    .state = AI_CHAT_STATE_IDLE,
    .status = "idle",
};
static ai_chat_media_state_t s_media;
static portMUX_TYPE s_media_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_start_task;
static TaskHandle_t s_start_retry_task;
static TaskHandle_t s_session_task;
static TaskHandle_t s_video_start_task;
static TaskHandle_t s_heartbeat_task;
static uint32_t s_heartbeat_generation;

static void ai_chat_notify_media_active(bool active)
{
    ai_chat_media_active_cb_t callback = NULL;
    void *callback_ctx = NULL;

    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        callback = s_ai.config.media_active_cb;
        callback_ctx = s_ai.config.media_active_ctx;
        xSemaphoreGive(s_lock);
    }

    if (callback != NULL) {
        callback(active, callback_ctx);
    }
}

static void ai_chat_start_task(void *ctx);
static void ai_chat_start_retry_task(void *ctx);
static esp_err_t ai_chat_schedule_start_retry(uint32_t generation,
                                              const char *reason);
static void ai_chat_session_task(void *ctx);
static void ai_chat_connect_timeout_task(void *ctx);
static void ai_chat_start_session_timeout_task(void *ctx);
static void ai_chat_heartbeat_task(void *ctx);
static void ai_chat_media_task(void *ctx);
static void ai_chat_video_start_task(void *ctx);
static void ai_chat_media_capture_cb(const uint8_t *data,
                                     size_t data_len,
                                     const audio_format_t *format,
                                     void *ctx);

static void ai_chat_log_heap(const char *stage)
{
    ESP_LOGI(TAG,
             "AI Chat heap %s: internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u",
             stage != NULL ? stage : "unknown",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static void ai_chat_trim_utf8_tail(char *text)
{
    if (text == NULL || text[0] == '\0') {
        return;
    }

    size_t len = strlen(text);
    while (len > 0U) {
        char tail = text[len - 1U];
        if (tail != '\r' && tail != '\n' && tail != '\t' && tail != ' ') {
            break;
        }
        text[--len] = '\0';
    }
    if (len == 0U) {
        return;
    }

    size_t lead_pos = len - 1U;
    while (lead_pos > 0U && (((uint8_t)text[lead_pos] & 0xC0U) == 0x80U)) {
        lead_pos--;
    }

    uint8_t lead = (uint8_t)text[lead_pos];
    size_t seq_len = len - lead_pos;
    size_t expected_len = 0;

    if ((lead & 0x80U) == 0U) {
        expected_len = 1U;
    } else if ((lead & 0xE0U) == 0xC0U) {
        expected_len = 2U;
    } else if ((lead & 0xF0U) == 0xE0U) {
        expected_len = 3U;
    } else if ((lead & 0xF8U) == 0xF0U) {
        expected_len = 4U;
    }

    if (expected_len == 0U || seq_len < expected_len) {
        text[lead_pos] = '\0';
    }
}

const char *ai_chat_state_name(ai_chat_state_t state)
{
    switch (state) {
    case AI_CHAT_STATE_IDLE:
        return "idle";
    case AI_CHAT_STATE_STARTING:
        return "starting";
    case AI_CHAT_STATE_TOKEN:
        return "token";
    case AI_CHAT_STATE_CONNECTING:
        return "connecting";
    case AI_CHAT_STATE_CONNECTED:
        return "connected";
    case AI_CHAT_STATE_STARTING_SESSION:
        return "starting_session";
    case AI_CHAT_STATE_IN_SESSION:
        return "in_session";
    case AI_CHAT_STATE_STOPPING:
        return "stopping";
    case AI_CHAT_STATE_ERROR:
        return "error";
    default:
        return "unknown";
    }
}

static void ai_chat_copy_str(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0U) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    strlcpy(dst, src, dst_size);
    ai_chat_trim_utf8_tail(dst);
}

static esp_err_t ai_chat_ensure_message_history(void)
{
    if (s_ai.messages != NULL) {
        return ESP_OK;
    }

    ai_chat_message_t *messages =
        (ai_chat_message_t *)ai_chat_calloc_psram(AI_CHAT_MESSAGE_HISTORY_MAX, sizeof(*messages));
    if (messages == NULL) {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_ai.messages == NULL) {
        s_ai.messages = messages;
        messages = NULL;
    }
    xSemaphoreGive(s_lock);

    free(messages);
    return ESP_OK;
}

static void ai_chat_clear_messages_locked(void)
{
    if (s_ai.messages != NULL) {
        memset(s_ai.messages,
               0,
               sizeof(s_ai.messages[0]) * AI_CHAT_MESSAGE_HISTORY_MAX);
    }
    s_ai.message_count = 0;
}

static int ai_chat_find_message_locked(int caption_type,
                                       int64_t utterance_id,
                                       const char *text)
{
    if (s_ai.messages == NULL) {
        return -1;
    }

    for (int index = (int)s_ai.message_count - 1; index >= 0; --index) {
        if ((int)s_ai.messages[index].caption_type == caption_type &&
            s_ai.messages[index].utterance_id == utterance_id) {
            if (!s_ai.messages[index].final) {
                return index;
            }
            if (text != NULL && strcmp(s_ai.messages[index].text, text) == 0) {
                return index;
            }
        }
    }
    return -1;
}

static ai_chat_message_t *ai_chat_append_message_locked(int caption_type, int64_t utterance_id)
{
    if (s_ai.messages == NULL) {
        return NULL;
    }

    if (s_ai.message_count >= AI_CHAT_MESSAGE_HISTORY_MAX) {
        memmove(&s_ai.messages[0],
                &s_ai.messages[1],
                sizeof(s_ai.messages[0]) * (AI_CHAT_MESSAGE_HISTORY_MAX - 1U));
        s_ai.message_count = AI_CHAT_MESSAGE_HISTORY_MAX - 1U;
    }

    ai_chat_message_t *message = &s_ai.messages[s_ai.message_count++];
    memset(message, 0, sizeof(*message));
    message->caption_type = (uint8_t)caption_type;
    message->utterance_id = utterance_id;
    return message;
}

static void ai_chat_update_message_locked(int caption_type,
                                          int64_t utterance_id,
                                          const char *text,
                                          bool final)
{
    int index = ai_chat_find_message_locked(caption_type, utterance_id, text);
    ai_chat_message_t *message = index >= 0 ?
        &s_ai.messages[index] :
        ai_chat_append_message_locked(caption_type, utterance_id);

    if (message == NULL) {
        return;
    }
    message->final = final;
    ai_chat_copy_str(message->text, sizeof(message->text), text);
}

static bool ai_chat_text_has_prefix(const char *text, const char *prefix)
{
    size_t prefix_len = 0;

    if (text == NULL || prefix == NULL) {
        return false;
    }

    prefix_len = strlen(prefix);
    return prefix_len == 0U || strncmp(text, prefix, prefix_len) == 0;
}

static bool ai_chat_utf8_is_boundary(const char *text, size_t offset)
{
    if (text == NULL) {
        return false;
    }
    if (offset == 0U || text[offset] == '\0') {
        return true;
    }
    return (((uint8_t)text[offset] & 0xC0U) != 0x80U);
}

static size_t ai_chat_text_tail_head_overlap(const char *existing, const char *incoming)
{
    size_t existing_len = 0;
    size_t incoming_len = 0;

    if (existing == NULL || incoming == NULL) {
        return 0U;
    }

    existing_len = strlen(existing);
    incoming_len = strlen(incoming);
    size_t max_len = existing_len < incoming_len ? existing_len : incoming_len;

    for (size_t overlap = max_len; overlap > 0U; --overlap) {
        size_t existing_offset = existing_len - overlap;
        if (!ai_chat_utf8_is_boundary(existing, existing_offset) ||
            !ai_chat_utf8_is_boundary(incoming, overlap)) {
            continue;
        }
        if (memcmp(existing + existing_offset, incoming, overlap) == 0) {
            return overlap;
        }
    }
    return 0U;
}

static void ai_chat_apply_caption_text(ai_chat_caption_group_t *group,
                                       const ai_chat_event_t *event)
{
    if (group == NULL || event == NULL) {
        return;
    }

    size_t used = strlen(group->text);
    size_t incoming_len = strlen(event->text);

    /* ASR mode=1 carries the latest full hypothesis; TTS mode=1 may be chunks. */
    if (event->mode != 1 || event->caption_type == 0) {
        ai_chat_copy_str(group->text, sizeof(group->text), event->text);
        return;
    }
    if (used == 0U) {
        ai_chat_copy_str(group->text, sizeof(group->text), event->text);
        return;
    }
    if (strcmp(group->text, event->text) == 0) {
        return;
    }
    if (incoming_len > used && ai_chat_text_has_prefix(event->text, group->text)) {
        ai_chat_copy_str(group->text, sizeof(group->text), event->text);
        return;
    }
    if (incoming_len < used && ai_chat_text_has_prefix(group->text, event->text)) {
        return;
    }

    size_t overlap = ai_chat_text_tail_head_overlap(group->text, event->text);
    const char *append_text = event->text + overlap;
    if (append_text[0] != '\0' && used < sizeof(group->text) - 1U) {
        strlcpy(group->text + used, append_text, sizeof(group->text) - used);
        ai_chat_trim_utf8_tail(group->text);
    }
}

static bool ai_chat_is_blank(const char *value)
{
    return value == NULL || value[0] == '\0';
}

static bool ai_chat_is_signaling_cmd(uint32_t cmdw)
{
    uint32_t low = cmdw & 0xFFFFU;

    return GET_CMD(cmdw) == AI_CHAT_SIGNALING_CMD ||
           GET_CMD(cmdw & ~RESPONSE_BIT) == AI_CHAT_SIGNALING_CMD ||
           cmdw == AI_CHAT_SIGNALING_CMD ||
           (cmdw & ~RESPONSE_BIT) == AI_CHAT_SIGNALING_CMD ||
           low == AI_CHAT_SIGNALING_CMD ||
           (low & ~RESPONSE_BIT) == AI_CHAT_SIGNALING_CMD;
}

static void ai_chat_set_state_locked(ai_chat_state_t state, const char *status)
{
    if (s_ai.state != state) {
        ESP_LOGI(TAG, "state %s -> %s", ai_chat_state_name(s_ai.state), ai_chat_state_name(state));
        s_ai.state = state;
    }
    if (status != NULL) {
        ai_chat_copy_str(s_ai.status, sizeof(s_ai.status), status);
    }
}

static uint32_t ai_chat_next_generation_locked(void)
{
    s_ai.generation++;
    if (s_ai.generation == 0U) {
        s_ai.generation = 1U;
    }
    /* A generation change invalidates the current session. Wake the heartbeat
     * immediately instead of retaining its task stack for another 30 seconds. */
    if (s_heartbeat_task != NULL) {
        xTaskNotifyGive(s_heartbeat_task);
    }
    return s_ai.generation;
}

static bool ai_chat_generation_matches_locked(uint32_t generation)
{
    return generation != 0U &&
           generation == s_ai.generation &&
           s_ai.state != AI_CHAT_STATE_IDLE &&
           s_ai.state != AI_CHAT_STATE_STOPPING;
}

static bool ai_chat_generation_matches(uint32_t generation)
{
    bool matches = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    matches = ai_chat_generation_matches_locked(generation);
    xSemaphoreGive(s_lock);
    return matches;
}

static bool ai_chat_video_session_is_current(tirtc_conn_t conn, void *ctx)
{
    const uint32_t *generation = (const uint32_t *)ctx;
    bool matches = false;

    if (conn == NULL || generation == NULL) {
        return false;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    matches = ai_chat_generation_matches_locked(*generation) &&
              s_ai.state == AI_CHAT_STATE_IN_SESSION &&
              s_ai.conn == conn;
    xSemaphoreGive(s_lock);
    return matches;
}

static esp_err_t ai_chat_validate_config(const ai_chat_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!config->enabled) {
        return ESP_OK;
    }
    if (ai_chat_is_blank(config->device_id) ||
        ai_chat_is_blank(config->user_id) ||
        ai_chat_is_blank(config->device_key) ||
        ai_chat_is_blank(config->token_api_base)) {
        ESP_LOGW(TAG,
                 "AI Chat config incomplete: device_id=%d user_id=%d device_key=%d api_base=%d role_id_optional=%d",
                 !ai_chat_is_blank(config->device_id),
                 !ai_chat_is_blank(config->user_id),
                 !ai_chat_is_blank(config->device_key),
                 !ai_chat_is_blank(config->token_api_base),
                 !ai_chat_is_blank(config->role_id));
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static esp_err_t ai_chat_send_json(tirtc_conn_t conn, const char *json)
{
    if (conn == NULL || json == NULL || json[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    return tirtc_session_send_command_raw(conn, AI_CHAT_SIGNALING_CMD, json, strlen(json));
}

static esp_err_t ai_chat_disconnect_conn(tirtc_conn_t conn)
{
    if (conn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return tirtc_session_disconnect_connection(conn) == 0 ? ESP_OK : ESP_FAIL;
}

static char *ai_chat_build_notification_json(const char *method)
{
    cJSON *root = cJSON_CreateObject();

    if (root == NULL || method == NULL) {
        cJSON_Delete(root);
        return NULL;
    }

    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddStringToObject(root, "method", method);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static void ai_chat_device_action_set_result(
    ai_chat_device_action_result_t *result,
    bool ok,
    const char *status,
    const char *message)
{
    if (result == NULL) {
        return;
    }
    result->ok = ok;
    ai_chat_copy_str(result->status, sizeof(result->status), status);
    ai_chat_copy_str(result->message, sizeof(result->message), message);
}

static const char *ai_chat_device_action_route_name(
    ai_chat_device_action_route_t route)
{
    switch (route) {
    case AI_CHAT_DEVICE_ACTION_ROUTE_DEVICE_CALL:
        return "device_call";
    case AI_CHAT_DEVICE_ACTION_ROUTE_WECHAT_VOIP:
        return "wechat_voip";
    case AI_CHAT_DEVICE_ACTION_ROUTE_NONE:
    default:
        return "none";
    }
}

static int ai_chat_device_action_error_code(
    const ai_chat_device_action_result_t *result)
{
    const char *status = result != NULL ? result->status : NULL;

    if (status == NULL) {
        return AI_CHAT_DEVICE_ACTION_ERROR_INTERNAL;
    }
    if (strcmp(status, "unsupported") == 0 ||
        strcmp(status, "unsupported_call_type") == 0 ||
        strcmp(status, "unsupported_contact_type") == 0 ||
        strcmp(status, "wechat_status_unsupported") == 0) {
        return AI_CHAT_DEVICE_ACTION_ERROR_UNSUPPORTED;
    }
    if (strcmp(status, "busy") == 0) {
        return AI_CHAT_DEVICE_ACTION_ERROR_BUSY;
    }
    if (strcmp(status, "not_found") == 0 ||
        strcmp(status, "contacts_empty") == 0 ||
        strcmp(status, "ambiguous") == 0 ||
        strcmp(status, "offline") == 0 ||
        strcmp(status, "network_offline") == 0) {
        return AI_CHAT_DEVICE_ACTION_ERROR_TARGET;
    }
    if (strcmp(status, "missing_target") == 0 ||
        strcmp(status, "unsupported_status_filter") == 0 ||
        strcmp(status, "invalid_request") == 0) {
        return AI_CHAT_DEVICE_ACTION_ERROR_INVALID;
    }
    if (strcmp(status, "contacts_loading") == 0 ||
        strcmp(status, "contacts_unavailable") == 0 ||
        strcmp(status, "service_loading") == 0) {
        return AI_CHAT_DEVICE_ACTION_ERROR_LOADING;
    }
    return AI_CHAT_DEVICE_ACTION_ERROR_INTERNAL;
}

static char *ai_chat_build_device_action_result_json(
    const char *id_json,
    const ai_chat_device_action_result_t *result)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *id = NULL;

    if (root == NULL || id_json == NULL || result == NULL) {
        cJSON_Delete(root);
        return NULL;
    }

    id = cJSON_Parse(id_json);
    if (id == NULL) {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddItemToObject(root, "id", id);

    if (result->ok) {
        cJSON *payload = cJSON_CreateObject();
        if (payload == NULL) {
            cJSON_Delete(root);
            return NULL;
        }
        cJSON_AddBoolToObject(payload, "ok", true);
        cJSON_AddStringToObject(payload,
                                "status",
                                result->status[0] != '\0' ?
                                    result->status : "ok");
        cJSON_AddStringToObject(payload,
                                "message",
                                result->message[0] != '\0' ?
                                    result->message : "已开始处理");
        if (result->call_route != AI_CHAT_DEVICE_ACTION_ROUTE_NONE) {
            cJSON_AddStringToObject(
                payload,
                "contact_type",
                ai_chat_device_action_route_name(result->call_route));
        }
        if (result->call_route == AI_CHAT_DEVICE_ACTION_ROUTE_DEVICE_CALL &&
            result->target_id[0] != '\0') {
            cJSON_AddStringToObject(payload,
                                    "target_device_id",
                                    result->target_id);
        }
        if (result->matched_name[0] != '\0') {
            cJSON_AddStringToObject(payload,
                                    "matched_name",
                                    result->matched_name);
        }
        if (result->has_contacts_result) {
            cJSON *contacts = cJSON_AddArrayToObject(payload, "contacts");
            if (contacts == NULL) {
                cJSON_Delete(root);
                return NULL;
            }
            cJSON_AddNumberToObject(payload, "count", result->contact_count);
            for (uint8_t index = 0U;
                 index < result->contact_count;
                 ++index) {
                const ai_chat_device_action_contact_t *source =
                    &result->contacts[index];
                cJSON *contact = cJSON_CreateObject();
                if (contact == NULL) {
                    cJSON_Delete(root);
                    return NULL;
                }
                cJSON_AddStringToObject(contact, "name", source->name);
                cJSON_AddStringToObject(contact,
                                        "device_id",
                                        source->device_id);
                cJSON_AddBoolToObject(contact, "online", source->online);
                cJSON_AddItemToArray(contacts, contact);
            }
        }
        cJSON_AddItemToObject(root, "result", payload);
    } else {
        cJSON *error = cJSON_CreateObject();
        cJSON *data = cJSON_CreateObject();
        if (error == NULL || data == NULL) {
            cJSON_Delete(root);
            cJSON_Delete(error);
            cJSON_Delete(data);
            return NULL;
        }
        cJSON_AddNumberToObject(error,
                               "code",
                               ai_chat_device_action_error_code(result));
        cJSON_AddStringToObject(
            error,
            "message",
            result->message[0] != '\0' ?
                result->message : "设备动作执行失败");
        cJSON_AddStringToObject(
            data,
            "status",
            result->status[0] != '\0' ? result->status : "failed");
        cJSON_AddBoolToObject(data, "ok", false);
        cJSON_AddItemToObject(error, "data", data);
        cJSON_AddItemToObject(root, "error", error);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static char *ai_chat_build_start_session_json(const char *device_id, const char *user_id, const char *role_id)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *params = cJSON_CreateObject();
    cJSON *input_audio = NULL;
    cJSON *output_audio = NULL;

    if (root == NULL || params == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(params);
        return NULL;
    }

    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddStringToObject(root, "id", AI_CHAT_START_SESSION_RPC_ID);
    cJSON_AddStringToObject(root, "method", "start_session");

    cJSON_AddStringToObject(params, "device_id", device_id);
    cJSON_AddStringToObject(params, "user_id", user_id);
    cJSON_AddStringToObject(params, "role_id", role_id);

#if AI_CHAT_START_SESSION_INCLUDE_AUDIO
    input_audio = cJSON_CreateObject();
    output_audio = cJSON_CreateObject();
    if (input_audio == NULL || output_audio == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(input_audio);
        cJSON_Delete(output_audio);
        return NULL;
    }

    cJSON_AddStringToObject(input_audio, "codec", AI_CHAT_AUDIO_CODEC_NAME);
    cJSON_AddNumberToObject(input_audio, "sample_rate", AI_CHAT_AUDIO_SAMPLE_RATE);
    cJSON_AddNumberToObject(input_audio, "channels", AI_CHAT_AUDIO_CHANNELS);

    cJSON_AddStringToObject(output_audio, "codec", AI_CHAT_AUDIO_CODEC_NAME);
    cJSON_AddNumberToObject(output_audio, "sample_rate", AI_CHAT_AUDIO_SAMPLE_RATE);
    cJSON_AddNumberToObject(output_audio, "channels", AI_CHAT_AUDIO_CHANNELS);

    cJSON_AddItemToObject(params, "input_audio", input_audio);
    cJSON_AddItemToObject(params, "output_audio", output_audio);
#endif
    cJSON_AddItemToObject(root, "params", params);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static bool ai_chat_audio_spec_supported(const ai_chat_audio_spec_t *spec)
{
    if (spec == NULL || !spec->valid) {
        return true;
    }
    return strcmp(spec->codec, AI_CHAT_AUDIO_CODEC_NAME) == 0 &&
           spec->sample_rate == AI_CHAT_AUDIO_SAMPLE_RATE &&
           spec->channels == AI_CHAT_AUDIO_CHANNELS;
}

static esp_err_t ai_chat_send_start_session(tirtc_conn_t conn)
{
    ai_chat_config_t config = {0};
    char role_id[AI_CHAT_ROLE_ID_MAX] = {0};

    xSemaphoreTake(s_lock, portMAX_DELAY);
    config = s_ai.config;
    ai_chat_copy_str(role_id, sizeof(role_id), s_ai.role_id);
    ai_chat_set_state_locked(AI_CHAT_STATE_STARTING_SESSION, "starting session");
    xSemaphoreGive(s_lock);

    if (role_id[0] == '\0') {
        ai_chat_copy_str(role_id, sizeof(role_id), config.role_id);
    }
    if (role_id[0] == '\0') {
        ESP_LOGW(TAG, "AI Chat start_session missing role_id");
        return ESP_ERR_INVALID_STATE;
    }

    char *json = ai_chat_build_start_session_json(config.device_id, config.user_id, role_id);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "send start_session: device_id_len=%u user_id_len=%u role_id_len=%u audio=%s/%uHz/%uch",
             (unsigned)strlen(config.device_id),
             (unsigned)strlen(config.user_id),
             (unsigned)strlen(role_id),
             AI_CHAT_AUDIO_CODEC_NAME,
             AI_CHAT_AUDIO_SAMPLE_RATE,
             AI_CHAT_AUDIO_CHANNELS);
    ESP_LOGD(TAG, "AI Chat start_session payload_len=%u", (unsigned)strlen(json));
    esp_err_t ret = ai_chat_send_json(conn, json);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG,
                 "AI Chat command sent: cmd=0x%04x method=start_session len=%u",
                 AI_CHAT_SIGNALING_CMD,
                 (unsigned)strlen(json));
    }
    free(json);
    return ret;
}

static bool ai_chat_media_can_accept_locked(void)
{
    return s_media.running &&
           s_media.uplink_enabled &&
           s_media.conn != NULL &&
           s_media.queue != NULL;
}

static bool ai_chat_media_make_pcm16_16k(const uint8_t *data,
                                         size_t data_len,
                                         const audio_format_t *format,
                                         uint8_t out[AI_CHAT_AUDIO_FRAME_BYTES])
{
    const int16_t *samples = (const int16_t *)data;
    int16_t *dst = (int16_t *)out;
    size_t sample_count = data_len / sizeof(int16_t);

    memset(out, 0, AI_CHAT_AUDIO_FRAME_BYTES);

    if (data == NULL || format == NULL ||
        format->bits_per_sample != 16 ||
        format->channels != 1) {
        return false;
    }

    if (format->sample_rate_hz == AI_CHAT_AUDIO_SAMPLE_RATE) {
        size_t copy_bytes = data_len > AI_CHAT_AUDIO_FRAME_BYTES ? AI_CHAT_AUDIO_FRAME_BYTES : data_len;
        memcpy(out, data, copy_bytes);
        return true;
    }

    if (format->sample_rate_hz == 8000U) {
        size_t out_index = 0;

        for (size_t index = 0; index < sample_count && out_index + 1U < AI_CHAT_AUDIO_FRAME_SAMPLES; ++index) {
            dst[out_index++] = samples[index];
            dst[out_index++] = samples[index];
        }
        return true;
    }

    return false;
}

static uint32_t ai_chat_pcm_peak_percent(const uint8_t *data, size_t data_len)
{
    const int16_t *samples = (const int16_t *)data;
    size_t sample_count = data_len / sizeof(int16_t);
    uint32_t peak = 0;

    if (data == NULL || data_len == 0U) {
        return 0;
    }

    for (size_t index = 0; index < sample_count; ++index) {
        int16_t sample = samples[index];
        uint32_t value = sample == INT16_MIN ? 32768U : (uint32_t)abs(sample);
        if (value > peak) {
            peak = value;
        }
    }

    uint32_t percent = (peak * 100U) / 32767U;
    return percent > 100U ? 100U : percent;
}

static void ai_chat_media_task(void *ctx)
{
    (void)ctx;
    ai_chat_media_packet_t packet = {0};

    while (xQueueReceive(s_media.queue, &packet, portMAX_DELAY) == pdTRUE) {
        bool running = false;
        tirtc_conn_t conn = NULL;

        taskENTER_CRITICAL(&s_media_lock);
        running = s_media.running;
        conn = s_media.conn;
        taskEXIT_CRITICAL(&s_media_lock);

        if (!running || conn == NULL || conn != packet.conn) {
            continue;
        }

        esp_err_t ret = tirtc_session_send_audio_frame(packet.conn, &packet.frame, packet.data);
        uint32_t peak_percent = ai_chat_pcm_peak_percent(packet.data, packet.frame.length);
        bool log_tx_started = false;
        bool log_tx_window = false;
        uint32_t window_frames = 0;
        uint32_t window_payload = 0;
        uint32_t window_peak = 0;
        uint32_t total_failures = 0;
        uint32_t total_dropped = 0;
        const TickType_t now_tick = xTaskGetTickCount();
        taskENTER_CRITICAL(&s_media_lock);
        if (ret == ESP_OK) {
            s_media.tx_frames++;
            if (!s_media.tx_started_logged) {
                s_media.tx_started_logged = true;
                s_media.last_tx_window_log_tick = now_tick;
                log_tx_started = true;
            }
            s_media.tx_window_frames++;
            s_media.tx_window_payload_bytes += packet.frame.length;
            if (peak_percent > s_media.tx_window_peak_percent) {
                s_media.tx_window_peak_percent = peak_percent;
            }
            if (!log_tx_started &&
                (s_media.last_tx_window_log_tick == 0 ||
                 now_tick - s_media.last_tx_window_log_tick >= pdMS_TO_TICKS(AI_CHAT_MEDIA_TX_LOG_INTERVAL_MS))) {
                log_tx_window = true;
                window_frames = s_media.tx_window_frames;
                window_payload = s_media.tx_window_payload_bytes;
                window_peak = s_media.tx_window_peak_percent;
                total_failures = s_media.tx_failures;
                total_dropped = s_media.dropped_frames;
                s_media.tx_window_frames = 0;
                s_media.tx_window_payload_bytes = 0;
                s_media.tx_window_peak_percent = 0;
                s_media.last_tx_window_log_tick = now_tick;
            }
        } else {
            s_media.tx_failures++;
        }
        taskEXIT_CRITICAL(&s_media_lock);

        if (log_tx_started) {
            ESP_LOGI(TAG,
                     "AI Chat uplink audio started: stream=%u media=pcm flags=0x%08lx frame_bytes=%u peak=%lu",
                     (unsigned)packet.frame.stream_id,
                     (unsigned long)packet.frame.flags,
                     (unsigned)packet.frame.length,
                     (unsigned long)peak_percent);
        } else if (log_tx_window) {
            ESP_LOGI(TAG,
                     "AI Chat uplink tx: frames=%lu payload=%lu peak=%lu failures=%lu dropped=%lu",
                     (unsigned long)window_frames,
                     (unsigned long)window_payload,
                     (unsigned long)window_peak,
                     (unsigned long)total_failures,
                     (unsigned long)total_dropped);
        }
        if (ret == ESP_ERR_TIMEOUT) {
            bool should_log = false;

            taskENTER_CRITICAL(&s_media_lock);
            if (s_media.last_backpressure_log_tick == 0 ||
                now_tick - s_media.last_backpressure_log_tick >= pdMS_TO_TICKS(1000)) {
                s_media.last_backpressure_log_tick = now_tick;
                should_log = true;
            }
            taskEXIT_CRITICAL(&s_media_lock);

            if (should_log) {
                ESP_LOGW(TAG,
                         "AI Chat uplink backpressure: TiRTC send buffer is not draining, pause this packet");
            }
        } else if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "send AI audio failed: %s", esp_err_to_name(ret));
        }
    }
}

static esp_err_t ai_chat_media_init(void)
{
    if (s_media.initialized) {
        return ESP_OK;
    }

    s_media.queue = xQueueCreateWithCaps(AI_CHAT_MEDIA_QUEUE_LEN,
                                         sizeof(ai_chat_media_packet_t),
                                         AI_CHAT_MEDIA_ALLOC_CAPS);
    if (s_media.queue == NULL) {
        ai_chat_log_heap("media queue alloc failed");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = microphone_register_observer(ai_chat_media_capture_cb, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "register AI microphone observer failed: %s", esp_err_to_name(ret));
        vQueueDeleteWithCaps(s_media.queue);
        memset(&s_media, 0, sizeof(s_media));
        return ret;
    }

    BaseType_t task_ret = xTaskCreateWithCaps(ai_chat_media_task,
                                              "ai_audio_tx",
                                              AI_CHAT_MEDIA_TASK_STACK,
                                              NULL,
                                              AI_CHAT_MEDIA_TASK_PRIORITY,
                                              &s_media.task,
                                              AI_CHAT_MEDIA_ALLOC_CAPS);
    if (task_ret != pdPASS) {
        ai_chat_log_heap("media task alloc failed");
        microphone_unregister_observer(ai_chat_media_capture_cb, NULL);
        vQueueDeleteWithCaps(s_media.queue);
        memset(&s_media, 0, sizeof(s_media));
        return ESP_ERR_NO_MEM;
    }

    s_media.initialized = true;
    ai_chat_log_heap("media initialized");
    return ESP_OK;
}

static esp_err_t ai_chat_media_start(tirtc_conn_t conn)
{
    if (conn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_media_lock);
    bool already_running = s_media.running &&
                           s_media.audio_prepared &&
                           s_media.conn == conn;
    taskEXIT_CRITICAL(&s_media_lock);
    if (already_running) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(ai_chat_media_init(), TAG, "init AI media failed");
    ESP_RETURN_ON_ERROR(microphone_prepare_capture_path(), TAG, "prepare AI microphone path failed");

    esp_err_t speaker_ret = speaker_prepare_playback_path();
    if (speaker_ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "AI speaker path is not ready, keep microphone uplink running: %s",
                 esp_err_to_name(speaker_ret));
    }

    taskENTER_CRITICAL(&s_media_lock);
    s_media.audio_prepared = true;
    s_media.conn = conn;
    s_media.running = true;
    s_media.uplink_enabled = false;
    s_media.next_ts_ms = 0;
    s_media.tx_frames = 0;
    s_media.tx_failures = 0;
    s_media.dropped_frames = 0;
    s_media.last_backpressure_log_tick = 0;
    s_media.last_format_drop_log_tick = 0;
    s_media.last_tx_window_log_tick = 0;
    s_media.tx_window_frames = 0;
    s_media.tx_window_payload_bytes = 0;
    s_media.tx_window_peak_percent = 0;
    s_media.tx_started_logged = false;
    taskEXIT_CRITICAL(&s_media_lock);

    xQueueReset(s_media.queue);
    esp_err_t ret = microphone_set_observer_enabled(ai_chat_media_capture_cb, NULL, false);
    if (ret != ESP_OK) {
        taskENTER_CRITICAL(&s_media_lock);
        s_media.conn = NULL;
        s_media.running = false;
        s_media.uplink_enabled = false;
        s_media.audio_prepared = false;
        taskEXIT_CRITICAL(&s_media_lock);
        audio_device_release();
        return ret;
    }

    ai_chat_notify_media_active(true);
    ESP_LOGI(TAG,
             "AI Chat real audio path ready: capture=pcm/%uHz/%uch uplink=app-controlled",
             (unsigned)AI_CHAT_AUDIO_SAMPLE_RATE,
             (unsigned)AI_CHAT_AUDIO_CHANNELS);
    return ESP_OK;
}

static void ai_chat_media_stop(tirtc_conn_t conn)
{
    bool should_stop = false;
    bool release_audio = false;

    taskENTER_CRITICAL(&s_media_lock);
    should_stop = conn == NULL || conn == s_media.conn;
    if (should_stop) {
        s_media.conn = NULL;
        s_media.running = false;
        s_media.uplink_enabled = false;
        s_media.tx_started_logged = false;
        release_audio = s_media.audio_prepared;
        s_media.audio_prepared = false;
    }
    taskEXIT_CRITICAL(&s_media_lock);

    if (should_stop) {
        (void)microphone_set_observer_enabled(ai_chat_media_capture_cb, NULL, false);
        if (s_media.queue != NULL) {
            xQueueReset(s_media.queue);
        }
        ai_chat_notify_media_active(false);
        if (release_audio) {
            audio_device_release();
        }
    }
}

static esp_err_t ai_chat_media_set_uplink_enabled(bool enabled)
{
    ESP_RETURN_ON_ERROR(ai_chat_media_init(), TAG, "init AI media failed");

    taskENTER_CRITICAL(&s_media_lock);
    if (!s_media.running || s_media.conn == NULL) {
        taskEXIT_CRITICAL(&s_media_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_media.uplink_enabled = enabled;
    taskEXIT_CRITICAL(&s_media_lock);

    esp_err_t ret = microphone_set_observer_enabled(ai_chat_media_capture_cb, NULL, enabled);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "set AI capture observer failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "AI Chat microphone uplink %s", enabled ? "enabled" : "disabled");
    }
    return ret;
}

static void ai_chat_media_get_stats(uint32_t *tx_frames, uint32_t *tx_failures)
{
    if (tx_frames == NULL || tx_failures == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_media_lock);
    *tx_frames = s_media.tx_frames;
    *tx_failures = s_media.tx_failures;
    taskEXIT_CRITICAL(&s_media_lock);
}

static void ai_chat_video_start_task(void *ctx)
{
    ai_chat_video_start_context_t *video_ctx =
        (ai_chat_video_start_context_t *)ctx;
    uint32_t generation = video_ctx != NULL ? video_ctx->generation : 0U;
    tirtc_conn_t conn = video_ctx != NULL ? video_ctx->conn : NULL;
    TaskHandle_t self = xTaskGetCurrentTaskHandle();

    free(video_ctx);
    if (generation != 0U && conn != NULL &&
        ai_chat_generation_matches(generation)) {
        esp_err_t ret = ai_chat_video_start(
            conn,
            ai_chat_video_session_is_current,
            &generation);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG,
                     "AI Chat video unavailable; audio session continues: %s",
                     esp_err_to_name(ret));
        }
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_video_start_task == self) {
        s_video_start_task = NULL;
    }
    xSemaphoreGive(s_lock);
    vTaskDeleteWithCaps(NULL);
}

static esp_err_t ai_chat_schedule_video_start(tirtc_conn_t conn,
                                              uint32_t generation)
{
    bool video_enabled = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    video_enabled = s_ai.config.video_enabled;
    xSemaphoreGive(s_lock);
    if (!video_enabled) {
        return ESP_OK;
    }
    if (conn == NULL || generation == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    ai_chat_video_start_context_t *video_ctx =
        ai_chat_calloc_psram(1, sizeof(*video_ctx));
    if (video_ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }
    video_ctx->generation = generation;
    video_ctx->conn = conn;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_video_start_task != NULL) {
        xSemaphoreGive(s_lock);
        free(video_ctx);
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t task_ret = xTaskCreateWithCaps(ai_chat_video_start_task,
                                              "ai_video_start",
                                              AI_CHAT_VIDEO_START_TASK_STACK,
                                              video_ctx,
                                              AI_CHAT_VIDEO_START_TASK_PRIORITY,
                                              &s_video_start_task,
                                              AI_CHAT_TASK_ALLOC_CAPS);
    if (task_ret != pdPASS) {
        free(video_ctx);
        s_video_start_task = NULL;
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

static void ai_chat_media_capture_cb(const uint8_t *data,
                                     size_t data_len,
                                     const audio_format_t *format,
                                     void *ctx)
{
    (void)ctx;
    ai_chat_media_packet_t packet = {0};
    bool accept = false;

    if (data == NULL || data_len == 0U || format == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_media_lock);
    accept = ai_chat_media_can_accept_locked();
    if (accept) {
        packet.conn = s_media.conn;
        packet.frame.stream_id = AI_CHAT_AUDIO_STREAM_ID;
        packet.frame.media = TIRTC_AUDIO_PCM;
        packet.frame.flags = TIRTC_AUDIOSAMPLE_16K16B1C;
        packet.frame.reserved = 0;
        packet.frame.ts = s_media.next_ts_ms;
        packet.frame.length = AI_CHAT_AUDIO_FRAME_BYTES;
        s_media.next_ts_ms += AI_CHAT_AUDIO_FRAME_MS;
    }
    taskEXIT_CRITICAL(&s_media_lock);

    if (!accept) {
        return;
    }

    if (!ai_chat_media_make_pcm16_16k(data, data_len, format, packet.data)) {
        TickType_t now_tick = xTaskGetTickCount();
        bool should_log = false;
        taskENTER_CRITICAL(&s_media_lock);
        s_media.dropped_frames++;
        if (s_media.last_format_drop_log_tick == 0 ||
            now_tick - s_media.last_format_drop_log_tick >= pdMS_TO_TICKS(1000)) {
            s_media.last_format_drop_log_tick = now_tick;
            should_log = true;
        }
        taskEXIT_CRITICAL(&s_media_lock);
        if (should_log) {
            ESP_LOGW(TAG,
                     "AI Chat uplink dropped unsupported capture frame: rate=%luHz bits=%u ch=%u len=%lu",
                     (unsigned long)format->sample_rate_hz,
                     (unsigned)format->bits_per_sample,
                     (unsigned)format->channels,
                     (unsigned long)data_len);
        }
        return;
    }
    if (xQueueSend(s_media.queue, &packet, 0) == pdTRUE) {
        return;
    }

    ai_chat_media_packet_t stale = {0};
    (void)xQueueReceive(s_media.queue, &stale, 0);
    if (xQueueSend(s_media.queue, &packet, 0) != pdTRUE) {
        taskENTER_CRITICAL(&s_media_lock);
        s_media.dropped_frames++;
        taskEXIT_CRITICAL(&s_media_lock);
    }
}

static esp_err_t ai_chat_wait_rtc_ready(uint32_t generation)
{
    uint32_t waited_ms = 0;
    uint32_t last_log_ms = 0;
    tirtc_session_stats_t last_stats = {0};

    ESP_LOGI(TAG, "AI Chat waiting for RTC ready before connect");
    while (waited_ms <= AI_CHAT_RTC_READY_WAIT_MS) {
        tirtc_session_stats_t stats = {0};

        if (!ai_chat_generation_matches(generation)) {
            return ESP_ERR_INVALID_STATE;
        }

        tirtc_session_get_stats(&stats);
        last_stats = stats;
        if (tirtc_session_is_ready_for_new_connection()) {
            ESP_LOGI(TAG,
                     "AI Chat RTC ready: waited=%ums state=%d last_event=%s",
                     (unsigned)waited_ms,
                     (int)stats.state,
                     stats.last_event[0] != '\0' ? stats.last_event : "none");
            return ESP_OK;
        }
        esp_err_t prepare_ret = ESP_OK;
        bool rtc_start_needed = !stats.sdk_initialized || !stats.sdk_started;
        if (system_time_has_valid_time() && rtc_start_needed &&
            stats.state != TIRTC_SESSION_STATE_DISCONNECTING) {
            prepare_ret = tirtc_session_prepare_sdk();
            if (prepare_ret != ESP_OK && prepare_ret != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "AI Chat RTC prepare failed before connect: %s", esp_err_to_name(prepare_ret));
                return prepare_ret;
            }
        } else if (!system_time_has_valid_time()) {
            prepare_ret = ESP_ERR_INVALID_STATE;
        }

        if (waited_ms == 0U || waited_ms - last_log_ms >= 1000U) {
            ESP_LOGI(TAG,
                     "AI Chat RTC wait: waited=%ums time_valid=%d sdk_initialized=%d sdk_started=%d state=%d last_event=%s last_error=%d prepare=%s",
                     (unsigned)waited_ms,
                     system_time_has_valid_time() ? 1 : 0,
                     stats.sdk_initialized ? 1 : 0,
                     stats.sdk_started ? 1 : 0,
                     (int)stats.state,
                     stats.last_event[0] != '\0' ? stats.last_event : "none",
                     stats.last_error,
                     esp_err_to_name(prepare_ret));
            last_log_ms = waited_ms;
        }
        vTaskDelay(pdMS_TO_TICKS(AI_CHAT_RTC_READY_POLL_MS));
        waited_ms += AI_CHAT_RTC_READY_POLL_MS;
    }

    ESP_LOGW(TAG,
             "rtc ready timeout: state=%d sdk_started=%d start_event=%s last_error=%d",
             (int)last_stats.state,
             last_stats.sdk_started ? 1 : 0,
             last_stats.last_event[0] != '\0' ? last_stats.last_event : "none",
             last_stats.last_error);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t ai_chat_verify_time_ready(void)
{
    time_t now = 0;

    time(&now);
    if (system_time_has_valid_time()) {
        ESP_LOGI(TAG, "AI Chat system time verified before token: unix=%lld", (long long)now);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "AI Chat system time invalid before token: unix=%lld", (long long)now);
    return ESP_ERR_INVALID_STATE;
}

static void ai_chat_start_heartbeat_once(uint32_t generation)
{
    if (generation == 0U) {
        return;
    }

    ai_chat_heartbeat_context_t *heartbeat_ctx = ai_chat_calloc_psram(1, sizeof(*heartbeat_ctx));
    if (heartbeat_ctx == NULL) {
        ESP_LOGW(TAG, "create heartbeat context failed");
        return;
    }
    heartbeat_ctx->generation = generation;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!ai_chat_generation_matches_locked(generation) ||
        s_ai.state != AI_CHAT_STATE_IN_SESSION ||
        s_heartbeat_task != NULL) {
        xSemaphoreGive(s_lock);
        free(heartbeat_ctx);
        return;
    }

    BaseType_t ret = xTaskCreateWithCaps(ai_chat_heartbeat_task,
                                         "ai_heartbeat",
                                         AI_CHAT_HEARTBEAT_TASK_STACK,
                                         heartbeat_ctx,
                                         AI_CHAT_HEARTBEAT_TASK_PRIORITY,
                                         &s_heartbeat_task,
                                         AI_CHAT_TASK_ALLOC_CAPS);
    if (ret != pdPASS) {
        s_heartbeat_task = NULL;
        xSemaphoreGive(s_lock);
        free(heartbeat_ctx);
        ai_chat_log_heap("heartbeat task create failed");
        ESP_LOGW(TAG, "create heartbeat task failed");
        return;
    }
    s_heartbeat_generation = generation;
    xSemaphoreGive(s_lock);
}

static void ai_chat_heartbeat_task(void *ctx)
{
    ai_chat_heartbeat_context_t *heartbeat_ctx = (ai_chat_heartbeat_context_t *)ctx;
    uint32_t generation = heartbeat_ctx != NULL ? heartbeat_ctx->generation : 0U;
    TaskHandle_t self = xTaskGetCurrentTaskHandle();

    free(heartbeat_ctx);

    while (true) {
        if (ulTaskNotifyTake(pdTRUE,
                             pdMS_TO_TICKS(AI_CHAT_HEARTBEAT_INTERVAL_MS)) > 0U) {
            break;
        }

        tirtc_conn_t conn = NULL;
        ai_chat_state_t state = AI_CHAT_STATE_IDLE;
        bool generation_ok = false;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        conn = s_ai.conn;
        state = s_ai.state;
        generation_ok = ai_chat_generation_matches_locked(generation);
        xSemaphoreGive(s_lock);

        if (!generation_ok || conn == NULL || state != AI_CHAT_STATE_IN_SESSION) {
            break;
        }

        char *json = ai_chat_build_notification_json("heartbeat");
        if (json == NULL) {
            continue;
        }
        (void)ai_chat_send_json(conn, json);
        free(json);
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_heartbeat_task == self && s_heartbeat_generation == generation) {
        s_heartbeat_task = NULL;
        s_heartbeat_generation = 0;
    }
    xSemaphoreGive(s_lock);
    vTaskDeleteWithCaps(NULL);
}

static esp_err_t ai_chat_send_device_action_failure(tirtc_conn_t conn,
                                                    const char *id_json,
                                                    const char *status,
                                                    const char *message)
{
    ai_chat_device_action_result_t result = {0};

    if (conn == NULL || id_json == NULL || id_json[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    ai_chat_device_action_set_result(&result, false, status, message);
    char *json = ai_chat_build_device_action_result_json(id_json, &result);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = ai_chat_send_json(conn, json);
    free(json);
    return ret;
}

static void ai_chat_device_action_task(void *arg)
{
    ai_chat_device_action_context_t *action_ctx =
        (ai_chat_device_action_context_t *)arg;
    ai_chat_device_action_cb_t action_cb = NULL;
    ai_chat_device_action_committed_cb_t committed_cb = NULL;
    void *callback_ctx = NULL;
    ai_chat_device_action_result_t result = {0};
    esp_err_t send_ret = ESP_ERR_INVALID_STATE;
    bool valid = false;

    if (action_ctx == NULL) {
        vTaskDeleteWithCaps(NULL);
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    valid = ai_chat_generation_matches_locked(action_ctx->generation) &&
            s_ai.conn == action_ctx->conn &&
            s_ai.state != AI_CHAT_STATE_IDLE &&
            s_ai.state != AI_CHAT_STATE_STOPPING;
    if (valid) {
        action_cb = s_ai.config.on_device_action;
        committed_cb = s_ai.config.on_device_action_committed;
        callback_ctx = s_ai.config.device_action_ctx;
    }
    xSemaphoreGive(s_lock);

    if (!valid) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (ai_chat_generation_matches_locked(action_ctx->generation)) {
            s_ai.device_action_pending = false;
        }
        xSemaphoreGive(s_lock);
        free(action_ctx);
        vTaskDeleteWithCaps(NULL);
        return;
    }

    if (action_cb == NULL) {
        ai_chat_device_action_set_result(&result,
                                         false,
                                         "unsupported",
                                         "当前固件不支持这个设备动作");
    } else {
        esp_err_t action_ret =
            action_cb(&action_ctx->action, &result, callback_ctx);
        if (action_ret != ESP_OK) {
            result.ok = false;
            if (result.status[0] == '\0') {
                ai_chat_copy_str(result.status,
                                 sizeof(result.status),
                                 esp_err_to_name(action_ret));
            }
            if (result.message[0] == '\0') {
                ai_chat_copy_str(result.message,
                                 sizeof(result.message),
                                 "设备动作执行失败");
            }
        }
    }

    char *json = ai_chat_build_device_action_result_json(
        action_ctx->jsonrpc_id_json,
        &result);
    if (json == NULL) {
        send_ret = ESP_ERR_NO_MEM;
    } else {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        valid = ai_chat_generation_matches_locked(action_ctx->generation) &&
                s_ai.conn == action_ctx->conn &&
                s_ai.state != AI_CHAT_STATE_IDLE &&
                s_ai.state != AI_CHAT_STATE_STOPPING;
        xSemaphoreGive(s_lock);
        send_ret = valid ?
            ai_chat_send_json(action_ctx->conn, json) :
            ESP_ERR_INVALID_STATE;
        free(json);
    }

    ESP_LOGI(DIALOG_TAG,
             "device_action up: rpc_id=%s action=%s ok=%d status=%s route=%s count=%u message=\"%s\" send=%s",
             action_ctx->jsonrpc_id_json,
             action_ctx->action.action[0] != '\0' ?
                 action_ctx->action.action : "(empty)",
             result.ok ? 1 : 0,
             result.status[0] != '\0' ?
                 result.status : (result.ok ? "ok" : "failed"),
             ai_chat_device_action_route_name(result.call_route),
             (unsigned)result.contact_count,
             result.message[0] != '\0' ? result.message : "",
             esp_err_to_name(send_ret));
    for (uint8_t index = 0U; index < result.contact_count; ++index) {
        ESP_LOGI(DIALOG_TAG,
                 "device_action contact: rpc_id=%s index=%u name=\"%s\" online=%d",
                 action_ctx->jsonrpc_id_json,
                 (unsigned)index,
                 result.contacts[index].name,
                 result.contacts[index].online ? 1 : 0);
    }

    if (send_ret == ESP_OK &&
        result.ok &&
        result.call_route != AI_CHAT_DEVICE_ACTION_ROUTE_NONE &&
        result.target_id[0] != '\0' &&
        committed_cb != NULL) {
        /*
         * The complete JSON-RPC response has been accepted by TiRTC. The app
         * lifecycle owner may now close AI Chat and start the selected call.
         */
        esp_err_t commit_ret =
            committed_cb(&action_ctx->action, &result, callback_ctx);
        if (commit_ret != ESP_OK) {
            ESP_LOGE(TAG,
                     "AI Chat device action handoff failed: %s",
                     esp_err_to_name(commit_ret));
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (ai_chat_generation_matches_locked(action_ctx->generation)) {
        s_ai.device_action_pending = false;
    }
    xSemaphoreGive(s_lock);
    free(action_ctx);
    vTaskDeleteWithCaps(NULL);
}

static esp_err_t ai_chat_schedule_device_action(tirtc_conn_t conn,
                                                const ai_chat_event_t *event)
{
    if (conn == NULL || event == NULL || !event->jsonrpc_id_valid) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t generation = 0U;
    bool valid = false;
    bool busy = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    valid = s_ai.conn == conn &&
            s_ai.state != AI_CHAT_STATE_IDLE &&
            s_ai.state != AI_CHAT_STATE_STOPPING;
    if (valid) {
        generation = s_ai.generation;
        if (generation == 0U) {
            valid = false;
        } else if (s_ai.device_action_pending) {
            busy = true;
        } else {
            s_ai.device_action_pending = true;
        }
    }
    xSemaphoreGive(s_lock);
    if (!valid || busy || generation == 0U) {
        return ESP_ERR_INVALID_STATE;
    }

    ai_chat_device_action_context_t *action_ctx =
        ai_chat_calloc_psram(1, sizeof(*action_ctx));
    if (action_ctx == NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (ai_chat_generation_matches_locked(generation)) {
            s_ai.device_action_pending = false;
        }
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }

    action_ctx->generation = generation;
    action_ctx->conn = conn;
    ai_chat_copy_str(action_ctx->jsonrpc_id_json,
                     sizeof(action_ctx->jsonrpc_id_json),
                     event->jsonrpc_id_json);
    ai_chat_copy_str(action_ctx->action.action,
                     sizeof(action_ctx->action.action),
                     event->action);
    ai_chat_copy_str(action_ctx->action.target,
                     sizeof(action_ctx->action.target),
                     event->target);
    ai_chat_copy_str(action_ctx->action.call_type,
                     sizeof(action_ctx->action.call_type),
                     event->call_type);
    ai_chat_copy_str(action_ctx->action.contact_type,
                     sizeof(action_ctx->action.contact_type),
                     event->contact_type);
    ai_chat_copy_str(action_ctx->action.status_filter,
                     sizeof(action_ctx->action.status_filter),
                     event->status_filter);

    BaseType_t task_ret = xTaskCreatePinnedToCoreWithCaps(
        ai_chat_device_action_task,
        "ai_action",
        AI_CHAT_DEVICE_ACTION_TASK_STACK,
        action_ctx,
        AI_CHAT_DEVICE_ACTION_TASK_PRIORITY,
        NULL,
        APP_TASK_CORE_BACKGROUND,
        AI_CHAT_TASK_ALLOC_CAPS);
    if (task_ret != pdPASS) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (ai_chat_generation_matches_locked(generation)) {
            s_ai.device_action_pending = false;
        }
        xSemaphoreGive(s_lock);
        free(action_ctx);
        ai_chat_log_heap("device action task create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void ai_chat_connect_timeout_task(void *ctx)
{
    ai_chat_connect_timeout_context_t *timeout_ctx = (ai_chat_connect_timeout_context_t *)ctx;
    uint32_t generation = timeout_ctx != NULL ? timeout_ctx->generation : 0U;
    uint32_t retry_generation = 0U;
    bool timed_out = false;
    bool retry_requested = false;

    free(timeout_ctx);
    uint32_t waited_ms = 0U;
    while (waited_ms < AI_CHAT_CONNECT_TIMEOUT_MS) {
        uint32_t wait_ms = AI_CHAT_CONNECT_TIMEOUT_MS - waited_ms;
        if (wait_ms > AI_CHAT_LIFECYCLE_WAIT_POLL_MS) {
            wait_ms = AI_CHAT_LIFECYCLE_WAIT_POLL_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
        waited_ms += wait_ms;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool still_connecting = ai_chat_generation_matches_locked(generation) &&
                                s_ai.state == AI_CHAT_STATE_CONNECTING &&
                                s_ai.conn == NULL;
        xSemaphoreGive(s_lock);
        if (!still_connecting) {
            vTaskDeleteWithCaps(NULL);
            return;
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (ai_chat_generation_matches_locked(generation) &&
        s_ai.state == AI_CHAT_STATE_CONNECTING &&
        s_ai.conn == NULL) {
        if (s_ai.start_retries == 0U) {
            s_ai.start_retries = 1U;
            s_ai.last_error = 0;
            retry_generation = ai_chat_next_generation_locked();
            ai_chat_set_state_locked(AI_CHAT_STATE_STARTING,
                                     "retrying stalled connect");
            retry_requested = true;
        } else {
            s_ai.last_error = ESP_ERR_TIMEOUT;
            ai_chat_set_state_locked(AI_CHAT_STATE_ERROR, "connect timeout");
            (void)ai_chat_next_generation_locked();
        }
        timed_out = true;
    }
    xSemaphoreGive(s_lock);

    if (timed_out) {
        ESP_LOGW(TAG,
                 "AI Chat WHIP connect timeout after %ums, disconnecting RTC session",
                 (unsigned)AI_CHAT_CONNECT_TIMEOUT_MS);
        ai_chat_video_stop(NULL);
        ai_chat_media_stop(NULL);
        tirtc_session_flush_remote_media();
        esp_err_t disconnect_ret = tirtc_session_disconnect();
        if (disconnect_ret != ESP_OK && disconnect_ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "AI Chat RTC disconnect after timeout failed: %s", esp_err_to_name(disconnect_ret));
        }
        if (retry_requested) {
            esp_err_t retry_ret = ai_chat_schedule_start_retry(
                retry_generation,
                "WHIP submit/connect timeout");
            if (retry_ret != ESP_OK) {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                if (ai_chat_generation_matches_locked(retry_generation)) {
                    s_ai.last_error = retry_ret;
                    ai_chat_set_state_locked(AI_CHAT_STATE_ERROR,
                                             "connect retry unavailable");
                }
                xSemaphoreGive(s_lock);
                ESP_LOGE(TAG,
                         "AI Chat automatic reconnect scheduling failed: %s",
                         esp_err_to_name(retry_ret));
            }
        }
    }

    vTaskDeleteWithCaps(NULL);
}

static void ai_chat_start_connect_timeout(uint32_t generation)
{
    ai_chat_connect_timeout_context_t *timeout_ctx = ai_chat_calloc_psram(1, sizeof(*timeout_ctx));
    if (timeout_ctx == NULL) {
        ESP_LOGW(TAG, "create AI Chat connect timeout context failed");
        return;
    }
    timeout_ctx->generation = generation;

    BaseType_t task_ret = xTaskCreateWithCaps(ai_chat_connect_timeout_task,
                                              "ai_conn_timeout",
                                              AI_CHAT_CONNECT_TIMEOUT_TASK_STACK,
                                              timeout_ctx,
                                              AI_CHAT_CONNECT_TIMEOUT_TASK_PRIORITY,
                                              NULL,
                                              AI_CHAT_TASK_ALLOC_CAPS);
    if (task_ret != pdPASS) {
        free(timeout_ctx);
        ai_chat_log_heap("connect timeout task create failed");
        ESP_LOGW(TAG, "create AI Chat connect timeout task failed");
    }
}

static void ai_chat_start_session_timeout_task(void *ctx)
{
    ai_chat_start_session_timeout_context_t *timeout_ctx =
        (ai_chat_start_session_timeout_context_t *)ctx;
    uint32_t generation = timeout_ctx != NULL ? timeout_ctx->generation : 0U;
    tirtc_conn_t conn = NULL;
    bool timed_out = false;

    free(timeout_ctx);
    uint32_t waited_ms = 0U;
    while (waited_ms < AI_CHAT_START_SESSION_TIMEOUT_MS) {
        uint32_t wait_ms = AI_CHAT_START_SESSION_TIMEOUT_MS - waited_ms;
        if (wait_ms > AI_CHAT_LIFECYCLE_WAIT_POLL_MS) {
            wait_ms = AI_CHAT_LIFECYCLE_WAIT_POLL_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
        waited_ms += wait_ms;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool still_starting = ai_chat_generation_matches_locked(generation) &&
                              s_ai.state == AI_CHAT_STATE_STARTING_SESSION &&
                              s_ai.conn != NULL;
        xSemaphoreGive(s_lock);
        if (!still_starting) {
            vTaskDeleteWithCaps(NULL);
            return;
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (ai_chat_generation_matches_locked(generation) &&
        s_ai.state == AI_CHAT_STATE_STARTING_SESSION &&
        s_ai.conn != NULL) {
        conn = s_ai.conn;
        s_ai.last_error = ESP_ERR_TIMEOUT;
        ai_chat_set_state_locked(AI_CHAT_STATE_ERROR, "start session timeout");
        (void)ai_chat_next_generation_locked();
        timed_out = true;
    }
    xSemaphoreGive(s_lock);

    if (timed_out) {
        ESP_LOGW(TAG,
                 "AI Chat start_session timeout after %ums, disconnecting RTC session",
                 (unsigned)AI_CHAT_START_SESSION_TIMEOUT_MS);
        ai_chat_video_stop(conn);
        ai_chat_media_stop(conn);
        tirtc_session_flush_remote_media();
        esp_err_t disconnect_ret = ai_chat_disconnect_conn(conn);
        if (disconnect_ret != ESP_OK && disconnect_ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG,
                     "AI Chat RTC disconnect after start_session timeout failed: %s",
                     esp_err_to_name(disconnect_ret));
        }
    }

    vTaskDeleteWithCaps(NULL);
}

static void ai_chat_start_start_session_timeout(uint32_t generation)
{
    ai_chat_start_session_timeout_context_t *timeout_ctx =
        ai_chat_calloc_psram(1, sizeof(*timeout_ctx));
    if (timeout_ctx == NULL) {
        ESP_LOGW(TAG, "create AI Chat start_session timeout context failed");
        return;
    }
    timeout_ctx->generation = generation;

    BaseType_t task_ret = xTaskCreateWithCaps(ai_chat_start_session_timeout_task,
                                              "ai_session_to",
                                              AI_CHAT_START_SESSION_TIMEOUT_TASK_STACK,
                                              timeout_ctx,
                                              AI_CHAT_START_SESSION_TIMEOUT_TASK_PRIORITY,
                                              NULL,
                                              AI_CHAT_TASK_ALLOC_CAPS);
    if (task_ret != pdPASS) {
        free(timeout_ctx);
        ai_chat_log_heap("start-session timeout task create failed");
        ESP_LOGW(TAG, "create AI Chat start_session timeout task failed");
    }
}

static void ai_chat_session_task(void *ctx)
{
    ai_chat_session_context_t *session_ctx = (ai_chat_session_context_t *)ctx;
    uint32_t generation = session_ctx != NULL ? session_ctx->generation : 0U;
    tirtc_conn_t conn = session_ctx != NULL ? session_ctx->conn : NULL;
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    int64_t media_start_us = 0;
    uint32_t media_elapsed_ms = 0;
    uint32_t settle_wait_ms = 0;

    free(session_ctx);
    if (generation == 0U || conn == NULL || !ai_chat_generation_matches(generation)) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_session_task == self) {
            s_session_task = NULL;
        }
        xSemaphoreGive(s_lock);
        vTaskDeleteWithCaps(NULL);
        return;
    }

    media_start_us = esp_timer_get_time();
    esp_err_t media_ret = ai_chat_media_start(conn);
    media_elapsed_ms = (uint32_t)((esp_timer_get_time() - media_start_us) / 1000LL);
    if (media_ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "AI Chat media prewarm failed before start_session: %s elapsed=%ums",
                 esp_err_to_name(media_ret),
                 (unsigned)media_elapsed_ms);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (ai_chat_generation_matches_locked(generation)) {
            s_ai.last_error = media_ret;
            ai_chat_set_state_locked(AI_CHAT_STATE_ERROR, "media prewarm failed");
        }
        xSemaphoreGive(s_lock);
        (void)ai_chat_disconnect_conn(conn);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_session_task == self) {
            s_session_task = NULL;
        }
        xSemaphoreGive(s_lock);
        vTaskDeleteWithCaps(NULL);
        return;
    }

    if (media_elapsed_ms < AI_CHAT_START_SESSION_SETTLE_MS) {
        settle_wait_ms = AI_CHAT_START_SESSION_SETTLE_MS - media_elapsed_ms;
        vTaskDelay(pdMS_TO_TICKS(settle_wait_ms));
    }
    ESP_LOGI(TAG,
             "AI Chat media prewarmed before start_session: elapsed=%ums settle_wait=%ums",
             (unsigned)media_elapsed_ms,
             (unsigned)settle_wait_ms);

    if (!ai_chat_generation_matches(generation)) {
        ai_chat_video_stop(conn);
        ai_chat_media_stop(conn);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_session_task == self) {
            s_session_task = NULL;
        }
        xSemaphoreGive(s_lock);
        vTaskDeleteWithCaps(NULL);
        return;
    }

    esp_err_t ret = ai_chat_send_start_session(conn);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AI Chat start_session send failed: %s", esp_err_to_name(ret));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (ai_chat_generation_matches_locked(generation)) {
            s_ai.last_error = ret;
            ai_chat_set_state_locked(AI_CHAT_STATE_ERROR, "start session failed");
        }
        xSemaphoreGive(s_lock);
        ai_chat_video_stop(conn);
        ai_chat_media_stop(conn);
        tirtc_session_flush_remote_media();
        (void)ai_chat_disconnect_conn(conn);
    } else {
        ai_chat_start_start_session_timeout(generation);
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_session_task == self) {
        s_session_task = NULL;
    }
    xSemaphoreGive(s_lock);
    vTaskDeleteWithCaps(NULL);
}

static esp_err_t ai_chat_schedule_start_session(tirtc_conn_t conn, uint32_t generation)
{
    if (conn == NULL || generation == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    ai_chat_session_context_t *session_ctx = ai_chat_calloc_psram(1, sizeof(*session_ctx));
    if (session_ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }
    session_ctx->generation = generation;
    session_ctx->conn = conn;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_session_task != NULL) {
        xSemaphoreGive(s_lock);
        free(session_ctx);
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t task_ret = xTaskCreateWithCaps(ai_chat_session_task,
                                              "ai_session",
                                              AI_CHAT_SESSION_TASK_STACK,
                                              session_ctx,
                                              AI_CHAT_SESSION_TASK_PRIORITY,
                                              &s_session_task,
                                              AI_CHAT_TASK_ALLOC_CAPS);
    if (task_ret != pdPASS) {
        free(session_ctx);
        s_session_task = NULL;
        xSemaphoreGive(s_lock);
        ai_chat_log_heap("session task create failed");
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

static void ai_chat_on_whip_connect(int error, tirtc_conn_t conn, void *user_data)
{
    ai_chat_whip_context_t *whip_ctx = (ai_chat_whip_context_t *)user_data;
    uint32_t generation = whip_ctx != NULL ? whip_ctx->generation : 0U;

    free(whip_ctx);

    if (!ai_chat_generation_matches(generation)) {
        if (conn != NULL) {
            (void)ai_chat_disconnect_conn(conn);
        }
        ESP_LOGI(TAG, "ignore stale AI Chat WHIP callback generation=%lu", (unsigned long)generation);
        return;
    }

    if (error != 0 || conn == NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (ai_chat_generation_matches_locked(generation)) {
            s_ai.conn = NULL;
            s_ai.last_error = error;
            ai_chat_set_state_locked(AI_CHAT_STATE_ERROR, "connect failed");
        }
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "AI Chat WHIP connect failed: %s (%d)", TiRtcGetErrorStr(error), error);
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ai.conn = conn;
    ai_chat_set_state_locked(AI_CHAT_STATE_CONNECTED, "connected");
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "AI Chat WHIP connected hconn=%p", conn);
    esp_err_t ret = ai_chat_schedule_start_session(conn, generation);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AI Chat schedule start_session failed: %s", esp_err_to_name(ret));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_ai.last_error = ret;
        ai_chat_set_state_locked(AI_CHAT_STATE_ERROR, "start session task failed");
        xSemaphoreGive(s_lock);
        (void)ai_chat_disconnect_conn(conn);
    }
}

static esp_err_t ai_chat_spawn_start_task(uint32_t generation)
{
    ai_chat_start_context_t *start_ctx =
        ai_chat_calloc_psram(1, sizeof(*start_ctx));
    if (start_ctx == NULL) {
        ai_chat_log_heap("start_ctx alloc failed");
        return ESP_ERR_NO_MEM;
    }
    start_ctx->generation = generation;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!ai_chat_generation_matches_locked(generation) ||
        s_start_task != NULL) {
        xSemaphoreGive(s_lock);
        free(start_ctx);
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t task_ret = xTaskCreateWithCaps(ai_chat_start_task,
                                              "ai_chat_start",
                                              AI_CHAT_START_TASK_STACK,
                                              start_ctx,
                                              AI_CHAT_START_TASK_PRIORITY,
                                              &s_start_task,
                                              AI_CHAT_TASK_ALLOC_CAPS);
    if (task_ret != pdPASS) {
        s_start_task = NULL;
        if (ai_chat_generation_matches_locked(generation)) {
            s_ai.last_error = ESP_ERR_NO_MEM;
            ai_chat_set_state_locked(AI_CHAT_STATE_ERROR, "task failed");
        }
        xSemaphoreGive(s_lock);
        free(start_ctx);
        ai_chat_log_heap("start task create failed");
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

static void ai_chat_start_retry_task(void *ctx)
{
    ai_chat_start_retry_context_t *retry_ctx =
        (ai_chat_start_retry_context_t *)ctx;
    uint32_t generation = retry_ctx != NULL ? retry_ctx->generation : 0U;
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    uint32_t waited_ms = 0U;
    bool timed_out = false;

    free(retry_ctx);
    while (waited_ms < AI_CHAT_START_RETRY_TIMEOUT_MS) {
        bool current = false;
        bool previous_start_running = false;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        current = ai_chat_generation_matches_locked(generation);
        previous_start_running = s_start_task != NULL;
        xSemaphoreGive(s_lock);

        if (!current) {
            break;
        }
        if (!previous_start_running) {
            ESP_LOGI(TAG,
                     "AI Chat previous start task released: retry generation=%lu waited=%ums",
                     (unsigned long)generation,
                     (unsigned)waited_ms);
            esp_err_t ret = ai_chat_spawn_start_task(generation);
            if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG,
                         "AI Chat deferred start failed: %s",
                         esp_err_to_name(ret));
            }
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(AI_CHAT_START_RETRY_POLL_MS));
        waited_ms += AI_CHAT_START_RETRY_POLL_MS;
    }

    if (waited_ms >= AI_CHAT_START_RETRY_TIMEOUT_MS) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (ai_chat_generation_matches_locked(generation)) {
            s_ai.last_error = ESP_ERR_TIMEOUT;
            ai_chat_set_state_locked(AI_CHAT_STATE_ERROR,
                                     "previous connect still stopping");
            timed_out = true;
        }
        xSemaphoreGive(s_lock);
    }
    if (timed_out) {
        ESP_LOGE(TAG,
                 "AI Chat previous WHIP submit did not return within %ums",
                 (unsigned)AI_CHAT_START_RETRY_TIMEOUT_MS);
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_start_retry_task == self) {
        s_start_retry_task = NULL;
    }
    xSemaphoreGive(s_lock);
    vTaskDeleteWithCaps(NULL);
}

static esp_err_t ai_chat_schedule_start_retry(uint32_t generation,
                                              const char *reason)
{
    ai_chat_start_retry_context_t *retry_ctx =
        ai_chat_calloc_psram(1, sizeof(*retry_ctx));
    if (retry_ctx == NULL) {
        ai_chat_log_heap("start retry context alloc failed");
        return ESP_ERR_NO_MEM;
    }
    retry_ctx->generation = generation;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!ai_chat_generation_matches_locked(generation) ||
        s_start_retry_task != NULL) {
        xSemaphoreGive(s_lock);
        free(retry_ctx);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGW(TAG,
             "AI Chat start deferred: reason=%s generation=%lu",
             reason != NULL ? reason : "previous submit running",
             (unsigned long)generation);
    BaseType_t task_ret = xTaskCreateWithCaps(ai_chat_start_retry_task,
                                              "ai_start_retry",
                                              AI_CHAT_START_RETRY_TASK_STACK,
                                              retry_ctx,
                                              AI_CHAT_START_RETRY_TASK_PRIORITY,
                                              &s_start_retry_task,
                                              AI_CHAT_TASK_ALLOC_CAPS);
    if (task_ret != pdPASS) {
        s_start_retry_task = NULL;
        xSemaphoreGive(s_lock);
        free(retry_ctx);
        ai_chat_log_heap("start retry task create failed");
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

static void ai_chat_start_task(void *ctx)
{
    ai_chat_start_context_t *start_ctx = (ai_chat_start_context_t *)ctx;
    uint32_t generation = start_ctx != NULL ? start_ctx->generation : 0U;
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    ai_chat_config_t *config = NULL;
    ai_chat_join_info_t *join_info = NULL;
    esp_err_t ret = ESP_OK;
    int64_t prerequisites_start_us = esp_timer_get_time();
    uint32_t token_elapsed_ms = 0U;
    uint32_t rtc_wait_elapsed_ms = 0U;

    free(start_ctx);

    config = (ai_chat_config_t *)ai_chat_calloc_psram(1, sizeof(*config));
    join_info = (ai_chat_join_info_t *)ai_chat_calloc_psram(1, sizeof(*join_info));
    if (config == NULL || join_info == NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (ai_chat_generation_matches_locked(generation)) {
            s_ai.last_error = ESP_ERR_NO_MEM;
            ai_chat_set_state_locked(AI_CHAT_STATE_ERROR, "start workspace failed");
        }
        xSemaphoreGive(s_lock);
        goto finish;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!ai_chat_generation_matches_locked(generation)) {
        xSemaphoreGive(s_lock);
        goto finish;
    }
    *config = s_ai.config;
    xSemaphoreGive(s_lock);

    ret = ai_chat_verify_time_ready();
    if (ret == ESP_OK && ai_chat_generation_matches(generation)) {
        int64_t rtc_wait_start_us = esp_timer_get_time();
        ret = ai_chat_wait_rtc_ready(generation);
        rtc_wait_elapsed_ms = (uint32_t)((esp_timer_get_time() - rtc_wait_start_us) / 1000LL);
    }
    if (ret == ESP_OK && ai_chat_generation_matches(generation)) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (!ai_chat_generation_matches_locked(generation)) {
            xSemaphoreGive(s_lock);
            goto finish;
        }
        ai_chat_set_state_locked(AI_CHAT_STATE_TOKEN, "request token");
        xSemaphoreGive(s_lock);

        int64_t token_start_us = esp_timer_get_time();
        ret = ai_chat_token_request_join(config, join_info);
        token_elapsed_ms = (uint32_t)((esp_timer_get_time() - token_start_us) / 1000LL);
    }
    if (ret == ESP_OK && !ai_chat_generation_matches(generation)) {
        goto finish;
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "AI Chat prerequisite stage failed: token=%ums rtc_wait=%ums ret=%s",
                 (unsigned)token_elapsed_ms,
                 (unsigned)rtc_wait_elapsed_ms,
                 esp_err_to_name(ret));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (ai_chat_generation_matches_locked(generation)) {
            s_ai.last_error = ret;
            ai_chat_set_state_locked(AI_CHAT_STATE_ERROR, "startup prerequisite failed");
        }
        xSemaphoreGive(s_lock);
        goto finish;
    }
    ESP_LOGI(TAG,
             "AI Chat prerequisites ready: token=%ums rtc_wait=%ums total=%ums",
             (unsigned)token_elapsed_ms,
             (unsigned)rtc_wait_elapsed_ms,
             (unsigned)((esp_timer_get_time() - prerequisites_start_us) / 1000LL));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!ai_chat_generation_matches_locked(generation)) {
        xSemaphoreGive(s_lock);
        goto finish;
    }
    ai_chat_copy_str(s_ai.role_id, sizeof(s_ai.role_id), join_info->role_id);
    ai_chat_set_state_locked(AI_CHAT_STATE_CONNECTING, "connecting");
    xSemaphoreGive(s_lock);

    ai_chat_whip_context_t *whip_ctx = ai_chat_calloc_psram(1, sizeof(*whip_ctx));
    if (whip_ctx == NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (ai_chat_generation_matches_locked(generation)) {
            s_ai.last_error = ESP_ERR_NO_MEM;
            ai_chat_set_state_locked(AI_CHAT_STATE_ERROR, "connect submit failed");
        }
        xSemaphoreGive(s_lock);
        goto finish;
    }
    whip_ctx->generation = generation;

    ESP_LOGI(TAG,
             "AI Chat WHIP submit: device_id_len=%u role_id_len=%u user_id_len=%u peer_id_len=%u token_len=%u",
             (unsigned)strlen(config->device_id),
             (unsigned)strlen(join_info->role_id),
             (unsigned)strlen(config->user_id),
             (unsigned)strlen(join_info->peer_id),
             (unsigned)strlen(join_info->token));
    tirtc_session_set_next_connection_auto_media(false);
    /*
     * Start the guard before entering the SDK. TiRtcWhipConnect is normally a
     * quick asynchronous submission, but a stale SDK transport can block the
     * submission itself, before the former post-submit guard was created.
     */
    ai_chat_start_connect_timeout(generation);
    int rc = tirtc_session_whip_connect(join_info->peer_id,
                                        join_info->token,
                                        ai_chat_on_whip_connect,
                                        whip_ctx);
    if (rc != 0) {
        tirtc_session_set_next_connection_auto_media(true);
        free(whip_ctx);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (ai_chat_generation_matches_locked(generation)) {
            s_ai.last_error = rc;
            ai_chat_set_state_locked(AI_CHAT_STATE_ERROR, "connect submit failed");
        }
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "AI Chat WHIP submit failed: %s (%d)", TiRtcGetErrorStr(rc), rc);
        if (rc == TIRTC_E_SERVER_ERROR) {
            ESP_LOGW(TAG,
                     "AI Chat server rejected WHIP: role_id_len=%u",
                     (unsigned)strlen(join_info->role_id));
        }
    }

finish:
    free(join_info);
    free(config);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_start_task == self) {
        s_start_task = NULL;
    }
    xSemaphoreGive(s_lock);
    vTaskDeleteWithCaps(NULL);
}

static void ai_chat_apply_caption_locked(const ai_chat_event_t *event)
{
    if (event == NULL || event->caption_type < 0 || event->caption_type > 1) {
        return;
    }
    if (event->text[0] == '\0') {
        ai_chat_set_state_locked(s_ai.state, "empty caption");
        return;
    }

    ai_chat_caption_group_t *group = &s_ai.captions[event->caption_type];
    if (group->utterance_id != event->utterance_id ||
        group->caption_type != event->caption_type ||
        group->final) {
        group->caption_type = event->caption_type;
        group->utterance_id = event->utterance_id;
        group->final = false;
        group->text[0] = '\0';
    }

    ai_chat_apply_caption_text(group, event);

    group->final = event->is_final;
    ai_chat_update_message_locked(event->caption_type,
                                  event->utterance_id,
                                  group->text,
                                  event->is_final);
    ai_chat_set_state_locked(s_ai.state, event->is_final ? "caption final" : "caption");
}

static void ai_chat_handle_event(tirtc_conn_t conn, const ai_chat_event_t *event)
{
    if (event == NULL) {
        return;
    }

    switch (event->type) {
    case AI_CHAT_EVENT_START_OK: {
        bool listening = false;
        uint32_t heartbeat_generation = 0;

        if (!ai_chat_audio_spec_supported(&event->input_audio) ||
            !ai_chat_audio_spec_supported(&event->output_audio)) {
            ESP_LOGW(TAG, "AI Chat audio format is unsupported");
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_ai.last_error = ESP_ERR_NOT_SUPPORTED;
            ai_chat_set_state_locked(AI_CHAT_STATE_ERROR, "unsupported audio");
            xSemaphoreGive(s_lock);
            (void)ai_chat_close();
            return;
        }

        ESP_LOGI(TAG,
                 "AI Chat start_session ok: session_id=%s input=%s/%luHz/%uch output=%s/%luHz/%uch",
                 event->session_id,
                 event->input_audio.codec[0] != '\0' ? event->input_audio.codec : "default",
                 (unsigned long)event->input_audio.sample_rate,
                 (unsigned)event->input_audio.channels,
                 event->output_audio.codec[0] != '\0' ? event->output_audio.codec : "default",
                 (unsigned long)event->output_audio.sample_rate,
                 (unsigned)event->output_audio.channels);

        esp_err_t subscribe_ret = tirtc_session_subscribe_audio(conn, AI_CHAT_AUDIO_STREAM_ID);
        if (subscribe_ret != ESP_OK) {
            ESP_LOGW(TAG, "AI Chat subscribe audio failed: %s", esp_err_to_name(subscribe_ret));
        }

        esp_err_t media_ret = ai_chat_media_start(conn);
        if (media_ret != ESP_OK) {
            ESP_LOGW(TAG, "AI Chat media start failed after session ok: %s", esp_err_to_name(media_ret));
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_ai.last_error = media_ret;
            ai_chat_set_state_locked(AI_CHAT_STATE_ERROR, "media failed");
            xSemaphoreGive(s_lock);
            (void)ai_chat_close();
            return;
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        ai_chat_copy_str(s_ai.session_id, sizeof(s_ai.session_id), event->session_id);
        ai_chat_set_state_locked(AI_CHAT_STATE_IN_SESSION, "in session");
        s_ai.listening = true;
        listening = s_ai.listening;
        heartbeat_generation = s_ai.generation;
        xSemaphoreGive(s_lock);

        ai_chat_start_heartbeat_once(heartbeat_generation);
        (void)ai_chat_media_set_uplink_enabled(listening);
        esp_err_t video_ret = ai_chat_schedule_video_start(conn,
                                                           heartbeat_generation);
        if (video_ret != ESP_OK) {
            ESP_LOGW(TAG,
                     "AI Chat video start scheduling failed; audio session continues: %s",
                     esp_err_to_name(video_ret));
        }
        break;
    }
    case AI_CHAT_EVENT_START_ERROR:
        ESP_LOGW(TAG,
                 "AI Chat start_session failed code=%d message=%s",
                 event->error_code,
                 event->error_message);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_ai.last_error = event->error_code;
        ai_chat_set_state_locked(AI_CHAT_STATE_ERROR, "start rejected");
        xSemaphoreGive(s_lock);
        (void)ai_chat_close();
        break;
    case AI_CHAT_EVENT_CAPTION:
        ESP_LOGD(TAG,
                 "AI Chat caption: type=%s final=%d mode=%d seq=%d utterance=%lld text_len=%u",
                 event->caption_type == 0 ? "ASR" : (event->caption_type == 1 ? "TTS" : "invalid"),
                 event->is_final ? 1 : 0,
                 event->mode,
                 event->seq_num,
                 (long long)event->utterance_id,
                 (unsigned)strlen(event->text));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        ai_chat_apply_caption_locked(event);
        xSemaphoreGive(s_lock);
        break;
    case AI_CHAT_EVENT_ROUND_START:
        ESP_LOGI(TAG, "AI Chat round_start");
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_ai.cloud_speaking = true;
        ai_chat_set_state_locked(s_ai.state, "cloud speaking");
        xSemaphoreGive(s_lock);
        break;
    case AI_CHAT_EVENT_ROUND_END:
        ESP_LOGI(TAG, "AI Chat round_end");
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_ai.cloud_speaking = false;
        ai_chat_set_state_locked(s_ai.state, "waiting input");
        xSemaphoreGive(s_lock);
        break;
    case AI_CHAT_EVENT_INTERRUPT:
        tirtc_session_flush_remote_media();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_ai.cloud_speaking = false;
        ai_chat_set_state_locked(s_ai.state, "interrupted");
        xSemaphoreGive(s_lock);
        break;
    case AI_CHAT_EVENT_END_SESSION:
        (void)ai_chat_close();
        break;
    case AI_CHAT_EVENT_DEVICE_ACTION: {
        ESP_LOGI(DIALOG_TAG,
                 "device_action down: rpc_id=%s action=%s target=\"%s\" status_filter=%s call_type=%s contact_type=%s",
                 event->jsonrpc_id_json[0] != '\0' ?
                     event->jsonrpc_id_json : "(missing)",
                 event->action[0] != '\0' ?
                     event->action : "(empty)",
                 event->target,
                 event->status_filter[0] != '\0' ?
                     event->status_filter : "(default)",
                 event->call_type[0] != '\0' ?
                     event->call_type : "(default)",
                 event->contact_type[0] != '\0' ?
                     event->contact_type : "(default)");
        esp_err_t ret = ai_chat_schedule_device_action(conn, event);
        if (ret != ESP_OK) {
            const char *status =
                ret == ESP_ERR_NO_MEM ? "internal" :
                ret == ESP_ERR_INVALID_ARG ? "invalid_request" : "busy";
            const char *message =
                ret == ESP_ERR_NO_MEM ? "设备资源不足，请稍后再试" :
                ret == ESP_ERR_INVALID_ARG ? "设备动作参数不完整" :
                                             "已有设备动作正在执行";
            ESP_LOGW(TAG,
                     "AI Chat device_action rejected locally: action=%s ret=%s",
                     event->action,
                     esp_err_to_name(ret));
            esp_err_t send_ret =
                ai_chat_send_device_action_failure(conn,
                                                   event->jsonrpc_id_json,
                                                   status,
                                                   message);
            ESP_LOGI(DIALOG_TAG,
                     "device_action up: rpc_id=%s action=%s ok=0 status=%s route=none count=0 message=\"%s\" send=%s",
                     event->jsonrpc_id_json[0] != '\0' ?
                         event->jsonrpc_id_json : "(missing)",
                     event->action[0] != '\0' ?
                         event->action : "(empty)",
                     status,
                     message,
                     esp_err_to_name(send_ret));
        }
        break;
    }
    default:
        break;
    }
}

static bool ai_chat_on_command(tirtc_conn_t conn, uint32_t cmdw, const void *data, uint32_t data_len, void *ctx)
{
    (void)ctx;

    if (conn == NULL || !ai_chat_is_signaling_cmd(cmdw)) {
        return false;
    }

    bool mine = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    mine = s_ai.conn == conn;
    if (mine) {
        s_ai.rx_commands++;
    }
    xSemaphoreGive(s_lock);

    if (!mine) {
        /* The command family still belongs to AI Chat after close() has
         * detached its connection. Consume a late server response here so it
         * cannot fall through to the generic device-call command decoder. */
        ESP_LOGD(TAG,
                 "ignore stale AI Chat command: hconn=%p cmdw=0x%08lx",
                 conn,
                 (unsigned long)cmdw);
        return true;
    }

    ESP_LOGI(TAG,
             "AI Chat command received: cmdw=0x%08lx len=%lu",
             (unsigned long)cmdw,
             (unsigned long)data_len);

    ai_chat_event_t event = {0};
    esp_err_t ret = ai_chat_events_parse(data, data_len, &event);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "parse AI Chat command failed cmdw=0x%08lx len=%lu",
                 (unsigned long)cmdw,
                 (unsigned long)data_len);
        return true;
    }

    ESP_LOGI(TAG, "AI Chat event=%s", ai_chat_event_type_name(event.type));
    ai_chat_handle_event(conn, &event);
    return true;
}

static bool ai_chat_on_message(tirtc_conn_t conn,
                               uint8_t media,
                               uint8_t stream_id,
                               uint8_t flags,
                               const void *data,
                               uint32_t data_len,
                               void *ctx)
{
    (void)ctx;

    if (conn == NULL || data == NULL || data_len == 0U) {
        return false;
    }

    bool mine = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    mine = s_ai.conn == conn;
    xSemaphoreGive(s_lock);
    if (!mine) {
        return false;
    }

    ESP_LOGI(TAG,
             "AI Chat message received: media=%u stream=%u flags=%u len=%lu",
             (unsigned)media,
             (unsigned)stream_id,
             (unsigned)flags,
             (unsigned long)data_len);

    ai_chat_event_t event = {0};
    esp_err_t ret = ai_chat_events_parse(data, data_len, &event);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "parse AI Chat message failed len=%lu",
                 (unsigned long)data_len);
        return false;
    }

    ESP_LOGI(TAG, "AI Chat message event=%s", ai_chat_event_type_name(event.type));
    ai_chat_handle_event(conn, &event);
    return true;
}

static void ai_chat_on_connection_error(tirtc_conn_t conn, int error, void *ctx)
{
    (void)ctx;
    bool mine = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    mine = conn != NULL && conn == s_ai.conn;
    if (mine) {
        s_ai.last_error = error;
        s_ai.cloud_speaking = false;
        s_ai.listening = false;
        s_ai.device_action_pending = false;
        (void)ai_chat_next_generation_locked();
        ai_chat_set_state_locked(AI_CHAT_STATE_ERROR, "connection error");
    }
    xSemaphoreGive(s_lock);

    if (mine) {
        ai_chat_video_stop(conn);
        ai_chat_media_stop(conn);
        tirtc_session_flush_remote_media();
    }
}

static void ai_chat_on_disconnected(tirtc_conn_t conn, void *ctx)
{
    (void)ctx;
    bool mine = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    mine = conn != NULL && conn == s_ai.conn;
    if (mine) {
        s_ai.conn = NULL;
        s_ai.cloud_speaking = false;
        s_ai.listening = false;
        s_ai.device_action_pending = false;
        (void)ai_chat_next_generation_locked();
        ai_chat_set_state_locked(AI_CHAT_STATE_IDLE, "disconnected");
    }
    xSemaphoreGive(s_lock);

    if (mine) {
        ai_chat_video_stop(conn);
        ai_chat_media_stop(conn);
        tirtc_session_flush_remote_media();
    }
}

esp_err_t ai_chat_configure(const ai_chat_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "AI Chat config is null");
    ESP_RETURN_ON_ERROR(ai_chat_validate_config(config), TAG, "invalid AI Chat config");

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexWithCaps(APP_SYNC_CAPS_CONTROL);
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    ESP_RETURN_ON_ERROR(ai_chat_ensure_message_history(), TAG, "allocate AI Chat message history failed");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ai.config = *config;
    if (!config->enabled) {
        ai_chat_set_state_locked(AI_CHAT_STATE_IDLE, "disabled");
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t ai_chat_init(const ai_chat_config_t *config)
{
    bool first_ready = false;

    ESP_RETURN_ON_ERROR(ai_chat_configure(config), TAG, "configure AI Chat failed");
    ESP_RETURN_ON_ERROR(ai_chat_video_init(), TAG, "initialize AI Chat video failed");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool registered = s_ai.observer_registered;
    xSemaphoreGive(s_lock);

    if (!registered) {
        const tirtc_session_observer_t observer = {
            .on_command = ai_chat_on_command,
            .on_message = ai_chat_on_message,
            .on_connection_error = ai_chat_on_connection_error,
            .on_disconnected = ai_chat_on_disconnected,
        };
        ESP_RETURN_ON_ERROR(tirtc_session_register_observer(&observer, NULL),
                            TAG,
                            "register AI Chat RTC observer failed");

        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_ai.observer_registered = true;
        s_ai.initialized = true;
        xSemaphoreGive(s_lock);
        first_ready = true;
    }

    if (first_ready) {
        ESP_LOGI(TAG, "AI Chat service ready");
    } else {
        ESP_LOGD(TAG, "AI Chat service configured");
    }
    return ESP_OK;
}

esp_err_t ai_chat_open(void)
{
    uint32_t generation = 0;
    bool wait_previous_start = false;

    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "AI Chat not initialized");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_ai.config.enabled) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ai.state == AI_CHAT_STATE_STARTING ||
        s_ai.state == AI_CHAT_STATE_IN_SESSION ||
        s_ai.state == AI_CHAT_STATE_TOKEN ||
        s_ai.state == AI_CHAT_STATE_CONNECTING ||
        s_ai.state == AI_CHAT_STATE_CONNECTED ||
        s_ai.state == AI_CHAT_STATE_STARTING_SESSION) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    if (s_ai.state == AI_CHAT_STATE_STOPPING || s_ai.conn != NULL) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_start_retry_task != NULL) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    wait_previous_start = s_start_task != NULL;
    memset(s_ai.session_id, 0, sizeof(s_ai.session_id));
    memset(s_ai.captions, 0, sizeof(s_ai.captions));
    ai_chat_clear_messages_locked();
    s_ai.last_error = 0;
    s_ai.rx_commands = 0;
    s_ai.device_action_pending = false;
    s_ai.start_retries = 0U;
    s_ai.cloud_speaking = false;
    s_ai.listening = false;
    generation = ai_chat_next_generation_locked();
    ai_chat_set_state_locked(AI_CHAT_STATE_STARTING, "starting");
    xSemaphoreGive(s_lock);

    /* A new generation must never inherit a camera route from a connection
     * whose final disconnect callback was lost during a network transition. */
    ai_chat_video_stop(NULL);

    if (!wait_previous_start) {
        ai_chat_log_heap("before start task");
        return ai_chat_spawn_start_task(generation);
    }

    esp_err_t retry_ret =
        ai_chat_schedule_start_retry(generation, "previous WHIP submit running");
    if (retry_ret != ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (ai_chat_generation_matches_locked(generation)) {
            s_ai.last_error = retry_ret;
            ai_chat_set_state_locked(AI_CHAT_STATE_ERROR,
                                     "start retry task failed");
        }
        xSemaphoreGive(s_lock);
        return retry_ret;
    }

    return ESP_OK;
}

esp_err_t ai_chat_close(void)
{
    tirtc_conn_t conn = NULL;
    TaskHandle_t heartbeat_task = NULL;

    if (s_lock == NULL) {
        return ESP_OK;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    conn = s_ai.conn;
    heartbeat_task = s_heartbeat_task;
    if (s_ai.state == AI_CHAT_STATE_IDLE && conn == NULL) {
        memset(s_ai.captions, 0, sizeof(s_ai.captions));
        ai_chat_clear_messages_locked();
        s_ai.device_action_pending = false;
        s_ai.start_retries = 0U;
        xSemaphoreGive(s_lock);
        if (heartbeat_task != NULL) {
            xTaskNotifyGive(heartbeat_task);
        }
        ai_chat_video_stop(NULL);
        return ESP_OK;
    }
    ai_chat_set_state_locked(AI_CHAT_STATE_STOPPING, "stopping");
    (void)ai_chat_next_generation_locked();
    s_ai.conn = NULL;
    s_ai.cloud_speaking = false;
    s_ai.device_action_pending = false;
    s_ai.start_retries = 0U;
    xSemaphoreGive(s_lock);

    if (heartbeat_task != NULL) {
        xTaskNotifyGive(heartbeat_task);
    }

    if (conn != NULL) {
        ai_chat_video_stop(conn);
        char *json = ai_chat_build_notification_json("end_session");
        if (json != NULL) {
            (void)ai_chat_send_json(conn, json);
            free(json);
        }
        (void)tirtc_session_unsubscribe_audio(conn, AI_CHAT_AUDIO_STREAM_ID);
        ai_chat_media_stop(conn);
        tirtc_session_flush_remote_media();
        (void)ai_chat_disconnect_conn(conn);
    } else {
        ai_chat_video_stop(NULL);
        ai_chat_media_stop(NULL);
        tirtc_session_flush_remote_media();
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_ai.session_id, 0, sizeof(s_ai.session_id));
    memset(s_ai.captions, 0, sizeof(s_ai.captions));
    ai_chat_clear_messages_locked();
    s_ai.listening = false;
    ai_chat_set_state_locked(AI_CHAT_STATE_IDLE, "idle");
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t ai_chat_wait_until_quiescent(uint32_t timeout_ms)
{
    uint32_t waited_ms = 0U;
    ai_chat_state_t state = AI_CHAT_STATE_IDLE;
    tirtc_conn_t conn = NULL;
    TaskHandle_t start_task = NULL;
    TaskHandle_t start_retry_task = NULL;
    TaskHandle_t session_task = NULL;
    TaskHandle_t video_start_task = NULL;
    TaskHandle_t heartbeat_task = NULL;

    if (s_lock == NULL) {
        return ESP_OK;
    }

    while (true) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        state = s_ai.state;
        conn = s_ai.conn;
        start_task = s_start_task;
        start_retry_task = s_start_retry_task;
        session_task = s_session_task;
        video_start_task = s_video_start_task;
        heartbeat_task = s_heartbeat_task;
        xSemaphoreGive(s_lock);

        if (state == AI_CHAT_STATE_IDLE && conn == NULL &&
            start_task == NULL && start_retry_task == NULL &&
            session_task == NULL && video_start_task == NULL &&
            heartbeat_task == NULL) {
            return ESP_OK;
        }
        if (waited_ms >= timeout_ms) {
            break;
        }

        uint32_t wait_ms = timeout_ms - waited_ms;
        if (wait_ms > AI_CHAT_LIFECYCLE_WAIT_POLL_MS) {
            wait_ms = AI_CHAT_LIFECYCLE_WAIT_POLL_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
        waited_ms += wait_ms;
    }

    ESP_LOGW(TAG,
             "AI Chat quiesce timeout: waited=%ums state=%s conn=%p tasks=start:%p retry:%p session:%p video:%p heartbeat:%p",
             (unsigned)waited_ms,
             ai_chat_state_name(state),
             conn,
             start_task,
             start_retry_task,
             session_task,
             video_start_task,
             heartbeat_task);
    return ESP_ERR_TIMEOUT;
}

esp_err_t ai_chat_clear_messages(void)
{
    if (s_lock == NULL) {
        return ESP_OK;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_ai.captions, 0, sizeof(s_ai.captions));
    ai_chat_clear_messages_locked();
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t ai_chat_handle_control_button(bool pressed)
{
    ai_chat_state_t state = AI_CHAT_STATE_IDLE;
    bool should_interrupt = false;
    tirtc_conn_t conn = NULL;

    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "AI Chat not initialized");

    if (!pressed) {
        return ESP_OK;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    state = s_ai.state;
    should_interrupt = pressed && s_ai.cloud_speaking;
    conn = s_ai.conn;
    xSemaphoreGive(s_lock);

    if (state != AI_CHAT_STATE_IN_SESSION || conn == NULL) {
        return ESP_OK;
    }

    if (should_interrupt) {
        char *json = ai_chat_build_notification_json("interrupt");
        if (json != NULL && conn != NULL) {
            (void)ai_chat_send_json(conn, json);
        }
        free(json);
        tirtc_session_flush_remote_media();
    }

    return ESP_OK;
}

bool ai_chat_owns_control_button(void)
{
    if (s_lock == NULL) {
        return false;
    }

    ai_chat_state_t state = AI_CHAT_STATE_IDLE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    state = s_ai.state;
    xSemaphoreGive(s_lock);

    return state == AI_CHAT_STATE_STARTING ||
           state == AI_CHAT_STATE_TOKEN ||
           state == AI_CHAT_STATE_CONNECTING ||
           state == AI_CHAT_STATE_CONNECTED ||
           state == AI_CHAT_STATE_STARTING_SESSION ||
           state == AI_CHAT_STATE_IN_SESSION;
}

bool ai_chat_can_start(void)
{
    if (s_lock == NULL) {
        return true;
    }

    bool can_start = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    can_start = s_ai.state == AI_CHAT_STATE_IDLE && s_ai.last_error == 0;
    xSemaphoreGive(s_lock);
    return can_start;
}

void ai_chat_get_snapshot(ai_chat_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    if (s_lock == NULL) {
        return;
    }

    uint32_t tx_frames = 0;
    uint32_t tx_failures = 0;
    ai_chat_video_stats_t video_stats = {0};
    ai_chat_media_get_stats(&tx_frames, &tx_failures);
    ai_chat_video_get_stats(&video_stats);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    snapshot->state = s_ai.state;
    snapshot->active = s_ai.state == AI_CHAT_STATE_IN_SESSION;
    snapshot->listening = s_ai.state == AI_CHAT_STATE_IN_SESSION && s_ai.listening;
    snapshot->cloud_speaking = s_ai.cloud_speaking;
    snapshot->video_active = video_stats.active;
    snapshot->tx_audio_frames = tx_frames;
    snapshot->tx_audio_failures = tx_failures;
    snapshot->tx_video_frames = video_stats.queued_frames;
    snapshot->tx_video_failures = video_stats.queue_failures;
    snapshot->rx_commands = s_ai.rx_commands;
    snapshot->last_error = s_ai.last_error;
    ai_chat_copy_str(snapshot->role_id, sizeof(snapshot->role_id), s_ai.role_id);
    ai_chat_copy_str(snapshot->session_id, sizeof(snapshot->session_id), s_ai.session_id);
    ai_chat_copy_str(snapshot->asr_caption, sizeof(snapshot->asr_caption), s_ai.captions[0].text);
    ai_chat_copy_str(snapshot->tts_caption, sizeof(snapshot->tts_caption), s_ai.captions[1].text);
    snapshot->message_count = s_ai.message_count > AI_CHAT_MESSAGE_SNAPSHOT_MAX ?
        AI_CHAT_MESSAGE_SNAPSHOT_MAX : s_ai.message_count;
    for (uint8_t index = 0; index < snapshot->message_count; ++index) {
        uint8_t source_index = (uint8_t)(s_ai.message_count - snapshot->message_count + index);
        snapshot->messages[index].caption_type = s_ai.messages[source_index].caption_type;
        snapshot->messages[index].utterance_id = s_ai.messages[source_index].utterance_id;
        snapshot->messages[index].final = s_ai.messages[source_index].final;
        ai_chat_copy_str(snapshot->messages[index].text,
                         sizeof(snapshot->messages[index].text),
                         s_ai.messages[source_index].text);
    }
    ai_chat_copy_str(snapshot->status, sizeof(snapshot->status), s_ai.status);
    xSemaphoreGive(s_lock);
}
