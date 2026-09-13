/*
 * 微信 VoIP 会话管理.
 *
 * 业务服务器下发入会通知后,本文件保存本次 peer_id/token,
 * 再由界面接听或主动呼叫自动入会触发 WHIP 建连.
 * 主动呼叫在 WHIP 入会成功后立即启动媒体; 被叫接听在 CALL_CONNECTED
 * 确认后启动媒体. 两条路径共用同一个幂等媒体启动工作项.
 */
#include "wechat_voip_session.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "app_memory_policy.h"
#include "tiRTC.h"
#include "tirtc_session.h"
#include "tirtc_voip_cmdw.h"
#include "wechat_voip_config.h"
#include "wechat_voip_media.h"
#include "wechat_voip_trace.h"

static const char *TAG = "wx_voip";

enum
{
    VOIP_ANSWER_TASK_STACK = 49152,
    VOIP_ANSWER_TASK_PRIORITY = 5,
    VOIP_UI_ANSWER_DELAY_MS = 0,
    VOIP_ACTIVE_ANSWER_DELAY_MS = 0,
    VOIP_ANSWER_RETRY_DELAY_MS = 100,
    VOIP_RING_TIMEOUT_MS = 35000,
    VOIP_CONNECT_TIMEOUT_MS = 20000,
    VOIP_CONNECTED_WAIT_TIMEOUT_MS = 15000,
    VOIP_CLOSE_WAIT_TIMEOUT_MS = 1500,
    VOIP_RECALL_GUARD_MS = 1200,
    VOIP_STATUS_WARN_INTERVAL_MS = 15000,
    VOIP_MEDIA_STOP_WAIT_MS = 300,
    VOIP_WORK_QUEUE_LEN = 8,
    VOIP_WORK_TASK_STACK = 12288,
    VOIP_WORK_TASK_PRIORITY = 5,
    VOIP_DISCONNECT_DELAY_MS = 120,
    VOIP_SHUTDOWN_POLL_MS = 20,
    VOIP_RECENT_ROOM_GUARD_MS = 60000,
    VOIP_WHIP_MIN_INTERNAL_FREE = 32 * 1024,
    VOIP_WHIP_MIN_INTERNAL_LARGEST = 8 * 1024,
    VOIP_WHIP_WARN_INTERNAL_LARGEST = 16 * 1024,
    VOIP_WHIP_PEER_ID_MAX = 2048,
    VOIP_WHIP_TOKEN_MAX = 1536,
    VOIP_WX_SESSION_TOKEN_MAX = 1024,
};

typedef enum
{
    VOIP_STATE_IDLE,
    VOIP_STATE_RINGING,
    VOIP_STATE_CONNECTING,
    VOIP_STATE_AWAITING_CONNECTED,
    VOIP_STATE_IN_CALL,
    VOIP_STATE_CLOSING,
} voip_state_t;

typedef struct
{
    voip_state_t state;
    tirtc_conn_t hconn;
    char peer_id[VOIP_WHIP_PEER_ID_MAX];
    char token[VOIP_WHIP_TOKEN_MAX];
    char wx_app_id[64];
    char wx_model_id[64];
    char wx_session_token[VOIP_WX_SESSION_TOKEN_MAX];
    char wx_room_id[128];
    char wx_payload[256];
    int64_t deadline_us;
    uint32_t generation;
    bool outbound_call;
    bool cancel_on_connect;
    wechat_voip_call_media_t call_media;
    bool connection_callback_claimed;
    tirtc_conn_t connection_callback_hconn;
    bool connection_tracked;
    bool connected_command_received;
    bool media_start_pending;
    bool media_started;
} voip_session_t;

typedef struct
{
    char wx_app_id[64];
    char wx_model_id[64];
    char wx_session_token[VOIP_WX_SESSION_TOKEN_MAX];
    char wx_room_id[128];
    char wx_payload[256];
} voip_reject_info_t;

typedef enum
{
    VOIP_WORK_START_MEDIA,
    VOIP_WORK_STOP_MEDIA,
    VOIP_WORK_DISCONNECT,
    VOIP_WORK_HANGUP,
    VOIP_WORK_REJECT,
} voip_work_type_t;

typedef struct
{
    voip_work_type_t type;
    tirtc_conn_t hconn;
    uint32_t generation;
    tirtc_voip_hangup_reason_t reason;
    bool only_if_current;
    voip_reject_info_t *reject;
} voip_work_item_t;

typedef struct
{
    voip_state_t state;
    tirtc_conn_t hconn;
    bool answer_pending;
    TaskHandle_t answer_worker;
    bool work_busy;
    voip_work_type_t work_type;
    uint32_t work_queue_len;
    bool media_running;
    wechat_voip_media_stats_t media;
    bool rtc_active_connection;
    bool rtc_call_active;
    tirtc_session_state_t rtc_state;
    int64_t deadline_left_ms;
    int64_t last_close_ago_ms;
} voip_status_t;

static SemaphoreHandle_t s_mutex;
static portMUX_TYPE s_init_lock = portMUX_INITIALIZER_UNLOCKED;
static EXT_RAM_BSS_ATTR voip_session_t s_session;
static TaskHandle_t s_answer_worker_task;
static QueueHandle_t s_work_queue;
static TaskHandle_t s_work_task;
static int64_t s_last_close_us;
static char s_last_close_room_id[sizeof(s_session.wx_room_id)];
static int64_t s_last_close_room_us;
static uint32_t s_session_generation;
static bool s_answer_pending;
static char s_answer_source[32];
static uint32_t s_answer_delay_ms;
static uint32_t s_answer_request_seq;
static portMUX_TYPE s_work_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_work_busy;
static voip_work_type_t s_work_type;
static int64_t s_last_status_warn_us;

static void voip_work_task(void *arg);
static esp_err_t ensure_work_worker(void);
static esp_err_t start_answer_worker(void);
static esp_err_t enqueue_work(const voip_work_item_t *item);
static esp_err_t send_reject_info(const voip_reject_info_t *info, tirtc_voip_hangup_reason_t reason);
static esp_err_t reject_info_later(const voip_reject_info_t *info, tirtc_voip_hangup_reason_t reason);
static void abort_connected_media_start(tirtc_conn_t hconn,
                                        uint32_t generation,
                                        const char *reason);
static void collect_status(voip_status_t *status);
static bool status_available_for_ringing(const voip_status_t *status);
static bool status_ready_for_next_call(const voip_status_t *status);
static void log_status(const char *reason, const voip_status_t *status, bool warning);

static void ensure_init(void)
{
    SemaphoreHandle_t created_mutex = NULL;
    bool ready = false;

    portENTER_CRITICAL(&s_init_lock);
    ready = s_mutex != NULL;
    portEXIT_CRITICAL(&s_init_lock);

    if (!ready)
    {
        created_mutex =
            xSemaphoreCreateMutexWithCaps(APP_SYNC_CAPS_CONTROL);
        if (created_mutex == NULL)
        {
            portENTER_CRITICAL(&s_init_lock);
            ready = s_mutex != NULL;
            portEXIT_CRITICAL(&s_init_lock);
            configASSERT(ready);
        }

        if (created_mutex != NULL)
        {
            portENTER_CRITICAL(&s_init_lock);
            if (s_mutex == NULL)
            {
                s_mutex = created_mutex;
                created_mutex = NULL;
                s_session.state = VOIP_STATE_IDLE;
            }
            portEXIT_CRITICAL(&s_init_lock);

            if (created_mutex != NULL)
            {
                vSemaphoreDeleteWithCaps(created_mutex);
            }
        }
    }

    (void)ensure_work_worker();
    (void)start_answer_worker();
}

static void lock_session(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void unlock_session(void)
{
    xSemaphoreGive(s_mutex);
}

static bool lock_session_wait(const char *where, uint32_t timeout_ms)
{
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE)
    {
        return true;
    }

    ESP_LOGW(TAG, "等待会话锁超时: %s", where ? where : "未知位置");
    return false;
}

static void copy_str(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0)
    {
        return;
    }

    if (src == NULL)
    {
        dst[0] = '\0';
        return;
    }

    strlcpy(dst, src, dst_size);
}

static const char *state_name(voip_state_t state)
{
    switch (state)
    {
    case VOIP_STATE_IDLE:
        return "空闲";
    case VOIP_STATE_RINGING:
        return "振铃";
    case VOIP_STATE_CONNECTING:
        return "连接中";
    case VOIP_STATE_AWAITING_CONNECTED:
        return "等待接通";
    case VOIP_STATE_IN_CALL:
        return "通话中";
    case VOIP_STATE_CLOSING:
        return "关闭中";
    default:
        return "未知";
    }
}

static const char *work_type_name(voip_work_type_t type)
{
    switch (type)
    {
    case VOIP_WORK_START_MEDIA:
        return "启动媒体";
    case VOIP_WORK_STOP_MEDIA:
        return "停止媒体";
    case VOIP_WORK_DISCONNECT:
        return "断开";
    case VOIP_WORK_HANGUP:
        return "挂断";
    case VOIP_WORK_REJECT:
        return "拒接";
    default:
        return "未知";
    }
}

static void clear_session_locked(void)
{
    WX_VOIP_TRACEI(TAG, "清空会话: old_state=%s hconn=%p", state_name(s_session.state), s_session.hconn);
    memset(&s_session, 0, sizeof(s_session));
    s_session.state = VOIP_STATE_IDLE;
    s_answer_pending = false;
    s_answer_source[0] = '\0';
    s_answer_delay_ms = 0;
}

static void finish_session_locked(void)
{
    WX_VOIP_TRACEI(TAG, "结束会话: state=%s hconn=%p", state_name(s_session.state), s_session.hconn);
    if (s_session.wx_room_id[0] != '\0')
    {
        copy_str(s_last_close_room_id,
                 sizeof(s_last_close_room_id),
                 s_session.wx_room_id);
        s_last_close_room_us = esp_timer_get_time();
    }
    clear_session_locked();
    s_last_close_us = esp_timer_get_time();
}

static void set_deadline_locked(uint32_t timeout_ms)
{
    s_session.deadline_us = timeout_ms == 0 ? 0 : esp_timer_get_time() + (int64_t)timeout_ms * 1000;
}

static void begin_close_locked(void)
{
    WX_VOIP_TRACEI(TAG, "进入关闭状态: old_state=%s hconn=%p", state_name(s_session.state), s_session.hconn);
    s_session.state = VOIP_STATE_CLOSING;
    s_answer_pending = false;
    s_answer_source[0] = '\0';
    s_answer_delay_ms = 0;
    set_deadline_locked(VOIP_CLOSE_WAIT_TIMEOUT_MS);
}

static bool connection_is_current(tirtc_conn_t hconn, uint32_t generation)
{
    if (hconn == NULL)
    {
        return false;
    }

    lock_session();
    bool current = s_session.hconn == hconn &&
                   s_session.state != VOIP_STATE_IDLE &&
                   (generation == 0U || s_session.generation == generation);
    unlock_session();
    return current;
}

