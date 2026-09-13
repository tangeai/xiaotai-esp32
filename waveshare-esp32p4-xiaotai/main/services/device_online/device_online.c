#include "device_online.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_memory_policy.h"
#include "device_auth_http.h"
#include "device_identity.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "app_task_affinity.h"
#include "sdkconfig.h"
#include "thing_mqtt_client.h"

static const char *TAG = "device_online";

#define DEVICE_ONLINE_TASK_STACK_SIZE (12 * 1024)
#define DEVICE_ONLINE_TASK_PRIORITY   2
#define DEVICE_ONLINE_MONITOR_MS      1000U
#define DEVICE_ONLINE_RECOVERY_RETRY_INITIAL_MS 5000U
#define DEVICE_ONLINE_RECOVERY_RETRY_MAX_MS     60000U
#define DEVICE_ONLINE_TOKEN_TTL_SECONDS (7U * 24U * 60U * 60U)
#define DEVICE_ONLINE_TOKEN_REFRESH_SKEW_US (60LL * 1000LL * 1000LL)
#define DEVICE_ONLINE_TOKEN_INVALID_REASON_ADMIN_ACTION 0x98U
#define DEVICE_ONLINE_TOKEN_INVALID_REASON_PAYLOAD      0x99U
#define DEVICE_ONLINE_STATUS_PAYLOAD_MAX_LEN            512

#ifndef CONFIG_APP_DEVICE_ONLINE_PAUSE_DURING_RTC
#define CONFIG_APP_DEVICE_ONLINE_PAUSE_DURING_RTC 0
#endif

typedef struct {
    device_online_config_t config;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t token_refresh_lock;
    TaskHandle_t task;
    bool stopping;
    bool restart_requested;
    bool owns_mqtt_client;
    bool token_reauth_requested;
    bool report_requested;
    bool realtime_media_active;
    uint8_t token_reauth_reason;
    uint32_t status_seq;
    thing_mqtt_listener_handle_t mqtt_listener;
    device_online_snapshot_t snapshot;
    bool credentials_valid;
    device_online_credentials_t credentials;
    device_auth_token_t token;
    int64_t token_expires_us;
    char reason[32];
    char restart_reason[32];
    char report_reason[DEVICE_ONLINE_STATUS_REASON_MAX];
} device_online_runtime_t;

static EXT_RAM_BSS_ATTR device_online_runtime_t s_online;

static void device_online_set_state(device_online_state_t state,
                                    esp_err_t last_error,
                                    const char *message);

static int64_t device_online_token_ttl_us(void)
{
    return (int64_t)DEVICE_ONLINE_TOKEN_TTL_SECONDS * 1000LL * 1000LL;
}

static bool device_online_credentials_match(const device_online_credentials_t *a,
                                            const device_online_credentials_t *b)
{
    return a != NULL && b != NULL &&
           strcmp(a->device_id, b->device_id) == 0 &&
           strcmp(a->device_key, b->device_key) == 0;
}

static bool device_online_token_valid_locked(const device_online_credentials_t *credentials,
                                             int64_t now_us)
{
    return credentials != NULL &&
           s_online.credentials_valid &&
           s_online.token.mqtt_token[0] != '\0' &&
           s_online.token_expires_us > now_us + DEVICE_ONLINE_TOKEN_REFRESH_SKEW_US &&
           device_online_credentials_match(credentials, &s_online.credentials);
}

static bool device_online_disconnect_reason_invalidates_token(uint8_t reason_code)
{
    return reason_code == DEVICE_ONLINE_TOKEN_INVALID_REASON_ADMIN_ACTION ||
           reason_code == DEVICE_ONLINE_TOKEN_INVALID_REASON_PAYLOAD;
}

static void device_online_invalidate_token(void)
{
    if (s_online.lock == NULL) {
        return;
    }

    xSemaphoreTake(s_online.lock, portMAX_DELAY);
    memset(&s_online.token, 0, sizeof(s_online.token));
    s_online.token_expires_us = 0;
    xSemaphoreGive(s_online.lock);
}

static void device_online_handle_rebind_required(const char *message)
{
    device_online_set_state(DEVICE_ONLINE_STATE_UNBOUND, ESP_ERR_NOT_FOUND, message);
    device_online_invalidate_token();
    if (s_online.config.on_rebind_required != NULL) {
        s_online.config.on_rebind_required(s_online.config.rebind_ctx);
    }
}

static void device_online_set_state_locked(device_online_state_t state,
                                           esp_err_t last_error,
                                           const char *message)
{
    s_online.snapshot.state = state;
    s_online.snapshot.running = s_online.task != NULL;
    s_online.snapshot.last_error = last_error;
    if (message != NULL) {
        strlcpy(s_online.snapshot.message, message, sizeof(s_online.snapshot.message));
    }
}

static void device_online_set_state(device_online_state_t state,
                                    esp_err_t last_error,
                                    const char *message)
{
    if (s_online.lock == NULL) {
        return;
    }

    xSemaphoreTake(s_online.lock, portMAX_DELAY);
    device_online_set_state_locked(state, last_error, message);
    xSemaphoreGive(s_online.lock);
}

