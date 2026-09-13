#include "device_call.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "app_memory_policy.h"
#include "app_log_policy.h"
#include "app_task_affinity.h"
#include "device_call_ringtone.h"
#include "device_online.h"
#include "rtc_transport.h"
#include "thing_http_client.h"
#include "thing_mqtt_client.h"
#include "tirtc_connect.h"
#include "tirtc_session_internal.h"

static const char *TAG = "device_call";
static const char *CALL_FLOW_TAG = "CALL_FLOW";

static void *device_call_calloc_control(size_t count, size_t size)
{
    return heap_caps_calloc(count, size, APP_MEMORY_CAPS_CONTROL);
}

#define DEVICE_CALL_API_BASE_MAX_LEN       256
#define DEVICE_CALL_HTTP_PATH_MAX_LEN      192
#define DEVICE_CALL_HTTP_RESPONSE_MAX_LEN  6144
#define DEVICE_CALL_CONTACT_RESPONSE_MAX_LEN 12288
#define DEVICE_CALL_HTTP_TIMEOUT_MS        10000U
#define DEVICE_CALL_WORK_TASK_STACK        (24U * 1024U)
#define DEVICE_CALL_TIMER_TASK_STACK       (6U * 1024U)
#define DEVICE_CALL_TASK_PRIORITY          5
#define DEVICE_CALL_RING_TIMEOUT_MS        30000U
#define DEVICE_CALL_RTC_READY_TIMEOUT_MS   10000U
/* Keep the business wait slightly longer than the TiRTC connect watchdog. */
#define DEVICE_CALL_CONNECT_TIMEOUT_MS     40000U
#define DEVICE_CALL_POLL_INTERVAL_MS       20U
#define DEVICE_CALL_TIMER_POLL_INTERVAL_MS 100U
#define DEVICE_CALL_ONLINE_READY_TIMEOUT_MS 30000U
#define DEVICE_CALL_CODE_ROOM_NOT_FOUND    40400

typedef enum {
    DEVICE_CALL_ROLE_NONE = 0,
    DEVICE_CALL_ROLE_CALLER,
    DEVICE_CALL_ROLE_CALLEE,
} device_call_role_t;

typedef enum {
    DEVICE_CALL_ACTION_CANCEL = 0,
    DEVICE_CALL_ACTION_REJECT,
    DEVICE_CALL_ACTION_HANGUP,
} device_call_action_t;

typedef struct {
    char target_device_id[128];
    char call_type[16];
    uint32_t generation;
} device_call_request_ctx_t;

typedef struct {
    char room_id[96];
    char caller_id[128];
    char call_type[16];
    char previous_room_id[96];
    uint32_t generation;
    bool switch_from_active_call;
} device_call_accept_ctx_t;

typedef struct {
    device_call_action_t action;
    char room_id[96];
    char reason[24];
} device_call_action_ctx_t;

typedef struct {
    uint32_t generation;
} device_call_room_recovery_ctx_t;

typedef struct {
    bool present;
    char room_id[96];
    char status[16];
    char role[16];
} device_call_room_info_t;

typedef enum {
    DEVICE_CALL_CONTACT_WORK_REFRESH = 0,
    DEVICE_CALL_CONTACT_WORK_REQUEST,
    DEVICE_CALL_CONTACT_WORK_RESPOND,
    DEVICE_CALL_CONTACT_WORK_REMARK,
    DEVICE_CALL_CONTACT_WORK_DELETE,
} device_call_contact_work_t;

typedef struct {
    device_call_contact_work_t work;
    char target_device_id[128];
    char remark[64];
    bool accept;
    uint32_t generation;
} device_call_contact_ctx_t;

typedef struct {
    bool initialized;
    bool enabled;
    bool ingress_enabled;
    bool listener_registered;
    bool observer_registered;
    bool request_running;
    bool accept_running;
    bool switch_disconnect_in_progress;
    bool contacts_ready;
    bool contacts_refresh_running;
    bool contacts_refresh_pending;
    bool contact_mutation_running;
    bool room_recovery_running;
    bool caller_peer_answered;
    bool caller_transport_accepted;
    bool caller_media_activation_running;
    char api_base[DEVICE_CALL_API_BASE_MAX_LEN];
    device_call_can_accept_incoming_cb_t can_accept_incoming;
    device_call_session_ended_cb_t on_session_ended;
    void *callback_ctx;
    SemaphoreHandle_t lock;
    thing_mqtt_listener_handle_t listener_handle;
    uint32_t generation;
    device_call_state_t state;
    device_call_role_t role;
    esp_err_t last_error;
    tirtc_conn_t rtc_conn;
    char message[96];
    char room_id[96];
    char peer_device_id[128];
    char call_type[16];
    bool pending_incoming;
    char pending_room_id[96];
    char pending_caller_id[128];
    char pending_call_type[16];
    uint8_t contact_count;
    uint8_t pending_contact_count;
    esp_err_t contacts_last_error;
    device_call_contact_t contacts[DEVICE_CALL_CONTACT_MAX];
    device_call_pending_contact_t pending_contacts[DEVICE_CALL_CONTACT_MAX];
} device_call_runtime_t;

static EXT_RAM_BSS_ATTR device_call_runtime_t s_call;

static void device_call_request_task(void *arg);
static void device_call_accept_task(void *arg);
static void device_call_action_task(void *arg);
static void device_call_ring_timer_task(void *arg);
static void device_call_contact_task(void *arg);
static void device_call_room_recovery_task(void *arg);

static const char *device_call_state_name(device_call_state_t state)
{
    switch (state) {
    case DEVICE_CALL_STATE_IDLE:
        return "idle";
    case DEVICE_CALL_STATE_OUTGOING:
        return "outgoing";
    case DEVICE_CALL_STATE_INCOMING:
        return "incoming";
    case DEVICE_CALL_STATE_CONNECTING:
        return "connecting";
    case DEVICE_CALL_STATE_IN_CALL:
        return "in_call";
    case DEVICE_CALL_STATE_ERROR:
        return "error";
    default:
        return "unknown";
    }
}

static const char *device_call_role_name(device_call_role_t role)
{
    switch (role) {
    case DEVICE_CALL_ROLE_CALLER:
        return "caller";
    case DEVICE_CALL_ROLE_CALLEE:
        return "callee";
    case DEVICE_CALL_ROLE_NONE:
    default:
        return "none";
    }
}

static const char *device_call_action_name(device_call_action_t action)
{
    switch (action) {
    case DEVICE_CALL_ACTION_CANCEL:
        return "cancel";
    case DEVICE_CALL_ACTION_REJECT:
        return "reject";
    case DEVICE_CALL_ACTION_HANGUP:
        return "hangup";
    default:
        return "unknown";
    }
}

static const char *device_call_normalize_reason(device_call_action_t action,
                                                const char *reason)
{
    if (action == DEVICE_CALL_ACTION_REJECT) {
        return reason != NULL && strcmp(reason, "busy") == 0 ? "busy" : "decline";
    }
    if (action == DEVICE_CALL_ACTION_HANGUP) {
        return reason != NULL && strcmp(reason, "hangup") == 0 ? "hangup" : "p2p_error";
    }
    return "";
}

static void device_call_reset_caller_media_gate(void)
{
    rtc_transport_set_next_connection_auto_media(true);
    rtc_transport_set_next_connection_defer_media(false);
}

static bool device_call_type_has_video(const char *call_type)
{
    return call_type != NULL && strcmp(call_type, "video") == 0;
}

static esp_err_t device_call_activate_media(const char *call_type)
{
    return rtc_transport_activate_deferred_media(device_call_type_has_video(call_type), true);
}

static void device_call_lock(void)
{
    if (s_call.lock != NULL) {
        xSemaphoreTake(s_call.lock, portMAX_DELAY);
    }
}

static void device_call_unlock(void)
{
    if (s_call.lock != NULL) {
        xSemaphoreGive(s_call.lock);
    }
}

static void device_call_notify_session_ended(void)
{
    device_call_session_ended_cb_t callback = NULL;
    void *callback_ctx = NULL;

    device_call_lock();
    callback = s_call.on_session_ended;
    callback_ctx = s_call.callback_ctx;
    device_call_unlock();
    if (callback != NULL) {
        callback(callback_ctx);
    }
}

static uint32_t device_call_next_generation_locked(void)
{
    s_call.generation++;
    if (s_call.generation == 0U) {
        s_call.generation = 1U;
    }
    s_call.caller_peer_answered = false;
    s_call.caller_transport_accepted = false;
    s_call.caller_media_activation_running = false;
    s_call.rtc_conn = NULL;
    return s_call.generation;
}

static void device_call_set_message_locked(const char *message)
{
    strlcpy(s_call.message, message != NULL ? message : "", sizeof(s_call.message));
}

static void device_call_set_error_locked(esp_err_t error, const char *message)
{
    s_call.request_running = false;
    s_call.accept_running = false;
    s_call.switch_disconnect_in_progress = false;
    s_call.state = DEVICE_CALL_STATE_ERROR;
    s_call.last_error = error;
    device_call_set_message_locked(message);
}

static void device_call_clear_current_locked(void)
{
    s_call.role = DEVICE_CALL_ROLE_NONE;
    s_call.caller_peer_answered = false;
    s_call.caller_transport_accepted = false;
    s_call.caller_media_activation_running = false;
    s_call.rtc_conn = NULL;
    s_call.room_id[0] = '\0';
    s_call.peer_device_id[0] = '\0';
    s_call.call_type[0] = '\0';
}

static void device_call_show_pending_or_idle_locked(const char *idle_message)
{
    /*
     * This is the terminal transition for the current room. Worker callbacks
     * are generation-fenced, so once the generation changes they cannot clear
     * their ownership flags later. Keep the idle-state invariant here.
     */
    s_call.request_running = false;
    s_call.accept_running = false;
    s_call.switch_disconnect_in_progress = false;
    device_call_clear_current_locked();
    s_call.last_error = ESP_OK;
    if (s_call.pending_incoming) {
        s_call.state = DEVICE_CALL_STATE_INCOMING;
        s_call.role = DEVICE_CALL_ROLE_CALLEE;
        strlcpy(s_call.room_id, s_call.pending_room_id, sizeof(s_call.room_id));
        strlcpy(s_call.peer_device_id, s_call.pending_caller_id, sizeof(s_call.peer_device_id));
        strlcpy(s_call.call_type, s_call.pending_call_type, sizeof(s_call.call_type));
        device_call_set_message_locked("incoming call");
    } else {
        s_call.state = DEVICE_CALL_STATE_IDLE;
        device_call_set_message_locked(idle_message);
    }
}

static bool device_call_generation_matches(uint32_t generation)
{
    bool matches = false;

    device_call_lock();
    matches = s_call.generation == generation;
    device_call_unlock();
    return matches;
}

static bool device_call_pending_incoming_matches(uint32_t generation, const char *room_id)
{
    bool matches = false;

    device_call_lock();
    matches = s_call.ingress_enabled &&
              s_call.generation == generation &&
              s_call.pending_incoming &&
              room_id != NULL &&
              strcmp(s_call.pending_room_id, room_id) == 0;
    device_call_unlock();
    return matches;
}