static bool connection_handle_reused(tirtc_conn_t hconn, uint32_t generation)
{
    if (hconn == NULL || generation == 0U)
    {
        return false;
    }

    lock_session();
    bool reused = s_session.state != VOIP_STATE_IDLE &&
                  s_session.hconn == hconn &&
                  s_session.generation != generation;
    unlock_session();
    return reused;
}

static bool generation_is_latest(uint32_t generation)
{
    bool latest = false;

    lock_session();
    latest = generation == 0U || generation == s_session_generation;
    unlock_session();
    return latest;
}

static bool media_start_is_current(tirtc_conn_t hconn, uint32_t generation)
{
    bool current = false;

    lock_session();
    current = s_session.generation == generation &&
              s_session.hconn == hconn &&
              s_session.state == VOIP_STATE_IN_CALL;
    unlock_session();
    return current;
}

static void disconnect_later(tirtc_conn_t hconn,
                             uint32_t generation,
                             bool only_if_current)
{
    if (hconn == NULL)
    {
        return;
    }

    WX_VOIP_TRACEI(TAG,
                   "投递断开任务: hconn=%p gen=%u only_if_current=%d",
                   hconn,
                   (unsigned)generation,
                   only_if_current ? 1 : 0);

    voip_work_item_t item = {
        .type = VOIP_WORK_DISCONNECT,
        .hconn = hconn,
        .generation = generation,
        .only_if_current = only_if_current,
    };
    if (enqueue_work(&item) != ESP_OK)
    {
        if (!only_if_current)
        {
            int rc = tirtc_session_disconnect_connection(hconn);
            ESP_LOGW(TAG,
                     "断开任务投递失败,直接提交 SDK 断开: rc=%d hconn=%p",
                     rc,
                     hconn);
        }
        else
        {
            ESP_LOGW(TAG, "断开任务投递失败,等待下一次维护");
        }
    }
}

static bool voip_cmd_is(uint32_t cmdw, uint32_t expected)
{
    return cmdw == expected ||
           (cmdw & 0xffffU) == expected ||
           (cmdw & 0x7fffU) == expected;
}

static void extract_query_param(const char *url, const char *key, char *out, size_t out_size)
{
    if (url == NULL || key == NULL || out == NULL || out_size == 0)
    {
        return;
    }
    out[0] = '\0';

    char search[64];
    snprintf(search, sizeof(search), "%s=", key);

    const char *p = strstr(url, search);
    if (p == NULL)
    {
        return;
    }
    p += strlen(search);

    size_t i = 0;
    while (*p != '\0' && *p != '&' && i < out_size - 1)
    {
        out[i++] = *p++;
    }
    out[i] = '\0';
}

static const char *json_string_any(cJSON *root, const char *name1, const char *name2)
{
    if (root == NULL || name1 == NULL)
    {
        return NULL;
    }

    const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, name1));
    if ((value == NULL || value[0] == '\0') && name2 != NULL)
    {
        value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, name2));
    }
    return value;
}

static void on_reject_response(const char *body, void *user_data)
{
    (void)user_data;
    if (body != NULL && body[0] != '\0')
    {
        WX_VOIP_TRACEI(TAG, "拒接响应: %.120s", body);
    }
}