static const char *device_online_state_name(device_online_state_t state)
{
    switch (state) {
    case DEVICE_ONLINE_STATE_DISABLED:
        return "disabled";
    case DEVICE_ONLINE_STATE_OFFLINE:
        return "offline";
    case DEVICE_ONLINE_STATE_UNBOUND:
        return "unbound";
    case DEVICE_ONLINE_STATE_AUTHENTICATING:
        return "authenticating";
    case DEVICE_ONLINE_STATE_MQTT_CONNECTING:
        return "mqtt_connecting";
    case DEVICE_ONLINE_STATE_ONLINE:
        return "online";
    case DEVICE_ONLINE_STATE_ERROR:
    default:
        return "error";
    }
}

static void device_online_request_report_locked(const char *reason)
{
    s_online.report_requested = true;
    strlcpy(s_online.report_reason,
            reason != NULL && reason[0] != '\0' ? reason : "state",
            sizeof(s_online.report_reason));
}

static void device_online_request_report(const char *reason)
{
    TaskHandle_t task = NULL;

    if (s_online.lock == NULL) {
        return;
    }

    xSemaphoreTake(s_online.lock, portMAX_DELAY);
    device_online_request_report_locked(reason);
    task = s_online.task;
    xSemaphoreGive(s_online.lock);

    if (task != NULL) {
        xTaskNotifyGive(task);
    }
}

static bool device_online_network_ready(void)
{
    bool ready = false;

    if (s_online.lock == NULL) {
        return false;
    }
    xSemaphoreTake(s_online.lock, portMAX_DELAY);
    ready = s_online.snapshot.network_ready;
    xSemaphoreGive(s_online.lock);
    return ready;
}

static bool device_online_stopping(void)
{
    bool stopping = false;

    if (s_online.lock == NULL) {
        return true;
    }
    xSemaphoreTake(s_online.lock, portMAX_DELAY);
    stopping = s_online.stopping;
    xSemaphoreGive(s_online.lock);
    return stopping;
}

static bool device_online_realtime_media_active(void)
{
    bool active = false;

    if (s_online.lock == NULL) {
        return false;
    }
    xSemaphoreTake(s_online.lock, portMAX_DELAY);
    active = s_online.realtime_media_active;
    xSemaphoreGive(s_online.lock);
    return active;
}

static bool device_online_task_active(void)
{
    bool active = false;

    if (s_online.lock == NULL) {
        return false;
    }
    xSemaphoreTake(s_online.lock, portMAX_DELAY);
    active = s_online.task != NULL;
    xSemaphoreGive(s_online.lock);
    return active;
}

static void device_online_set_device_id(const char *device_id)
{
    if (s_online.lock == NULL) {
        return;
    }

    xSemaphoreTake(s_online.lock, portMAX_DELAY);
    strlcpy(s_online.snapshot.device_id,
            device_id != NULL ? device_id : "",
            sizeof(s_online.snapshot.device_id));
    s_online.snapshot.bound = device_id != NULL && device_id[0] != '\0';
    xSemaphoreGive(s_online.lock);
}

static bool device_online_set_mqtt_connected(bool connected)
{
    bool changed = false;
    device_online_ready_cb_t on_online_ready = NULL;
    void *online_ready_ctx = NULL;

    if (s_online.lock == NULL) {
        return false;
    }

    xSemaphoreTake(s_online.lock, portMAX_DELAY);
    changed = s_online.snapshot.mqtt_connected != connected;
    s_online.snapshot.mqtt_connected = connected;
    if (connected) {
        device_online_set_state_locked(DEVICE_ONLINE_STATE_ONLINE, ESP_OK, "device online");
    } else if (s_online.snapshot.state == DEVICE_ONLINE_STATE_ONLINE) {
        device_online_set_state_locked(DEVICE_ONLINE_STATE_MQTT_CONNECTING, ESP_OK, "mqtt reconnecting");
    }
    if (changed) {
        device_online_request_report_locked(connected ? "mqtt-connected" : "mqtt-disconnected");
    }
    if (changed && connected) {
        on_online_ready = s_online.config.on_online_ready;
        online_ready_ctx = s_online.config.online_ready_ctx;
    }
    xSemaphoreGive(s_online.lock);

    if (on_online_ready != NULL) {
        on_online_ready(online_ready_ctx);
    }
    return changed;
}

static void device_online_release_mqtt(void)
{
    bool owns_mqtt_client = false;
    thing_mqtt_listener_handle_t listener = -1;

    if (s_online.lock != NULL) {
        xSemaphoreTake(s_online.lock, portMAX_DELAY);
        owns_mqtt_client = s_online.owns_mqtt_client;
        listener = s_online.mqtt_listener;
        s_online.owns_mqtt_client = false;
        s_online.token_reauth_requested = false;
        s_online.token_reauth_reason = 0;
        s_online.mqtt_listener = -1;
        xSemaphoreGive(s_online.lock);
    }

    if (owns_mqtt_client) {
        thing_mqtt_client_stop();
    } else if (listener >= 0) {
        thing_mqtt_client_remove_listener(listener);
    }
}

static void device_online_on_mqtt_message(const char *topic,
                                          const char *payload,
                                          size_t payload_len,
                                          void *ctx)
{
    (void)ctx;

    ESP_LOGD(TAG, "mqtt message: topic_len=%u payload_len=%u",
             topic != NULL ? (unsigned)strlen(topic) : 0U,
             (unsigned)payload_len);
    if (s_online.config.on_message != NULL) {
        s_online.config.on_message(topic, payload, payload_len, s_online.config.ctx);
    }
}