static esp_err_t device_call_launch_task(TaskFunction_t task,
                                         const char *name,
                                         uint32_t stack_size,
                                         void *ctx)
{
    BaseType_t task_ret = xTaskCreateWithCaps(task,
                                              name,
                                              stack_size,
                                              ctx,
                                              DEVICE_CALL_TASK_PRIORITY,
                                              NULL,
                                              APP_TASK_STACK_CAPS_BACKGROUND);
    return task_ret == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t device_call_get_bearer(char *buffer, size_t buffer_size)
{
    device_auth_token_t *token = NULL;
    esp_err_t ret = ESP_OK;

    if (buffer == NULL || buffer_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    token = app_memory_calloc_psram(1, sizeof(*token));
    if (token == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ret = device_online_get_cached_mqtt_token(token);
    if (ret == ESP_OK) {
        int written = snprintf(buffer, buffer_size, "Bearer %s", token->mqtt_token);
        if (written <= 0 || written >= (int)buffer_size) {
            ret = ESP_ERR_INVALID_SIZE;
        }
    }

    free(token);
    return ret;
}

static esp_err_t device_call_http_request(const char *method,
                                          const char *path,
                                          const char *body,
                                          char *response,
                                          size_t response_size,
                                          int *status_code,
                                          const char *trace_name)
{
    char url[DEVICE_CALL_API_BASE_MAX_LEN + DEVICE_CALL_HTTP_PATH_MAX_LEN] = {0};
    char bearer[DEVICE_AUTH_MQTT_TOKEN_MAX_LEN + 8] = {0};
    char api_base[DEVICE_CALL_API_BASE_MAX_LEN] = {0};
    thing_http_header_t headers[1] = {0};

    if (method == NULL || method[0] == '\0' ||
        path == NULL || response == NULL || response_size < 2U) {
        return ESP_ERR_INVALID_ARG;
    }

    device_call_lock();
    strlcpy(api_base, s_call.api_base, sizeof(api_base));
    device_call_unlock();

    esp_err_t ret = device_call_get_bearer(bearer, sizeof(bearer));
    if (ret != ESP_OK) {
        return ret;
    }
    ret = thing_http_join_url(url, sizeof(url), api_base, path);
    if (ret != ESP_OK) {
        return ret;
    }

    headers[0] = (thing_http_header_t){
        .name = "Authorization",
        .value = bearer,
    };
    thing_http_request_t request = {
        .url = url,
        .method = method,
        .body = body,
        .headers = headers,
        .header_count = 1,
        .timeout_ms = DEVICE_CALL_HTTP_TIMEOUT_MS,
    };
    ESP_LOGI(CALL_FLOW_TAG,
             "stage=http_begin trace=%s method=%s path=%s",
             trace_name != NULL ? trace_name : "-",
             method,
             path);
    ret = thing_http_request_json(&request, response, response_size, status_code);
    ESP_LOGI(CALL_FLOW_TAG,
             "stage=http_done trace=%s status=%d ret=%s",
             trace_name != NULL ? trace_name : "-",
             status_code != NULL ? *status_code : 0,
             esp_err_to_name(ret));
    return ret;
}

static esp_err_t device_call_http_post(const char *path,
                                       const char *body,
                                       char *response,
                                       size_t response_size,
                                       int *status_code,
                                       const char *trace_name)
{
    return device_call_http_request("POST",
                                    path,
                                    body != NULL ? body : "{}",
                                    response,
                                    response_size,
                                    status_code,
                                    trace_name);
}

static esp_err_t device_call_http_put(const char *path,
                                      const char *body,
                                      char *response,
                                      size_t response_size,
                                      int *status_code,
                                      const char *trace_name)
{
    return device_call_http_request("PUT",
                                    path,
                                    body != NULL ? body : "{}",
                                    response,
                                    response_size,
                                    status_code,
                                    trace_name);
}

static esp_err_t device_call_http_get(const char *path,
                                      char *response,
                                      size_t response_size,
                                      int *status_code,
                                      const char *trace_name)
{
    return device_call_http_request("GET",
                                    path,
                                    NULL,
                                    response,
                                    response_size,
                                    status_code,
                                    trace_name);
}

static esp_err_t device_call_parse_response(const char *response,
                                            cJSON **root_out,
                                            cJSON **data_out,
                                            int *business_code)
{
    cJSON *root = NULL;
    cJSON *code = NULL;

    if (root_out != NULL) {
        *root_out = NULL;
    }
    if (data_out != NULL) {
        *data_out = NULL;
    }
    if (business_code != NULL) {
        *business_code = -1;
    }
    if (response == NULL || response[0] == '\0') {
        return ESP_ERR_INVALID_RESPONSE;
    }

    root = cJSON_Parse(response);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    code = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (!cJSON_IsNumber(code)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (business_code != NULL) {
        *business_code = code->valueint;
    }
    if (code->valueint != 200) {
        if (root_out != NULL) {
            *root_out = root;
        } else {
            cJSON_Delete(root);
        }
        return ESP_FAIL;
    }

    if (data_out != NULL) {
        *data_out = cJSON_GetObjectItemCaseSensitive(root, "data");
    }
    if (root_out != NULL) {
        *root_out = root;
    } else {
        cJSON_Delete(root);
    }
    return ESP_OK;
}

static const char *device_call_json_string(const cJSON *object, const char *name)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(value) && value->valuestring != NULL ? value->valuestring : "";
}

static esp_err_t device_call_post_room_action(device_call_action_t action,
                                              const char *room_id,
                                              const char *reason)
{
    const char *path = NULL;
    const char *trace_name = NULL;
    const char *wire_reason = device_call_normalize_reason(action, reason);
    char body[256] = {0};
    char *response = NULL;
    int status_code = 0;
    int business_code = -1;
    int written = 0;
    esp_err_t ret = ESP_OK;

    if (room_id == NULL || room_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    switch (action) {
    case DEVICE_CALL_ACTION_CANCEL:
        path = "/v1/call/cancel";
        trace_name = "call_cancel";
        written = snprintf(body, sizeof(body), "{\"room_id\":\"%s\"}", room_id);
        break;
    case DEVICE_CALL_ACTION_REJECT:
        path = "/v1/call/reject";
        trace_name = "call_reject";
        written = snprintf(body,
                           sizeof(body),
                           "{\"room_id\":\"%s\",\"reason\":\"%s\"}",
                           room_id,
                           wire_reason);
        break;
    case DEVICE_CALL_ACTION_HANGUP:
        path = "/v1/call/hangup";
        trace_name = "call_hangup";
        written = snprintf(body,
                           sizeof(body),
                           "{\"room_id\":\"%s\",\"reason\":\"%s\"}",
                           room_id,
                           wire_reason);
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    if (written <= 0 || written >= (int)sizeof(body)) {
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(CALL_FLOW_TAG,
             "stage=room_action_begin action=%s room=%s reason=%s",
             device_call_action_name(action),
             room_id,
             wire_reason[0] != '\0' ? wire_reason : "-");

    response = app_memory_calloc_psram(1, DEVICE_CALL_HTTP_RESPONSE_MAX_LEN);
    if (response == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ret = device_call_http_post(path,
                                body,
                                response,
                                DEVICE_CALL_HTTP_RESPONSE_MAX_LEN,
                                &status_code,
                                trace_name);
    if (ret == ESP_OK && status_code >= 200 && status_code < 300) {
        ret = device_call_parse_response(response, NULL, NULL, &business_code);
    } else if (ret == ESP_OK) {
        ret = ESP_FAIL;
    }
    if (ret != ESP_OK && business_code == DEVICE_CALL_CODE_ROOM_NOT_FOUND) {
        /* Room actions are terminal and idempotent. The peer may have already
         * released the shared room while this request was in flight. */
        ESP_LOGI(TAG,
                 "%s already complete: room=%s code=%d",
                 trace_name,
                 room_id,
                 business_code);
        ret = ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "%s failed: http=%d code=%d ret=%s",
                 trace_name,
                 status_code,
                 business_code,
                 esp_err_to_name(ret));
    }
    ESP_LOGI(CALL_FLOW_TAG,
             "stage=room_action_done action=%s room=%s http=%d code=%d ret=%s",
             device_call_action_name(action),
             room_id,
             status_code,
             business_code,
             esp_err_to_name(ret));
    free(response);
    return ret;
}

static void device_call_action_task(void *arg)
{
    device_call_action_ctx_t *ctx = (device_call_action_ctx_t *)arg;

    if (ctx != NULL) {
        (void)device_call_post_room_action(ctx->action, ctx->room_id, ctx->reason);
        free(ctx);
    }
    vTaskDeleteWithCaps(NULL);
}

static esp_err_t device_call_post_room_action_async(device_call_action_t action,
                                                    const char *room_id,
                                                    const char *reason)
{
    device_call_action_ctx_t *ctx = device_call_calloc_control(1, sizeof(*ctx));
    if (ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ctx->action = action;
    strlcpy(ctx->room_id, room_id != NULL ? room_id : "", sizeof(ctx->room_id));
    strlcpy(ctx->reason, reason != NULL ? reason : "", sizeof(ctx->reason));

    ESP_LOGI(CALL_FLOW_TAG,
             "stage=room_action_queued action=%s room=%s reason=%s",
             device_call_action_name(action),
             ctx->room_id,
             ctx->reason[0] != '\0' ? ctx->reason : "-");

    esp_err_t ret = device_call_launch_task(device_call_action_task,
                                            "dev_call_act",
                                            DEVICE_CALL_WORK_TASK_STACK,
                                            ctx);
    if (ret != ESP_OK) {
        free(ctx);
    }
    return ret;
}

static bool device_call_transport_is_connected(void)
{
    rtc_transport_stats_t stats = {0};
    rtc_transport_get_stats(&stats);
    return stats.active_connection;
}

static esp_err_t device_call_wait_for_rtc_ready(uint32_t generation)
{
    ESP_LOGI(CALL_FLOW_TAG,
             "stage=rtc_ready_wait_begin gen=%lu timeout_ms=%u",
             (unsigned long)generation,
             (unsigned)DEVICE_CALL_RTC_READY_TIMEOUT_MS);
    esp_err_t ret = rtc_transport_prepare_sdk();
    if (ret != ESP_OK) {
        ESP_LOGW(CALL_FLOW_TAG,
                 "stage=rtc_ready_wait_done gen=%lu ret=%s reason=prepare_sdk",
                 (unsigned long)generation,
                 esp_err_to_name(ret));
        return ret;
    }

    uint32_t elapsed_ms = 0;
    while (elapsed_ms < DEVICE_CALL_RTC_READY_TIMEOUT_MS) {
        rtc_transport_stats_t stats = {0};

        if (!device_call_generation_matches(generation)) {
            ESP_LOGW(CALL_FLOW_TAG,
                     "stage=rtc_ready_wait_done gen=%lu ret=%s reason=generation_changed",
                     (unsigned long)generation,
                     esp_err_to_name(ESP_ERR_INVALID_STATE));
            return ESP_ERR_INVALID_STATE;
        }
        rtc_transport_get_stats(&stats);
        if (stats.sdk_started && !stats.active_connection &&
            stats.state != RTC_TRANSPORT_STATE_DISCONNECTING &&
            stats.state != RTC_TRANSPORT_STATE_ERROR) {
            ESP_LOGI(CALL_FLOW_TAG,
                     "stage=rtc_ready_wait_done gen=%lu elapsed_ms=%u state=%u ret=ESP_OK",
                     (unsigned long)generation,
                     (unsigned)elapsed_ms,
                     (unsigned)stats.state);
            return ESP_OK;
        }
        if (stats.state == RTC_TRANSPORT_STATE_ERROR) {
            ESP_LOGW(CALL_FLOW_TAG,
                     "stage=rtc_ready_wait_done gen=%lu elapsed_ms=%u state=%u ret=ESP_FAIL",
                     (unsigned long)generation,
                     (unsigned)elapsed_ms,
                     (unsigned)stats.state);
            return ESP_FAIL;
        }
        vTaskDelay(pdMS_TO_TICKS(DEVICE_CALL_POLL_INTERVAL_MS));
        elapsed_ms += DEVICE_CALL_POLL_INTERVAL_MS;
    }
    ESP_LOGW(CALL_FLOW_TAG,
             "stage=rtc_ready_wait_done gen=%lu elapsed_ms=%u ret=ESP_ERR_TIMEOUT",
             (unsigned long)generation,
             (unsigned)elapsed_ms);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t device_call_wait_for_connection(uint32_t generation)
{
    uint32_t elapsed_ms = 0;

    ESP_LOGI(CALL_FLOW_TAG,
             "stage=p2p_wait_begin gen=%lu timeout_ms=%u",
             (unsigned long)generation,
             (unsigned)DEVICE_CALL_CONNECT_TIMEOUT_MS);

    while (elapsed_ms < DEVICE_CALL_CONNECT_TIMEOUT_MS) {
        rtc_transport_stats_t stats = {0};
        bool connection_tracked = false;

        if (!device_call_generation_matches(generation)) {
            ESP_LOGW(CALL_FLOW_TAG,
                     "stage=p2p_wait_done gen=%lu ret=%s reason=generation_changed",
                     (unsigned long)generation,
                     esp_err_to_name(ESP_ERR_INVALID_STATE));
            return ESP_ERR_INVALID_STATE;
        }
        rtc_transport_get_stats(&stats);
        device_call_lock();
        connection_tracked = s_call.generation == generation && s_call.rtc_conn != NULL;
        device_call_unlock();
        if (stats.active_connection && connection_tracked) {
            ESP_LOGI(CALL_FLOW_TAG,
                     "stage=p2p_wait_done gen=%lu elapsed_ms=%u state=%u ret=ESP_OK",
                     (unsigned long)generation,
                     (unsigned)elapsed_ms,
                     (unsigned)stats.state);
            return ESP_OK;
        }
        if (stats.state == RTC_TRANSPORT_STATE_ERROR ||
            (!stats.sdk_started && elapsed_ms > DEVICE_CALL_RTC_READY_TIMEOUT_MS)) {
            esp_err_t wait_ret =
                stats.last_error == TIRTC_E_TIMEOUTED ? ESP_ERR_TIMEOUT : ESP_FAIL;
            ESP_LOGW(CALL_FLOW_TAG,
                     "stage=p2p_wait_done gen=%lu elapsed_ms=%u sdk_started=%d state=%u sdk_error=%d ret=%s",
                     (unsigned long)generation,
                     (unsigned)elapsed_ms,
                     stats.sdk_started ? 1 : 0,
                     (unsigned)stats.state,
                     stats.last_error,
                     esp_err_to_name(wait_ret));
            return wait_ret;
        }
        vTaskDelay(pdMS_TO_TICKS(DEVICE_CALL_POLL_INTERVAL_MS));
        elapsed_ms += DEVICE_CALL_POLL_INTERVAL_MS;
    }
    ESP_LOGW(CALL_FLOW_TAG,
             "stage=p2p_wait_done gen=%lu elapsed_ms=%u ret=ESP_ERR_TIMEOUT",
             (unsigned long)generation,
             (unsigned)elapsed_ms);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t device_call_wait_for_disconnect(uint32_t timeout_ms)
{
    uint32_t elapsed_ms = 0;

    while (elapsed_ms < timeout_ms) {
        rtc_transport_stats_t stats = {0};

        rtc_transport_get_stats(&stats);
        if (!stats.active_connection && stats.state != RTC_TRANSPORT_STATE_DISCONNECTING) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(DEVICE_CALL_POLL_INTERVAL_MS));
        elapsed_ms += DEVICE_CALL_POLL_INTERVAL_MS;
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t device_call_send_connected_notice(const char *room_id)
{
    char payload[160] = {0};
    int written = snprintf(payload, sizeof(payload), "{\"room_id\":\"%s\"}", room_id);
    if (written <= 0 || written >= (int)sizeof(payload)) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t ret = rtc_transport_send_command(TIRTC_SESSION_CMD_DEVICE_CALL_CONNECTED,
                                               payload,
                                               (size_t)written);
    ESP_LOGI(CALL_FLOW_TAG,
             "stage=connected_notice_tx room=%s cmd=0x%04x ret=%s",
             room_id,
             TIRTC_SESSION_CMD_DEVICE_CALL_CONNECTED,
             esp_err_to_name(ret));
    return ret;
}

static esp_err_t device_call_fetch_connect_token(const device_call_accept_ctx_t *ctx,
                                                 char *remote_device_id,
                                                 size_t remote_device_id_size,
                                                 char *connect_token,
                                                 size_t connect_token_size)
{
    static const char *path = "/v1/call/device/info";
    char body[384] = {0};
    char *response = NULL;
    int written = 0;
    int status_code = 0;
    int business_code = -1;
    esp_err_t ret = ESP_FAIL;
    cJSON *root = NULL;
    cJSON *data = NULL;

    ESP_LOGI(CALL_FLOW_TAG,
             "stage=token_request_begin gen=%lu room=%s peer=%s",
             (unsigned long)ctx->generation,
             ctx->room_id,
             ctx->caller_id);

    written = snprintf(body,
                       sizeof(body),
                       "{\"device_id\":\"%s\",\"room_id\":\"%s\",\"purpose\":\"call\"}",
                       ctx->caller_id,
                       ctx->room_id);
    if (written <= 0 || written >= (int)sizeof(body)) {
        return ESP_ERR_INVALID_SIZE;
    }

    response = app_memory_calloc_psram(1, DEVICE_CALL_HTTP_RESPONSE_MAX_LEN);
    if (response == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ret = device_call_http_post(path,
                                body,
                                response,
                                DEVICE_CALL_HTTP_RESPONSE_MAX_LEN,
                                &status_code,
                                "call_device_info");
    if (ret == ESP_OK && status_code >= 200 && status_code < 300) {
        ret = device_call_parse_response(response, &root, &data, &business_code);
    } else if (ret == ESP_OK) {
        ret = ESP_FAIL;
    }
    if (ret == ESP_OK && cJSON_IsObject(data)) {
        const char *token = device_call_json_string(data, "token");
        const char *device_id = device_call_json_string(data, "device_id");
        const char *resolved_device_id = device_id[0] != '\0' ? device_id : ctx->caller_id;

        if (token[0] != '\0' && strlen(token) < connect_token_size &&
            strlen(resolved_device_id) < remote_device_id_size) {
            strlcpy(connect_token, token, connect_token_size);
            strlcpy(remote_device_id, resolved_device_id, remote_device_id_size);
            ESP_LOGI(CALL_FLOW_TAG,
                     "stage=token_request_done gen=%lu room=%s peer=%s path=%s ret=ESP_OK",
                     (unsigned long)ctx->generation,
                     ctx->room_id,
                     remote_device_id,
                     path);
        } else {
            ret = ESP_ERR_INVALID_RESPONSE;
        }
    }

    if (root != NULL) {
        cJSON_Delete(root);
    }
    if (ret == ESP_OK) {
        free(response);
        return ESP_OK;
    }

    ESP_LOGW(TAG,
             "device info request failed: path=%s http=%d code=%d ret=%s",
             path,
             status_code,
             business_code,
             esp_err_to_name(ret));

    free(response);
    ESP_LOGW(CALL_FLOW_TAG,
             "stage=token_request_done gen=%lu room=%s peer=%s ret=%s",
             (unsigned long)ctx->generation,
             ctx->room_id,
             ctx->caller_id,
             esp_err_to_name(ret));
    return ret;
}

static void device_call_accept_failed(const device_call_accept_ctx_t *ctx,
                                      esp_err_t error,
                                      const char *message,
                                      bool server_accepted)
{
    bool session_ended = false;

    device_call_lock();
    if (s_call.generation == ctx->generation) {
        device_call_set_error_locked(error, message);
        s_call.accept_running = false;
        s_call.switch_disconnect_in_progress = false;
        session_ended = true;
    }
    device_call_unlock();
    ESP_LOGW(CALL_FLOW_TAG,
             "stage=accept_failed gen=%lu room=%s peer=%s error=%s message=%s",
             (unsigned long)ctx->generation,
             ctx->room_id,
             ctx->caller_id,
             esp_err_to_name(error),
             message != NULL ? message : "-");
    (void)device_call_post_room_action_async(server_accepted ? DEVICE_CALL_ACTION_HANGUP :
                                                               DEVICE_CALL_ACTION_REJECT,
                                             ctx->room_id,
                                             server_accepted ? "p2p_error" : "busy");
    if (session_ended) {
        device_call_notify_session_ended();
    }
}

static void device_call_accept_task(void *arg)
{
    device_call_accept_ctx_t *ctx = (device_call_accept_ctx_t *)arg;
    char remote_device_id[128] = {0};
    char connect_token[TIRTC_CONNECT_TOKEN_MAX_LEN] = {0};
    esp_err_t ret = ESP_OK;

    if (ctx == NULL) {
        vTaskDeleteWithCaps(NULL);
        return;
    }

    ESP_LOGI(CALL_FLOW_TAG,
             "stage=accept_worker_begin gen=%lu room=%s peer=%s switch=%d",
             (unsigned long)ctx->generation,
             ctx->room_id,
             ctx->caller_id,
             ctx->switch_from_active_call ? 1 : 0);

    if (ctx->switch_from_active_call) {
        ESP_LOGI(CALL_FLOW_TAG,
                 "stage=previous_call_close_begin gen=%lu room=%s",
                 (unsigned long)ctx->generation,
                 ctx->previous_room_id);
        (void)rtc_transport_send_command(TIRTC_SESSION_CMD_DEVICE_CALL_HANGUP, NULL, 0);
        (void)rtc_transport_disconnect();
        if (ctx->previous_room_id[0] != '\0') {
            (void)device_call_post_room_action(DEVICE_CALL_ACTION_HANGUP,
                                               ctx->previous_room_id,
                                               "hangup");
        }
        ret = device_call_wait_for_disconnect(5000U);
        device_call_lock();
        s_call.switch_disconnect_in_progress = false;
        device_call_unlock();
        if (ret != ESP_OK) {
            device_call_accept_failed(ctx, ret, "previous call did not close", false);
            free(ctx);
            vTaskDeleteWithCaps(NULL);
            return;
        }
        rtc_transport_flush_remote_media();
        ESP_LOGI(CALL_FLOW_TAG,
                 "stage=previous_call_close_done gen=%lu ret=ESP_OK",
                 (unsigned long)ctx->generation);
    }

    /*
     * /v1/call/device/info accepts the business call and notifies the caller.
     * Do not invoke that side-effecting endpoint until this device can really
     * enter TiRTC P2P connect mode.
     */
    ret = device_call_wait_for_rtc_ready(ctx->generation);
    if (ret != ESP_OK) {
        device_call_accept_failed(ctx, ret, "RTC listener not ready", false);
        free(ctx);
        vTaskDeleteWithCaps(NULL);
        return;
    }

    ret = device_call_fetch_connect_token(ctx,
                                          remote_device_id,
                                          sizeof(remote_device_id),
                                          connect_token,
                                          sizeof(connect_token));
    if (ret != ESP_OK) {
        device_call_accept_failed(ctx, ret, "call token request failed", false);
        free(ctx);
        vTaskDeleteWithCaps(NULL);
        return;
    }

    /* Hangup may complete while the side-effecting token request is in
     * flight. Do not let the stale accept worker resurrect that room by
     * starting a new TiRTC connection after the lifecycle has moved on. */
    if (!device_call_generation_matches(ctx->generation)) {
        ESP_LOGI(CALL_FLOW_TAG,
                 "stage=accept_abandoned gen=%lu room=%s reason=generation_changed_before_connect",
                 (unsigned long)ctx->generation,
                 ctx->room_id);
        memset(connect_token, 0, sizeof(connect_token));
        free(ctx);
        vTaskDeleteWithCaps(NULL);
        return;
    }

    if (ret == ESP_OK) {
        rtc_transport_set_next_connection_auto_media(true);
        rtc_transport_set_next_connection_defer_media(true);
        ESP_LOGI(CALL_FLOW_TAG,
                 "stage=p2p_connect_begin gen=%lu room=%s peer=%s",
                 (unsigned long)ctx->generation,
                 ctx->room_id,
                 remote_device_id);
        ret = rtc_transport_connect_peer_with_token(remote_device_id, connect_token);
        ESP_LOGI(CALL_FLOW_TAG,
                 "stage=p2p_connect_submitted gen=%lu room=%s peer=%s ret=%s",
                 (unsigned long)ctx->generation,
                 ctx->room_id,
                 remote_device_id,
                 esp_err_to_name(ret));
    }
    memset(connect_token, 0, sizeof(connect_token));
    if (ret == ESP_OK && !device_call_generation_matches(ctx->generation)) {
        ESP_LOGI(CALL_FLOW_TAG,
                 "stage=accept_abandoned gen=%lu room=%s reason=generation_changed_during_connect_submit",
                 (unsigned long)ctx->generation,
                 ctx->room_id);
        (void)rtc_transport_disconnect();
        free(ctx);
        vTaskDeleteWithCaps(NULL);
        return;
    }
    if (ret == ESP_OK) {
        ret = device_call_wait_for_connection(ctx->generation);
    }
    if (ret == ESP_OK) {
        ret = device_call_send_connected_notice(ctx->room_id);
    }
    if (ret == ESP_OK) {
        ret = device_call_activate_media(ctx->call_type);
    }
    if (ret != ESP_OK && !device_call_generation_matches(ctx->generation)) {
        ESP_LOGI(CALL_FLOW_TAG,
                 "stage=accept_abandoned gen=%lu room=%s reason=generation_changed_while_connecting",
                 (unsigned long)ctx->generation,
                 ctx->room_id);
        (void)rtc_transport_disconnect();
        free(ctx);
        vTaskDeleteWithCaps(NULL);
        return;
    }
    if (ret != ESP_OK) {
        device_call_accept_failed(ctx,
                                  ret,
                                  ret == ESP_ERR_TIMEOUT ? "peer connection timed out" :
                                                           "peer connection failed",
                                  true);
        (void)rtc_transport_disconnect();
        free(ctx);
        vTaskDeleteWithCaps(NULL);
        return;
    }

    device_call_lock();
    if (s_call.generation == ctx->generation) {
        s_call.state = DEVICE_CALL_STATE_IN_CALL;
        s_call.last_error = ESP_OK;
        s_call.accept_running = false;
        device_call_set_message_locked("call connected");
    }
    device_call_unlock();
    ESP_LOGI(CALL_FLOW_TAG,
             "stage=in_call gen=%lu role=callee room=%s peer=%s",
             (unsigned long)ctx->generation,
             ctx->room_id,
             remote_device_id);
    ESP_LOGI(TAG, "callee P2P connected: room=%s peer=%s", ctx->room_id, remote_device_id);

    free(ctx);
    vTaskDeleteWithCaps(NULL);
}

static esp_err_t device_call_extract_room(const char *response,
                                          char *room_id,
                                          size_t room_id_size,
                                          int *business_code)
{
    cJSON *root = NULL;
    cJSON *data = NULL;
    esp_err_t ret = device_call_parse_response(response, &root, &data, business_code);

    if (ret == ESP_OK && cJSON_IsObject(data)) {
        const char *value = device_call_json_string(data, "room_id");
        if (value[0] != '\0' && strlen(value) < room_id_size) {
            strlcpy(room_id, value, room_id_size);
        } else {
            ret = ESP_ERR_INVALID_RESPONSE;
        }
    }
    if (root != NULL) {
        cJSON_Delete(root);
    }
    return ret;
}

static bool device_call_extract_error_room(const char *response,
                                           char *room_id,
                                           size_t room_id_size)
{
    cJSON *root = cJSON_Parse(response);
    cJSON *data = NULL;
    const char *value = "";

    if (root == NULL) {
        return false;
    }
    data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (cJSON_IsObject(data)) {
        value = device_call_json_string(data, "room_id");
    }
    bool found = value[0] != '\0' && strlen(value) < room_id_size;
    if (found) {
        strlcpy(room_id, value, room_id_size);
    }
    cJSON_Delete(root);
    return found;
}

static esp_err_t device_call_query_current_room(device_call_room_info_t *info)
{
    char *response = NULL;
    cJSON *root = NULL;
    cJSON *data = NULL;
    int status_code = 0;
    int business_code = -1;
    esp_err_t ret = ESP_OK;

    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(info, 0, sizeof(*info));

    response = app_memory_calloc_psram(1, DEVICE_CALL_HTTP_RESPONSE_MAX_LEN);
    if (response == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ret = device_call_http_get("/v1/call/room",
                               response,
                               DEVICE_CALL_HTTP_RESPONSE_MAX_LEN,
                               &status_code,
                               "call_room_query");
    if (ret == ESP_OK && status_code >= 200 && status_code < 300) {
        ret = device_call_parse_response(response, &root, &data, &business_code);
    } else if (ret == ESP_OK) {
        ret = ESP_FAIL;
    }

    /* Some deployed call-server versions omit data instead of returning null
     * when no room is active. Both responses mean the device is idle. */
    if (ret == ESP_OK && (data == NULL || cJSON_IsNull(data))) {
        ESP_LOGI(CALL_FLOW_TAG, "stage=room_query_done present=0");
    } else if (ret == ESP_OK && cJSON_IsObject(data)) {
        const char *room_id = device_call_json_string(data, "room_id");
        const char *status = device_call_json_string(data, "status");
        const char *role = device_call_json_string(data, "role");

        if (room_id[0] == '\0' ||
            strlen(room_id) >= sizeof(info->room_id) ||
            strlen(status) >= sizeof(info->status) ||
            strlen(role) >= sizeof(info->role)) {
            ret = ESP_ERR_INVALID_RESPONSE;
        } else {
            info->present = true;
            strlcpy(info->room_id, room_id, sizeof(info->room_id));
            strlcpy(info->status, status, sizeof(info->status));
            strlcpy(info->role, role, sizeof(info->role));
            ESP_LOGI(CALL_FLOW_TAG,
                     "stage=room_query_done present=1 room=%s status=%s role=%s",
                     info->room_id,
                     info->status[0] != '\0' ? info->status : "-",
                     info->role[0] != '\0' ? info->role : "-");
        }
    } else if (ret == ESP_OK) {
        ret = ESP_ERR_INVALID_RESPONSE;
    }

    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "current call room query failed: http=%d code=%d ret=%s",
                 status_code,
                 business_code,
                 esp_err_to_name(ret));
    }
    if (root != NULL) {
        cJSON_Delete(root);
    }
    free(response);
    return ret;
}

static esp_err_t device_call_recover_stale_room(const char *reported_room_id)
{
    device_call_room_info_t info = {0};
    device_call_action_t action = DEVICE_CALL_ACTION_CANCEL;
    const char *room_id = reported_room_id;
    const char *reason = "";
    esp_err_t ret = device_call_query_current_room(&info);

    if (ret == ESP_OK && !info.present) {
        ESP_LOGI(TAG, "stale call room already released; retry request");
        return ESP_OK;
    }
    if (ret == ESP_OK) {
        room_id = info.room_id;
        if (strcmp(info.status, "answered") == 0) {
            action = DEVICE_CALL_ACTION_HANGUP;
            reason = "p2p_error";
        } else if (strcmp(info.status, "active") == 0 &&
                   strcmp(info.role, "callee") == 0) {
            action = DEVICE_CALL_ACTION_REJECT;
            reason = "decline";
        } else if (strcmp(info.status, "active") != 0) {
            ESP_LOGW(TAG,
                     "unknown stale call room status; use caller cancel: room=%s status=%s role=%s",
                     info.room_id,
                     info.status[0] != '\0' ? info.status : "-",
                     info.role[0] != '\0' ? info.role : "-");
        }
    } else {
        /* Preserve the server's documented stale-room recovery path when the
         * room query itself is temporarily unavailable. */
        ESP_LOGW(TAG,
                 "stale call room query unavailable; fall back to cancel: room=%s",
                 reported_room_id);
    }

    ESP_LOGW(TAG,
             "recover stale call room before retry: room=%s action=%s status=%s role=%s",
             room_id,
             device_call_action_name(action),
             info.present && info.status[0] != '\0' ? info.status : "unknown",
             info.present && info.role[0] != '\0' ? info.role : "unknown");
    return device_call_post_room_action(action, room_id, reason);
}

static void device_call_room_recovery_task(void *arg)
{
    device_call_room_recovery_ctx_t *ctx = (device_call_room_recovery_ctx_t *)arg;
    device_call_room_info_t info = {0};
    device_call_action_t action = DEVICE_CALL_ACTION_CANCEL;
    const char *reason = "";
    esp_err_t ret = ESP_ERR_INVALID_ARG;
    bool local_idle = false;

    if (ctx != NULL) {
        uint32_t elapsed_ms = 0;
        while (device_call_generation_matches(ctx->generation) &&
               !device_online_is_online() &&
               elapsed_ms < DEVICE_CALL_ONLINE_READY_TIMEOUT_MS) {
            vTaskDelay(pdMS_TO_TICKS(DEVICE_CALL_POLL_INTERVAL_MS));
            elapsed_ms += DEVICE_CALL_POLL_INTERVAL_MS;
        }
        if (!device_call_generation_matches(ctx->generation)) {
            ret = ESP_ERR_INVALID_STATE;
        } else if (!device_online_is_online()) {
            ret = ESP_ERR_TIMEOUT;
            ESP_LOGW(CALL_FLOW_TAG,
                     "stage=room_recovery_wait timeout_ms=%u ret=%s",
                     (unsigned)elapsed_ms,
                     esp_err_to_name(ret));
        } else {
            ret = device_call_query_current_room(&info);
        }
        if (ret == ESP_OK && info.present) {
            device_call_lock();
            local_idle = s_call.generation == ctx->generation &&
                         s_call.state == DEVICE_CALL_STATE_IDLE &&
                         !s_call.request_running &&
                         !s_call.accept_running &&
                         !s_call.pending_incoming;
            device_call_unlock();

            if (!local_idle) {
                ESP_LOGI(CALL_FLOW_TAG,
                         "stage=room_recovery_skip gen=%lu room=%s reason=local_session_changed",
                         (unsigned long)ctx->generation,
                         info.room_id);
                ret = ESP_ERR_INVALID_STATE;
            } else if (strcmp(info.status, "answered") == 0) {
                action = DEVICE_CALL_ACTION_HANGUP;
                reason = "p2p_error";
                ret = device_call_post_room_action(action, info.room_id, reason);
            } else if (strcmp(info.status, "active") == 0 &&
                       strcmp(info.role, "caller") == 0) {
                action = DEVICE_CALL_ACTION_CANCEL;
                ret = device_call_post_room_action(action, info.room_id, reason);
            } else if (strcmp(info.status, "active") == 0 &&
                       strcmp(info.role, "callee") == 0) {
                action = DEVICE_CALL_ACTION_REJECT;
                reason = "decline";
                ret = device_call_post_room_action(action, info.room_id, reason);
            } else {
                ESP_LOGW(TAG,
                         "call room recovery skipped unknown state: room=%s status=%s role=%s",
                         info.room_id,
                         info.status[0] != '\0' ? info.status : "-",
                         info.role[0] != '\0' ? info.role : "-");
                ret = ESP_ERR_INVALID_RESPONSE;
            }

            ESP_LOGI(CALL_FLOW_TAG,
                     "stage=room_recovery_done gen=%lu room=%s status=%s role=%s action=%s ret=%s",
                     (unsigned long)ctx->generation,
                     info.room_id,
                     info.status[0] != '\0' ? info.status : "-",
                     info.role[0] != '\0' ? info.role : "-",
                     device_call_action_name(action),
                     esp_err_to_name(ret));
        } else if (ret == ESP_OK) {
            ESP_LOGI(CALL_FLOW_TAG,
                     "stage=room_recovery_done gen=%lu present=0 ret=ESP_OK",
                     (unsigned long)ctx->generation);
        }
    }

    device_call_lock();
    if (ctx != NULL && s_call.generation == ctx->generation) {
        s_call.room_recovery_running = false;
    }
    device_call_unlock();

    free(ctx);
    vTaskDeleteWithCaps(NULL);
}

static esp_err_t device_call_request_room(const char *target_device_id,
                                          const char *call_type,
                                          char *room_id,
                                          size_t room_id_size)
{
    char body[256] = {0};
    char stale_room_id[96] = {0};
    char *response = NULL;
    esp_err_t ret = ESP_FAIL;
    int written = snprintf(body,
                           sizeof(body),
                           "{\"targets\":[\"%s\"],\"call_type\":\"%s\"}",
                           target_device_id,
                           call_type);
    if (written <= 0 || written >= (int)sizeof(body)) {
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(CALL_FLOW_TAG,
             "stage=call_request_begin peer=%s type=%s",
             target_device_id,
             call_type);

    response = app_memory_calloc_psram(1, DEVICE_CALL_HTTP_RESPONSE_MAX_LEN);
    if (response == NULL) {
        return ESP_ERR_NO_MEM;
    }

    for (unsigned attempt = 0; attempt < 2U; ++attempt) {
        int status_code = 0;
        int business_code = -1;

        response[0] = '\0';
        ret = device_call_http_post("/v1/call/request",
                                    body,
                                    response,
                                    DEVICE_CALL_HTTP_RESPONSE_MAX_LEN,
                                    &status_code,
                                    "call_request");
        if (ret == ESP_OK && status_code >= 200 && status_code < 300) {
            ret = device_call_extract_room(response, room_id, room_id_size, &business_code);
        } else if (ret == ESP_OK) {
            ret = ESP_FAIL;
        }
        if (ret == ESP_OK) {
            ESP_LOGI(CALL_FLOW_TAG,
                     "stage=call_request_done peer=%s attempt=%u room=%s http=%d code=%d ret=ESP_OK",
                     target_device_id,
                     attempt + 1U,
                     room_id,
                     status_code,
                     business_code);
            break;
        }

        if (attempt == 0U &&
            device_call_extract_error_room(response, stale_room_id, sizeof(stale_room_id))) {
            ESP_LOGW(TAG, "stale call room reported; reconcile before retry: room=%s", stale_room_id);
            if (device_call_recover_stale_room(stale_room_id) == ESP_OK) {
                continue;
            }
        }
        ESP_LOGW(TAG,
                 "call request failed: http=%d code=%d ret=%s",
                 status_code,
                 business_code,
                 esp_err_to_name(ret));
        ESP_LOGW(CALL_FLOW_TAG,
                 "stage=call_request_done peer=%s attempt=%u http=%d code=%d ret=%s",
                 target_device_id,
                 attempt + 1U,
                 status_code,
                 business_code,
                 esp_err_to_name(ret));
        break;
    }

    free(response);
    return ret;
}

static void device_call_ring_timer_task(void *arg)
{
    device_call_request_ctx_t *ctx = (device_call_request_ctx_t *)arg;
    char room_id[96] = {0};
    device_call_action_t timeout_action = DEVICE_CALL_ACTION_CANCEL;
    const char *timeout_reason = "timeout";
    device_call_state_t phase = DEVICE_CALL_STATE_OUTGOING;
    uint32_t phase_elapsed_ms = 0U;
    bool expired = false;

    if (ctx == NULL) {
        vTaskDeleteWithCaps(NULL);
        return;
    }

    while (true) {
        bool phase_changed = false;
        bool finished = false;
        uint32_t timeout_ms = phase == DEVICE_CALL_STATE_CONNECTING ?
                                  DEVICE_CALL_CONNECT_TIMEOUT_MS :
                                  DEVICE_CALL_RING_TIMEOUT_MS;

        device_call_lock();
        if (s_call.generation != ctx->generation ||
            s_call.role != DEVICE_CALL_ROLE_CALLER ||
            (s_call.state != DEVICE_CALL_STATE_OUTGOING &&
             s_call.state != DEVICE_CALL_STATE_CONNECTING)) {
            finished = true;
        } else {
            if (s_call.state == DEVICE_CALL_STATE_CONNECTING &&
                phase != DEVICE_CALL_STATE_CONNECTING) {
                phase = DEVICE_CALL_STATE_CONNECTING;
                phase_elapsed_ms = 0U;
                timeout_ms = DEVICE_CALL_CONNECT_TIMEOUT_MS;
                strlcpy(room_id, s_call.room_id, sizeof(room_id));
                phase_changed = true;
            }
            if (phase_elapsed_ms >= timeout_ms) {
                strlcpy(room_id, s_call.room_id, sizeof(room_id));
                if (phase == DEVICE_CALL_STATE_CONNECTING) {
                    timeout_action = DEVICE_CALL_ACTION_HANGUP;
                    timeout_reason = "p2p_error";
                    device_call_show_pending_or_idle_locked("peer connection timed out");
                } else {
                    device_call_show_pending_or_idle_locked("call timed out");
                }
                device_call_next_generation_locked();
                expired = true;
                finished = true;
            }
        }
        device_call_unlock();

        if (phase_changed) {
            ESP_LOGI(CALL_FLOW_TAG,
                     "stage=p2p_wait_begin gen=%lu role=caller room=%s timeout_ms=%u",
                     (unsigned long)ctx->generation,
                     room_id[0] != '\0' ? room_id : "-",
                     (unsigned)DEVICE_CALL_CONNECT_TIMEOUT_MS);
        }
        if (finished) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(DEVICE_CALL_TIMER_POLL_INTERVAL_MS));
        phase_elapsed_ms += DEVICE_CALL_TIMER_POLL_INTERVAL_MS;
    }

    if (expired) {
        UBaseType_t stack_hwm = uxTaskGetStackHighWaterMark(NULL);
        if (phase == DEVICE_CALL_STATE_CONNECTING) {
            ESP_LOGW(CALL_FLOW_TAG,
                     "stage=p2p_wait_timeout gen=%lu role=caller room=%s timeout_ms=%u stack_hwm=%u",
                     (unsigned long)ctx->generation,
                     room_id,
                     (unsigned)DEVICE_CALL_CONNECT_TIMEOUT_MS,
                     (unsigned)stack_hwm);
        } else {
            ESP_LOGW(CALL_FLOW_TAG,
                     "stage=ring_timeout gen=%lu room=%s timeout_ms=%u stack_hwm=%u",
                     (unsigned long)ctx->generation,
                     room_id,
                     (unsigned)DEVICE_CALL_RING_TIMEOUT_MS,
                     (unsigned)stack_hwm);
        }
        ESP_LOGI(TAG,
                 "%s timed out: room=%s",
                 phase == DEVICE_CALL_STATE_CONNECTING ? "caller P2P connection" :
                                                         "outgoing call",
                 room_id);
        esp_err_t action_ret = device_call_post_room_action_async(timeout_action,
                                                                  room_id,
                                                                  timeout_reason);
        if (action_ret != ESP_OK) {
            ESP_LOGE(CALL_FLOW_TAG,
                     "stage=timeout_room_action_queue_failed action=%s room=%s ret=%s",
                     device_call_action_name(timeout_action),
                     room_id,
                     esp_err_to_name(action_ret));
        }
        device_call_reset_caller_media_gate();
        (void)rtc_transport_disconnect();
        device_call_notify_session_ended();
    }
    free(ctx);
    vTaskDeleteWithCaps(NULL);
}

static void device_call_request_failed(const device_call_request_ctx_t *ctx,
                                       esp_err_t error,
                                       const char *message)
{
    bool session_ended = false;

    device_call_lock();
    if (s_call.generation == ctx->generation) {
        device_call_set_error_locked(error, message);
        s_call.request_running = false;
        session_ended = true;
    }
    device_call_unlock();
    ESP_LOGW(CALL_FLOW_TAG,
             "stage=request_failed gen=%lu peer=%s error=%s message=%s",
             (unsigned long)ctx->generation,
             ctx->target_device_id,
             esp_err_to_name(error),
             message != NULL ? message : "-");
    device_call_reset_caller_media_gate();
    if (session_ended) {
        device_call_notify_session_ended();
    }
}

static void device_call_request_task(void *arg)
{
    device_call_request_ctx_t *ctx = (device_call_request_ctx_t *)arg;
    char room_id[96] = {0};
    esp_err_t ret = ESP_OK;

    if (ctx == NULL) {
        vTaskDeleteWithCaps(NULL);
        return;
    }

    ESP_LOGI(CALL_FLOW_TAG,
             "stage=request_worker_begin gen=%lu peer=%s",
             (unsigned long)ctx->generation,
             ctx->target_device_id);

    /*
     * The caller is the TiRTC listener.  Creating the cloud room before the
     * listener is ready lets the callee accept a call that this device cannot
     * receive, leaving MQTT and local state out of sync.
     */
    ret = device_call_wait_for_rtc_ready(ctx->generation);
    if (ret != ESP_OK) {
        device_call_request_failed(ctx, ret, "RTC listener not ready");
        free(ctx);
        vTaskDeleteWithCaps(NULL);
        return;
    }
    if (!device_call_generation_matches(ctx->generation)) {
        device_call_reset_caller_media_gate();
        free(ctx);
        vTaskDeleteWithCaps(NULL);
        return;
    }

    /* The callee confirms room_id with command 0x2000 before caller media starts. */
    rtc_transport_set_next_connection_auto_media(true);
    rtc_transport_set_next_connection_defer_media(true);

    ret = device_call_request_room(ctx->target_device_id,
                                   ctx->call_type,
                                   room_id,
                                   sizeof(room_id));
    if (ret != ESP_OK) {
        device_call_request_failed(ctx, ret, "call request failed");
        free(ctx);
        vTaskDeleteWithCaps(NULL);
        return;
    }

    if (!device_call_generation_matches(ctx->generation)) {
        /* The user may hang up while the HTTP request is still in flight. The
         * server room still has to be released even though local state moved on. */
        ESP_LOGW(CALL_FLOW_TAG,
                 "stage=request_abandoned gen=%lu room=%s peer=%s reason=generation_changed",
                 (unsigned long)ctx->generation,
                 room_id,
                 ctx->target_device_id);
        (void)device_call_post_room_action(DEVICE_CALL_ACTION_CANCEL,
                                           room_id,
                                           "local_cancel");
        device_call_reset_caller_media_gate();
        free(ctx);
        vTaskDeleteWithCaps(NULL);
        return;
    }

    device_call_lock();
    if (s_call.generation == ctx->generation) {
        strlcpy(s_call.room_id, room_id, sizeof(s_call.room_id));
        s_call.state = DEVICE_CALL_STATE_OUTGOING;
        s_call.last_error = ESP_OK;
        s_call.request_running = false;
        device_call_set_message_locked("waiting for answer");
    }
    device_call_unlock();

    if (device_call_generation_matches(ctx->generation)) {
        bool session_ended = false;
        device_call_request_ctx_t *timer_ctx = device_call_calloc_control(1, sizeof(*timer_ctx));

        ret = timer_ctx != NULL ? ESP_OK : ESP_ERR_NO_MEM;
        if (timer_ctx != NULL) {
            *timer_ctx = *ctx;
            ret = device_call_launch_task(device_call_ring_timer_task,
                                          "dev_call_timer",
                                          DEVICE_CALL_TIMER_TASK_STACK,
                                          timer_ctx);
            if (ret != ESP_OK) {
                free(timer_ctx);
            }
        }
        if (ret != ESP_OK) {
            device_call_lock();
            if (s_call.generation == ctx->generation) {
                device_call_next_generation_locked();
                device_call_set_error_locked(ret, "call timeout task unavailable");
                session_ended = true;
            }
            device_call_unlock();
            ESP_LOGW(TAG, "call timeout task unavailable: %s", esp_err_to_name(ret));
            if (session_ended) {
                (void)device_call_post_room_action_async(DEVICE_CALL_ACTION_CANCEL,
                                                         room_id,
                                                         "local_resource_error");
                device_call_reset_caller_media_gate();
                (void)rtc_transport_disconnect();
                device_call_notify_session_ended();
            }
            free(ctx);
            vTaskDeleteWithCaps(NULL);
            return;
        }
    }
    ESP_LOGI(CALL_FLOW_TAG,
             "stage=ringing gen=%lu role=caller room=%s peer=%s timeout_ms=%u",
             (unsigned long)ctx->generation,
             room_id,
             ctx->target_device_id,
             (unsigned)DEVICE_CALL_RING_TIMEOUT_MS);
    ESP_LOGI(TAG, "outgoing call created: room=%s peer=%s", room_id, ctx->target_device_id);

    free(ctx);
    vTaskDeleteWithCaps(NULL);
}

static device_call_contact_source_t device_call_contact_source_from_string(const char *source)
{
    if (source != NULL && strcmp(source, "manual") == 0) {
        return DEVICE_CALL_CONTACT_SOURCE_MANUAL;
    }
    if (source != NULL && strcmp(source, "auto") == 0) {
        return DEVICE_CALL_CONTACT_SOURCE_AUTO;
    }
    return DEVICE_CALL_CONTACT_SOURCE_UNKNOWN;
}

static const char *device_call_contact_source_name(device_call_contact_source_t source)
{
    switch (source) {
    case DEVICE_CALL_CONTACT_SOURCE_MANUAL:
        return "manual";
    case DEVICE_CALL_CONTACT_SOURCE_AUTO:
        return "auto";
    case DEVICE_CALL_CONTACT_SOURCE_UNKNOWN:
    default:
        return "unknown";
    }
}

static uint8_t device_call_contact_priority(const device_call_contact_t *contact)
{
    if (contact != NULL && contact->online) {
        return 0U;
    }
    if (contact != NULL && contact->source == DEVICE_CALL_CONTACT_SOURCE_MANUAL) {
        return 1U;
    }
    return 2U;
}

static bool device_call_select_contact(device_call_contact_t *contacts,
                                       uint8_t *count,
                                       const device_call_contact_t *candidate)
{
    uint8_t insert_at = 0;
    uint8_t candidate_priority = 0;

    if (contacts == NULL || count == NULL || candidate == NULL) {
        return false;
    }
    for (uint8_t index = 0; index < *count; ++index) {
        if (strcmp(contacts[index].device_id, candidate->device_id) == 0) {
            return false;
        }
    }

    candidate_priority = device_call_contact_priority(candidate);
    insert_at = *count;
    for (uint8_t index = 0; index < *count; ++index) {
        if (candidate_priority < device_call_contact_priority(&contacts[index])) {
            insert_at = index;
            break;
        }
    }
    if (*count >= DEVICE_CALL_CONTACT_MAX && insert_at >= DEVICE_CALL_CONTACT_MAX) {
        return false;
    }

    uint8_t shift_from = *count < DEVICE_CALL_CONTACT_MAX ?
        *count : (DEVICE_CALL_CONTACT_MAX - 1U);
    for (uint8_t index = shift_from; index > insert_at; --index) {
        contacts[index] = contacts[index - 1U];
    }
    contacts[insert_at] = *candidate;
    if (*count < DEVICE_CALL_CONTACT_MAX) {
        ++(*count);
    }
    return true;
}

static esp_err_t device_call_fetch_contacts(char *response,
                                            size_t response_size,
                                            device_call_contact_t *contacts,
                                            uint8_t *count_out,
                                            uint8_t *online_count_out)
{
    cJSON *root = NULL;
    cJSON *data = NULL;
    cJSON *items = NULL;
    uint8_t count = 0;
    uint8_t online_count = 0;
    uint16_t valid_count = 0;
    uint16_t online_total = 0;
    int status_code = 0;
    int business_code = -1;
    esp_err_t ret = ESP_OK;

    if (response == NULL || response_size < 2U || contacts == NULL ||
        count_out == NULL || online_count_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    response[0] = '\0';
    ret = device_call_http_get("/v1/call/device/contacts",
                               response,
                               response_size,
                               &status_code,
                               "call_contacts");
    if (ret == ESP_OK && status_code >= 200 && status_code < 300) {
        ret = device_call_parse_response(response, &root, &data, &business_code);
    } else if (ret == ESP_OK) {
        ret = ESP_FAIL;
    }
    if (ret != ESP_OK || !cJSON_IsObject(data)) {
        ESP_LOGW(TAG,
                 "contact refresh failed: http=%d code=%d ret=%s",
                 status_code,
                 business_code,
                 esp_err_to_name(ret));
        if (root != NULL) {
            cJSON_Delete(root);
        }
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_RESPONSE;
    }

    items = cJSON_GetObjectItemCaseSensitive(data, "contacts");
    if (cJSON_IsArray(items)) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, items) {
            device_call_contact_t candidate = {0};
            const char *device_id = device_call_json_string(item, "device_id");
            const char *type = device_call_json_string(item, "type");
            const char *remark = device_call_json_string(item, "remark");
            const char *source = device_call_json_string(item, "source");
            cJSON *online = cJSON_GetObjectItemCaseSensitive(item, "online");

            if ((type[0] != '\0' && strcmp(type, "device") != 0) ||
                device_id[0] == '\0' || strlen(device_id) >= sizeof(contacts[0].device_id)) {
                continue;
            }

            strlcpy(candidate.device_id, device_id, sizeof(candidate.device_id));
            strlcpy(candidate.remark, remark, sizeof(candidate.remark));
            candidate.online = cJSON_IsTrue(online);
            candidate.source = device_call_contact_source_from_string(source);
            ++valid_count;
            if (candidate.online) {
                ++online_total;
            }
            (void)device_call_select_contact(contacts, &count, &candidate);
        }
    }

    for (uint8_t index = 0; index < count; ++index) {
        if (contacts[index].online) {
            ++online_count;
        }
    }
    if (valid_count > count) {
        ESP_LOGW(TAG,
                 "device contact snapshot selected=%u total=%u online_total=%u policy=online-manual-auto",
                 (unsigned)count,
                 (unsigned)valid_count,
                 (unsigned)online_total);
    }

    cJSON_Delete(root);
    *count_out = count;
    *online_count_out = online_count;
    return ESP_OK;
}

static esp_err_t device_call_fetch_pending_contacts(char *response,
                                                    size_t response_size,
                                                    device_call_pending_contact_t *pending,
                                                    uint8_t *count_out)
{
    cJSON *root = NULL;
    cJSON *data = NULL;
    cJSON *items = NULL;
    uint8_t count = 0;
    int status_code = 0;
    int business_code = -1;
    esp_err_t ret = ESP_OK;

    if (response == NULL || response_size < 2U || pending == NULL || count_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    response[0] = '\0';
    ret = device_call_http_get("/v1/call/device/contacts/pending",
                               response,
                               response_size,
                               &status_code,
                               "call_contacts_pending");
    if (ret == ESP_OK && status_code >= 200 && status_code < 300) {
        ret = device_call_parse_response(response, &root, &data, &business_code);
    } else if (ret == ESP_OK) {
        ret = ESP_FAIL;
    }
    if (ret != ESP_OK || !cJSON_IsObject(data)) {
        ESP_LOGW(TAG,
                 "pending contact refresh failed: http=%d code=%d ret=%s",
                 status_code,
                 business_code,
                 esp_err_to_name(ret));
        if (root != NULL) {
            cJSON_Delete(root);
        }
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_RESPONSE;
    }

    items = cJSON_GetObjectItemCaseSensitive(data, "pending");
    if (cJSON_IsArray(items)) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, items) {
            const char *type = device_call_json_string(item, "type");
            const char *peer_device_id = device_call_json_string(item, "peer_device_id");
            const char *created_at = device_call_json_string(item, "created_at");
            bool duplicate = false;

            if ((type[0] != '\0' && strcmp(type, "device") != 0) ||
                peer_device_id[0] == '\0' ||
                strlen(peer_device_id) >= sizeof(pending[0].peer_device_id)) {
                continue;
            }
            for (uint8_t index = 0; index < count; ++index) {
                if (strcmp(pending[index].peer_device_id, peer_device_id) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                continue;
            }
            if (count >= DEVICE_CALL_CONTACT_MAX) {
                ESP_LOGW(TAG,
                         "pending contact snapshot truncated at %u entries",
                         (unsigned)DEVICE_CALL_CONTACT_MAX);
                break;
            }

            strlcpy(pending[count].peer_device_id,
                    peer_device_id,
                    sizeof(pending[count].peer_device_id));
            strlcpy(pending[count].created_at,
                    created_at,
                    sizeof(pending[count].created_at));
            ++count;
        }
    }

    cJSON_Delete(root);
    *count_out = count;
    return ESP_OK;
}

static esp_err_t device_call_refresh_contacts_now(uint32_t generation)
{
    device_call_contact_t contacts[DEVICE_CALL_CONTACT_MAX] = {0};
    device_call_pending_contact_t pending[DEVICE_CALL_CONTACT_MAX] = {0};
    char *response = NULL;
    uint8_t count = 0;
    uint8_t pending_count = 0;
    uint8_t online_count = 0;
    esp_err_t ret = ESP_OK;

    response = app_memory_calloc_psram(1, DEVICE_CALL_CONTACT_RESPONSE_MAX_LEN);
    if (response == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ret = device_call_fetch_contacts(response,
                                     DEVICE_CALL_CONTACT_RESPONSE_MAX_LEN,
                                     contacts,
                                     &count,
                                     &online_count);
    if (ret == ESP_OK) {
        ret = device_call_fetch_pending_contacts(response,
                                                 DEVICE_CALL_CONTACT_RESPONSE_MAX_LEN,
                                                 pending,
                                                 &pending_count);
    }
    free(response);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Accepted and pending lists are one cloud snapshot. Commit them together
     * so the UI cannot mix new contacts with stale approval requests. */
    device_call_lock();
    if (s_call.generation != generation) {
        device_call_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    memset(s_call.contacts, 0, sizeof(s_call.contacts));
    memset(s_call.pending_contacts, 0, sizeof(s_call.pending_contacts));
    memcpy(s_call.contacts, contacts, (size_t)count * sizeof(contacts[0]));
    memcpy(s_call.pending_contacts, pending, (size_t)pending_count * sizeof(pending[0]));
    s_call.contact_count = count;
    s_call.pending_contact_count = pending_count;
    s_call.contacts_ready = true;
    s_call.contacts_last_error = ESP_OK;
    device_call_unlock();

    ESP_LOGI(CALL_FLOW_TAG,
             "stage=contacts_ready accepted=%u pending=%u online=%u offline=%u",
             (unsigned)count,
             (unsigned)pending_count,
             (unsigned)online_count,
             (unsigned)(count - online_count));
    for (uint8_t index = 0; index < count; ++index) {
        APP_LOG_DETAIL(CALL_FLOW_TAG,
                       "stage=contact index=%u peer=%s online=%d source=%s",
                       (unsigned)index,
                       contacts[index].device_id,
                       contacts[index].online ? 1 : 0,
                       device_call_contact_source_name(contacts[index].source));
    }
    for (uint8_t index = 0; index < pending_count; ++index) {
        APP_LOG_DETAIL(CALL_FLOW_TAG,
                       "stage=contact_pending index=%u peer=%s",
                       (unsigned)index,
                       pending[index].peer_device_id);
    }
    return ESP_OK;
}

static esp_err_t device_call_write_contact_now(const char *method,
                                               const char *path,
                                               const char *trace_name,
                                               const char *first_name,
                                               const char *first_value,
                                               const char *second_name,
                                               const char *second_value,
                                               char *status,
                                               size_t status_size)
{
    cJSON *payload = NULL;
    char *body = NULL;
    char *response = NULL;
    cJSON *root = NULL;
    cJSON *data = NULL;
    int status_code = 0;
    int business_code = -1;
    esp_err_t ret = ESP_OK;

    if (method == NULL || path == NULL || trace_name == NULL ||
        first_name == NULL || first_value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    payload = cJSON_CreateObject();
    if (payload == NULL ||
        cJSON_AddStringToObject(payload, first_name, first_value) == NULL ||
        (second_name != NULL &&
         cJSON_AddStringToObject(payload,
                                second_name,
                                second_value != NULL ? second_value : "") == NULL)) {
        cJSON_Delete(payload);
        return ESP_ERR_NO_MEM;
    }
    body = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    response = app_memory_calloc_psram(1, DEVICE_CALL_HTTP_RESPONSE_MAX_LEN);
    if (response == NULL) {
        free(body);
        return ESP_ERR_NO_MEM;
    }

    if (strcmp(method, "POST") == 0) {
        ret = device_call_http_post(path,
                                    body,
                                    response,
                                    DEVICE_CALL_HTTP_RESPONSE_MAX_LEN,
                                    &status_code,
                                    trace_name);
    } else if (strcmp(method, "PUT") == 0) {
        ret = device_call_http_put(path,
                                   body,
                                   response,
                                   DEVICE_CALL_HTTP_RESPONSE_MAX_LEN,
                                   &status_code,
                                   trace_name);
    } else {
        ret = ESP_ERR_NOT_SUPPORTED;
    }
    free(body);

    if (ret == ESP_OK && status_code >= 200 && status_code < 300) {
        ret = device_call_parse_response(response, &root, &data, &business_code);
    } else if (ret == ESP_OK) {
        ret = ESP_FAIL;
    }

    if (ret == ESP_OK && status != NULL && status_size > 0U) {
        if (!cJSON_IsObject(data)) {
            ret = ESP_ERR_INVALID_RESPONSE;
        } else {
            const char *value = device_call_json_string(data, "status");
            if (value[0] == '\0') {
                ret = ESP_ERR_INVALID_RESPONSE;
            } else {
                strlcpy(status, value, status_size);
            }
        }
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "%s failed: http=%d code=%d ret=%s",
                 trace_name,
                 status_code,
                 business_code,
                 esp_err_to_name(ret));
    }
    if (root != NULL) {
        cJSON_Delete(root);
    }
    free(response);
    return ret;
}

static esp_err_t device_call_request_contact_now(const char *target_device_id,
                                                 char *status,
                                                 size_t status_size)
{
    return device_call_write_contact_now("POST",
                                         "/v1/call/device/contacts/request",
                                         "contact_request",
                                         "target_device_id",
                                         target_device_id,
                                         NULL,
                                         NULL,
                                         status,
                                         status_size);
}

static esp_err_t device_call_respond_contact_now(const char *peer_device_id,
                                                 bool accept,
                                                 char *status,
                                                 size_t status_size)
{
    return device_call_write_contact_now("POST",
                                         "/v1/call/device/contacts/respond",
                                         "contact_respond",
                                         "peer_device_id",
                                         peer_device_id,
                                         "action",
                                         accept ? "accept" : "reject",
                                         status,
                                         status_size);
}

static esp_err_t device_call_update_contact_remark_now(const char *peer_id,
                                                       const char *remark)
{
    return device_call_write_contact_now("PUT",
                                         "/v1/call/device/contacts/remark",
                                         "contact_remark",
                                         "peer_id",
                                         peer_id,
                                         "remark",
                                         remark,
                                         NULL,
                                         0U);
}

static esp_err_t device_call_delete_contact_now(const char *peer_device_id)
{
    char path[DEVICE_CALL_HTTP_PATH_MAX_LEN] = {0};
    char *response = NULL;
    cJSON *root = NULL;
    int status_code = 0;
    int business_code = -1;
    esp_err_t ret = ESP_OK;

    if (peer_device_id == NULL || peer_device_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    int written = snprintf(path,
                           sizeof(path),
                           "/v1/call/device/contacts?peer_id=%s",
                           peer_device_id);
    if (written <= 0 || written >= (int)sizeof(path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    response = app_memory_calloc_psram(1, DEVICE_CALL_HTTP_RESPONSE_MAX_LEN);
    if (response == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ret = device_call_http_request("DELETE",
                                   path,
                                   NULL,
                                   response,
                                   DEVICE_CALL_HTTP_RESPONSE_MAX_LEN,
                                   &status_code,
                                   "contact_delete");
    if (ret == ESP_OK && status_code >= 200 && status_code < 300) {
        ret = device_call_parse_response(response, &root, NULL, &business_code);
    } else if (ret == ESP_OK) {
        ret = ESP_FAIL;
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "contact_delete failed: http=%d code=%d ret=%s",
                 status_code,
                 business_code,
                 esp_err_to_name(ret));
    }
    if (root != NULL) {
        cJSON_Delete(root);
    }
    free(response);
    return ret;
}

static void device_call_contact_task(void *arg)
{
    device_call_contact_ctx_t *ctx = (device_call_contact_ctx_t *)arg;
    esp_err_t ret = ESP_ERR_INVALID_ARG;
    char mutation_status[16] = {0};

    if (ctx == NULL) {
        vTaskDeleteWithCaps(NULL);
        return;
    }

    if (ctx->work == DEVICE_CALL_CONTACT_WORK_REFRESH) {
        bool rerun = false;

        do {
            uint32_t elapsed_ms = 0;
            while (device_call_generation_matches(ctx->generation) &&
                   !device_online_is_online() &&
                   elapsed_ms < DEVICE_CALL_ONLINE_READY_TIMEOUT_MS) {
                vTaskDelay(pdMS_TO_TICKS(DEVICE_CALL_POLL_INTERVAL_MS));
                elapsed_ms += DEVICE_CALL_POLL_INTERVAL_MS;
            }
            if (!device_call_generation_matches(ctx->generation)) {
                ret = ESP_ERR_INVALID_STATE;
            } else {
                ret = device_online_is_online() ?
                      device_call_refresh_contacts_now(ctx->generation) : ESP_ERR_TIMEOUT;
            }
            device_call_lock();
            rerun = s_call.generation == ctx->generation &&
                    s_call.contacts_refresh_pending;
            if (s_call.generation == ctx->generation) {
                s_call.contacts_refresh_pending = false;
                s_call.contacts_last_error = ret;
                if (!rerun) {
                    s_call.contacts_refresh_running = false;
                }
            }
            device_call_unlock();

            if (rerun) {
                ESP_LOGI(CALL_FLOW_TAG,
                         "stage=contacts_refresh_coalesced gen=%lu",
                         (unsigned long)ctx->generation);
            }
        } while (rerun);

        free(ctx);
        vTaskDeleteWithCaps(NULL);
        return;
    }

    uint32_t elapsed_ms = 0;
    bool refresh_running = false;

    /* Serialize mutations behind a running refresh so the resulting snapshot
     * always reflects the accepted server mutation. */
    do {
        if (!device_call_generation_matches(ctx->generation)) {
            break;
        }
        device_call_lock();
        refresh_running = s_call.contacts_refresh_running;
        device_call_unlock();
        if (!refresh_running) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(DEVICE_CALL_POLL_INTERVAL_MS));
        elapsed_ms += DEVICE_CALL_POLL_INTERVAL_MS;
    } while (elapsed_ms < DEVICE_CALL_ONLINE_READY_TIMEOUT_MS);

    if (!device_call_generation_matches(ctx->generation)) {
        ret = ESP_ERR_INVALID_STATE;
    } else if (refresh_running) {
        ret = ESP_ERR_TIMEOUT;
    } else if (!device_online_is_online()) {
        ret = ESP_ERR_INVALID_STATE;
    } else {
        switch (ctx->work) {
        case DEVICE_CALL_CONTACT_WORK_REQUEST:
            ret = device_call_request_contact_now(ctx->target_device_id,
                                                  mutation_status,
                                                  sizeof(mutation_status));
            break;
        case DEVICE_CALL_CONTACT_WORK_RESPOND:
            ret = device_call_respond_contact_now(ctx->target_device_id,
                                                  ctx->accept,
                                                  mutation_status,
                                                  sizeof(mutation_status));
            break;
        case DEVICE_CALL_CONTACT_WORK_REMARK:
            ret = device_call_update_contact_remark_now(ctx->target_device_id,
                                                        ctx->remark);
            break;
        case DEVICE_CALL_CONTACT_WORK_DELETE:
            ret = device_call_delete_contact_now(ctx->target_device_id);
            break;
        default:
            ret = ESP_ERR_NOT_SUPPORTED;
            break;
        }
        if (ret == ESP_OK) {
            ESP_LOGI(TAG,
                     "contact mutation completed: work=%u target=%s status=%s",
                     (unsigned)ctx->work,
                     ctx->target_device_id,
                     mutation_status[0] != '\0' ? mutation_status : "ok");
            ret = device_call_refresh_contacts_now(ctx->generation);
        }
    }

    bool schedule_refresh = false;
    device_call_lock();
    if (s_call.generation == ctx->generation) {
        s_call.contact_mutation_running = false;
        s_call.contacts_last_error = ret;
        schedule_refresh = s_call.contacts_refresh_pending;
        s_call.contacts_refresh_pending = false;
    }
    device_call_unlock();

    if (schedule_refresh) {
        esp_err_t refresh_ret = device_call_refresh_contacts_async();
        if (refresh_ret != ESP_OK) {
            ESP_LOGW(TAG,
                     "deferred contact refresh not scheduled: %s",
                     esp_err_to_name(refresh_ret));
        }
    }

    free(ctx);
    vTaskDeleteWithCaps(NULL);
}

static void device_call_handle_incoming(const cJSON *payload, uint32_t message_generation)
{
    const char *room_id = device_call_json_string(payload, "room_id");
    const char *caller_id = device_call_json_string(payload, "caller_id");
    const char *call_type = device_call_json_string(payload, "call_type");
    device_call_state_t state = DEVICE_CALL_STATE_IDLE;
    device_call_can_accept_incoming_cb_t can_accept_incoming = NULL;
    void *callback_ctx = NULL;
    uint32_t generation = 0;
    bool local_busy = false;
    bool transport_busy = false;
    bool foreground_available = true;
    bool request_running = false;
    bool accept_running = false;
    bool switch_running = false;

    if (room_id[0] == '\0' || caller_id[0] == '\0' ||
        strlen(room_id) >= sizeof(s_call.pending_room_id) ||
        strlen(caller_id) >= sizeof(s_call.pending_caller_id)) {
        ESP_LOGW(TAG, "invalid call_incoming payload");
        return;
    }

    device_call_lock();
    if (!s_call.ingress_enabled || s_call.generation != message_generation) {
        device_call_unlock();
        return;
    }
    if (s_call.pending_incoming && strcmp(s_call.pending_room_id, room_id) == 0) {
        state = s_call.state;
        generation = s_call.generation;
        device_call_unlock();
        ESP_LOGI(CALL_FLOW_TAG,
                 "stage=incoming_duplicate gen=%lu state=%s room=%s peer=%s",
                 (unsigned long)generation,
                 device_call_state_name(state),
                 room_id,
                 caller_id);
        return;
    }
    local_busy = s_call.pending_incoming ||
                 s_call.request_running ||
                 s_call.accept_running ||
                 s_call.switch_disconnect_in_progress ||
                 (s_call.state != DEVICE_CALL_STATE_IDLE &&
                  s_call.state != DEVICE_CALL_STATE_ERROR);
    state = s_call.state;
    request_running = s_call.request_running;
    accept_running = s_call.accept_running;
    switch_running = s_call.switch_disconnect_in_progress;
    can_accept_incoming = s_call.can_accept_incoming;
    callback_ctx = s_call.callback_ctx;
    device_call_unlock();

    if (can_accept_incoming != NULL) {
        foreground_available = can_accept_incoming(callback_ctx);
    }
    transport_busy = device_call_transport_is_connected();
    if (local_busy || transport_busy || !foreground_available) {
        esp_err_t reject_ret = device_call_post_room_action_async(DEVICE_CALL_ACTION_REJECT,
                                                                  room_id,
                                                                  "busy");
        ESP_LOGI(CALL_FLOW_TAG,
                 "stage=incoming_busy_reject gen=%lu room=%s peer=%s state=%s request=%d accept=%d switch=%d local_busy=%d transport_busy=%d foreground_available=%d ret=%s",
                 (unsigned long)message_generation,
                 room_id,
                 caller_id,
                 device_call_state_name(state),
                 request_running ? 1 : 0,
                 accept_running ? 1 : 0,
                 switch_running ? 1 : 0,
                 local_busy ? 1 : 0,
                 transport_busy ? 1 : 0,
                 foreground_available ? 1 : 0,
                 esp_err_to_name(reject_ret));
        return;
    }

    /* Revalidate after the application policy callback; another MQTT event or
     * UI action may have claimed the single foreground media session. */
    transport_busy = device_call_transport_is_connected();
    device_call_lock();
    if (!s_call.ingress_enabled || s_call.generation != message_generation) {
        device_call_unlock();
        return;
    }
    if (s_call.pending_incoming && strcmp(s_call.pending_room_id, room_id) == 0) {
        device_call_unlock();
        return;
    }
    local_busy = s_call.pending_incoming ||
                 s_call.request_running ||
                 s_call.accept_running ||
                 s_call.switch_disconnect_in_progress ||
                 (s_call.state != DEVICE_CALL_STATE_IDLE &&
                  s_call.state != DEVICE_CALL_STATE_ERROR);
    state = s_call.state;
    request_running = s_call.request_running;
    accept_running = s_call.accept_running;
    switch_running = s_call.switch_disconnect_in_progress;
    if (local_busy || transport_busy) {
        device_call_unlock();
        esp_err_t reject_ret = device_call_post_room_action_async(DEVICE_CALL_ACTION_REJECT,
                                                                  room_id,
                                                                  "busy");
        ESP_LOGI(CALL_FLOW_TAG,
                 "stage=incoming_race_busy_reject gen=%lu room=%s peer=%s state=%s request=%d accept=%d switch=%d local_busy=%d transport_busy=%d ret=%s",
                 (unsigned long)message_generation,
                 room_id,
                 caller_id,
                 device_call_state_name(state),
                 request_running ? 1 : 0,
                 accept_running ? 1 : 0,
                 switch_running ? 1 : 0,
                 local_busy ? 1 : 0,
                 transport_busy ? 1 : 0,
                 esp_err_to_name(reject_ret));
        return;
    }
    s_call.pending_incoming = true;
    strlcpy(s_call.pending_room_id, room_id, sizeof(s_call.pending_room_id));
    strlcpy(s_call.pending_caller_id, caller_id, sizeof(s_call.pending_caller_id));
    strlcpy(s_call.pending_call_type,
            call_type[0] != '\0' ? call_type : "audio",
            sizeof(s_call.pending_call_type));

    if (s_call.state == DEVICE_CALL_STATE_IDLE || s_call.state == DEVICE_CALL_STATE_ERROR) {
        s_call.state = DEVICE_CALL_STATE_INCOMING;
        s_call.role = DEVICE_CALL_ROLE_CALLEE;
        strlcpy(s_call.room_id, room_id, sizeof(s_call.room_id));
        strlcpy(s_call.peer_device_id, caller_id, sizeof(s_call.peer_device_id));
        strlcpy(s_call.call_type, s_call.pending_call_type, sizeof(s_call.call_type));
        s_call.last_error = ESP_OK;
        device_call_set_message_locked("incoming call");
    }
    state = s_call.state;
    generation = s_call.generation;
    device_call_unlock();
    esp_err_t ringtone_ret = device_call_ringtone_start();
    if (ringtone_ret != ESP_OK) {
        ESP_LOGW(CALL_FLOW_TAG,
                 "stage=ringtone_start_failed gen=%lu room=%s ret=%s",
                 (unsigned long)generation,
                 room_id,
                 esp_err_to_name(ringtone_ret));
    } else if (!device_call_pending_incoming_matches(generation, room_id)) {
        /* The cancel/reset path may win between committing the incoming state
         * and starting the audio task. Never leave a stale ringtone running. */
        esp_err_t stop_ret = device_call_ringtone_stop();
        if (stop_ret != ESP_OK) {
            ESP_LOGW(CALL_FLOW_TAG,
                     "stage=ringtone_stop_failed reason=stale_incoming room=%s ret=%s",
                     room_id,
                     esp_err_to_name(stop_ret));
        }
    }
    ESP_LOGI(CALL_FLOW_TAG,
             "stage=incoming_received gen=%lu role=callee state=%s room=%s peer=%s type=%s",
             (unsigned long)generation,
             device_call_state_name(state),
             room_id,
             caller_id,
             call_type[0] != '\0' ? call_type : "audio");
    ESP_LOGI(TAG, "incoming call: room=%s caller=%s type=%s", room_id, caller_id, call_type);
}

static void device_call_handle_room_cancel(const cJSON *payload, uint32_t message_generation)
{
    const char *room_id = device_call_json_string(payload, "room_id");
    const char *reason = device_call_json_string(payload, "reason");
    device_call_role_t role = DEVICE_CALL_ROLE_NONE;
    device_call_state_t state = DEVICE_CALL_STATE_IDLE;
    bool pending_cleared = false;
    bool close_transport = false;

    if (room_id[0] == '\0') {
        return;
    }

    device_call_lock();
    if (!s_call.ingress_enabled || s_call.generation != message_generation) {
        device_call_unlock();
        return;
    }
    role = s_call.role;
    state = s_call.state;
    if (s_call.pending_incoming && strcmp(s_call.pending_room_id, room_id) == 0) {
        pending_cleared = true;
        s_call.pending_incoming = false;
        s_call.pending_room_id[0] = '\0';
        s_call.pending_caller_id[0] = '\0';
        s_call.pending_call_type[0] = '\0';
        if (s_call.state == DEVICE_CALL_STATE_INCOMING && strcmp(s_call.room_id, room_id) == 0) {
            device_call_show_pending_or_idle_locked("incoming call canceled");
        }
    }
    if (s_call.room_id[0] != '\0' && strcmp(s_call.room_id, room_id) == 0 &&
        s_call.state != DEVICE_CALL_STATE_INCOMING) {
        close_transport = true;
        device_call_next_generation_locked();
        device_call_show_pending_or_idle_locked("call ended");
    }
    device_call_unlock();

    if (pending_cleared) {
        esp_err_t ringtone_ret = device_call_ringtone_stop();
        if (ringtone_ret != ESP_OK) {
            ESP_LOGW(CALL_FLOW_TAG,
                     "stage=ringtone_stop_failed reason=room_cancel room=%s ret=%s",
                     room_id,
                     esp_err_to_name(ringtone_ret));
        }
    }

    if (close_transport) {
        device_call_reset_caller_media_gate();
        (void)rtc_transport_disconnect();
        device_call_notify_session_ended();
    }
    ESP_LOGI(CALL_FLOW_TAG,
             "stage=room_cancel_rx role=%s state=%s room=%s reason=%s pending_cleared=%d close_transport=%d",
             device_call_role_name(role),
             device_call_state_name(state),
             room_id,
             reason[0] != '\0' ? reason : "-",
             pending_cleared ? 1 : 0,
             close_transport ? 1 : 0);
    ESP_LOGI(TAG,
             "room canceled: room=%s reason=%s",
             room_id,
             reason[0] != '\0' ? reason : "-");
}

static void device_call_handle_reject(const cJSON *payload, uint32_t message_generation)
{
    const char *room_id = device_call_json_string(payload, "room_id");
    const char *reason = device_call_json_string(payload, "reason");
    bool matched = false;

    device_call_lock();
    if (!s_call.ingress_enabled || s_call.generation != message_generation) {
        device_call_unlock();
        return;
    }
    if (room_id[0] != '\0' && strcmp(s_call.room_id, room_id) == 0 &&
        s_call.role == DEVICE_CALL_ROLE_CALLER &&
        (s_call.state == DEVICE_CALL_STATE_OUTGOING ||
         s_call.state == DEVICE_CALL_STATE_CONNECTING)) {
        /* One callee rejected a multi-target call. room_cancel is terminal. */
        device_call_set_message_locked("one peer rejected");
        matched = true;
    }
    device_call_unlock();

    if (matched) {
        ESP_LOGI(CALL_FLOW_TAG,
                 "stage=call_reject_rx room=%s reason=%s matched=1 terminal=0",
                 room_id,
                 reason[0] != '\0' ? reason : "-");
        ESP_LOGI(TAG,
                 "one outgoing call target rejected: room=%s reason=%s",
                 room_id,
                 reason[0] != '\0' ? reason : "-");
    } else {
        ESP_LOGW(CALL_FLOW_TAG,
                 "stage=call_reject_rx room=%s reason=%s matched=0",
                 room_id[0] != '\0' ? room_id : "-",
                 reason[0] != '\0' ? reason : "-");
    }
}

static void device_call_try_complete_caller_handshake(uint32_t generation, const char *source)
{
    tirtc_conn_t conn = NULL;
    char room_id[96] = {0};
    char call_type[16] = {0};
    bool ready = false;

    device_call_lock();
    ready = s_call.generation == generation &&
            s_call.role == DEVICE_CALL_ROLE_CALLER &&
            (s_call.state == DEVICE_CALL_STATE_OUTGOING ||
             s_call.state == DEVICE_CALL_STATE_CONNECTING) &&
            s_call.caller_peer_answered &&
            s_call.caller_transport_accepted &&
            s_call.rtc_conn != NULL &&
            !s_call.caller_media_activation_running &&
            s_call.room_id[0] != '\0';
    if (ready) {
        s_call.caller_media_activation_running = true;
        conn = s_call.rtc_conn;
        strlcpy(room_id, s_call.room_id, sizeof(room_id));
        strlcpy(call_type, s_call.call_type, sizeof(call_type));
    }
    device_call_unlock();

    if (!ready) {
        return;
    }

    esp_err_t media_ret = device_call_activate_media(call_type);
    bool completed = false;
    bool failed = false;

    device_call_lock();
    if (s_call.generation == generation) {
        s_call.caller_media_activation_running = false;
        if (media_ret == ESP_OK &&
            s_call.role == DEVICE_CALL_ROLE_CALLER &&
            (s_call.state == DEVICE_CALL_STATE_OUTGOING ||
             s_call.state == DEVICE_CALL_STATE_CONNECTING) &&
            s_call.caller_peer_answered &&
            s_call.caller_transport_accepted &&
            s_call.rtc_conn == conn &&
            strcmp(s_call.room_id, room_id) == 0) {
            s_call.state = DEVICE_CALL_STATE_IN_CALL;
            s_call.last_error = ESP_OK;
            device_call_set_message_locked("call connected");
            completed = true;
        } else if (media_ret != ESP_OK &&
                   s_call.role == DEVICE_CALL_ROLE_CALLER &&
                   (s_call.state == DEVICE_CALL_STATE_OUTGOING ||
                    s_call.state == DEVICE_CALL_STATE_CONNECTING)) {
            device_call_next_generation_locked();
            device_call_set_error_locked(media_ret, "call media activation failed");
            failed = true;
        }
    }
    device_call_unlock();

    ESP_LOGI(CALL_FLOW_TAG,
             "stage=caller_handshake gen=%lu room=%s source=%s peer_answered=1 p2p=1 media=%s completed=%d",
             (unsigned long)generation,
             room_id,
             source != NULL ? source : "-",
             esp_err_to_name(media_ret),
             completed ? 1 : 0);
    if (completed) {
        ESP_LOGI(CALL_FLOW_TAG,
                 "stage=in_call gen=%lu role=caller room=%s source=%s",
                 (unsigned long)generation,
                 room_id,
                 source != NULL ? source : "peer+p2p");
    } else if (failed) {
        device_call_reset_caller_media_gate();
        (void)rtc_transport_disconnect();
        device_call_notify_session_ended();
    }
}

static void device_call_handle_answered(const cJSON *payload, uint32_t message_generation)
{
    const char *room_id = device_call_json_string(payload, "room_id");
    const char *callee_id = device_call_json_string(payload, "callee_id");
    bool matched = false;

    device_call_lock();
    if (!s_call.ingress_enabled || s_call.generation != message_generation) {
        device_call_unlock();
        return;
    }
    if (room_id[0] != '\0' && strcmp(s_call.room_id, room_id) == 0 &&
        s_call.role == DEVICE_CALL_ROLE_CALLER &&
         (s_call.state == DEVICE_CALL_STATE_OUTGOING ||
          s_call.state == DEVICE_CALL_STATE_CONNECTING)) {
        matched = true;
        s_call.state = DEVICE_CALL_STATE_CONNECTING;
        s_call.caller_peer_answered = true;
        if (callee_id[0] != '\0') {
            strlcpy(s_call.peer_device_id, callee_id, sizeof(s_call.peer_device_id));
        }
        device_call_set_message_locked("peer is connecting");
    }
    device_call_unlock();
    ESP_LOGI(CALL_FLOW_TAG,
             "stage=callee_answered_rx room=%s peer=%s matched=%d",
             room_id[0] != '\0' ? room_id : "-",
             callee_id[0] != '\0' ? callee_id : "-",
             matched ? 1 : 0);
    if (matched) {
        device_call_try_complete_caller_handshake(message_generation, "cloud-answer");
    }
}

static void device_call_mqtt_message(const char *topic,
                                     const char *payload,
                                     size_t payload_len,
                                     void *ctx)
{
    char *json = NULL;
    cJSON *root = NULL;
    cJSON *event_payload = NULL;
    const char *channel = "";
    const char *type = "";
    uint32_t message_generation = 0;

    (void)topic;
    (void)ctx;

    device_call_lock();
    if (!s_call.ingress_enabled) {
        device_call_unlock();
        return;
    }
    message_generation = s_call.generation;
    device_call_unlock();

    if (payload == NULL || payload_len == 0U || payload_len > 4096U) {
        if (payload_len > 4096U) {
            ESP_LOGW(CALL_FLOW_TAG,
                     "stage=mqtt_drop reason=oversized payload_len=%u",
                     (unsigned)payload_len);
        }
        return;
    }
    json = app_memory_alloc_psram(payload_len + 1U);
    if (json == NULL) {
        ESP_LOGW(CALL_FLOW_TAG,
                 "stage=mqtt_drop reason=no_memory payload_len=%u",
                 (unsigned)payload_len);
        return;
    }
    memcpy(json, payload, payload_len);
    json[payload_len] = '\0';
    root = cJSON_Parse(json);
    free(json);
    if (root == NULL) {
        ESP_LOGW(CALL_FLOW_TAG,
                 "stage=mqtt_drop reason=invalid_json payload_len=%u",
                 (unsigned)payload_len);
        return;
    }

    channel = device_call_json_string(root, "channel");
    type = device_call_json_string(root, "type");
    event_payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    if (!cJSON_IsObject(event_payload)) {
        event_payload = cJSON_GetObjectItemCaseSensitive(root, "msg");
    }
    if ((channel[0] != '\0' && strcmp(channel, "device") != 0) ||
        type[0] == '\0' || !cJSON_IsObject(event_payload)) {
        cJSON_Delete(root);
        return;
    }

    bool recognized = strcmp(type, "call_incoming") == 0 ||
                      strcmp(type, "room_cancel") == 0 ||
                      strcmp(type, "call_reject") == 0 ||
                      strcmp(type, "callee_answered") == 0 ||
                      strcmp(type, "callers_update") == 0;
    if (recognized) {
        const char *room_id = device_call_json_string(event_payload, "room_id");
        const char *peer_id = device_call_json_string(event_payload, "caller_id");
        const char *action = device_call_json_string(event_payload, "action");
        const char *contact_type = device_call_json_string(event_payload, "contact_type");
        if (peer_id[0] == '\0') {
            peer_id = device_call_json_string(event_payload, "callee_id");
        }
        if (peer_id[0] == '\0') {
            peer_id = device_call_json_string(event_payload, "peer_id");
        }
        ESP_LOGI(CALL_FLOW_TAG,
                 "stage=mqtt_rx type=%s room=%s peer=%s action=%s contact_type=%s topic=%s",
                 type,
                 room_id[0] != '\0' ? room_id : "-",
                 peer_id[0] != '\0' ? peer_id : "-",
                 action[0] != '\0' ? action : "-",
                 contact_type[0] != '\0' ? contact_type : "-",
                 topic != NULL ? topic : "-");
    }

    if (strcmp(type, "call_incoming") == 0) {
        device_call_handle_incoming(event_payload, message_generation);
    } else if (strcmp(type, "room_cancel") == 0) {
        device_call_handle_room_cancel(event_payload, message_generation);
    } else if (strcmp(type, "call_reject") == 0) {
        device_call_handle_reject(event_payload, message_generation);
    } else if (strcmp(type, "callee_answered") == 0) {
        device_call_handle_answered(event_payload, message_generation);
    } else if (strcmp(type, "callers_update") == 0 &&
               device_call_generation_matches(message_generation)) {
        const char *contact_type = device_call_json_string(event_payload, "contact_type");

        /* Empty payloads are accepted for compatibility. Explicit VoIP
         * updates belong to the independent WeChat contact service. */
        if (contact_type[0] == '\0' || strcmp(contact_type, "device") == 0) {
            esp_err_t ret = device_call_refresh_contacts_async();
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "contact refresh notification failed: %s", esp_err_to_name(ret));
            }
        }
    }
    cJSON_Delete(root);
}

static bool device_call_on_rtc_command(tirtc_conn_t conn,
                                       uint32_t cmdw,
                                       const void *data,
                                       uint32_t data_len,
                                       void *ctx)
{
    uint16_t command = (uint16_t)(cmdw & ~TIRTC_SESSION_CMD_RESP_BIT);
    char room_id[96] = {0};
    bool matched = false;
    bool expected = false;
    bool owns_command = false;
    bool already_connected = false;
    uint32_t generation = 0;

    (void)ctx;

    if (command != TIRTC_SESSION_CMD_DEVICE_CALL_CONNECTED &&
        command != TIRTC_SESSION_CMD_DEVICE_CALL_HANGUP) {
        return false;
    }

    /*
     * 0x2000/0x2001 are shared by device calls and WeChat VoIP. An observer
     * may consume a command only while its own session is active; otherwise
     * dispatch must continue to the WeChat observer.
     */
    device_call_lock();
    owns_command = s_call.role != DEVICE_CALL_ROLE_NONE &&
                   (s_call.state == DEVICE_CALL_STATE_OUTGOING ||
                    s_call.state == DEVICE_CALL_STATE_CONNECTING ||
                    s_call.state == DEVICE_CALL_STATE_IN_CALL);
    device_call_unlock();
    if (!owns_command) {
        return false;
    }

    if (command == TIRTC_SESSION_CMD_DEVICE_CALL_HANGUP) {
        char active_room[96] = {0};
        device_call_role_t role = DEVICE_CALL_ROLE_NONE;
        bool active = false;

        device_call_lock();
        /* The transport dispatch layer has already verified that conn is the
         * current connection, so OUTGOING remains valid for the rare case in
         * which P2P control arrives before the MQTT answered notification. */
        active = (s_call.state == DEVICE_CALL_STATE_OUTGOING ||
                  s_call.state == DEVICE_CALL_STATE_CONNECTING ||
                  s_call.state == DEVICE_CALL_STATE_IN_CALL) &&
                 s_call.rtc_conn != NULL && s_call.rtc_conn == conn;
        if (active) {
            strlcpy(active_room, s_call.room_id, sizeof(active_room));
            role = s_call.role;
            device_call_next_generation_locked();
            device_call_show_pending_or_idle_locked("peer hung up");
        }
        device_call_unlock();
        if (active) {
            device_call_reset_caller_media_gate();
            (void)rtc_transport_disconnect();
            device_call_notify_session_ended();
        }
        ESP_LOGI(CALL_FLOW_TAG,
                 "stage=peer_hangup_rx role=%s room=%s cmd=0x%04x active=%d",
                 device_call_role_name(role),
                 active_room[0] != '\0' ? active_room : "-",
                 TIRTC_SESSION_CMD_DEVICE_CALL_HANGUP,
                 active ? 1 : 0);
        return true;
    }
    if (data == NULL || data_len == 0U || data_len >= 256U) {
        return true;
    }

    char json[256] = {0};
    memcpy(json, data, data_len);
    cJSON *root = cJSON_Parse(json);
    if (root != NULL) {
        strlcpy(room_id, device_call_json_string(root, "room_id"), sizeof(room_id));
        cJSON_Delete(root);
    }

    device_call_lock();
    expected = s_call.role == DEVICE_CALL_ROLE_CALLER &&
               (s_call.state == DEVICE_CALL_STATE_OUTGOING ||
                s_call.state == DEVICE_CALL_STATE_CONNECTING ||
                s_call.state == DEVICE_CALL_STATE_IN_CALL);
    matched = expected && room_id[0] != '\0' && s_call.room_id[0] != '\0' &&
              strcmp(room_id, s_call.room_id) == 0 &&
              (s_call.rtc_conn == NULL || s_call.rtc_conn == conn);
    if (matched) {
        already_connected = s_call.state == DEVICE_CALL_STATE_IN_CALL;
        generation = s_call.generation;
        s_call.rtc_conn = conn;
        s_call.caller_peer_answered = true;
        s_call.caller_transport_accepted = true;
        if (!already_connected) {
            s_call.state = DEVICE_CALL_STATE_CONNECTING;
            device_call_set_message_locked("peer is connecting");
        }
    } else if (expected) {
        device_call_set_message_locked("waiting for current room confirmation");
    }
    device_call_unlock();

    if (!expected) {
        ESP_LOGI(CALL_FLOW_TAG,
                 "stage=connected_notice_rx room=%s ignored=not_expected cmd=0x%04x",
                 room_id[0] != '\0' ? room_id : "-",
                 TIRTC_SESSION_CMD_DEVICE_CALL_CONNECTED);
        return true;
    }
    if (!matched) {
        ESP_LOGW(CALL_FLOW_TAG,
                 "stage=connected_notice_rx room=%s matched=0 cmd=0x%04x",
                 room_id[0] != '\0' ? room_id : "-",
                 TIRTC_SESSION_CMD_DEVICE_CALL_CONNECTED);
        ESP_LOGW(TAG, "ignore stale 0x2000 room confirmation: room=%s", room_id);
        return true;
    }

    ESP_LOGI(CALL_FLOW_TAG,
             "stage=connected_notice_rx room=%s matched=1 already_connected=%d cmd=0x%04x",
             room_id,
             already_connected ? 1 : 0,
             TIRTC_SESSION_CMD_DEVICE_CALL_CONNECTED);
    if (!already_connected) {
        device_call_try_complete_caller_handshake(generation, "legacy-command");
    }
    return true;
}

static void device_call_on_rtc_connection_accepted(tirtc_conn_t conn, void *ctx)
{
    uint32_t generation = 0;
    char room_id[96] = {0};
    bool tracked = false;
    bool peer_answered = false;

    (void)ctx;

    device_call_lock();
    if (s_call.role == DEVICE_CALL_ROLE_CALLER &&
        (s_call.state == DEVICE_CALL_STATE_OUTGOING ||
         s_call.state == DEVICE_CALL_STATE_CONNECTING) &&
        s_call.room_id[0] != '\0' &&
        (s_call.rtc_conn == NULL || s_call.rtc_conn == conn)) {
        s_call.rtc_conn = conn;
        s_call.caller_transport_accepted = true;
        generation = s_call.generation;
        peer_answered = s_call.caller_peer_answered;
        strlcpy(room_id, s_call.room_id, sizeof(room_id));
        tracked = true;
    } else if (s_call.role == DEVICE_CALL_ROLE_CALLEE &&
               (s_call.state == DEVICE_CALL_STATE_CONNECTING ||
                s_call.state == DEVICE_CALL_STATE_IN_CALL) &&
               (s_call.rtc_conn == NULL || s_call.rtc_conn == conn)) {
        s_call.rtc_conn = conn;
    }
    device_call_unlock();

    if (!tracked) {
        return;
    }

    ESP_LOGI(CALL_FLOW_TAG,
             "stage=p2p_accepted gen=%lu role=caller room=%s peer_answered=%d",
             (unsigned long)generation,
             room_id,
             peer_answered ? 1 : 0);
    if (peer_answered) {
        device_call_try_complete_caller_handshake(generation, "p2p-accepted");
    }
}

static void device_call_on_rtc_connection_error(tirtc_conn_t conn, int error, void *ctx)
{
    char room_id[96] = {0};
    device_call_role_t role = DEVICE_CALL_ROLE_NONE;
    device_call_state_t state = DEVICE_CALL_STATE_IDLE;
    bool active = false;
    bool pending_caller = false;

    (void)ctx;

    device_call_lock();
    if (s_call.switch_disconnect_in_progress) {
        device_call_unlock();
        ESP_LOGI(CALL_FLOW_TAG, "stage=rtc_error ignored=call_switch sdk_error=%d", error);
        return;
    }
    pending_caller = s_call.role == DEVICE_CALL_ROLE_CALLER &&
                     (s_call.state == DEVICE_CALL_STATE_OUTGOING ||
                      s_call.state == DEVICE_CALL_STATE_CONNECTING) &&
                     s_call.rtc_conn != NULL && s_call.rtc_conn == conn;
    if (pending_caller) {
        s_call.rtc_conn = NULL;
        s_call.caller_transport_accepted = false;
        device_call_set_message_locked("peer connection retrying");
    }
    active = s_call.state == DEVICE_CALL_STATE_IN_CALL &&
             s_call.rtc_conn != NULL && s_call.rtc_conn == conn;
    if (active) {
        strlcpy(room_id, s_call.room_id, sizeof(room_id));
        role = s_call.role;
        state = s_call.state;
        device_call_next_generation_locked();
        device_call_set_error_locked(ESP_FAIL, "RTC connection failed");
    }
    device_call_unlock();

    if (!active) {
        APP_LOG_DETAIL(CALL_FLOW_TAG,
                       "stage=rtc_error ignored=inactive sdk_error=%d pending_caller=%d",
                       error,
                       pending_caller ? 1 : 0);
        return;
    }

    if (room_id[0] != '\0') {
        device_call_action_t action = role == DEVICE_CALL_ROLE_CALLER &&
                                      state != DEVICE_CALL_STATE_IN_CALL ?
                                      DEVICE_CALL_ACTION_CANCEL :
                                      DEVICE_CALL_ACTION_HANGUP;
        (void)device_call_post_room_action_async(action, room_id, "p2p_error");
    }
    device_call_reset_caller_media_gate();
    device_call_notify_session_ended();
    if (error == TIRTC_E_CONN_REMOTECLOSE) {
        ESP_LOGI(CALL_FLOW_TAG,
                 "stage=rtc_closed_by_peer role=%s state=%s room=%s sdk_error=%d",
                 device_call_role_name(role),
                 device_call_state_name(state),
                 room_id[0] != '\0' ? room_id : "-",
                 error);
        ESP_LOGI(TAG, "call connection closed by peer: room=%s", room_id);
    } else {
        APP_LOG_DETAIL(CALL_FLOW_TAG,
                       "stage=rtc_error role=%s state=%s room=%s sdk_error=%d",
                       device_call_role_name(role),
                       device_call_state_name(state),
                       room_id[0] != '\0' ? room_id : "-",
                       error);
        ESP_LOGW(TAG, "RTC connection error during device call: error=%d", error);
    }
}

static void device_call_on_rtc_disconnected(tirtc_conn_t conn, void *ctx)
{
    char room_id[96] = {0};
    device_call_role_t role = DEVICE_CALL_ROLE_NONE;
    device_call_state_t state = DEVICE_CALL_STATE_IDLE;
    bool notify_server = false;
    bool session_ended = false;
    bool pending_caller = false;

    (void)ctx;

    device_call_lock();
    if (s_call.switch_disconnect_in_progress) {
        device_call_unlock();
        ESP_LOGI(CALL_FLOW_TAG, "stage=rtc_disconnected ignored=call_switch");
        return;
    }
    pending_caller = s_call.role == DEVICE_CALL_ROLE_CALLER &&
                     (s_call.state == DEVICE_CALL_STATE_OUTGOING ||
                      s_call.state == DEVICE_CALL_STATE_CONNECTING) &&
                     s_call.rtc_conn != NULL && s_call.rtc_conn == conn;
    if (pending_caller) {
        s_call.rtc_conn = NULL;
        s_call.caller_transport_accepted = false;
        device_call_set_message_locked("peer connection retrying");
    }
    if (s_call.state == DEVICE_CALL_STATE_IN_CALL &&
        s_call.rtc_conn != NULL && s_call.rtc_conn == conn) {
        strlcpy(room_id, s_call.room_id, sizeof(room_id));
        role = s_call.role;
        state = s_call.state;
        device_call_next_generation_locked();
        device_call_show_pending_or_idle_locked("RTC disconnected");
        notify_server = room_id[0] != '\0';
        session_ended = true;
    }
    device_call_unlock();

    if (notify_server) {
        device_call_action_t action = role == DEVICE_CALL_ROLE_CALLER &&
                                      state != DEVICE_CALL_STATE_IN_CALL ?
                                      DEVICE_CALL_ACTION_CANCEL :
                                      DEVICE_CALL_ACTION_HANGUP;
        (void)device_call_post_room_action_async(action, room_id, "p2p_error");
    }
    if (session_ended) {
        device_call_reset_caller_media_gate();
        device_call_notify_session_ended();
    }
    ESP_LOGI(CALL_FLOW_TAG,
             "stage=rtc_disconnected role=%s state=%s room=%s pending_caller=%d notify_server=%d session_ended=%d",
             device_call_role_name(role),
             device_call_state_name(state),
             room_id[0] != '\0' ? room_id : "-",
             pending_caller ? 1 : 0,
             notify_server ? 1 : 0,
             session_ended ? 1 : 0);
}

static void device_call_on_rtc_start_error(int error,
                                           const char *device_id,
                                           const char *client_id,
                                           void *ctx)
{
    char room_id[96] = {0};
    device_call_role_t role = DEVICE_CALL_ROLE_NONE;
    device_call_state_t state = DEVICE_CALL_STATE_IDLE;
    bool active = false;

    (void)ctx;

    device_call_lock();
    active = s_call.state == DEVICE_CALL_STATE_OUTGOING ||
             s_call.state == DEVICE_CALL_STATE_CONNECTING ||
             s_call.state == DEVICE_CALL_STATE_IN_CALL;
    if (active) {
        strlcpy(room_id, s_call.room_id, sizeof(room_id));
        role = s_call.role;
        state = s_call.state;
        device_call_next_generation_locked();
        s_call.request_running = false;
        s_call.accept_running = false;
        s_call.switch_disconnect_in_progress = false;
        device_call_set_error_locked(ESP_FAIL,
                                     error == TIRTC_SESSION_SERVICE_CODE_CLIENT_ID_CONFLICT ?
                                     "RTC client ID conflict" : "RTC start failed");
    }
    device_call_unlock();

    if (active) {
        device_call_reset_caller_media_gate();
        device_call_notify_session_ended();
    }
    if (active && role == DEVICE_CALL_ROLE_CALLER && room_id[0] != '\0') {
        (void)device_call_post_room_action_async(state == DEVICE_CALL_STATE_IN_CALL ?
                                                DEVICE_CALL_ACTION_HANGUP :
                                                DEVICE_CALL_ACTION_CANCEL,
                                                room_id,
                                                state == DEVICE_CALL_STATE_IN_CALL ?
                                                "p2p_error" : "rtc_start_failed");
    }
    ESP_LOGW(CALL_FLOW_TAG,
             "stage=rtc_start_error sdk_error=%d active=%d role=%s state=%s room=%s device_id=%s client_id=%s",
             error,
             active ? 1 : 0,
             device_call_role_name(role),
             device_call_state_name(state),
             room_id[0] != '\0' ? room_id : "-",
             device_id != NULL ? device_id : "-",
             client_id != NULL ? client_id : "-");
    ESP_LOGW(TAG, "RTC start error during device call: error=%d", error);
}

static const rtc_transport_observer_t s_rtc_observer = {
    .on_command = device_call_on_rtc_command,
    .on_connection_accepted = device_call_on_rtc_connection_accepted,
    .on_connection_error = device_call_on_rtc_connection_error,
    .on_disconnected = device_call_on_rtc_disconnected,
    .on_start_error = device_call_on_rtc_start_error,
};

esp_err_t device_call_init(const device_call_config_t *config)
{
    if (config == NULL || (config->enabled && (config->api_base == NULL || config->api_base[0] == '\0'))) {
        ESP_LOGW(CALL_FLOW_TAG, "stage=service_init ret=ESP_ERR_INVALID_ARG");
        return ESP_ERR_INVALID_ARG;
    }
    if (config->api_base != NULL && strlen(config->api_base) >= sizeof(s_call.api_base)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_call.lock == NULL) {
        s_call.lock = xSemaphoreCreateMutexWithCaps(APP_SYNC_CAPS_CONTROL);
        if (s_call.lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    device_call_lock();
    s_call.initialized = true;
    s_call.enabled = config->enabled;
    s_call.ingress_enabled = false;
    s_call.can_accept_incoming = config->can_accept_incoming;
    s_call.on_session_ended = config->on_session_ended;
    s_call.callback_ctx = config->ctx;
    strlcpy(s_call.api_base, config->api_base != NULL ? config->api_base : "", sizeof(s_call.api_base));
    s_call.state = DEVICE_CALL_STATE_IDLE;
    s_call.role = DEVICE_CALL_ROLE_NONE;
    s_call.last_error = ESP_OK;
    s_call.switch_disconnect_in_progress = false;
    s_call.contacts_ready = false;
    s_call.contacts_refresh_running = false;
    s_call.contacts_refresh_pending = false;
    s_call.contact_mutation_running = false;
    s_call.room_recovery_running = false;
    s_call.contact_count = 0;
    s_call.pending_contact_count = 0;
    s_call.contacts_last_error = ESP_OK;
    memset(s_call.contacts, 0, sizeof(s_call.contacts));
    memset(s_call.pending_contacts, 0, sizeof(s_call.pending_contacts));
    device_call_set_message_locked(config->enabled ? "ready" : "disabled");
    device_call_unlock();
    ESP_LOGI(CALL_FLOW_TAG,
             "stage=service_init enabled=%d api_base=%s ret=ESP_OK",
             config->enabled ? 1 : 0,
             config->api_base != NULL ? config->api_base : "-");
    return ESP_OK;
}

esp_err_t device_call_set_api_base(const char *api_base)
{
    if (api_base == NULL || api_base[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(api_base) >= sizeof(s_call.api_base)) {
        return ESP_ERR_INVALID_SIZE;
    }

    device_call_lock();
    if (!s_call.initialized) {
        device_call_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    strlcpy(s_call.api_base, api_base, sizeof(s_call.api_base));
    device_call_unlock();
    ESP_LOGI(CALL_FLOW_TAG, "stage=service_endpoint_update api_base=%s ret=ESP_OK", api_base);
    return ESP_OK;
}

esp_err_t device_call_reconcile_room_async(void)
{
    device_call_room_recovery_ctx_t *ctx = NULL;
    uint32_t generation = 0;

    device_call_lock();
    if (!s_call.initialized || !s_call.enabled || !s_call.ingress_enabled) {
        device_call_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_call.room_recovery_running) {
        device_call_unlock();
        return ESP_OK;
    }
    s_call.room_recovery_running = true;
    generation = s_call.generation;
    device_call_unlock();

    ctx = device_call_calloc_control(1, sizeof(*ctx));
    if (ctx == NULL) {
        device_call_lock();
        if (s_call.generation == generation) {
            s_call.room_recovery_running = false;
        }
        device_call_unlock();
        return ESP_ERR_NO_MEM;
    }
    ctx->generation = generation;

    esp_err_t ret = device_call_launch_task(device_call_room_recovery_task,
                                            "dev_call_recover",
                                            DEVICE_CALL_WORK_TASK_STACK,
                                            ctx);
    if (ret != ESP_OK) {
        device_call_lock();
        if (s_call.generation == generation) {
            s_call.room_recovery_running = false;
        }
        device_call_unlock();
        free(ctx);
    }
    return ret;
}

esp_err_t device_call_start(void)
{
    bool initialized = false;
    bool enabled = false;
    bool register_listener = false;
    bool register_observer = false;
    bool mqtt_connected = false;
    uint32_t start_generation = 0;

    device_call_lock();
    initialized = s_call.initialized;
    enabled = s_call.enabled;
    register_listener = !s_call.listener_registered;
    register_observer = !s_call.observer_registered;
    start_generation = s_call.generation;
    device_call_unlock();

    if (!initialized) {
        ESP_LOGW(CALL_FLOW_TAG, "stage=service_start ret=ESP_ERR_INVALID_STATE reason=not_initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!enabled) {
        ESP_LOGI(CALL_FLOW_TAG, "stage=service_start enabled=0 ret=ESP_OK");
        return ESP_OK;
    }

    if (register_listener) {
        thing_mqtt_listener_handle_t handle = 0;
        esp_err_t ret = thing_mqtt_client_add_listener(device_call_mqtt_message, NULL, &handle);
        if (ret != ESP_OK) {
            ESP_LOGW(CALL_FLOW_TAG,
                     "stage=service_start ret=%s reason=mqtt_listener",
                     esp_err_to_name(ret));
            return ret;
        }
        device_call_lock();
        s_call.listener_handle = handle;
        s_call.listener_registered = true;
        device_call_unlock();
    }
    if (register_observer) {
        esp_err_t ret = rtc_transport_register_observer(&s_rtc_observer, NULL);
        if (ret != ESP_OK) {
            ESP_LOGW(CALL_FLOW_TAG,
                     "stage=service_start ret=%s reason=rtc_observer",
                     esp_err_to_name(ret));
            return ret;
        }
        device_call_lock();
        s_call.observer_registered = true;
        device_call_unlock();
    }

    device_call_lock();
    if (s_call.generation != start_generation) {
        device_call_unlock();
        ESP_LOGW(CALL_FLOW_TAG,
                 "stage=service_start ret=ESP_ERR_INVALID_STATE reason=identity_changed");
        return ESP_ERR_INVALID_STATE;
    }
    s_call.ingress_enabled = true;
    device_call_unlock();

    mqtt_connected = thing_mqtt_client_is_connected();
    esp_err_t room_ret = ESP_ERR_INVALID_STATE;
    esp_err_t contacts_ret = ESP_ERR_INVALID_STATE;
    if (mqtt_connected) {
        room_ret = device_call_reconcile_room_async();
        if (room_ret != ESP_OK) {
            ESP_LOGW(TAG, "call room recovery not scheduled: %s", esp_err_to_name(room_ret));
        }

        contacts_ret = device_call_refresh_contacts_async();
        if (contacts_ret != ESP_OK) {
            ESP_LOGW(TAG, "initial device contact refresh not scheduled: %s", esp_err_to_name(contacts_ret));
        }
    } else {
        ESP_LOGI(TAG, "device call recovery deferred until formal MQTT is online");
    }

    ESP_LOGI(CALL_FLOW_TAG,
             "stage=service_ready mqtt=%d listener=1 observer=1 room_recovery=%s contacts_refresh=%s",
             mqtt_connected ? 1 : 0,
             mqtt_connected ? esp_err_to_name(room_ret) : "deferred",
             mqtt_connected ? esp_err_to_name(contacts_ret) : "deferred");
    ESP_LOGI(TAG, "device call service started");
    return ESP_OK;
}

void device_call_reset_identity_state(void)
{
    device_call_role_t role = DEVICE_CALL_ROLE_NONE;
    device_call_state_t state = DEVICE_CALL_STATE_IDLE;
    char room_id[96] = {0};
    bool pending_incoming = false;
    bool notify_session_ended = false;

    device_call_lock();
    if (s_call.initialized) {
        role = s_call.role;
        state = s_call.state;
        pending_incoming = s_call.pending_incoming;
        strlcpy(room_id,
                s_call.pending_incoming ? s_call.pending_room_id : s_call.room_id,
                sizeof(room_id));
        notify_session_ended = s_call.request_running ||
                               s_call.accept_running ||
                               s_call.switch_disconnect_in_progress ||
                               s_call.pending_incoming ||
                               s_call.state != DEVICE_CALL_STATE_IDLE;
        s_call.ingress_enabled = false;
        device_call_next_generation_locked();
        s_call.request_running = false;
        s_call.accept_running = false;
        s_call.switch_disconnect_in_progress = false;
        s_call.pending_incoming = false;
        s_call.pending_room_id[0] = '\0';
        s_call.pending_caller_id[0] = '\0';
        s_call.pending_call_type[0] = '\0';
        s_call.contacts_ready = false;
        s_call.contacts_refresh_running = false;
        s_call.contacts_refresh_pending = false;
        s_call.contact_mutation_running = false;
        s_call.room_recovery_running = false;
        s_call.contact_count = 0;
        s_call.pending_contact_count = 0;
        s_call.contacts_last_error = ESP_OK;
        memset(s_call.contacts, 0, sizeof(s_call.contacts));
        memset(s_call.pending_contacts, 0, sizeof(s_call.pending_contacts));
        device_call_show_pending_or_idle_locked("identity changed");
    }
    device_call_unlock();

    esp_err_t ringtone_ret = device_call_ringtone_stop();
    if (ringtone_ret != ESP_OK) {
        ESP_LOGW(CALL_FLOW_TAG,
                 "stage=ringtone_stop_failed reason=identity_reset ret=%s",
                 esp_err_to_name(ringtone_ret));
    }
    device_call_reset_caller_media_gate();
    if (notify_session_ended) {
        device_call_notify_session_ended();
    }
    ESP_LOGI(CALL_FLOW_TAG,
             "stage=identity_reset role=%s state=%s room=%s pending=%d notify_session_ended=%d",
             device_call_role_name(role),
             device_call_state_name(state),
             room_id[0] != '\0' ? room_id : "-",
             pending_incoming ? 1 : 0,
             notify_session_ended ? 1 : 0);
}

esp_err_t device_call_request(const char *target_device_id, device_call_type_t call_type)
{
    device_call_request_ctx_t *ctx = NULL;
    uint32_t generation = 0;
    device_call_state_t rejected_state = DEVICE_CALL_STATE_IDLE;
    bool rejected_initialized = false;
    bool rejected_enabled = false;
    bool rejected_ingress_enabled = false;
    bool rejected_listener_registered = false;
    bool rejected_observer_registered = false;
    bool rejected_request_running = false;
    bool rejected_accept_running = false;
    bool rejected_room_recovery_running = false;
    bool service_hooks_ready = false;

    if (target_device_id == NULL || target_device_id[0] == '\0' ||
        (call_type != DEVICE_CALL_TYPE_AUDIO && call_type != DEVICE_CALL_TYPE_VIDEO)) {
        ESP_LOGW(CALL_FLOW_TAG, "stage=request_rejected reason=invalid_peer");
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(target_device_id) >= sizeof(ctx->target_device_id)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!device_online_is_online()) {
        ESP_LOGW(CALL_FLOW_TAG,
                 "stage=request_rejected peer=%s reason=device_offline",
                 target_device_id);
        return ESP_ERR_INVALID_STATE;
    }

    device_call_lock();
    service_hooks_ready = s_call.initialized && s_call.enabled && s_call.ingress_enabled &&
                          s_call.listener_registered && s_call.observer_registered;
    device_call_unlock();
    if (!service_hooks_ready) {
        ESP_LOGW(CALL_FLOW_TAG,
                 "stage=request_service_recover peer=%s reason=hooks_not_ready",
                 target_device_id);
        esp_err_t start_ret = device_call_start();
        if (start_ret != ESP_OK) {
            ESP_LOGW(CALL_FLOW_TAG,
                     "stage=request_rejected peer=%s reason=service_start ret=%s",
                     target_device_id,
                     esp_err_to_name(start_ret));
            return start_ret;
        }
    }

    ctx = device_call_calloc_control(1, sizeof(*ctx));
    if (ctx == NULL) {
        ESP_LOGW(CALL_FLOW_TAG,
                 "stage=request_rejected peer=%s reason=no_memory",
                 target_device_id);
        return ESP_ERR_NO_MEM;
    }

    device_call_lock();
    if (!s_call.initialized || !s_call.enabled || !s_call.ingress_enabled ||
        !s_call.listener_registered ||
        !s_call.observer_registered ||
        s_call.request_running || s_call.accept_running ||
        s_call.room_recovery_running ||
        (s_call.state != DEVICE_CALL_STATE_IDLE && s_call.state != DEVICE_CALL_STATE_ERROR)) {
        rejected_state = s_call.state;
        rejected_initialized = s_call.initialized;
        rejected_enabled = s_call.enabled;
        rejected_ingress_enabled = s_call.ingress_enabled;
        rejected_listener_registered = s_call.listener_registered;
        rejected_observer_registered = s_call.observer_registered;
        rejected_request_running = s_call.request_running;
        rejected_accept_running = s_call.accept_running;
        rejected_room_recovery_running = s_call.room_recovery_running;
        device_call_unlock();
        free(ctx);
        ESP_LOGW(CALL_FLOW_TAG,
                 "stage=request_rejected peer=%s state=%s initialized=%d enabled=%d ingress=%d listener=%d observer=%d request_running=%d accept_running=%d room_recovery=%d reason=busy_or_not_ready",
                 target_device_id,
                 device_call_state_name(rejected_state),
                 rejected_initialized ? 1 : 0,
                 rejected_enabled ? 1 : 0,
                 rejected_ingress_enabled ? 1 : 0,
                 rejected_listener_registered ? 1 : 0,
                 rejected_observer_registered ? 1 : 0,
                 rejected_request_running ? 1 : 0,
                 rejected_accept_running ? 1 : 0,
                 rejected_room_recovery_running ? 1 : 0);
        return ESP_ERR_INVALID_STATE;
    }
    generation = device_call_next_generation_locked();
    s_call.request_running = true;
    s_call.state = DEVICE_CALL_STATE_OUTGOING;
    s_call.role = DEVICE_CALL_ROLE_CALLER;
    s_call.last_error = ESP_OK;
    s_call.room_id[0] = '\0';
    strlcpy(s_call.peer_device_id, target_device_id, sizeof(s_call.peer_device_id));
    const char *call_type_name = call_type == DEVICE_CALL_TYPE_VIDEO ? "video" : "audio";
    strlcpy(s_call.call_type, call_type_name, sizeof(s_call.call_type));
    device_call_set_message_locked("requesting call");
    device_call_unlock();

    strlcpy(ctx->target_device_id, target_device_id, sizeof(ctx->target_device_id));
    strlcpy(ctx->call_type, call_type_name, sizeof(ctx->call_type));
    ctx->generation = generation;
    ESP_LOGI(TAG,
             "outgoing call queued: peer=%s generation=%lu",
             target_device_id,
             (unsigned long)generation);
    ESP_LOGI(CALL_FLOW_TAG,
             "stage=request_queued gen=%lu role=caller peer=%s",
             (unsigned long)generation,
             target_device_id);
    esp_err_t ret = device_call_launch_task(device_call_request_task,
                                            "dev_call_req",
                                            DEVICE_CALL_WORK_TASK_STACK,
                                            ctx);
    if (ret != ESP_OK) {
        device_call_lock();
        if (s_call.generation == generation) {
            s_call.request_running = false;
            device_call_set_error_locked(ret, "call task unavailable");
        }
        device_call_unlock();
        device_call_reset_caller_media_gate();
        free(ctx);
        ESP_LOGW(CALL_FLOW_TAG,
                 "stage=request_task_failed gen=%lu peer=%s ret=%s",
                 (unsigned long)generation,
                 target_device_id,
                 esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t device_call_accept_pending(void)
{
    device_call_accept_ctx_t *ctx = device_call_calloc_control(1, sizeof(*ctx));
    uint32_t generation = 0;
    device_call_state_t rejected_state = DEVICE_CALL_STATE_IDLE;
    bool rejected_pending = false;
    bool rejected_accept_running = false;

    if (ctx == NULL) {
        ESP_LOGW(CALL_FLOW_TAG, "stage=accept_rejected reason=no_memory");
        return ESP_ERR_NO_MEM;
    }
    if (!device_online_is_online()) {
        free(ctx);
        ESP_LOGW(CALL_FLOW_TAG, "stage=accept_rejected reason=device_offline");
        return ESP_ERR_INVALID_STATE;
    }

    device_call_lock();
    if (!s_call.pending_incoming || s_call.accept_running ||
        s_call.pending_room_id[0] == '\0' || s_call.pending_caller_id[0] == '\0') {
        rejected_state = s_call.state;
        rejected_pending = s_call.pending_incoming;
        rejected_accept_running = s_call.accept_running;
        device_call_unlock();
        free(ctx);
        ESP_LOGW(CALL_FLOW_TAG,
                 "stage=accept_rejected state=%s pending=%d accept_running=%d reason=no_pending_call",
                 device_call_state_name(rejected_state),
                 rejected_pending ? 1 : 0,
                 rejected_accept_running ? 1 : 0);
        return ESP_ERR_INVALID_STATE;
    }

    ctx->switch_from_active_call = s_call.state == DEVICE_CALL_STATE_CONNECTING ||
                                   s_call.state == DEVICE_CALL_STATE_IN_CALL;
    if (ctx->switch_from_active_call) {
        strlcpy(ctx->previous_room_id, s_call.room_id, sizeof(ctx->previous_room_id));
        s_call.switch_disconnect_in_progress = true;
    }
    strlcpy(ctx->room_id, s_call.pending_room_id, sizeof(ctx->room_id));
    strlcpy(ctx->caller_id, s_call.pending_caller_id, sizeof(ctx->caller_id));
    strlcpy(ctx->call_type, s_call.pending_call_type, sizeof(ctx->call_type));
    s_call.pending_incoming = false;
    s_call.pending_room_id[0] = '\0';
    s_call.pending_caller_id[0] = '\0';
    s_call.pending_call_type[0] = '\0';

    generation = device_call_next_generation_locked();
    ctx->generation = generation;
    s_call.accept_running = true;
    s_call.state = DEVICE_CALL_STATE_CONNECTING;
    s_call.role = DEVICE_CALL_ROLE_CALLEE;
    s_call.last_error = ESP_OK;
    strlcpy(s_call.room_id, ctx->room_id, sizeof(s_call.room_id));
    strlcpy(s_call.peer_device_id, ctx->caller_id, sizeof(s_call.peer_device_id));
    strlcpy(s_call.call_type,
            ctx->call_type[0] != '\0' ? ctx->call_type : "audio",
            sizeof(s_call.call_type));
    device_call_set_message_locked("answering call");
    device_call_unlock();

    esp_err_t ringtone_ret = device_call_ringtone_stop();
    if (ringtone_ret != ESP_OK) {
        device_call_accept_failed(ctx, ringtone_ret, "ringtone stop failed", false);
        ESP_LOGE(CALL_FLOW_TAG,
                 "stage=accept_rejected gen=%lu room=%s reason=ringtone_stop ret=%s",
                 (unsigned long)generation,
                 ctx->room_id,
                 esp_err_to_name(ringtone_ret));
        free(ctx);
        return ringtone_ret;
    }

    ESP_LOGI(CALL_FLOW_TAG,
             "stage=accept_queued gen=%lu role=callee room=%s peer=%s switch=%d",
             (unsigned long)generation,
             ctx->room_id,
             ctx->caller_id,
             ctx->switch_from_active_call ? 1 : 0);

    esp_err_t ret = device_call_launch_task(device_call_accept_task,
                                            "dev_call_ans",
                                            DEVICE_CALL_WORK_TASK_STACK,
                                            ctx);
    if (ret != ESP_OK) {
        device_call_lock();
        if (s_call.generation == generation) {
            s_call.accept_running = false;
            s_call.switch_disconnect_in_progress = false;
            device_call_set_error_locked(ret, "answer task unavailable");
        }
        device_call_unlock();
        ESP_LOGW(CALL_FLOW_TAG,
                 "stage=accept_task_failed gen=%lu room=%s ret=%s",
                 (unsigned long)generation,
                 ctx->room_id,
                 esp_err_to_name(ret));
        free(ctx);
    }
    return ret;
}

esp_err_t device_call_reject_pending(void)
{
    char room_id[96] = {0};

    device_call_lock();
    if (!s_call.pending_incoming || s_call.pending_room_id[0] == '\0') {
        device_call_unlock();
        ESP_LOGW(CALL_FLOW_TAG, "stage=reject_rejected reason=no_pending_call");
        return ESP_ERR_INVALID_STATE;
    }
    strlcpy(room_id, s_call.pending_room_id, sizeof(room_id));
    s_call.pending_incoming = false;
    s_call.pending_room_id[0] = '\0';
    s_call.pending_caller_id[0] = '\0';
    s_call.pending_call_type[0] = '\0';
    if (s_call.state == DEVICE_CALL_STATE_INCOMING && strcmp(s_call.room_id, room_id) == 0) {
        device_call_show_pending_or_idle_locked("call rejected");
    }
    device_call_unlock();

    esp_err_t ringtone_ret = device_call_ringtone_stop();
    if (ringtone_ret != ESP_OK) {
        ESP_LOGW(CALL_FLOW_TAG,
                 "stage=ringtone_stop_failed reason=reject room=%s ret=%s",
                 room_id,
                 esp_err_to_name(ringtone_ret));
    }

    ESP_LOGI(CALL_FLOW_TAG,
             "stage=reject_queued room=%s",
             room_id);

    return device_call_post_room_action_async(DEVICE_CALL_ACTION_REJECT, room_id, "decline");
}

esp_err_t device_call_hangup(void)
{
    char room_id[96] = {0};
    device_call_role_t role = DEVICE_CALL_ROLE_NONE;
    device_call_state_t state = DEVICE_CALL_STATE_IDLE;
    bool connected = false;

    device_call_lock();
    state = s_call.state;
    if (state != DEVICE_CALL_STATE_OUTGOING && state != DEVICE_CALL_STATE_CONNECTING &&
        state != DEVICE_CALL_STATE_IN_CALL) {
        device_call_unlock();
        ESP_LOGW(CALL_FLOW_TAG,
                 "stage=hangup_rejected state=%s reason=no_active_call",
                 device_call_state_name(state));
        return ESP_ERR_INVALID_STATE;
    }
    strlcpy(room_id, s_call.room_id, sizeof(room_id));
    role = s_call.role;
    device_call_next_generation_locked();
    s_call.request_running = false;
    s_call.accept_running = false;
    s_call.switch_disconnect_in_progress = false;
    device_call_show_pending_or_idle_locked("call ended");
    device_call_unlock();

    device_call_reset_caller_media_gate();
    connected = device_call_transport_is_connected();
    ESP_LOGI(CALL_FLOW_TAG,
             "stage=hangup_begin role=%s state=%s room=%s connected=%d",
             device_call_role_name(role),
             device_call_state_name(state),
             room_id[0] != '\0' ? room_id : "-",
             connected ? 1 : 0);
    if (connected) {
        esp_err_t command_ret = rtc_transport_send_command(TIRTC_SESSION_CMD_DEVICE_CALL_HANGUP,
                                                           NULL,
                                                           0);
        ESP_LOGI(CALL_FLOW_TAG,
                 "stage=hangup_command_tx room=%s cmd=0x%04x ret=%s",
                 room_id[0] != '\0' ? room_id : "-",
                 TIRTC_SESSION_CMD_DEVICE_CALL_HANGUP,
                 esp_err_to_name(command_ret));
    }
    esp_err_t disconnect_ret = rtc_transport_disconnect();
    ESP_LOGI(CALL_FLOW_TAG,
             "stage=disconnect_requested room=%s ret=%s",
             room_id[0] != '\0' ? room_id : "-",
             esp_err_to_name(disconnect_ret));

    if (room_id[0] == '\0') {
        return ESP_OK;
    }
    device_call_action_t action = role == DEVICE_CALL_ROLE_CALLER &&
                                  state != DEVICE_CALL_STATE_IN_CALL && !connected ?
                                  DEVICE_CALL_ACTION_CANCEL :
                                  DEVICE_CALL_ACTION_HANGUP;
    esp_err_t ret = device_call_post_room_action_async(action, room_id, "hangup");
    ESP_LOGI(CALL_FLOW_TAG,
             "stage=hangup_done action=%s room=%s ret=%s",
             device_call_action_name(action),
             room_id,
             esp_err_to_name(ret));
    return ret == ESP_ERR_NO_MEM ? ESP_OK : ret;
}

bool device_call_has_pending_incoming(void)
{
    bool pending = false;

    device_call_lock();
    pending = s_call.pending_incoming;
    device_call_unlock();
    return pending;
}

void device_call_get_snapshot(device_call_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    device_call_lock();
    snapshot->state = s_call.state;
    snapshot->pending_incoming = s_call.pending_incoming;
    strlcpy(snapshot->room_id, s_call.room_id, sizeof(snapshot->room_id));
    strlcpy(snapshot->peer_device_id, s_call.peer_device_id, sizeof(snapshot->peer_device_id));
    strlcpy(snapshot->call_type, s_call.call_type, sizeof(snapshot->call_type));
    snapshot->last_error = s_call.last_error;
    strlcpy(snapshot->message, s_call.message, sizeof(snapshot->message));
    device_call_unlock();
}

esp_err_t device_call_refresh_contacts_async(void)
{
    device_call_contact_ctx_t *ctx = NULL;
    uint32_t generation = 0;

    device_call_lock();
    if (!s_call.initialized || !s_call.enabled || !s_call.ingress_enabled ||
        s_call.request_running || s_call.accept_running ||
        s_call.state == DEVICE_CALL_STATE_OUTGOING ||
        s_call.state == DEVICE_CALL_STATE_INCOMING ||
        s_call.state == DEVICE_CALL_STATE_CONNECTING ||
        s_call.state == DEVICE_CALL_STATE_IN_CALL) {
        device_call_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_call.contacts_refresh_running || s_call.contact_mutation_running) {
        s_call.contacts_refresh_pending = true;
        device_call_unlock();
        return ESP_OK;
    }
    s_call.contacts_refresh_running = true;
    generation = s_call.generation;
    device_call_unlock();

    ctx = device_call_calloc_control(1, sizeof(*ctx));
    if (ctx == NULL) {
        device_call_lock();
        s_call.contacts_refresh_running = false;
        s_call.contacts_last_error = ESP_ERR_NO_MEM;
        device_call_unlock();
        return ESP_ERR_NO_MEM;
    }
    ctx->work = DEVICE_CALL_CONTACT_WORK_REFRESH;
    ctx->generation = generation;

    esp_err_t ret = device_call_launch_task(device_call_contact_task,
                                            "dev_ct_refresh",
                                            DEVICE_CALL_WORK_TASK_STACK,
                                            ctx);
    if (ret != ESP_OK) {
        device_call_lock();
        if (s_call.generation == generation) {
            s_call.contacts_refresh_running = false;
            s_call.contacts_last_error = ret;
        }
        device_call_unlock();
        free(ctx);
    }
    return ret;
}

static esp_err_t device_call_start_contact_mutation(device_call_contact_work_t work,
                                                    const char *peer_device_id,
                                                    const char *remark,
                                                    bool accept)
{
    device_call_contact_ctx_t *ctx = NULL;
    const char *task_name = NULL;
    uint32_t generation = 0;

    if (work == DEVICE_CALL_CONTACT_WORK_REFRESH ||
        peer_device_id == NULL || peer_device_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(peer_device_id) >= sizeof(ctx->target_device_id) ||
        (work == DEVICE_CALL_CONTACT_WORK_REMARK &&
         (remark == NULL || strlen(remark) >= sizeof(ctx->remark)))) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!device_online_is_online()) {
        return ESP_ERR_INVALID_STATE;
    }

    ctx = device_call_calloc_control(1, sizeof(*ctx));
    if (ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ctx->work = work;
    ctx->accept = accept;
    strlcpy(ctx->target_device_id, peer_device_id, sizeof(ctx->target_device_id));
    if (remark != NULL) {
        strlcpy(ctx->remark, remark, sizeof(ctx->remark));
    }

    device_call_lock();
    if (!s_call.initialized || !s_call.enabled || !s_call.ingress_enabled ||
        s_call.contact_mutation_running) {
        device_call_unlock();
        free(ctx);
        return ESP_ERR_INVALID_STATE;
    }
    if (work == DEVICE_CALL_CONTACT_WORK_DELETE) {
        bool found = false;
        device_call_contact_source_t source = DEVICE_CALL_CONTACT_SOURCE_UNKNOWN;

        for (uint8_t index = 0; index < s_call.contact_count; ++index) {
            if (strcmp(s_call.contacts[index].device_id, peer_device_id) == 0) {
                found = true;
                source = s_call.contacts[index].source;
                break;
            }
        }
        if (!found || source != DEVICE_CALL_CONTACT_SOURCE_MANUAL) {
            device_call_unlock();
            free(ctx);
            return found ? ESP_ERR_NOT_ALLOWED : ESP_ERR_NOT_FOUND;
        }
    }
    s_call.contact_mutation_running = true;
    generation = s_call.generation;
    ctx->generation = generation;
    device_call_unlock();

    switch (work) {
    case DEVICE_CALL_CONTACT_WORK_REQUEST:
        task_name = "dev_ct_request";
        break;
    case DEVICE_CALL_CONTACT_WORK_RESPOND:
        task_name = "dev_ct_respond";
        break;
    case DEVICE_CALL_CONTACT_WORK_REMARK:
        task_name = "dev_ct_remark";
        break;
    case DEVICE_CALL_CONTACT_WORK_DELETE:
        task_name = "dev_ct_delete";
        break;
    default:
        task_name = "dev_ct_mutate";
        break;
    }

    esp_err_t ret = device_call_launch_task(device_call_contact_task,
                                            task_name,
                                            DEVICE_CALL_WORK_TASK_STACK,
                                            ctx);
    if (ret != ESP_OK) {
        device_call_lock();
        if (s_call.generation == generation) {
            s_call.contact_mutation_running = false;
            s_call.contacts_last_error = ret;
        }
        device_call_unlock();
        free(ctx);
    }
    return ret;
}

esp_err_t device_call_request_contact_async(const char *target_device_id)
{
    return device_call_start_contact_mutation(DEVICE_CALL_CONTACT_WORK_REQUEST,
                                              target_device_id,
                                              NULL,
                                              false);
}

esp_err_t device_call_respond_contact_async(const char *peer_device_id, bool accept)
{
    return device_call_start_contact_mutation(DEVICE_CALL_CONTACT_WORK_RESPOND,
                                              peer_device_id,
                                              NULL,
                                              accept);
}

esp_err_t device_call_update_contact_remark_async(const char *peer_id, const char *remark)
{
    if (remark == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return device_call_start_contact_mutation(DEVICE_CALL_CONTACT_WORK_REMARK,
                                              peer_id,
                                              remark,
                                              false);
}

esp_err_t device_call_delete_contact_async(const char *peer_device_id)
{
    return device_call_start_contact_mutation(DEVICE_CALL_CONTACT_WORK_DELETE,
                                              peer_device_id,
                                              NULL,
                                              false);
}

void device_call_get_contacts_snapshot(device_call_contacts_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    device_call_lock();
    snapshot->ready = s_call.contacts_ready;
    snapshot->refreshing = s_call.contacts_refresh_running || s_call.contact_mutation_running;
    snapshot->count = s_call.contact_count;
    snapshot->pending_count = s_call.pending_contact_count;
    snapshot->last_error = s_call.contacts_last_error;
    memcpy(snapshot->contacts,
           s_call.contacts,
           (size_t)s_call.contact_count * sizeof(snapshot->contacts[0]));
    memcpy(snapshot->pending,
           s_call.pending_contacts,
           (size_t)s_call.pending_contact_count * sizeof(snapshot->pending[0]));
    device_call_unlock();
}