static esp_err_t send_reject(const voip_reject_info_t *info, tirtc_voip_hangup_reason_t reason)
{
    if (info == NULL || info->wx_room_id[0] == '\0')
    {
        ESP_LOGW(TAG, "拒接请求缺少房间信息");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
    {
        ESP_LOGE(TAG, "拒接请求内容创建失败");
        return ESP_ERR_NO_MEM;
    }
    /* 平台拒接接口使用 wx_* 字段; wxa_* 作为旧示例服务端兼容字段保留. */
    cJSON_AddStringToObject(root, "wx_app_id", info->wx_app_id);
    cJSON_AddStringToObject(root, "wx_model_id", info->wx_model_id);
    cJSON_AddStringToObject(root, "wx_session_token", info->wx_session_token);
    cJSON_AddStringToObject(root, "wx_room_id", info->wx_room_id);
    cJSON_AddStringToObject(root, "wx_payload", info->wx_payload);
    cJSON_AddStringToObject(root, "wxa_app_id", info->wx_app_id);
    cJSON_AddStringToObject(root, "wxa_model_id", info->wx_model_id);
    cJSON_AddStringToObject(root, "wxa_session_token", info->wx_session_token);
    cJSON_AddStringToObject(root, "wxa_room_id", info->wx_room_id);
    cJSON_AddStringToObject(root, "wxa_payload", info->wx_payload);
    cJSON_AddNumberToObject(root, "hangup_reason", (int)reason);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL)
    {
        ESP_LOGE(TAG, "拒接请求内容序列化失败");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "发送微信拒接: reason=%d", (int)reason);
    WX_VOIP_TRACEI(TAG,
                   "调用 TiRTC service request 拒接: room=%s app_id=%s model_id=%s session_token_len=%u payload_len=%u",
                   info->wx_room_id,
                   info->wx_app_id[0] ? info->wx_app_id : "(空)",
                   info->wx_model_id[0] ? info->wx_model_id : "(空)",
                   (unsigned)strlen(info->wx_session_token),
                   (unsigned)strlen(info->wx_payload));
    int rc = tirtc_session_service_request("/v1/wxvoip/reject", body, NULL, on_reject_response, NULL);
    free(body);
    if (rc != 0)
    {
        ESP_LOGW(TAG, "通知 TiRTC 拒接失败: %d %s", rc, TiRtcGetErrorStr(rc));
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void fill_reject_info(voip_reject_info_t *info, const voip_session_t *sess)
{
    if (info == NULL)
    {
        return;
    }

    memset(info, 0, sizeof(*info));
    if (sess == NULL)
    {
        return;
    }

    copy_str(info->wx_app_id, sizeof(info->wx_app_id), sess->wx_app_id);
    copy_str(info->wx_model_id, sizeof(info->wx_model_id), sess->wx_model_id);
    copy_str(info->wx_session_token, sizeof(info->wx_session_token), sess->wx_session_token);
    copy_str(info->wx_room_id, sizeof(info->wx_room_id), sess->wx_room_id);
    copy_str(info->wx_payload, sizeof(info->wx_payload), sess->wx_payload);
}

static void fill_reject_info_from_join(voip_reject_info_t *info, cJSON *payload)
{
    const char *peer_id = NULL;
    const char *app_id = NULL;
    const char *model_id = NULL;
    const char *session_token = NULL;
    const char *room_id = NULL;
    const char *wx_payload = NULL;
    char app_id_from_peer[sizeof(info->wx_app_id)] = {0};
    char model_id_from_peer[sizeof(info->wx_model_id)] = {0};

    if (info == NULL) {
        return;
    }
    memset(info, 0, sizeof(*info));
    if (!cJSON_IsObject(payload)) {
        return;
    }

    peer_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(payload, "peer_id"));
    app_id = json_string_any(payload, "wx_app_id", "wxa_app_id");
    model_id = json_string_any(payload, "wx_model_id", "wxa_model_id");
    session_token = json_string_any(payload, "wx_session_token", "wxa_session_token");
    room_id = json_string_any(payload, "wx_room_id", "wxa_room_id");
    wx_payload = json_string_any(payload, "wx_payload", "wxa_payload");
    if (session_token == NULL || session_token[0] == '\0') {
        session_token = json_string_any(payload, "wx_server_token", "wxa_server_token");
    }
    if ((app_id == NULL || app_id[0] == '\0') && peer_id != NULL) {
        extract_query_param(peer_id,
                            "x_wx_app_id",
                            app_id_from_peer,
                            sizeof(app_id_from_peer));
        if (app_id_from_peer[0] == '\0') {
            extract_query_param(peer_id,
                                "x_wxa_app_id",
                                app_id_from_peer,
                                sizeof(app_id_from_peer));
        }
        app_id = app_id_from_peer;
    }
    if ((model_id == NULL || model_id[0] == '\0') && peer_id != NULL) {
        extract_query_param(peer_id,
                            "x_wx_model_id",
                            model_id_from_peer,
                            sizeof(model_id_from_peer));
        if (model_id_from_peer[0] == '\0') {
            extract_query_param(peer_id,
                                "x_wxa_model_id",
                                model_id_from_peer,
                                sizeof(model_id_from_peer));
        }
        model_id = model_id_from_peer;
    }

    copy_str(info->wx_app_id, sizeof(info->wx_app_id), app_id);
    copy_str(info->wx_model_id, sizeof(info->wx_model_id), model_id);
    copy_str(info->wx_session_token, sizeof(info->wx_session_token), session_token);
    copy_str(info->wx_room_id, sizeof(info->wx_room_id), room_id);
    copy_str(info->wx_payload, sizeof(info->wx_payload), wx_payload);
}

static esp_err_t reject_join_room(cJSON *payload,
                                  tirtc_voip_hangup_reason_t reason)
{
    voip_reject_info_t reject_info = {0};

    ensure_init();
    fill_reject_info_from_join(&reject_info, payload);
    if (reject_info.wx_room_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    return reject_info_later(&reject_info, reason);
}

esp_err_t wechat_voip_session_reject_join_room_busy(cJSON *payload)
{
    return reject_join_room(payload, TIRTC_VOIP_HANGUP_REASON_BUSY);
}

static esp_err_t send_reject_info(const voip_reject_info_t *info, tirtc_voip_hangup_reason_t reason)
{
    if (info == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    return send_reject(info, reason);
}

static void set_work_busy(bool busy, voip_work_type_t type)
{
    portENTER_CRITICAL(&s_work_state_lock);
    s_work_busy = busy;
    s_work_type = type;
    portEXIT_CRITICAL(&s_work_state_lock);
    WX_VOIP_TRACEI(TAG, "工作队列状态: busy=%d type=%s", busy ? 1 : 0, work_type_name(type));
}

static void voip_work_task(void *arg)
{
    (void)arg;

    /*
     * TiRTC 的挂断、拒接、断开都可能触发网络和 SDK 内部收尾.
     * 统一放到固定 worker 中串行执行,避免按键任务、维护任务和 SDK 回调被拖住.
     */
    while (true)
    {
        voip_work_item_t item;
        if (xQueueReceive(s_work_queue, &item, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        set_work_busy(true, item.type);
        WX_VOIP_TRACEI(TAG,
                       "开始处理工作项: type=%s hconn=%p reason=%d only_if_current=%d",
                       work_type_name(item.type),
                       item.hconn,
                       (int)item.reason,
                       item.only_if_current ? 1 : 0);
        switch (item.type)
        {
        case VOIP_WORK_START_MEDIA:
        {
            bool current = false;
            bool local_video_enabled = false;
            bool remote_video_enabled = false;

            lock_session();
            current = s_session.generation == item.generation &&
                      s_session.hconn == item.hconn &&
                      s_session.state == VOIP_STATE_IN_CALL &&
                      s_session.media_start_pending &&
                      !s_session.media_started;
            if (current) {
                s_session.media_start_pending = false;
                bool video_call =
                    s_session.call_media == WECHAT_VOIP_CALL_MEDIA_VIDEO;
                local_video_enabled =
                    video_call && WECHAT_VOIP_LOCAL_VIDEO_ENABLE;
                remote_video_enabled =
                    video_call && WECHAT_VOIP_REMOTE_VIDEO_ENABLE;
            }
            unlock_session();
            if (!current) {
                WX_VOIP_TRACEI(TAG,
                               "忽略过期媒体启动: gen=%u hconn=%p",
                               (unsigned)item.generation,
                               item.hconn);
                break;
            }

            esp_err_t ret = wechat_voip_media_prepare(
                local_video_enabled,
                remote_video_enabled);
            if (ret == ESP_OK &&
                !media_start_is_current(item.hconn, item.generation)) {
                (void)wechat_voip_media_stop_wait(item.hconn,
                                                  VOIP_MEDIA_STOP_WAIT_MS);
                WX_VOIP_TRACEI(TAG,
                               "媒体准备完成时通话已结束: gen=%u hconn=%p",
                               (unsigned)item.generation,
                               item.hconn);
                break;
            }
            if (ret == ESP_OK) {
                ret = tirtc_session_set_external_media_call_active(
                    item.hconn,
                    true,
                    local_video_enabled,
                    remote_video_enabled);
            }
            if (ret == ESP_OK &&
                !media_start_is_current(item.hconn, item.generation)) {
                (void)tirtc_session_set_external_media_call_active(
                    item.hconn,
                    false,
                    false,
                    false);
                (void)wechat_voip_media_stop_wait(item.hconn,
                                                  VOIP_MEDIA_STOP_WAIT_MS);
                WX_VOIP_TRACEI(TAG,
                               "媒体登记完成时通话已结束: gen=%u hconn=%p",
                               (unsigned)item.generation,
                               item.hconn);
                break;
            }
            if (ret == ESP_OK) {
                ret = wechat_voip_media_start(item.hconn);
            }
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "微信通话媒体启动失败: %s", esp_err_to_name(ret));
                (void)tirtc_session_set_external_media_call_active(
                    item.hconn,
                    false,
                    false,
                    false);
                (void)wechat_voip_media_stop_wait(item.hconn, VOIP_MEDIA_STOP_WAIT_MS);
                abort_connected_media_start(item.hconn,
                                            item.generation,
                                            "media pipeline start failed");
                break;
            }

            lock_session();
            current = s_session.generation == item.generation &&
                      s_session.hconn == item.hconn &&
                      s_session.state == VOIP_STATE_IN_CALL;
            if (current) {
                s_session.media_started = true;
            }
            unlock_session();
            if (!current) {
                (void)wechat_voip_media_stop_wait(item.hconn, VOIP_MEDIA_STOP_WAIT_MS);
                (void)tirtc_session_set_external_media_call_active(
                    item.hconn,
                    false,
                    false,
                    false);
                break;
            }

            ESP_LOGI(TAG,
                     "微信双向媒体已启动: audio=pcma/a-law up_video=%d down_video=%d target=%ux%u",
                     local_video_enabled ? 1 : 0,
                     remote_video_enabled ? 1 : 0,
                     (unsigned)WECHAT_VOIP_VIDEO_WIDTH,
                     (unsigned)WECHAT_VOIP_VIDEO_HEIGHT);
            break;
        }

        case VOIP_WORK_STOP_MEDIA:
            if (generation_is_latest(item.generation)) {
                (void)wechat_voip_media_stop_wait(item.hconn, VOIP_MEDIA_STOP_WAIT_MS);
            } else {
                WX_VOIP_TRACEI(TAG,
                               "忽略过期媒体停止: gen=%u hconn=%p",
                               (unsigned)item.generation,
                               item.hconn);
            }
            /* The TiRTC disconnect callback has already reset connection and
             * AEC state. This work item only drains app-owned media resources. */
            break;

        case VOIP_WORK_HANGUP:
            if (item.hconn != NULL)
            {
                char body[32];
                int n = snprintf(body, sizeof(body), "{\"reason\":%d}", (int)item.reason);
                if (connection_is_current(item.hconn, item.generation))
                {
                    (void)wechat_voip_media_stop_wait(item.hconn, VOIP_MEDIA_STOP_WAIT_MS);
                    (void)tirtc_session_set_external_media_call_active(
                        item.hconn,
                        false,
                        false,
                        false);
                    if (n > 0 && n < (int)sizeof(body))
                    {
                        WX_VOIP_TRACEI(TAG,
                                       "发送 TiRTC 挂断命令: hconn=%p reason=%d",
                                       item.hconn,
                                       (int)item.reason);
                        (void)tirtc_session_send_command_raw(item.hconn, TIRTC_VOIP_HANGUP, body, strlen(body));
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(VOIP_DISCONNECT_DELAY_MS));
                if (connection_is_current(item.hconn, item.generation))
                {
                    WX_VOIP_TRACEI(TAG, "调用 TiRTC disconnect wrapper: hconn=%p", item.hconn);
                    (void)tirtc_session_disconnect_connection(item.hconn);
                }
                else
                {
                    WX_VOIP_TRACEI(TAG,
                                   "忽略过期挂断工作项: gen=%u hconn=%p",
                                   (unsigned)item.generation,
                                   item.hconn);
                }
                ESP_LOGI(TAG, "微信通话挂断流程已提交");
            }
            break;

        case VOIP_WORK_DISCONNECT:
            if (item.hconn != NULL)
            {
                bool current = connection_is_current(item.hconn, item.generation);
                bool safe_to_disconnect =
                    !connection_handle_reused(item.hconn, item.generation);
                if (current || (!item.only_if_current && safe_to_disconnect))
                {
                    (void)wechat_voip_media_stop_wait(item.hconn, VOIP_MEDIA_STOP_WAIT_MS);
                    if (current) {
                        (void)tirtc_session_set_external_media_call_active(
                            item.hconn,
                            false,
                            false,
                            false);
                    }
                    vTaskDelay(pdMS_TO_TICKS(VOIP_DISCONNECT_DELAY_MS));
                    current = connection_is_current(item.hconn, item.generation);
                    safe_to_disconnect =
                        !connection_handle_reused(item.hconn, item.generation);
                    if (current || (!item.only_if_current && safe_to_disconnect))
                    {
                        WX_VOIP_TRACEI(TAG, "释放微信通话连接");
                        WX_VOIP_TRACEI(TAG, "调用 TiRTC disconnect wrapper: hconn=%p", item.hconn);
                        (void)tirtc_session_disconnect_connection(item.hconn);
                    }
                }
                else
                {
                    WX_VOIP_TRACEI(TAG,
                                   "忽略过期断开工作项: gen=%u hconn=%p",
                                   (unsigned)item.generation,
                                   item.hconn);
                }
            }
            break;

        case VOIP_WORK_REJECT:
            if (send_reject_info(item.reject, item.reason) == ESP_OK)
            {
                ESP_LOGI(TAG, "%s", item.reason == TIRTC_VOIP_HANGUP_REASON_REJECT ? "已拒接微信来电" : "已取消微信通话");
            }
            free(item.reject);
            break;
        }
        WX_VOIP_TRACEI(TAG, "工作项处理完成: type=%s", work_type_name(item.type));
        set_work_busy(false, item.type);
    }
}

static esp_err_t ensure_work_worker(void)
{
    lock_session();
    if (s_work_queue == NULL)
    {
        s_work_queue = xQueueCreateWithCaps(VOIP_WORK_QUEUE_LEN,
                                            sizeof(voip_work_item_t),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_work_queue == NULL)
        {
            unlock_session();
            ESP_LOGE(TAG, "创建微信 VoIP 工作队列失败");
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_work_task != NULL)
    {
        unlock_session();
        return ESP_OK;
    }

    BaseType_t ret = xTaskCreateWithCaps(voip_work_task,
                                         "wx_voip_work",
                                         VOIP_WORK_TASK_STACK,
                                         NULL,
                                         VOIP_WORK_TASK_PRIORITY,
                                         &s_work_task,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret != pdPASS)
    {
        s_work_task = NULL;
        unlock_session();
        ESP_LOGE(TAG, "创建微信 VoIP 工作任务失败");
        return ESP_ERR_NO_MEM;
    }

    unlock_session();
    return ESP_OK;
}

static esp_err_t enqueue_work(const voip_work_item_t *item)
{
    if (item == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ensure_work_worker();
    if (ret != ESP_OK)
    {
        return ret;
    }
    WX_VOIP_TRACEI(TAG,
                   "投递工作项: type=%s hconn=%p reason=%d queue_before=%u",
                   work_type_name(item->type),
                   item->hconn,
                   (int)item->reason,
                   s_work_queue ? (unsigned)uxQueueMessagesWaiting(s_work_queue) : 0);
    return xQueueSend(s_work_queue, item, 0) == pdPASS ? ESP_OK : ESP_ERR_TIMEOUT;
}

static esp_err_t reject_info_later(const voip_reject_info_t *info, tirtc_voip_hangup_reason_t reason)
{
    if (info == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    voip_reject_info_t *copy =
        app_memory_alloc_psram(sizeof(*copy));
    if (copy == NULL)
    {
        ESP_LOGW(TAG, "拒接参数申请失败");
        return ESP_ERR_NO_MEM;
    }
    *copy = *info;

    voip_work_item_t item = {
        .type = VOIP_WORK_REJECT,
        .reason = reason,
        .reject = copy,
    };

    esp_err_t ret = enqueue_work(&item);
    if (ret != ESP_OK)
    {
        free(copy);
        ESP_LOGW(TAG, "拒接任务投递失败: %s", esp_err_to_name(ret));
    }
    return ret;
}

static void on_whip_connect(int error, tirtc_conn_t hconn, void *user_data)
{
    const uint32_t generation = (uint32_t)(uintptr_t)user_data;
    ensure_init();

    WX_VOIP_TRACEI(TAG,
                   "WHIP 回调: error=%d hconn=%p",
                   error,
                   hconn);

    if (error != 0)
    {
        ESP_LOGE(TAG, "通话连接失败: %d %s", error, TiRtcGetErrorStr(error));
        voip_reject_info_t failed = {0};
        lock_session();
        bool current = s_session.generation == generation &&
                       s_session.state == VOIP_STATE_CONNECTING &&
                       !s_session.connection_callback_claimed &&
                       !s_session.connection_tracked;
        if (current) {
            fill_reject_info(&failed, &s_session);
            finish_session_locked();
        }
        unlock_session();
        if (current) {
            (void)reject_info_later(&failed, TIRTC_VOIP_HANGUP_REASON_EXCEPTION);
        }
        return;
    }

    if (hconn == NULL)
    {
        ESP_LOGE(TAG, "通话连接失败: WHIP 返回空连接");
        voip_reject_info_t failed = {0};
        lock_session();
        bool current = s_session.generation == generation &&
                       s_session.state == VOIP_STATE_CONNECTING &&
                       !s_session.connection_callback_claimed &&
                       !s_session.connection_tracked;
        if (current) {
            fill_reject_info(&failed, &s_session);
            finish_session_locked();
        }
        unlock_session();
        if (current) {
            (void)reject_info_later(&failed, TIRTC_VOIP_HANGUP_REASON_EXCEPTION);
        }
        return;
    }

    bool should_accept = false;
    bool duplicate_callback = false;
    lock_session();
    if (s_session.generation == generation)
    {
        duplicate_callback =
            s_session.connection_tracked &&
            s_session.hconn == hconn &&
            (s_session.state == VOIP_STATE_AWAITING_CONNECTED ||
             s_session.state == VOIP_STATE_IN_CALL ||
             s_session.state == VOIP_STATE_CLOSING);
        if (!duplicate_callback &&
            s_session.state == VOIP_STATE_CONNECTING &&
            !s_session.connection_tracked)
        {
            if (s_session.connection_callback_claimed)
            {
                duplicate_callback =
                    s_session.connection_callback_hconn == hconn;
            }
            else if (s_session.hconn == NULL || s_session.hconn == hconn)
            {
                s_session.connection_callback_claimed = true;
                s_session.connection_callback_hconn = hconn;
                should_accept = true;
            }
        }
    }
    unlock_session();

    if (duplicate_callback)
    {
        ESP_LOGD(TAG,
                 "忽略重复连接成功回调: gen=%u hconn=%p",
                 (unsigned)generation,
                 hconn);
        return;
    }

    if (!should_accept)
    {
        ESP_LOGW(TAG, "连接回调到达时会话已结束,释放连接");
        disconnect_later(hconn, generation, false);
        return;
    }

    esp_err_t track_ret = tirtc_session_track_external_connection(hconn, false);
    if (track_ret != ESP_OK)
    {
        ESP_LOGE(TAG, "登记 WHIP 连接失败: %s", esp_err_to_name(track_ret));
        (void)TiRtcDisconnect(hconn);
        voip_reject_info_t failed = {0};
        lock_session();
        bool current = s_session.generation == generation &&
                       s_session.state == VOIP_STATE_CONNECTING &&
                       s_session.connection_callback_claimed &&
                       s_session.connection_callback_hconn == hconn;
        if (current) {
            fill_reject_info(&failed, &s_session);
            finish_session_locked();
        }
        unlock_session();
        if (current) {
            (void)reject_info_later(&failed, TIRTC_VOIP_HANGUP_REASON_EXCEPTION);
        }
        return;
    }

    lock_session();
    if (s_session.state == VOIP_STATE_CONNECTING &&
        s_session.generation == generation &&
        s_session.connection_callback_claimed &&
        s_session.connection_callback_hconn == hconn)
    {
        bool outbound_call = s_session.outbound_call;
        bool cancel_on_connect = s_session.cancel_on_connect;
        bool connected_received = s_session.connected_command_received;
        WX_VOIP_TRACEI(TAG,
                       "WHIP 连接成功: old_state=%s hconn=%p outbound=%d cancel=%d connected=%d",
                       state_name(s_session.state),
                       hconn,
                       outbound_call ? 1 : 0,
                       cancel_on_connect ? 1 : 0,
                       connected_received ? 1 : 0);
        s_session.hconn = hconn;
        s_session.connection_callback_claimed = false;
        s_session.connection_callback_hconn = NULL;
        s_session.connection_tracked = true;
        s_session.state = cancel_on_connect ? VOIP_STATE_CLOSING :
                          (outbound_call || connected_received) ? VOIP_STATE_IN_CALL :
                                          VOIP_STATE_AWAITING_CONNECTED;
        s_session.media_start_pending =
            (outbound_call || connected_received) && !cancel_on_connect;
        set_deadline_locked(cancel_on_connect ? VOIP_CLOSE_WAIT_TIMEOUT_MS :
                            (outbound_call || connected_received) ? 0 :
                                            VOIP_CONNECTED_WAIT_TIMEOUT_MS);
        unlock_session();

        if (cancel_on_connect) {
            voip_work_item_t item = {
                .type = VOIP_WORK_HANGUP,
                .hconn = hconn,
                .generation = generation,
                .reason = TIRTC_VOIP_HANGUP_REASON_MANUAL,
            };
            ESP_LOGI(TAG, "已取消的主动呼叫完成入会,发送 0x2001 后断开");
            if (enqueue_work(&item) != ESP_OK) {
                disconnect_later(hconn, generation, true);
            }
        } else if (outbound_call || connected_received) {
            voip_work_item_t item = {
                .type = VOIP_WORK_START_MEDIA,
                .hconn = hconn,
                .generation = generation,
            };
            ESP_LOGI(TAG,
                     "%s,立即启动双向媒体",
                     outbound_call ? "主动呼叫已入会" :
                                     "被叫接通确认先到,WHIP 入会已完成");
            if (enqueue_work(&item) != ESP_OK) {
                ESP_LOGE(TAG, "微信通话媒体启动任务投递失败");
                abort_connected_media_start(hconn,
                                            generation,
                                            "media work queue full");
            }
        } else {
            ESP_LOGI(TAG, "被叫通话连接已建立,等待 0x2000 接通确认");
        }
        return;
    }
    if (s_session.connection_tracked &&
        (s_session.state == VOIP_STATE_AWAITING_CONNECTED ||
         s_session.state == VOIP_STATE_IN_CALL) &&
        s_session.hconn == hconn &&
        s_session.generation == generation)
    {
        voip_state_t state = s_session.state;
        unlock_session();
        ESP_LOGD(TAG, "连接回调重复到达,当前状态=%s", state_name(state));
        return;
    }
    unlock_session();

    ESP_LOGW(TAG, "连接回调到达时会话已结束,释放连接");
    disconnect_later(hconn, generation, false);
}

static esp_err_t answer_current_call(const char *source)
{
    tirtc_session_stats_t rtc_stats = {0};
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (internal_free < VOIP_WHIP_MIN_INTERNAL_FREE ||
        internal_largest < VOIP_WHIP_MIN_INTERNAL_LARGEST)
    {
        ESP_LOGW(TAG,
                 "微信接听等待内存恢复: internal_free=%u internal_largest=%u need=%u/%u",
                 (unsigned)internal_free,
                 (unsigned)internal_largest,
                 (unsigned)VOIP_WHIP_MIN_INTERNAL_FREE,
                 (unsigned)VOIP_WHIP_MIN_INTERNAL_LARGEST);
        return ESP_ERR_NO_MEM;
    }
    if (internal_largest < VOIP_WHIP_WARN_INTERNAL_LARGEST)
    {
        ESP_LOGW(TAG,
                 "微信接听内部最大块偏低,继续尝试: internal_free=%u internal_largest=%u warn=%u",
                 (unsigned)internal_free,
                 (unsigned)internal_largest,
                 (unsigned)VOIP_WHIP_WARN_INTERNAL_LARGEST);
    }

    tirtc_session_get_stats(&rtc_stats);
    if (!rtc_stats.sdk_started || rtc_stats.state == TIRTC_SESSION_STATE_STARTING ||
        rtc_stats.state == TIRTC_SESSION_STATE_DISCONNECTING)
    {
        ESP_LOGW(TAG,
                 "微信接听等待 RTC 就绪: sdk_init=%d sdk_start=%d state=%d",
                 rtc_stats.sdk_initialized ? 1 : 0,
                 rtc_stats.sdk_started ? 1 : 0,
                 (int)rtc_stats.state);
        return ESP_ERR_INVALID_STATE;
    }
    if (rtc_stats.active_connection || rtc_stats.call_active ||
        rtc_stats.state == TIRTC_SESSION_STATE_CONNECTED ||
        rtc_stats.state == TIRTC_SESSION_STATE_MEDIA_BOOTSTRAPPING)
    {
        ESP_LOGW(TAG,
                 "微信接听等待 RTC 资源释放: state=%u active=%d call=%d",
                 (unsigned)rtc_stats.state,
                 rtc_stats.active_connection ? 1 : 0,
                 rtc_stats.call_active ? 1 : 0);
        return ESP_ERR_INVALID_STATE;
    }

    lock_session();
    if (s_session.state != VOIP_STATE_RINGING)
    {
        unlock_session();
        return ESP_ERR_INVALID_STATE;
    }

    s_session.state = VOIP_STATE_CONNECTING;
    set_deadline_locked(VOIP_CONNECT_TIMEOUT_MS);
    voip_session_t snapshot = s_session;
    unlock_session();

    ESP_LOGI(TAG,
             "%s,正在建立通话: room=%s peer_id_len=%u token_len=%u",
             source ? source : "(空)",
             snapshot.wx_room_id[0] ? snapshot.wx_room_id : "(空)",
             (unsigned)strlen(snapshot.peer_id),
             (unsigned)strlen(snapshot.token));
    WX_VOIP_TRACEI(TAG,
                   "准备调用 TiRTC WHIP external: source=%s room=%s peer_id_len=%u token_len=%u",
                   source ? source : "(空)",
                   snapshot.wx_room_id[0] ? snapshot.wx_room_id : "(空)",
                   (unsigned)strlen(snapshot.peer_id),
                   (unsigned)strlen(snapshot.token));
    int64_t start_us = esp_timer_get_time();
    WX_VOIP_TRACEI(TAG,
                   "WHIP submit begin external: service_desc_len=%u token_len=%u",
                   (unsigned)strlen(snapshot.peer_id),
                   (unsigned)strlen(snapshot.token));
    int rc = tirtc_session_whip_connect_external(snapshot.peer_id,
                                                  snapshot.token,
                                                  on_whip_connect,
                                                  (void *)(uintptr_t)snapshot.generation);
    int64_t cost_ms = (esp_timer_get_time() - start_us) / 1000;
    WX_VOIP_TRACEI(TAG, "TiRTC WHIP external 返回: rc=%d cost=%lldms", rc, (long long)cost_ms);
    if (rc == 0)
    {
        return ESP_OK;
    }

    ESP_LOGE(TAG, "提交通话连接失败: %d %s", rc, TiRtcGetErrorStr(rc));
    voip_reject_info_t failed = {0};
    lock_session();
    bool current = s_session.generation == snapshot.generation &&
                   s_session.state == VOIP_STATE_CONNECTING &&
                   !s_session.connection_tracked;
    if (current) {
        fill_reject_info(&failed, &s_session);
        finish_session_locked();
    }
    unlock_session();
    if (current) {
        (void)reject_info_later(&failed, TIRTC_VOIP_HANGUP_REASON_EXCEPTION);
    }
    return ESP_FAIL;
}

/* 常驻接听 worker 的大栈只在 PSRAM 分配一次。每轮请求用序号隔离，
 * 避免频繁创建任务造成碎片，也避免旧请求清理新一轮状态。 */
static void answer_task(void *arg)
{
    (void)arg;
    while (true)
    {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        char source[sizeof(s_answer_source)] = {0};
        uint32_t delay_ms = 0;
        uint32_t request_seq = 0;

        lock_session();
        bool has_request = s_answer_pending && s_session.state == VOIP_STATE_RINGING;
        if (has_request)
        {
            copy_str(source,
                     sizeof(source),
                     s_answer_source[0] ? s_answer_source : "界面接听");
            delay_ms = s_answer_delay_ms;
            request_seq = s_answer_request_seq;
        }
        unlock_session();

        if (!has_request)
        {
            continue;
        }

        /* 主呼收到入会参数后立即 WHIP，成功后直接开媒体；被叫仍由
         * 0x2000 确认接通，两种角色都由 0x2001 收口。 */
        if (delay_ms > 0)
        {
            WX_VOIP_TRACEI(TAG,
                           "接听任务等待房间稳定: source=%s delay=%ums",
                           source,
                           (unsigned)delay_ms);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }

        lock_session();
        bool still_pending = s_answer_pending &&
                             s_session.state == VOIP_STATE_RINGING &&
                             s_answer_request_seq == request_seq;
        unlock_session();
        if (!still_pending)
        {
            WX_VOIP_TRACEI(TAG, "接听任务取消: source=%s seq=%u", source, (unsigned)request_seq);
            continue;
        }

        WX_VOIP_TRACEI(TAG,
                       "接听任务开始: source=%s seq=%u",
                       source,
                       (unsigned)request_seq);
        esp_err_t ret = answer_current_call(source);

        bool retry_pending = false;
        bool transient_wait = ret == ESP_ERR_INVALID_STATE || ret == ESP_ERR_NO_MEM;
        lock_session();
        if (s_answer_request_seq == request_seq)
        {
            if (transient_wait &&
                s_answer_pending &&
                s_session.state == VOIP_STATE_RINGING)
            {
                retry_pending = true;
            }
            else
            {
                s_answer_pending = false;
                s_answer_source[0] = '\0';
                s_answer_delay_ms = 0;
            }
        }
        unlock_session();

        if (ret == ESP_ERR_INVALID_STATE)
        {
            ESP_LOGW(TAG, "当前暂不能接听,等待来电或 RTC 就绪");
        }
        else if (ret == ESP_ERR_NO_MEM)
        {
            ESP_LOGW(TAG, "当前内存不足,保留来电状态,请稍后再次接听");
        }
        if (retry_pending)
        {
            vTaskDelay(pdMS_TO_TICKS(VOIP_ANSWER_RETRY_DELAY_MS));
            xTaskNotifyGive(xTaskGetCurrentTaskHandle());
        }

        WX_VOIP_TRACEI(TAG,
                       "接听任务结束: seq=%u ret=%s stack_hwm=%u",
                       (unsigned)request_seq,
                       esp_err_to_name(ret),
                       (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
}

static esp_err_t start_answer_worker(void)
{
    if (!lock_session_wait("创建接听 worker", 1000))
    {
        return ESP_ERR_TIMEOUT;
    }
    if (s_answer_worker_task != NULL)
    {
        unlock_session();
        return ESP_OK;
    }

    TaskHandle_t worker = NULL;
    BaseType_t ret = xTaskCreateWithCaps(answer_task,
                                        "wx_voip_answer",
                                        VOIP_ANSWER_TASK_STACK,
                                        NULL,
                                        VOIP_ANSWER_TASK_PRIORITY,
                                        &worker,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret != pdPASS)
    {
        unlock_session();
        ESP_LOGE(TAG,
                 "创建接听任务失败: stack=%u internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u",
                 (unsigned)VOIP_ANSWER_TASK_STACK,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        return ESP_ERR_NO_MEM;
    }

    s_answer_worker_task = worker;
    unlock_session();
    ESP_LOGI(TAG,
             "微信接听 worker 已就绪: task=%p stack=%u caps=PSRAM",
             worker,
             (unsigned)VOIP_ANSWER_TASK_STACK);
    return ESP_OK;
}

static esp_err_t start_answer_task(const char *source, uint32_t delay_ms)
{
    if (!lock_session_wait("启动接听任务", 1000))
    {
        return ESP_ERR_TIMEOUT;
    }
    bool ringing = (s_session.state == VOIP_STATE_RINGING);
    bool pending = s_answer_pending;
    voip_state_t state = s_session.state;
    uint32_t request_seq = s_answer_request_seq;
    if (ringing && !pending)
    {
        s_answer_pending = true;
        copy_str(s_answer_source, sizeof(s_answer_source), source ? source : "界面接听");
        s_answer_delay_ms = delay_ms;
        s_answer_request_seq++;
        if (s_answer_request_seq == 0U)
        {
            s_answer_request_seq = 1U;
        }
        request_seq = s_answer_request_seq;
    }
    unlock_session();
    if (!ringing)
    {
        return ESP_ERR_INVALID_STATE;
    }

    WX_VOIP_TRACEI(TAG,
                   "请求启动接听任务: source=%s state=%s pending=%d delay=%ums",
                   source ? source : "界面接听",
                   state_name(state),
                   pending ? 1 : 0,
                   (unsigned)delay_ms);

    ESP_LOGD(TAG, "准备启动接听任务: %s", source ? source : "界面接听");

    if (pending)
    {
        ESP_LOGW(TAG, "正在接听,请勿重复触发");
        return ESP_OK;
    }

    ESP_LOGD(TAG, "%s请求已收到", source ? source : "界面接听");

    esp_err_t worker_ret = start_answer_worker();
    if (worker_ret != ESP_OK)
    {
        if (lock_session_wait("回滚接听任务状态", 1000))
        {
            if (s_answer_request_seq == request_seq)
            {
                s_answer_pending = false;
                s_answer_source[0] = '\0';
                s_answer_delay_ms = 0;
            }
            unlock_session();
        }
        return worker_ret;
    }

    lock_session();
    TaskHandle_t worker = s_answer_worker_task;
    unlock_session();
    if (worker == NULL)
    {
        if (lock_session_wait("回滚接听任务状态", 1000))
        {
            if (s_answer_request_seq == request_seq)
            {
                s_answer_pending = false;
                s_answer_source[0] = '\0';
                s_answer_delay_ms = 0;
            }
            unlock_session();
        }
        return ESP_ERR_INVALID_STATE;
    }

    xTaskNotifyGive(worker);
    ESP_LOGD(TAG, "接听 worker 已唤醒: seq=%u", (unsigned)request_seq);
    return ESP_OK;
}

esp_err_t wechat_voip_session_handle_join_room(cJSON *root,
                                               bool auto_answer,
                                               bool cancel_on_connect,
                                               wechat_voip_call_media_t call_media)
{
    ensure_init();

    if (!cJSON_IsObject(root) ||
        (call_media != WECHAT_VOIP_CALL_MEDIA_AUDIO &&
         call_media != WECHAT_VOIP_CALL_MEDIA_VIDEO))
    {
        ESP_LOGE(TAG, "入会消息或媒体类型无效");
        return ESP_ERR_INVALID_ARG;
    }

    WX_VOIP_TRACEI(TAG,
                   "开始处理入会消息: auto=%d cancel=%d media=%s",
                   auto_answer ? 1 : 0,
                   cancel_on_connect ? 1 : 0,
                   call_media == WECHAT_VOIP_CALL_MEDIA_VIDEO ? "video" : "audio");
    WX_VOIP_TRACEI(TAG, "入会消息已解析");

    const char *peer_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "peer_id"));
    const char *token = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "token"));
    const char *app_id = json_string_any(root, "wx_app_id", "wxa_app_id");
    const char *model_id = json_string_any(root, "wx_model_id", "wxa_model_id");
    const char *session_token = json_string_any(root, "wx_session_token", "wxa_session_token");
    const char *room_id = json_string_any(root, "wx_room_id", "wxa_room_id");
    const char *wx_payload = json_string_any(root, "wx_payload", "wxa_payload");

    if (session_token == NULL || session_token[0] == '\0')
    {
        session_token = json_string_any(root, "wx_server_token", "wxa_server_token");
    }

    char app_id_from_peer[64] = {0};
    char model_id_from_peer[64] = {0};
    if ((app_id == NULL || app_id[0] == '\0') && peer_id != NULL)
    {
        extract_query_param(peer_id, "x_wx_app_id", app_id_from_peer, sizeof(app_id_from_peer));
        if (app_id_from_peer[0] == '\0')
        {
            extract_query_param(peer_id, "x_wxa_app_id", app_id_from_peer, sizeof(app_id_from_peer));
        }
        app_id = app_id_from_peer;
    }
    if ((model_id == NULL || model_id[0] == '\0') && peer_id != NULL)
    {
        extract_query_param(peer_id, "x_wx_model_id", model_id_from_peer, sizeof(model_id_from_peer));
        if (model_id_from_peer[0] == '\0')
        {
            extract_query_param(peer_id, "x_wxa_model_id", model_id_from_peer, sizeof(model_id_from_peer));
        }
        model_id = model_id_from_peer;
    }

    if (peer_id == NULL || peer_id[0] == '\0' || token == NULL || token[0] == '\0')
    {
        ESP_LOGE(TAG, "来电消息缺少连接信息");
        (void)reject_join_room(root, TIRTC_VOIP_HANGUP_REASON_EXCEPTION);
        return ESP_ERR_INVALID_ARG;
    }
    size_t peer_id_len = strlen(peer_id);
    size_t token_len = strlen(token);
    size_t session_token_len = session_token != NULL ? strlen(session_token) : 0U;
    bool peer_id_truncated = peer_id_len >= sizeof(s_session.peer_id);
    bool token_truncated = token_len >= sizeof(s_session.token);
    bool session_token_truncated =
        session_token_len >= sizeof(s_session.wx_session_token);

    WX_VOIP_TRACEI(TAG,
                   "入会连接信息: peer_id_len=%u token_len=%u room_id=%s app_id=%s model_id=%s session_token_len=%u",
                   (unsigned)peer_id_len,
                   (unsigned)token_len,
                   room_id && room_id[0] ? room_id : "(空)",
                   app_id && app_id[0] ? app_id : "(空)",
                   model_id && model_id[0] ? model_id : "(空)",
                   (unsigned)session_token_len);
    ESP_LOGI(TAG,
             "微信入会参数: room=%s peer_id_len=%u token_len=%u app_id=%s model_id=%s truncated=%d/%d/%d",
             room_id && room_id[0] ? room_id : "(空)",
             (unsigned)peer_id_len,
             (unsigned)token_len,
             app_id && app_id[0] ? app_id : "(空)",
             model_id && model_id[0] ? model_id : "(空)",
             peer_id_truncated ? 1 : 0,
             token_truncated ? 1 : 0,
             session_token_truncated ? 1 : 0);
    if (peer_id_truncated || token_truncated || session_token_truncated)
    {
        ESP_LOGE(TAG,
                 "微信入会连接信息过长,已拒绝保存: peer_id_len=%u/%u token_len=%u/%u session_token_len=%u/%u",
                 (unsigned)peer_id_len,
                 (unsigned)sizeof(s_session.peer_id),
                 (unsigned)token_len,
                 (unsigned)sizeof(s_session.token),
                 (unsigned)session_token_len,
                 (unsigned)sizeof(s_session.wx_session_token));
        (void)reject_join_room(root, TIRTC_VOIP_HANGUP_REASON_EXCEPTION);
        return ESP_ERR_INVALID_SIZE;
    }
    WX_VOIP_TRACEI(TAG, "准备保存入会信息");

    voip_status_t status;
    collect_status(&status);
    if (!status_available_for_ringing(&status))
    {
        log_status("微信入会前检查", &status, true);
        esp_err_t reject_ret =
            wechat_voip_session_reject_join_room_busy(root);
        return reject_ret == ESP_OK ? ESP_ERR_INVALID_STATE : reject_ret;
    }

    if (!lock_session_wait("保存入会信息", 1000))
    {
        (void)wechat_voip_session_reject_join_room_busy(root);
        return ESP_ERR_TIMEOUT;
    }
    if (s_session.state != VOIP_STATE_IDLE)
    {
        voip_state_t state = s_session.state;
        voip_reject_info_t busy = {0};
        copy_str(busy.wx_app_id, sizeof(busy.wx_app_id), app_id);
        copy_str(busy.wx_model_id, sizeof(busy.wx_model_id), model_id);
        copy_str(busy.wx_session_token, sizeof(busy.wx_session_token), session_token);
        copy_str(busy.wx_room_id, sizeof(busy.wx_room_id), room_id);
        copy_str(busy.wx_payload, sizeof(busy.wx_payload), wx_payload);
        unlock_session();

        ESP_LOGW(TAG, "当前状态=%s,拒接新的微信来电", state_name(state));
        (void)reject_info_later(&busy, TIRTC_VOIP_HANGUP_REASON_BUSY);
        return ESP_ERR_INVALID_STATE;
    }

    clear_session_locked();
    copy_str(s_session.peer_id, sizeof(s_session.peer_id), peer_id);
    copy_str(s_session.token, sizeof(s_session.token), token);
    copy_str(s_session.wx_app_id, sizeof(s_session.wx_app_id), app_id);
    copy_str(s_session.wx_model_id, sizeof(s_session.wx_model_id), model_id);
    copy_str(s_session.wx_session_token, sizeof(s_session.wx_session_token), session_token);
    copy_str(s_session.wx_room_id, sizeof(s_session.wx_room_id), room_id);
    copy_str(s_session.wx_payload, sizeof(s_session.wx_payload), wx_payload);
    uint32_t generation = ++s_session_generation;
    s_session.generation = generation;
    s_session.outbound_call = auto_answer;
    s_session.cancel_on_connect = cancel_on_connect;
    s_session.call_media = call_media;
    s_session.state = VOIP_STATE_RINGING;
    set_deadline_locked(VOIP_RING_TIMEOUT_MS);
    unlock_session();

    WX_VOIP_TRACEI(TAG,
                   "入会信息已保存: auto=%d cancel=%d media=%s",
                   auto_answer ? 1 : 0,
                   cancel_on_connect ? 1 : 0,
                   call_media == WECHAT_VOIP_CALL_MEDIA_VIDEO ? "video" : "audio");

    if (auto_answer)
    {
        WX_VOIP_TRACEI(TAG, "主动呼叫入会参数已收到,准备 WHIP 建连");
        esp_err_t answer_ret =
            start_answer_task("主动呼叫入会", VOIP_ACTIVE_ANSWER_DELAY_MS);
        if (answer_ret == ESP_OK)
        {
            return ESP_OK;
        }

        voip_reject_info_t failed = {0};
        lock_session();
        bool current = s_session.generation == generation &&
                       s_session.state == VOIP_STATE_RINGING;
        if (current)
        {
            fill_reject_info(&failed, &s_session);
            finish_session_locked();
        }
        unlock_session();
        if (current)
        {
            (void)reject_info_later(&failed,
                                    TIRTC_VOIP_HANGUP_REASON_EXCEPTION);
        }
        return answer_ret;
    }

    ESP_LOGI(TAG, "收到微信来电,等待界面接听");
    return ESP_OK;
}

esp_err_t wechat_voip_session_answer(void)
{
    ensure_init();
    return start_answer_task("界面接听", VOIP_UI_ANSWER_DELAY_MS);
}

bool wechat_voip_session_has_incoming_call(void)
{
    bool pending = false;

    ensure_init();
    lock_session();
    pending = (s_session.state == VOIP_STATE_RINGING && !s_answer_pending);
    unlock_session();
    return pending;
}

wechat_voip_session_state_t wechat_voip_session_get_state(void)
{
    wechat_voip_session_state_t state = WECHAT_VOIP_SESSION_STATE_IDLE;

    ensure_init();
    lock_session();
    switch (s_session.state)
    {
    case VOIP_STATE_RINGING:
        state = s_answer_pending ? WECHAT_VOIP_SESSION_STATE_CONNECTING :
                                   WECHAT_VOIP_SESSION_STATE_RINGING;
        break;
    case VOIP_STATE_CONNECTING:
        state = WECHAT_VOIP_SESSION_STATE_CONNECTING;
        break;
    case VOIP_STATE_AWAITING_CONNECTED:
        state = WECHAT_VOIP_SESSION_STATE_AWAITING_CONNECTED;
        break;
    case VOIP_STATE_IN_CALL:
        state = WECHAT_VOIP_SESSION_STATE_IN_CALL;
        break;
    case VOIP_STATE_CLOSING:
        state = WECHAT_VOIP_SESSION_STATE_CLOSING;
        break;
    case VOIP_STATE_IDLE:
    default:
        state = WECHAT_VOIP_SESSION_STATE_IDLE;
        break;
    }
    unlock_session();

    return state;
}

esp_err_t wechat_voip_session_reject_incoming(void)
{
    ensure_init();

    voip_state_t state = VOIP_STATE_IDLE;
    char room_id[sizeof(s_session.wx_room_id)] = {0};
    voip_reject_info_t reject_info = {0};

    lock_session();
    if (s_session.state != VOIP_STATE_RINGING)
    {
        unlock_session();
        return ESP_ERR_INVALID_STATE;
    }

    state = s_session.state;
    copy_str(room_id, sizeof(room_id), s_session.wx_room_id);
    fill_reject_info(&reject_info, &s_session);
    WX_VOIP_TRACEI(TAG,
                   "界面拒接: room=%s state=%s",
                   room_id[0] ? room_id : "(空)",
                   state_name(state));
    finish_session_locked();
    unlock_session();

    esp_err_t ret = reject_info_later(&reject_info, TIRTC_VOIP_HANGUP_REASON_REJECT);
    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "已拒接微信来电");
    }
    return ret;
}

bool wechat_voip_session_is_idle(void)
{
    ensure_init();

    lock_session();
    bool idle = (s_session.state == VOIP_STATE_IDLE);
    unlock_session();
    return idle;
}

bool wechat_voip_session_is_closing(void)
{
    ensure_init();

    lock_session();
    bool closing = (s_session.state == VOIP_STATE_CLOSING);
    unlock_session();
    return closing;
}

static void collect_status(voip_status_t *status)
{
    tirtc_session_stats_t rtc = {0};

    if (status == NULL)
    {
        return;
    }

    memset(status, 0, sizeof(*status));
    int64_t now_us = esp_timer_get_time();

    lock_session();
    status->state = s_session.state;
    status->hconn = s_session.hconn;
    status->answer_pending = s_answer_pending;
    status->answer_worker = s_answer_worker_task;
    status->deadline_left_ms = s_session.deadline_us > 0 ? (s_session.deadline_us - now_us) / 1000 : 0;
    status->last_close_ago_ms = s_last_close_us > 0 ? (now_us - s_last_close_us) / 1000 : -1;
    unlock_session();

    portENTER_CRITICAL(&s_work_state_lock);
    status->work_busy = s_work_busy;
    status->work_type = s_work_type;
    portEXIT_CRITICAL(&s_work_state_lock);

    status->work_queue_len = s_work_queue ? (uint32_t)uxQueueMessagesWaiting(s_work_queue) : 0;
    wechat_voip_media_get_stats(&status->media);
    status->media_running = status->media.running;
    tirtc_session_get_stats(&rtc);
    status->rtc_active_connection = rtc.active_connection;
    status->rtc_call_active = rtc.call_active;
    status->rtc_state = rtc.state;
}

static bool status_ready_for_next_call(const voip_status_t *status)
{
    /* answer_worker is permanent infrastructure. Only answer_pending belongs
     * to a call generation and participates in the release barrier. */
    return status != NULL &&
           status->state == VOIP_STATE_IDLE &&
           status->hconn == NULL &&
           !status->answer_pending &&
           !status->work_busy &&
           status->work_queue_len == 0 &&
           !status->media_running &&
           !status->rtc_active_connection &&
           !status->rtc_call_active &&
           status->rtc_state != TIRTC_SESSION_STATE_STARTING &&
           status->rtc_state != TIRTC_SESSION_STATE_CONNECTED &&
           status->rtc_state != TIRTC_SESSION_STATE_MEDIA_BOOTSTRAPPING &&
           status->rtc_state != TIRTC_SESSION_STATE_DISCONNECTING &&
           (status->last_close_ago_ms < 0 || status->last_close_ago_ms >= VOIP_RECALL_GUARD_MS);
}

static bool status_available_for_ringing(const voip_status_t *status)
{
    /*
     * Ringing only reserves signalling state. RTC may still be starting; the
     * answer worker waits for it instead of rejecting a valid incoming call.
     */
    return status != NULL &&
           status->state == VOIP_STATE_IDLE &&
           status->hconn == NULL &&
           !status->answer_pending &&
           !status->work_busy &&
           status->work_queue_len == 0 &&
           !status->media_running &&
           !status->rtc_active_connection &&
           !status->rtc_call_active &&
           status->rtc_state != TIRTC_SESSION_STATE_CONNECTED &&
           status->rtc_state != TIRTC_SESSION_STATE_MEDIA_BOOTSTRAPPING &&
           status->rtc_state != TIRTC_SESSION_STATE_DISCONNECTING &&
           (status->last_close_ago_ms < 0 ||
            status->last_close_ago_ms >= VOIP_RECALL_GUARD_MS);
}

static bool status_resources_released(const voip_status_t *status)
{
    return status != NULL &&
           status->state == VOIP_STATE_IDLE &&
           status->hconn == NULL &&
           !status->answer_pending &&
           !status->work_busy &&
           status->work_queue_len == 0 &&
           !status->media_running &&
           !status->rtc_active_connection &&
           !status->rtc_call_active &&
           status->rtc_state != TIRTC_SESSION_STATE_CONNECTED &&
           status->rtc_state != TIRTC_SESSION_STATE_MEDIA_BOOTSTRAPPING &&
           status->rtc_state != TIRTC_SESSION_STATE_DISCONNECTING;
}

static bool status_local_resources_released(const voip_status_t *status)
{
    return status != NULL &&
           status->state == VOIP_STATE_IDLE &&
           status->hconn == NULL &&
           !status->answer_pending &&
           !status->work_busy &&
           status->work_queue_len == 0 &&
           !status->media_running;
}

static void log_status(const char *reason, const voip_status_t *status, bool warning)
{
    if (status == NULL)
    {
        return;
    }

    if (warning)
    {
        ESP_LOGW(TAG,
                 "通话资源%s: %s state=%s hconn=%p answer_worker=%p answer_pending=%d "
                 "work_busy=%d work=%s workq=%u media=%d media_tx=%llu drop=%lu fail=%lu "
                 "rtc=%u/%d/%d deadline=%lldms last_close=%lldms",
                 status_ready_for_next_call(status) ? "已就绪" : "未就绪",
                 reason ? reason : "状态检查",
                 state_name(status->state),
                 status->hconn,
                 status->answer_worker,
                 status->answer_pending ? 1 : 0,
                 status->work_busy ? 1 : 0,
                 work_type_name(status->work_type),
                 (unsigned)status->work_queue_len,
                 status->media_running ? 1 : 0,
                 (unsigned long long)status->media.tx_frames,
                 (unsigned long)status->media.dropped_frames,
                 (unsigned long)status->media.tx_failures,
                 (unsigned)status->rtc_state,
                 status->rtc_active_connection ? 1 : 0,
                 status->rtc_call_active ? 1 : 0,
                 (long long)status->deadline_left_ms,
                 (long long)status->last_close_ago_ms);
        return;
    }

    WX_VOIP_TRACEI(
        TAG,
        "通话资源%s: %s state=%s hconn=%p answer_worker=%p answer_pending=%d "
        "work_busy=%d work=%s workq=%u media=%d media_tx=%llu drop=%lu fail=%lu "
        "rtc=%u/%d/%d deadline=%lldms last_close=%lldms",
        status_ready_for_next_call(status) ? "已就绪" : "未就绪",
        reason ? reason : "状态检查",
        state_name(status->state),
        status->hconn,
        status->answer_worker,
        status->answer_pending ? 1 : 0,
        status->work_busy ? 1 : 0,
        work_type_name(status->work_type),
        (unsigned)status->work_queue_len,
        status->media_running ? 1 : 0,
        (unsigned long long)status->media.tx_frames,
        (unsigned long)status->media.dropped_frames,
        (unsigned long)status->media.tx_failures,
        (unsigned)status->rtc_state,
        status->rtc_active_connection ? 1 : 0,
        status->rtc_call_active ? 1 : 0,
        (long long)status->deadline_left_ms,
        (long long)status->last_close_ago_ms);
}

bool wechat_voip_session_ready_for_next_call(bool log_detail)
{
    ensure_init();

    voip_status_t status;
    collect_status(&status);
    bool ready = status_ready_for_next_call(&status);
    if (log_detail || !ready)
    {
        log_status("发起呼叫前检查", &status, !ready);
    }
    return ready;
}

bool wechat_voip_session_is_current_room(const char *room_id)
{
    bool current = false;

    if (room_id == NULL || room_id[0] == '\0')
    {
        return false;
    }

    ensure_init();
    lock_session();
    current = s_session.state != VOIP_STATE_IDLE &&
              s_session.wx_room_id[0] != '\0' &&
              strcmp(room_id, s_session.wx_room_id) == 0;
    unlock_session();
    return current;
}

bool wechat_voip_session_is_recent_room(const char *room_id)
{
    bool recent = false;
    int64_t now_us = esp_timer_get_time();

    if (room_id == NULL || room_id[0] == '\0')
    {
        return false;
    }

    ensure_init();
    lock_session();
    recent = s_last_close_room_id[0] != '\0' &&
             strcmp(room_id, s_last_close_room_id) == 0 &&
             s_last_close_room_us > 0 &&
             now_us - s_last_close_room_us <=
                 (int64_t)VOIP_RECENT_ROOM_GUARD_MS * 1000;
    unlock_session();
    return recent;
}

esp_err_t wechat_voip_session_cancel_outbound_on_connect(void)
{
    tirtc_conn_t hconn = NULL;
    uint32_t generation = 0;
    bool hangup_now = false;

    ensure_init();
    lock_session();
    if (s_session.state == VOIP_STATE_IDLE || !s_session.outbound_call)
    {
        unlock_session();
        return ESP_ERR_INVALID_STATE;
    }

    s_session.cancel_on_connect = true;
    s_session.media_start_pending = false;
    hconn = s_session.hconn;
    generation = s_session.generation;
    if (s_session.connection_tracked &&
        hconn != NULL &&
        s_session.state != VOIP_STATE_CLOSING)
    {
        begin_close_locked();
        hangup_now = true;
    }
    unlock_session();

    if (!hangup_now)
    {
        ESP_LOGI(TAG, "主动外呼已标记取消,等待 WHIP 连接后发送 0x2001");
        return ESP_OK;
    }

    wechat_voip_media_stop(hconn);
    voip_work_item_t item = {
        .type = VOIP_WORK_HANGUP,
        .hconn = hconn,
        .generation = generation,
        .reason = TIRTC_VOIP_HANGUP_REASON_MANUAL,
    };
    if (enqueue_work(&item) != ESP_OK)
    {
        disconnect_later(hconn, generation, true);
    }
    return ESP_OK;
}

esp_err_t wechat_voip_session_wait_until_released(uint32_t timeout_ms)
{
    const int64_t deadline_us =
        esp_timer_get_time() + (int64_t)timeout_ms * 1000;

    ensure_init();
    while (true)
    {
        voip_status_t status;
        collect_status(&status);
        if (status_resources_released(&status))
        {
            return ESP_OK;
        }
        if (timeout_ms == 0U || esp_timer_get_time() >= deadline_us)
        {
            log_status("等待微信通话资源释放超时", &status, true);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(VOIP_SHUTDOWN_POLL_MS));
    }
}

void wechat_voip_session_dump_status(const char *reason)
{
    ensure_init();

    voip_status_t status;
    collect_status(&status);
    bool ready = status_ready_for_next_call(&status);
    if (!ready) {
        s_last_status_warn_us = esp_timer_get_time();
    }
    log_status(reason, &status, !ready);
}

void wechat_voip_session_maintenance(void)
{
    ensure_init();

    int64_t now_us = esp_timer_get_time();
    voip_state_t state = VOIP_STATE_IDLE;
    tirtc_conn_t hconn = NULL;
    uint32_t generation = 0;
    int64_t deadline_us = 0;
    voip_work_item_t item = {0};
    voip_reject_info_t reject_info = {0};

    lock_session();
    bool expired = (s_session.state != VOIP_STATE_IDLE &&
                    s_session.deadline_us > 0 &&
                    now_us >= s_session.deadline_us);
    state = s_session.state;
    hconn = s_session.hconn;
    generation = s_session.generation;
    deadline_us = s_session.deadline_us;
    if (expired && hconn == NULL)
    {
        fill_reject_info(&reject_info, &s_session);
    }
    if (expired)
    {
        if (s_session.state == VOIP_STATE_CLOSING)
        {
            set_deadline_locked(VOIP_CLOSE_WAIT_TIMEOUT_MS);
        }
        else if (s_session.hconn == NULL)
        {
            finish_session_locked();
        }
        else
        {
            begin_close_locked();
        }
    }
    unlock_session();

    if (!expired)
    {
        voip_status_t status;
        collect_status(&status);
        /*
         * The shared RTC can legitimately be occupied by device-call, IPC or
         * AI Chat while WeChat is idle. Maintenance should warn only about
         * resources owned by this WeChat session; the full readiness check is
         * still used when a new WeChat call actually starts.
         */
        if (!status_local_resources_released(&status) &&
            status.state == VOIP_STATE_IDLE &&
            now_us - s_last_status_warn_us >= (int64_t)VOIP_STATUS_WARN_INTERVAL_MS * 1000)
        {
            s_last_status_warn_us = now_us;
            log_status("空闲但资源未收干净", &status, true);
        }
        return;
    }

    WX_VOIP_TRACEW(TAG,
                   "会话超时维护: state=%s hconn=%p deadline=%lld now=%lld",
                   state_name(state),
                   hconn,
                   (long long)deadline_us,
                   (long long)now_us);

    if (state == VOIP_STATE_CLOSING)
    {
        ESP_LOGW(TAG, "微信通话关闭等待超时,重试断开");
        wechat_voip_media_stop(hconn);
        disconnect_later(hconn, generation, true);
        return;
    }

    ESP_LOGW(TAG, "微信通话%s超时,准备结束", state_name(state));
    if (hconn != NULL)
    {
        item.type = VOIP_WORK_HANGUP;
        item.hconn = hconn;
        item.generation = generation;
        item.reason = TIRTC_VOIP_HANGUP_REASON_TIMEOUT;
        (void)enqueue_work(&item);
        return;
    }

    (void)reject_info_later(&reject_info,
                            TIRTC_VOIP_HANGUP_REASON_TIMEOUT);
}

static void abort_connected_media_start(tirtc_conn_t hconn,
                                        uint32_t generation,
                                        const char *reason)
{
    if (hconn == NULL)
    {
        return;
    }

    ESP_LOGW(TAG, "微信通话媒体启动失败,释放连接: %s", reason != NULL ? reason : "unknown");
    (void)wechat_voip_media_stop_wait(hconn, VOIP_MEDIA_STOP_WAIT_MS);
    (void)tirtc_session_set_external_media_call_active(hconn,
                                                       false,
                                                       false,
                                                       false);

    lock_session();
    if (s_session.generation == generation &&
        s_session.hconn == hconn &&
        s_session.state != VOIP_STATE_IDLE)
    {
        begin_close_locked();
    }
    unlock_session();

    disconnect_later(hconn, generation, true);
}

bool wechat_voip_session_on_command(tirtc_conn_t hconn, uint32_t cmdw, const void *data, uint32_t len)
{
    ensure_init();

    WX_VOIP_TRACEI(TAG,
                   "会话命令回调: hconn=%p cmdw=0x%08x len=%u",
                   hconn,
                   (unsigned)cmdw,
                   (unsigned)len);

    if (hconn == NULL)
    {
        return false;
    }

    lock_session();
    tirtc_conn_t current = s_session.hconn;
    voip_state_t state = s_session.state;
    if (current == NULL &&
        (state == VOIP_STATE_CONNECTING || state == VOIP_STATE_AWAITING_CONNECTED) &&
        (voip_cmd_is(cmdw, TIRTC_VOIP_CALL_CONNECTED) ||
         voip_cmd_is(cmdw, TIRTC_VOIP_HANGUP)))
    {
        s_session.hconn = hconn;
        current = hconn;
        ESP_LOGD(TAG, "通话命令先于连接回调到达 hconn=%p cmdw=0x%08x",
                 hconn, (unsigned)cmdw);
    }
    uint32_t command_generation = s_session.generation;
    unlock_session();

    if (hconn != current)
    {
        ESP_LOGD(TAG,
                 "忽略非当前通话命令 hconn=%p current=%p state=%s cmdw=0x%08x",
                 hconn,
                 current,
                 state_name(state),
                 (unsigned)cmdw);
        return false;
    }

    if (voip_cmd_is(cmdw, TIRTC_VOIP_CALL_CONNECTED))
    {
        bool already_in_call = false;
        bool should_queue_media = false;
        bool wait_for_whip_callback = false;
        bool cancel_on_connect = false;
        bool cancel_already_queued = false;
        voip_state_t command_state = VOIP_STATE_IDLE;
        uint32_t generation = 0;

        lock_session();
        if (s_session.generation != command_generation ||
            s_session.hconn != hconn ||
            s_session.state == VOIP_STATE_IDLE)
        {
            unlock_session();
            ESP_LOGD(TAG,
                     "忽略过期接通命令: gen=%u hconn=%p",
                     (unsigned)command_generation,
                     hconn);
            return false;
        }
        command_state = s_session.state;
        already_in_call = (s_session.state == VOIP_STATE_IN_CALL);
        generation = command_generation;
        cancel_on_connect = s_session.cancel_on_connect;
        if (s_session.state == VOIP_STATE_CONNECTING ||
            s_session.state == VOIP_STATE_AWAITING_CONNECTED ||
            s_session.state == VOIP_STATE_IN_CALL) {
            s_session.connected_command_received = true;
        }
        if (cancel_on_connect && s_session.state == VOIP_STATE_CLOSING) {
            cancel_already_queued = true;
        } else if (cancel_on_connect && s_session.connection_tracked &&
                   (s_session.state == VOIP_STATE_AWAITING_CONNECTED ||
                    s_session.state == VOIP_STATE_IN_CALL)) {
            begin_close_locked();
        } else if (cancel_on_connect &&
                   s_session.state == VOIP_STATE_CONNECTING) {
            wait_for_whip_callback = true;
        } else if (s_session.state == VOIP_STATE_AWAITING_CONNECTED ||
                   (s_session.state == VOIP_STATE_CONNECTING &&
                    s_session.connection_tracked))
        {
            s_session.state = VOIP_STATE_IN_CALL;
            s_session.media_start_pending = true;
            set_deadline_locked(0);
            should_queue_media = true;
        } else if (s_session.state == VOIP_STATE_CONNECTING) {
            wait_for_whip_callback = true;
        }
        unlock_session();

        if (cancel_on_connect) {
            if (cancel_already_queued) {
                return true;
            }
            if (wait_for_whip_callback) {
                ESP_LOGI(TAG, "取消确认先于 WHIP 回调到达,等待连接登记后发送 0x2001");
                return true;
            }
            voip_work_item_t item = {
                .type = VOIP_WORK_HANGUP,
                .hconn = hconn,
                .generation = generation,
                .reason = TIRTC_VOIP_HANGUP_REASON_MANUAL,
            };
            (void)enqueue_work(&item);
            return true;
        }

        if (wait_for_whip_callback) {
            ESP_LOGI(TAG, "0x2000 先于 WHIP 回调到达,等待连接登记后启动媒体");
            return true;
        }

        if (already_in_call)
        {
            WX_VOIP_TRACEI(TAG,
                           "CALL_CONNECTED 重复到达,媒体状态保持不变: hconn=%p",
                           hconn);
            return true;
        }

        if (!should_queue_media)
        {
            ESP_LOGW(TAG,
                     "忽略非等待状态的 CALL_CONNECTED: state=%s hconn=%p",
                     state_name(command_state),
                     hconn);
            return true;
        }

        voip_work_item_t item = {
            .type = VOIP_WORK_START_MEDIA,
            .hconn = hconn,
            .generation = generation,
        };
        ESP_LOGI(TAG, "微信被叫已接通,提交双向媒体启动");
        if (enqueue_work(&item) != ESP_OK) {
            abort_connected_media_start(hconn,
                                        generation,
                                        "media work queue full");
        }
        return true;
    }

    if (voip_cmd_is(cmdw, TIRTC_VOIP_HANGUP))
    {
        int reason = -1;
        uint32_t hangup_generation = 0;
        if (data != NULL && len > 0)
        {
            char buf[96];
            if (len < sizeof(buf))
            {
                memcpy(buf, data, len);
                buf[len] = '\0';
                cJSON *root = cJSON_Parse(buf);
                if (root != NULL)
                {
                    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "reason");
                    if (cJSON_IsNumber(item))
                    {
                        reason = item->valueint;
                    }
                    cJSON_Delete(root);
                }
            }
            else
            {
                ESP_LOGD(TAG, "挂断命令内容过长,跳过原因解析 len=%u", (unsigned)len);
            }
        }

        ESP_LOGI(TAG, "微信通话已结束,原因=%d,state=%s", reason, state_name(state));
        WX_VOIP_TRACEI(TAG,
                       "收到 HANGUP: hconn=%p reason=%d payload_len=%u",
                       hconn,
                       reason,
                       (unsigned)len);

        lock_session();
        if (s_session.generation != command_generation ||
            s_session.hconn != hconn ||
            s_session.state == VOIP_STATE_IDLE)
        {
            unlock_session();
            ESP_LOGD(TAG,
                     "忽略过期挂断命令: gen=%u hconn=%p",
                     (unsigned)command_generation,
                     hconn);
            return false;
        }
        hangup_generation = command_generation;
        begin_close_locked();
        unlock_session();

        wechat_voip_media_stop(hconn);
        disconnect_later(hconn, hangup_generation, true);
        return true;
    }

    ESP_LOGD(TAG, "通话中收到未处理命令 cmdw=0x%08x", (unsigned)cmdw);
    return true;
}

bool wechat_voip_session_on_conn_error(tirtc_conn_t hconn, int error)
{
    ensure_init();

    WX_VOIP_TRACEI(TAG,
                   "会话连接错误回调: hconn=%p error=%d",
                   hconn,
                   error);

    uint32_t generation = 0;
    lock_session();
    bool mine = (hconn != NULL && hconn == s_session.hconn);
    if (mine)
    {
        generation = s_session.generation;
        finish_session_locked();
    }
    unlock_session();

    if (!mine)
    {
        return false;
    }

    ESP_LOGW(TAG, "微信通话连接错误: %d %s", error, TiRtcGetErrorStr(error));
    wechat_voip_media_stop(hconn);
    /* The TiRTC session worker owns SDK-error teardown. After notifying
     * observers it moves this handle from active to closing, calls
     * TiRtcDisconnect once, and waits for the disconnected callback. Queuing
     * another delayed disconnect here can run after that callback has freed
     * the peer connection, turning the saved handle into a use-after-free. */
    (void)generation;
    return true;
}

bool wechat_voip_session_on_disconnected(tirtc_conn_t hconn)
{
    ensure_init();

    WX_VOIP_TRACEI(TAG, "会话断开回调: hconn=%p", hconn);

    uint32_t generation = 0;
    lock_session();
    bool mine = (hconn != NULL && hconn == s_session.hconn);
    if (mine)
    {
        generation = s_session.generation;
        finish_session_locked();
    }
    unlock_session();

    if (mine)
    {
        wechat_voip_media_stop(hconn);
        voip_work_item_t item = {
            .type = VOIP_WORK_STOP_MEDIA,
            .hconn = hconn,
            .generation = generation,
        };
        if (enqueue_work(&item) != ESP_OK) {
            ESP_LOGW(TAG, "断开后的媒体清理任务投递失败");
        }
        ESP_LOGI(TAG, "微信通话已断开");
        /* Media cleanup is asynchronous after the disconnect callback. Give
         * it one quiet interval before reporting a genuinely stuck resource. */
        s_last_status_warn_us = esp_timer_get_time();
    }

    return mine;
}

bool wechat_voip_session_cancel_by_room(const char *room_id)
{
    ensure_init();

    WX_VOIP_TRACEI(TAG, "微信取消事件: room=%s", room_id && room_id[0] ? room_id : "(空)");

    tirtc_conn_t hconn = NULL;
    uint32_t generation = 0;
    bool already_closing = false;
    char current_room_id[sizeof(s_session.wx_room_id)] = {0};

    lock_session();
    bool active = (s_session.state != VOIP_STATE_IDLE);
    hconn = s_session.hconn;
    generation = s_session.generation;
    copy_str(current_room_id, sizeof(current_room_id), s_session.wx_room_id);
    bool room_match = true;
    if (active && current_room_id[0] != '\0')
    {
        room_match = (room_id != NULL && room_id[0] != '\0' &&
                      strcmp(current_room_id, room_id) == 0);
    }
    if (active && room_match)
    {
        if (s_session.hconn != NULL)
        {
            already_closing = s_session.state == VOIP_STATE_CLOSING;
            if (!already_closing)
            {
                begin_close_locked();
            }
        }
        else
        {
            finish_session_locked();
        }
    }
    unlock_session();

    if (!active)
    {
        return false;
    }
    if (!room_match)
    {
        ESP_LOGD(TAG, "忽略非当前房间取消: room_id=%s current=%s",
                 room_id ? room_id : "(空)",
                 current_room_id);
        return false;
    }

    if (hconn != NULL)
    {
        if (!already_closing)
        {
            wechat_voip_media_stop(hconn);
            disconnect_later(hconn, generation, true);
            ESP_LOGI(TAG, "微信侧已结束当前通话");
        }
        else
        {
            ESP_LOGD(TAG, "微信通话已在关闭,忽略重复取消");
        }
    }
    else
    {
        ESP_LOGI(TAG, "微信侧已取消当前来电");
    }

    return true;
}

void wechat_voip_session_hangup(void)
{
    ensure_init();

    voip_state_t state = VOIP_STATE_IDLE;
    tirtc_conn_t hconn = NULL;
    uint32_t generation = 0;
    voip_work_item_t item = {0};
    voip_reject_info_t reject_info = {0};

    lock_session();
    bool active = (s_session.state != VOIP_STATE_IDLE);
    state = s_session.state;
    hconn = s_session.hconn;
    generation = s_session.generation;
    if (active && hconn == NULL)
    {
        fill_reject_info(&reject_info, &s_session);
    }
    WX_VOIP_TRACEI(TAG,
                   "本地请求挂断: active=%d state=%s hconn=%p",
                   active ? 1 : 0,
                   state_name(state),
                   hconn);
    if (!active)
    {
        unlock_session();
        return;
    }
    if (active && s_session.state == VOIP_STATE_CLOSING)
    {
        unlock_session();
        WX_VOIP_TRACEI(TAG, "微信通话正在关闭,忽略重复挂断");
        return;
    }
    if (s_session.hconn != NULL)
    {
        begin_close_locked();
    }
    else
    {
        finish_session_locked();
    }
    unlock_session();

    if (hconn != NULL)
    {
        item.type = VOIP_WORK_HANGUP;
        item.hconn = hconn;
        item.generation = generation;
        item.reason = TIRTC_VOIP_HANGUP_REASON_MANUAL;
        if (enqueue_work(&item) != ESP_OK)
        {
            ESP_LOGW(TAG, "挂断任务投递失败,等待维护重试");
            (void)wechat_voip_media_stop_wait(hconn, VOIP_MEDIA_STOP_WAIT_MS);
            (void)tirtc_session_set_external_media_call_active(hconn,
                                                               false,
                                                               false,
                                                               false);
        }
        return;
    }

    tirtc_voip_hangup_reason_t reason =
        state == VOIP_STATE_RINGING
            ? TIRTC_VOIP_HANGUP_REASON_REJECT
            : TIRTC_VOIP_HANGUP_REASON_MANUAL;
    if (reject_info_later(&reject_info, reason) == ESP_OK)
    {
        ESP_LOGI(TAG, "%s", state == VOIP_STATE_RINGING ? "已拒接微信来电" : "已取消微信通话");
    }
}