static void device_online_on_mqtt_disconnect(uint8_t reason_code, void *ctx)
{
    (void)ctx;

    if (!device_online_disconnect_reason_invalidates_token(reason_code)) {
        return;
    }

    TaskHandle_t task = NULL;
    xSemaphoreTake(s_online.lock, portMAX_DELAY);
    s_online.token_reauth_requested = true;
    s_online.token_reauth_reason = reason_code;
    task = s_online.task;
    xSemaphoreGive(s_online.lock);

    ESP_LOGW(TAG,
             "mqtt token rejected by broker: reason=0x%02x, scheduling token refresh",
             reason_code);
    if (task != NULL) {
        xTaskNotifyGive(task);
    }
}

static esp_err_t device_online_build_status_payload(char *buffer,
                                                    size_t buffer_size,
                                                    const char *reason,
                                                    uint32_t seq)
{
    device_online_snapshot_t snapshot = {0};
    const char *safe_reason = reason != NULL && reason[0] != '\0' ? reason : "state";

    if (buffer == NULL || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    buffer[0] = '\0';

    if (s_online.config.build_status != NULL) {
        esp_err_t ret = s_online.config.build_status(buffer,
                                                     buffer_size,
                                                     safe_reason,
                                                     seq,
                                                     s_online.config.status_ctx);
        if (ret == ESP_OK && buffer[0] != '\0') {
            return ESP_OK;
        }
    }

    if (s_online.lock != NULL) {
        xSemaphoreTake(s_online.lock, portMAX_DELAY);
        snapshot = s_online.snapshot;
        xSemaphoreGive(s_online.lock);
    } else {
        snapshot.state = DEVICE_ONLINE_STATE_DISABLED;
    }

    int written = snprintf(buffer,
                           buffer_size,
                           "{\"type\":\"status\",\"reason\":\"%s\",\"seq\":%lu,"
                           "\"ts\":%lld,\"state\":\"%s\",\"bound\":%d,"
                           "\"mqtt\":%d,\"network\":%d,\"device_id\":\"%s\"}",
                           safe_reason,
                           (unsigned long)seq,
                           (long long)(esp_timer_get_time() / 1000000LL),
                           device_online_state_name(snapshot.state),
                           snapshot.bound ? 1 : 0,
                           snapshot.mqtt_connected ? 1 : 0,
                           snapshot.network_ready ? 1 : 0,
                           snapshot.device_id);
    if (written <= 0 || written >= (int)buffer_size) {
        buffer[0] = '\0';
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t device_online_build_heartbeat_payload(char *buffer,
                                                       size_t buffer_size,
                                                       uint32_t seq,
                                                       void *ctx)
{
    (void)ctx;

    return device_online_build_status_payload(buffer, buffer_size, "heartbeat", seq);
}

static bool device_online_take_report_request(char *reason, size_t reason_size)
{
    bool requested = false;

    if (reason == NULL || reason_size == 0 || s_online.lock == NULL) {
        return false;
    }

    reason[0] = '\0';
    xSemaphoreTake(s_online.lock, portMAX_DELAY);
    if (s_online.report_requested) {
        requested = true;
        s_online.report_requested = false;
        strlcpy(reason,
                s_online.report_reason[0] != '\0' ? s_online.report_reason : "state",
                reason_size);
        s_online.report_reason[0] = '\0';
    }
    xSemaphoreGive(s_online.lock);
    return requested;
}

static esp_err_t device_online_publish_status(const char *reason)
{
    char payload[DEVICE_ONLINE_STATUS_PAYLOAD_MAX_LEN] = {0};
    uint32_t seq = 0;

    if (!thing_mqtt_client_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_online.lock, portMAX_DELAY);
    seq = ++s_online.status_seq;
    xSemaphoreGive(s_online.lock);

    esp_err_t ret = device_online_build_status_payload(payload,
                                                       sizeof(payload),
                                                       reason,
                                                       seq);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = thing_mqtt_client_publish_up(payload, 0);
    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "state report published: reason=%s seq=%lu",
                 reason != NULL ? reason : "state",
                 (unsigned long)seq);
    }
    return ret;
}

static esp_err_t device_online_load_credentials(device_online_credentials_t *credentials)
{
    if (credentials == NULL || s_online.config.load_credentials == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(credentials, 0, sizeof(*credentials));
    esp_err_t ret = s_online.config.load_credentials(credentials, s_online.config.ctx);
    if (ret != ESP_OK) {
        return ret;
    }
    if (credentials->device_id[0] == '\0' || credentials->device_key[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t device_online_get_cached_credentials(device_online_credentials_t *credentials)
{
    if (credentials == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_online.lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(credentials, 0, sizeof(*credentials));
    xSemaphoreTake(s_online.lock, portMAX_DELAY);
    if (s_online.credentials_valid) {
        *credentials = s_online.credentials;
        xSemaphoreGive(s_online.lock);
        return ESP_OK;
    }
    xSemaphoreGive(s_online.lock);

    device_online_credentials_t loaded = {0};
    esp_err_t ret = device_online_load_credentials(&loaded);
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(s_online.lock, portMAX_DELAY);
    s_online.credentials = loaded;
    s_online.credentials_valid = true;
    strlcpy(s_online.snapshot.device_id, loaded.device_id, sizeof(s_online.snapshot.device_id));
    s_online.snapshot.bound = true;
    xSemaphoreGive(s_online.lock);

    ESP_LOGD(TAG, "device credentials cached: device_id_len=%u", (unsigned)strlen(loaded.device_id));
    *credentials = loaded;
    return ESP_OK;
}

void device_online_invalidate_cache(void)
{
    if (s_online.lock == NULL) {
        return;
    }

    xSemaphoreTake(s_online.lock, portMAX_DELAY);
    memset(&s_online.credentials, 0, sizeof(s_online.credentials));
    memset(&s_online.token, 0, sizeof(s_online.token));
    s_online.credentials_valid = false;
    s_online.token_expires_us = 0;
    s_online.token_reauth_requested = false;
    s_online.token_reauth_reason = 0;
    s_online.snapshot.bound = false;
    s_online.snapshot.device_id[0] = '\0';
    xSemaphoreGive(s_online.lock);
}

esp_err_t device_online_get_cached_mqtt_token(device_auth_token_t *token)
{
    if (token == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_online.lock == NULL || s_online.token_refresh_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(token, 0, sizeof(*token));

    device_online_credentials_t credentials = {0};
    esp_err_t ret = device_online_get_cached_credentials(&credentials);
    if (ret != ESP_OK) {
        return ret;
    }

    int64_t now_us = esp_timer_get_time();
    xSemaphoreTake(s_online.lock, portMAX_DELAY);
    if (device_online_token_valid_locked(&credentials, now_us)) {
        *token = s_online.token;
        int64_t ttl_left_s = (s_online.token_expires_us - now_us) / 1000000LL;
        xSemaphoreGive(s_online.lock);
        ESP_LOGD(TAG,
                 "mqtt token cache hit: device_id_len=%u ttl_left=%llds",
                 (unsigned)strlen(credentials.device_id),
                 (long long)ttl_left_s);
        return ESP_OK;
    }
    xSemaphoreGive(s_online.lock);

    xSemaphoreTake(s_online.token_refresh_lock, portMAX_DELAY);
    now_us = esp_timer_get_time();
    xSemaphoreTake(s_online.lock, portMAX_DELAY);
    if (device_online_token_valid_locked(&credentials, now_us)) {
        *token = s_online.token;
        int64_t ttl_left_s = (s_online.token_expires_us - now_us) / 1000000LL;
        xSemaphoreGive(s_online.lock);
        xSemaphoreGive(s_online.token_refresh_lock);
        ESP_LOGD(TAG,
                 "mqtt token cache hit after wait: device_id_len=%u ttl_left=%llds",
                 (unsigned)strlen(credentials.device_id),
                 (long long)ttl_left_s);
        return ESP_OK;
    }
    xSemaphoreGive(s_online.lock);

    device_binding_identity_t identity = {0};
    ret = device_identity_get(&identity);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "device identity unavailable for token: %s", esp_err_to_name(ret));
        identity.mac[0] = '\0';
    }

    device_auth_token_t *refreshed = app_memory_calloc_psram(1, sizeof(*refreshed));
    if (refreshed == NULL) {
        xSemaphoreGive(s_online.token_refresh_lock);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGD(TAG,
             "mqtt token cache refresh: ttl=%us",
             (unsigned)DEVICE_ONLINE_TOKEN_TTL_SECONDS);
    ret = device_auth_http_get_mqtt_token(s_online.config.api_base,
                                          credentials.device_id,
                                          credentials.device_key,
                                          identity.mac,
                                          refreshed);
    if (ret == ESP_OK) {
        now_us = esp_timer_get_time();
        xSemaphoreTake(s_online.lock, portMAX_DELAY);
        if (s_online.credentials_valid &&
            device_online_credentials_match(&credentials, &s_online.credentials)) {
            s_online.token = *refreshed;
            s_online.token_expires_us = now_us + device_online_token_ttl_us();
            *token = *refreshed;
            ESP_LOGD(TAG,
                     "mqtt token cached: expires_in=%us",
                     (unsigned)DEVICE_ONLINE_TOKEN_TTL_SECONDS);
        } else {
            ret = ESP_ERR_INVALID_STATE;
        }
        xSemaphoreGive(s_online.lock);
    }

    free(refreshed);
    xSemaphoreGive(s_online.token_refresh_lock);
    if (ret == ESP_ERR_NOT_FOUND) {
        device_online_handle_rebind_required("device reset required");
    }
    return ret;
}

static esp_err_t device_online_connect(const device_online_credentials_t *credentials)
{
    device_auth_token_t *token = NULL;
    thing_mqtt_listener_handle_t listener = -1;
    bool owns_mqtt_client = false;
    esp_err_t ret = ESP_OK;

    if (credentials == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    token = app_memory_calloc_psram(1, sizeof(*token));
    if (token == NULL) {
        return ESP_ERR_NO_MEM;
    }

    device_online_set_state(DEVICE_ONLINE_STATE_AUTHENTICATING, ESP_OK, "request mqtt token");
    ret = device_online_get_cached_mqtt_token(token);
    if (ret != ESP_OK) {
        if (ret != ESP_ERR_NOT_FOUND) {
            device_online_set_state(DEVICE_ONLINE_STATE_ERROR, ret, "mqtt token failed");
        }
        goto done;
    }

    device_online_set_state(DEVICE_ONLINE_STATE_MQTT_CONNECTING, ESP_OK, "mqtt connecting");
    owns_mqtt_client = !thing_mqtt_client_is_started();
    if (owns_mqtt_client) {
        thing_mqtt_client_config_t mqtt_config = {
            .broker_uri = s_online.config.mqtt_uri,
            .device_id = credentials->device_id,
            .mqtt_token = token->mqtt_token,
            .heartbeat_interval_ms = s_online.config.heartbeat_interval_ms,
            .on_message = device_online_on_mqtt_message,
            .ctx = NULL,
            .on_disconnect = device_online_on_mqtt_disconnect,
            .disconnect_ctx = NULL,
            .build_heartbeat = device_online_build_heartbeat_payload,
            .heartbeat_ctx = NULL,
        };
        ret = thing_mqtt_client_start(&mqtt_config);
    } else {
        ret = thing_mqtt_client_add_listener(device_online_on_mqtt_message, NULL, &listener);
    }
    if (ret != ESP_OK) {
        device_online_set_state(DEVICE_ONLINE_STATE_ERROR, ret, "mqtt start failed");
        goto done;
    }
    xSemaphoreTake(s_online.lock, portMAX_DELAY);
    s_online.owns_mqtt_client = owns_mqtt_client;
    s_online.mqtt_listener = listener;
    device_online_request_report_locked("mqtt-start");
    xSemaphoreGive(s_online.lock);

done:
    free(token);
    return ret;
}

static bool device_online_mqtt_token_refresh_due(device_online_credentials_t *credentials)
{
    bool due = false;
    int64_t now_us = esp_timer_get_time();

    if (credentials == NULL || s_online.lock == NULL) {
        return false;
    }

    xSemaphoreTake(s_online.lock, portMAX_DELAY);
    if (s_online.owns_mqtt_client &&
        s_online.credentials_valid &&
        s_online.token.mqtt_token[0] != '\0' &&
        s_online.token_expires_us > 0 &&
        s_online.token_expires_us <= now_us + DEVICE_ONLINE_TOKEN_REFRESH_SKEW_US) {
        *credentials = s_online.credentials;
        due = true;
    }
    xSemaphoreGive(s_online.lock);
    return due;
}

static bool device_online_take_token_reauth_request(device_online_credentials_t *credentials,
                                                    uint8_t *reason_code)
{
    bool requested = false;

    if (credentials == NULL || reason_code == NULL || s_online.lock == NULL) {
        return false;
    }

    xSemaphoreTake(s_online.lock, portMAX_DELAY);
    if (s_online.owns_mqtt_client &&
        s_online.token_reauth_requested &&
        s_online.credentials_valid) {
        *credentials = s_online.credentials;
        *reason_code = s_online.token_reauth_reason;
        s_online.token_reauth_requested = false;
        s_online.token_reauth_reason = 0;
        requested = true;
    }
    xSemaphoreGive(s_online.lock);
    return requested;
}

static esp_err_t device_online_restart_owned_mqtt(const device_online_credentials_t *credentials,
                                                  bool force_token_refresh)
{
    device_auth_token_t *token = NULL;

    if (credentials == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (force_token_refresh) {
        device_online_invalidate_token();
    }

    token = app_memory_calloc_psram(1, sizeof(*token));
    if (token == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = device_online_get_cached_mqtt_token(token);
    if (ret != ESP_OK) {
        free(token);
        return ret;
    }

    thing_mqtt_client_config_t mqtt_config = {
        .broker_uri = s_online.config.mqtt_uri,
        .device_id = credentials->device_id,
        .mqtt_token = token->mqtt_token,
        .heartbeat_interval_ms = s_online.config.heartbeat_interval_ms,
        .on_message = device_online_on_mqtt_message,
        .ctx = NULL,
        .on_disconnect = device_online_on_mqtt_disconnect,
        .disconnect_ctx = NULL,
        .build_heartbeat = device_online_build_heartbeat_payload,
        .heartbeat_ctx = NULL,
    };

    thing_mqtt_client_stop();
    ret = thing_mqtt_client_start(&mqtt_config);
    if (ret == ESP_OK) {
        device_online_set_mqtt_connected(false);
        device_online_request_report("mqtt-restart");
    }
    free(token);
    return ret;
}

static esp_err_t device_online_monitor_loop(device_online_credentials_t *credentials)
{
    if (credentials == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    while (!device_online_stopping() &&
           device_online_network_ready() &&
           !device_online_realtime_media_active()) {
        uint8_t reason_code = 0;
        if (device_online_take_token_reauth_request(credentials, &reason_code)) {
            device_online_set_state(DEVICE_ONLINE_STATE_AUTHENTICATING, ESP_OK, "reauth mqtt token");
            esp_err_t ret = device_online_restart_owned_mqtt(credentials, true);
            if (ret != ESP_OK) {
                if (ret != ESP_ERR_NOT_FOUND) {
                    device_online_set_state(DEVICE_ONLINE_STATE_ERROR, ret, "mqtt token self-heal failed");
                    ESP_LOGW(TAG, "mqtt token self-heal failed: %s", esp_err_to_name(ret));
                }
                return ret;
            }
        }

        if (device_online_mqtt_token_refresh_due(credentials)) {
            device_online_set_state(DEVICE_ONLINE_STATE_AUTHENTICATING, ESP_OK, "refresh mqtt token");
            esp_err_t ret = device_online_restart_owned_mqtt(credentials, true);
            if (ret != ESP_OK) {
                if (ret != ESP_ERR_NOT_FOUND) {
                    device_online_set_state(DEVICE_ONLINE_STATE_ERROR, ret, "mqtt token refresh failed");
                    ESP_LOGW(TAG, "mqtt token refresh failed: %s", esp_err_to_name(ret));
                }
                return ret;
            }
        }
        bool connected = thing_mqtt_client_is_connected();
        device_online_set_mqtt_connected(connected);
        if (connected) {
            char report_reason[DEVICE_ONLINE_STATUS_REASON_MAX] = {0};
            if (device_online_take_report_request(report_reason, sizeof(report_reason))) {
                esp_err_t report_ret = device_online_publish_status(report_reason);
                if (report_ret != ESP_OK && report_ret != ESP_ERR_INVALID_STATE) {
                    ESP_LOGW(TAG,
                             "state report failed: reason=%s ret=%s",
                             report_reason,
                             esp_err_to_name(report_ret));
                }
            }
        }
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(DEVICE_ONLINE_MONITOR_MS));
    }
    return ESP_OK;
}

static void device_online_wait_recovery(uint32_t wait_ms)
{
    int64_t deadline_us = esp_timer_get_time() + (int64_t)wait_ms * 1000LL;

    while (!device_online_stopping() &&
           device_online_network_ready() &&
           !device_online_realtime_media_active()) {
        int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) {
            break;
        }
        uint32_t slice_ms = (uint32_t)((remaining_us + 999LL) / 1000LL);
        if (slice_ms > DEVICE_ONLINE_MONITOR_MS) {
            slice_ms = DEVICE_ONLINE_MONITOR_MS;
        }
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(slice_ms));
    }
}

static void device_online_task(void *arg)
{
    (void)arg;
    device_online_credentials_t *credentials = app_memory_calloc_psram(1, sizeof(*credentials));
    esp_err_t ret = ESP_OK;
    uint32_t recovery_wait_ms = DEVICE_ONLINE_RECOVERY_RETRY_INITIAL_MS;
    bool restart_requested = false;
    char restart_reason[sizeof(s_online.restart_reason)] = {0};

    if (credentials == NULL) {
        device_online_set_state(DEVICE_ONLINE_STATE_ERROR, ESP_ERR_NO_MEM, "online workspace allocation failed");
        goto done;
    }

    if (!s_online.config.enabled) {
        device_online_set_state(DEVICE_ONLINE_STATE_DISABLED, ESP_OK, "online disabled");
        goto done;
    }
    if (s_online.config.api_base == NULL || s_online.config.api_base[0] == '\0') {
        device_online_set_state(DEVICE_ONLINE_STATE_DISABLED, ESP_OK, "online api empty");
        goto done;
    }
    if (s_online.config.mqtt_uri == NULL || s_online.config.mqtt_uri[0] == '\0') {
        device_online_set_state(DEVICE_ONLINE_STATE_ERROR, ESP_ERR_INVALID_ARG, "online mqtt uri empty");
        goto done;
    }
    if (!device_online_network_ready()) {
        device_online_set_state(DEVICE_ONLINE_STATE_OFFLINE, ESP_ERR_INVALID_STATE, "network not ready");
        goto done;
    }
    if (device_online_realtime_media_active()) {
        device_online_set_state(DEVICE_ONLINE_STATE_OFFLINE, ESP_OK, "rtc media active");
        goto done;
    }

    while (!device_online_stopping() &&
           device_online_network_ready() &&
           !device_online_realtime_media_active()) {
        bool mqtt_started = false;
        memset(credentials, 0, sizeof(*credentials));

        ret = device_online_get_cached_credentials(credentials);
        if (ret == ESP_ERR_NOT_FOUND) {
            device_online_set_device_id("");
            device_online_set_state(DEVICE_ONLINE_STATE_UNBOUND, ESP_OK, "device unbound");
            break;
        }
        if (ret != ESP_OK) {
            device_online_set_state(DEVICE_ONLINE_STATE_ERROR, ret, "credentials load failed");
        } else {
            device_online_set_device_id(credentials->device_id);
            ret = device_online_connect(credentials);
            if (ret == ESP_OK) {
                mqtt_started = true;
                recovery_wait_ms = DEVICE_ONLINE_RECOVERY_RETRY_INITIAL_MS;
                ret = device_online_monitor_loop(credentials);
            }
        }

        if (mqtt_started) {
            device_online_release_mqtt();
            device_online_set_mqtt_connected(false);
        }
        if (ret == ESP_ERR_NOT_FOUND ||
            device_online_stopping() ||
            !device_online_network_ready() ||
            device_online_realtime_media_active()) {
            break;
        }

        device_online_set_state(DEVICE_ONLINE_STATE_ERROR, ret, "online recovery pending");
        ESP_LOGW(TAG,
                 "online service recovery scheduled: ret=%s wait_ms=%lu",
                 esp_err_to_name(ret),
                 (unsigned long)recovery_wait_ms);
        device_online_wait_recovery(recovery_wait_ms);
        if (recovery_wait_ms < DEVICE_ONLINE_RECOVERY_RETRY_MAX_MS) {
            recovery_wait_ms *= 2U;
            if (recovery_wait_ms > DEVICE_ONLINE_RECOVERY_RETRY_MAX_MS) {
                recovery_wait_ms = DEVICE_ONLINE_RECOVERY_RETRY_MAX_MS;
            }
        }
    }

    if (!device_online_network_ready()) {
        device_online_set_state(DEVICE_ONLINE_STATE_OFFLINE, ESP_OK, "network offline");
    }

done:
    free(credentials);
    xSemaphoreTake(s_online.lock, portMAX_DELAY);
    restart_requested = s_online.restart_requested &&
                        s_online.config.enabled &&
                        s_online.snapshot.network_ready &&
                        !s_online.realtime_media_active;
    if (restart_requested) {
        strlcpy(restart_reason,
                s_online.restart_reason[0] != '\0' ?
                    s_online.restart_reason : "network-recovered",
                sizeof(restart_reason));
    }
    s_online.restart_requested = false;
    s_online.restart_reason[0] = '\0';
    s_online.task = NULL;
    s_online.stopping = false;
    s_online.snapshot.running = false;
    xSemaphoreGive(s_online.lock);

    if (restart_requested) {
        ESP_LOGI(TAG, "online service restart queued: reason=%s", restart_reason);
        esp_err_t restart_ret = device_online_start_async(restart_reason);
        if (restart_ret != ESP_OK && restart_ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG,
                     "online service restart failed: reason=%s ret=%s",
                     restart_reason,
                     esp_err_to_name(restart_ret));
        }
    }
    vTaskDeleteWithCaps(NULL);
}

esp_err_t device_online_init(const device_online_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_online.lock == NULL) {
        s_online.lock = xSemaphoreCreateMutexWithCaps(APP_SYNC_CAPS_CONTROL);
        if (s_online.lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_online.token_refresh_lock == NULL) {
        s_online.token_refresh_lock = xSemaphoreCreateMutexWithCaps(APP_SYNC_CAPS_CONTROL);
        if (s_online.token_refresh_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_online.lock, portMAX_DELAY);
    s_online.config = *config;
    s_online.task = NULL;
    s_online.stopping = false;
    s_online.restart_requested = false;
    s_online.owns_mqtt_client = false;
    s_online.mqtt_listener = -1;
    s_online.credentials_valid = false;
    s_online.token_expires_us = 0;
    s_online.token_reauth_requested = false;
    s_online.token_reauth_reason = 0;
    s_online.report_requested = false;
    s_online.realtime_media_active = false;
    s_online.status_seq = 0;
    s_online.restart_reason[0] = '\0';
    s_online.report_reason[0] = '\0';
    memset(&s_online.credentials, 0, sizeof(s_online.credentials));
    memset(&s_online.token, 0, sizeof(s_online.token));
    memset(&s_online.snapshot, 0, sizeof(s_online.snapshot));
    s_online.snapshot.state = config->enabled ? DEVICE_ONLINE_STATE_OFFLINE : DEVICE_ONLINE_STATE_DISABLED;
    strlcpy(s_online.snapshot.message,
            config->enabled ? "online idle" : "online disabled",
            sizeof(s_online.snapshot.message));
    xSemaphoreGive(s_online.lock);
    return ESP_OK;
}

void device_online_set_network_ready(bool ready)
{
    TaskHandle_t task = NULL;

    if (s_online.lock == NULL) {
        return;
    }

    xSemaphoreTake(s_online.lock, portMAX_DELAY);
    s_online.snapshot.network_ready = ready;
    if (!ready) {
        s_online.snapshot.mqtt_connected = false;
        if (s_online.snapshot.state != DEVICE_ONLINE_STATE_DISABLED &&
            s_online.snapshot.state != DEVICE_ONLINE_STATE_UNBOUND) {
            device_online_set_state_locked(DEVICE_ONLINE_STATE_OFFLINE, ESP_OK, "network offline");
        }
        s_online.stopping = true;
        task = s_online.task;
    }
    xSemaphoreGive(s_online.lock);

    if (task != NULL) {
        xTaskNotifyGive(task);
    }
}

void device_online_set_realtime_media_active(bool active)
{
#if CONFIG_APP_DEVICE_ONLINE_PAUSE_DURING_RTC
    TaskHandle_t task = NULL;
    bool changed = false;
    bool should_release_mqtt = false;
    bool should_restart = false;

    if (s_online.lock == NULL) {
        return;
    }

    xSemaphoreTake(s_online.lock, portMAX_DELAY);
    if (s_online.realtime_media_active != active) {
        s_online.realtime_media_active = active;
        changed = true;
    }

    if (active) {
        s_online.snapshot.mqtt_connected = false;
        if (s_online.snapshot.state != DEVICE_ONLINE_STATE_DISABLED &&
            s_online.snapshot.state != DEVICE_ONLINE_STATE_UNBOUND) {
            device_online_set_state_locked(DEVICE_ONLINE_STATE_OFFLINE, ESP_OK, "rtc media active");
        }
        s_online.stopping = true;
        task = s_online.task;
        should_release_mqtt = task == NULL;
    } else {
        s_online.stopping = false;
        should_restart = s_online.config.enabled && s_online.snapshot.network_ready && s_online.task == NULL;
    }
    xSemaphoreGive(s_online.lock);

    if (!changed && !should_restart) {
        return;
    }

    ESP_LOGI(TAG,
             "device online realtime media gate: active=%d action=%s",
             active ? 1 : 0,
             active ? (task != NULL ? "stop-task" : "release-mqtt") :
                      (should_restart ? "restart" : "resume-task"));

    if (active) {
        if (task != NULL) {
            xTaskNotifyGive(task);
        } else if (should_release_mqtt) {
            device_online_release_mqtt();
        }
        return;
    }

    if (task != NULL) {
        xTaskNotifyGive(task);
    } else if (should_restart) {
        esp_err_t ret = device_online_start_async("rtc-idle");
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE && ret != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "device online restart after rtc idle failed: %s", esp_err_to_name(ret));
        }
    }
#else
    (void)active;
#endif
}

esp_err_t device_online_start_async(const char *reason)
{
    if (s_online.lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_online.lock, portMAX_DELAY);
    if (!s_online.config.enabled) {
        xSemaphoreGive(s_online.lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_online.snapshot.network_ready) {
        device_online_set_state_locked(DEVICE_ONLINE_STATE_OFFLINE, ESP_ERR_INVALID_STATE, "network not ready");
        xSemaphoreGive(s_online.lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_online.realtime_media_active) {
        device_online_set_state_locked(DEVICE_ONLINE_STATE_OFFLINE, ESP_OK, "rtc media active");
        xSemaphoreGive(s_online.lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_online.task != NULL) {
        if (s_online.stopping) {
            s_online.restart_requested = true;
            strlcpy(s_online.restart_reason,
                    reason != NULL && reason[0] != '\0' ? reason : "network-recovered",
                    sizeof(s_online.restart_reason));
            ESP_LOGI(TAG,
                     "online service restart requested while stopping: reason=%s",
                     s_online.restart_reason);
        }
        xSemaphoreGive(s_online.lock);
        return ESP_OK;
    }

    s_online.stopping = false;
    s_online.restart_requested = false;
    s_online.restart_reason[0] = '\0';
    s_online.snapshot.running = true;
    strlcpy(s_online.reason, reason != NULL ? reason : "manual", sizeof(s_online.reason));
    /* This worker owns network state only; credential persistence is handled by
     * the binding/NVS worker, so its large stack can stay in PSRAM. */
    BaseType_t task_ret = xTaskCreateWithCaps(device_online_task,
                                              "device_online",
                                              DEVICE_ONLINE_TASK_STACK_SIZE,
                                              NULL,
                                              DEVICE_ONLINE_TASK_PRIORITY,
                                              &s_online.task,
                                              APP_TASK_STACK_CAPS_BACKGROUND);
    if (task_ret != pdPASS) {
        s_online.task = NULL;
        s_online.snapshot.running = false;
        xSemaphoreGive(s_online.lock);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_online.lock);
    return ESP_OK;
}

void device_online_stop(void)
{
    TaskHandle_t task = NULL;

    if (s_online.lock == NULL) {
        return;
    }

    xSemaphoreTake(s_online.lock, portMAX_DELAY);
    s_online.stopping = true;
    s_online.restart_requested = false;
    s_online.restart_reason[0] = '\0';
    task = s_online.task;
    xSemaphoreGive(s_online.lock);

    if (task != NULL) {
        xTaskNotifyGive(task);
    } else {
        device_online_release_mqtt();
        device_online_set_state(DEVICE_ONLINE_STATE_OFFLINE, ESP_OK, "online stopped");
    }
}

esp_err_t device_online_notify_credentials_changed(const char *reason)
{
    device_online_stop();
    for (int retry = 0; retry < 50 && device_online_task_active(); ++retry) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    device_online_invalidate_cache();
    return device_online_start_async(reason != NULL ? reason : "credentials");
}

void device_online_notify_credentials_cleared(const char *reason)
{
    device_online_stop();
    for (int retry = 0; retry < 50 && device_online_task_active(); ++retry) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    device_online_invalidate_cache();
    device_online_set_state(DEVICE_ONLINE_STATE_UNBOUND,
                            ESP_OK,
                            reason != NULL ? reason : "credentials cleared");
}

esp_err_t device_online_report_state_async(const char *reason)
{
    if (s_online.lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    device_online_request_report(reason != NULL ? reason : "state");
    return ESP_OK;
}

bool device_online_is_online(void)
{
    bool online = false;

    if (s_online.lock == NULL) {
        return false;
    }
    xSemaphoreTake(s_online.lock, portMAX_DELAY);
    online = s_online.snapshot.state == DEVICE_ONLINE_STATE_ONLINE && s_online.snapshot.mqtt_connected;
    xSemaphoreGive(s_online.lock);
    return online;
}

void device_online_get_snapshot(device_online_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    if (s_online.lock == NULL) {
        snapshot->state = DEVICE_ONLINE_STATE_DISABLED;
        strlcpy(snapshot->message, "online not initialized", sizeof(snapshot->message));
        return;
    }

    xSemaphoreTake(s_online.lock, portMAX_DELAY);
    *snapshot = s_online.snapshot;
    xSemaphoreGive(s_online.lock);
}
