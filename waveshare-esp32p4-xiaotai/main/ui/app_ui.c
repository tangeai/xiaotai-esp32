#include "app_ui.h"

#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "app.h"
#include "app_memory_policy.h"
#include "app_task_affinity.h"

#define APP_UI_MEMORY_SNAPSHOT_PERIOD_MS 1000U

static const char *TAG = "app_ui";
static portMUX_TYPE s_contact_scan_stop_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_contact_scan_stop_task_running;
static portMUX_TYPE s_tirtc_config_scan_stop_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_tirtc_config_scan_stop_task_running;
static portMUX_TYPE s_wechat_contact_scan_stop_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_wechat_contact_scan_stop_task_running;
static app_snapshot_t *s_display_snapshot;
static app_memory_snapshot_t s_display_memory_snapshot;
static TickType_t s_display_memory_snapshot_tick;
static bool s_display_memory_snapshot_valid;

static display_memory_health_t app_ui_memory_health(app_memory_health_t health)
{
    switch (health) {
    case APP_MEMORY_HEALTH_WARNING:
        return DISPLAY_MEMORY_HEALTH_WARNING;
    case APP_MEMORY_HEALTH_CRITICAL:
        return DISPLAY_MEMORY_HEALTH_CRITICAL;
    case APP_MEMORY_HEALTH_NORMAL:
    default:
        return DISPLAY_MEMORY_HEALTH_NORMAL;
    }
}

static void app_ui_fill_memory_status(display_status_t *status)
{
    TickType_t now;

    if (status == NULL) {
        return;
    }

    now = xTaskGetTickCount();
    if (!s_display_memory_snapshot_valid ||
        now - s_display_memory_snapshot_tick >=
            pdMS_TO_TICKS(APP_UI_MEMORY_SNAPSHOT_PERIOD_MS)) {
        app_memory_get_snapshot(&s_display_memory_snapshot);
        s_display_memory_snapshot_tick = now;
        s_display_memory_snapshot_valid = true;
    }

    status->memory_internal_free = s_display_memory_snapshot.internal_free;
    status->memory_internal_largest = s_display_memory_snapshot.internal_largest;
    status->memory_dma_largest = s_display_memory_snapshot.dma_largest;
    status->memory_psram_largest = s_display_memory_snapshot.psram_largest;
    status->memory_health =
        app_ui_memory_health(app_memory_classify(&s_display_memory_snapshot));
}

static BaseType_t app_ui_create_background_task(TaskFunction_t task_func,
                                                const char *name,
                                                uint32_t stack_size,
                                                void *arg,
                                                UBaseType_t priority)
{
    BaseType_t task_ret = xTaskCreateWithCaps(task_func,
                                              name,
                                              stack_size,
                                              arg,
                                              priority,
                                              NULL,
                                              APP_TASK_STACK_CAPS_BACKGROUND);
    return task_ret;
}

static app_snapshot_t *app_ui_display_snapshot(void)
{
    if (s_display_snapshot == NULL) {
        s_display_snapshot = (app_snapshot_t *)app_memory_calloc_psram(1, sizeof(*s_display_snapshot));
    }
    return s_display_snapshot;
}

static void app_ui_set_contact_scan_stop_task_running(bool running)
{
    taskENTER_CRITICAL(&s_contact_scan_stop_lock);
    s_contact_scan_stop_task_running = running;
    taskEXIT_CRITICAL(&s_contact_scan_stop_lock);
}

static void app_ui_set_tirtc_config_scan_stop_task_running(bool running)
{
    taskENTER_CRITICAL(&s_tirtc_config_scan_stop_lock);
    s_tirtc_config_scan_stop_task_running = running;
    taskEXIT_CRITICAL(&s_tirtc_config_scan_stop_lock);
}

static void app_ui_set_wechat_contact_scan_stop_task_running(bool running)
{
    taskENTER_CRITICAL(&s_wechat_contact_scan_stop_lock);
    s_wechat_contact_scan_stop_task_running = running;
    taskEXIT_CRITICAL(&s_wechat_contact_scan_stop_lock);
}

static bool app_ui_contact_scan_stop_task_is_running(void)
{
    bool running = false;

    taskENTER_CRITICAL(&s_contact_scan_stop_lock);
    running = s_contact_scan_stop_task_running;
    taskEXIT_CRITICAL(&s_contact_scan_stop_lock);
    return running;
}

static bool app_ui_tirtc_config_scan_stop_task_is_running(void)
{
    bool running = false;

    taskENTER_CRITICAL(&s_tirtc_config_scan_stop_lock);
    running = s_tirtc_config_scan_stop_task_running;
    taskEXIT_CRITICAL(&s_tirtc_config_scan_stop_lock);
    return running;
}

static bool app_ui_wechat_contact_scan_stop_task_is_running(void)
{
    bool running = false;

    taskENTER_CRITICAL(&s_wechat_contact_scan_stop_lock);
    running = s_wechat_contact_scan_stop_task_running;
    taskEXIT_CRITICAL(&s_wechat_contact_scan_stop_lock);
    return running;
}

static void app_ui_stop_contact_scan_task(void *arg)
{
    (void)arg;

    esp_err_t ret = ESP_OK;

    for (uint8_t attempt = 0; attempt < 5U; ++attempt) {
        ret = app_stop_contact_scan();
        if (ret != ESP_ERR_TIMEOUT) {
            break;
        }
        ESP_LOGW(TAG, "waiting for contact scan to stop: attempt=%u", (unsigned)(attempt + 1U));
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "stop contact scan failed in background: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGD(TAG, "contact scan stop completed: %s", esp_err_to_name(ret));
    }
    app_ui_set_contact_scan_stop_task_running(false);
    vTaskDeleteWithCaps(NULL);
}

static void app_ui_stop_tirtc_config_scan_task(void *arg)
{
    (void)arg;

    esp_err_t ret = ESP_OK;

    for (uint8_t attempt = 0; attempt < 5U; ++attempt) {
        ret = app_stop_tirtc_config_scan();
        if (ret != ESP_ERR_TIMEOUT) {
            break;
        }
        ESP_LOGW(TAG, "waiting for tirtc config scan to stop: attempt=%u", (unsigned)(attempt + 1U));
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "stop tirtc config scan failed in background: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGD(TAG, "tirtc config scan stop completed: %s", esp_err_to_name(ret));
    }
    app_ui_set_tirtc_config_scan_stop_task_running(false);
    vTaskDeleteWithCaps(NULL);
}

static void app_ui_stop_wechat_contact_scan_task(void *arg)
{
    (void)arg;

    esp_err_t ret = ESP_OK;

    for (uint8_t attempt = 0; attempt < 5U; ++attempt) {
        ret = app_stop_wechat_contact_scan();
        if (ret != ESP_ERR_TIMEOUT) {
            break;
        }
        ESP_LOGW(TAG, "waiting for wechat contact scan to stop: attempt=%u", (unsigned)(attempt + 1U));
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "stop wechat contact scan failed in background: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGD(TAG, "wechat contact scan stop completed: %s", esp_err_to_name(ret));
    }
    app_ui_set_wechat_contact_scan_stop_task_running(false);
    vTaskDeleteWithCaps(NULL);
}

static app_rtc_config_field_t app_ui_from_display_rtc_field(display_tirtc_config_field_t field)
{
    switch (field) {
    case DISPLAY_TIRTC_CONFIG_FIELD_DEVICE_SECRET:
        return APP_RTC_CONFIG_FIELD_DEVICE_SECRET;
    case DISPLAY_TIRTC_CONFIG_FIELD_TOKEN_SUBJECT:
        return APP_RTC_CONFIG_FIELD_TOKEN_SUBJECT;
    case DISPLAY_TIRTC_CONFIG_FIELD_ACCESS_KEY_ID:
        return APP_RTC_CONFIG_FIELD_ACCESS_KEY_ID;
    case DISPLAY_TIRTC_CONFIG_FIELD_ACCESS_KEY_SECRET:
        return APP_RTC_CONFIG_FIELD_ACCESS_KEY_SECRET;
    case DISPLAY_TIRTC_CONFIG_FIELD_DEVICE_ID:
    default:
        return APP_RTC_CONFIG_FIELD_DEVICE_ID;
    }
}

static app_rtc_server_env_t app_ui_from_display_rtc_env(display_tirtc_server_env_t env)
{
    switch (env) {
    case DISPLAY_TIRTC_SERVER_ENV_TEST:
        return APP_RTC_SERVER_ENV_TEST;
    case DISPLAY_TIRTC_SERVER_ENV_PRE:
        return APP_RTC_SERVER_ENV_PRE;
    case DISPLAY_TIRTC_SERVER_ENV_PROD:
    default:
        return APP_RTC_SERVER_ENV_PROD;
    }
}

static display_tirtc_server_env_t app_ui_to_display_rtc_env(app_rtc_server_env_t env)
{
    switch (env) {
    case APP_RTC_SERVER_ENV_TEST:
        return DISPLAY_TIRTC_SERVER_ENV_TEST;
    case APP_RTC_SERVER_ENV_PRE:
        return DISPLAY_TIRTC_SERVER_ENV_PRE;
    case APP_RTC_SERVER_ENV_PROD:
    default:
        return DISPLAY_TIRTC_SERVER_ENV_PROD;
    }
}

static display_device_binding_state_t app_ui_to_display_binding_state(device_binding_state_t state)
{
    switch (state) {
    case DEVICE_BINDING_STATE_IDLE:
        return DISPLAY_DEVICE_BINDING_STATE_IDLE;
    case DEVICE_BINDING_STATE_REPORTING:
        return DISPLAY_DEVICE_BINDING_STATE_REPORTING;
    case DEVICE_BINDING_STATE_WAITING_USER:
        return DISPLAY_DEVICE_BINDING_STATE_WAITING_USER;
    case DEVICE_BINDING_STATE_BOUND:
        return DISPLAY_DEVICE_BINDING_STATE_BOUND;
    case DEVICE_BINDING_STATE_ERROR:
        return DISPLAY_DEVICE_BINDING_STATE_ERROR;
    case DEVICE_BINDING_STATE_DISABLED:
    default:
        return DISPLAY_DEVICE_BINDING_STATE_DISABLED;
    }
}

static display_wechat_call_state_t app_ui_to_display_wechat_call_state(app_wechat_call_state_t state)
{
    switch (state) {
    case APP_WECHAT_CALL_STATE_INCOMING:
        return DISPLAY_WECHAT_CALL_STATE_INCOMING;
    case APP_WECHAT_CALL_STATE_CALLING:
        return DISPLAY_WECHAT_CALL_STATE_CALLING;
    case APP_WECHAT_CALL_STATE_CONNECTING:
        return DISPLAY_WECHAT_CALL_STATE_CONNECTING;
    case APP_WECHAT_CALL_STATE_IN_CALL:
        return DISPLAY_WECHAT_CALL_STATE_IN_CALL;
    case APP_WECHAT_CALL_STATE_CLOSING:
        return DISPLAY_WECHAT_CALL_STATE_CLOSING;
    case APP_WECHAT_CALL_STATE_IDLE:
    default:
        return DISPLAY_WECHAT_CALL_STATE_IDLE;
    }
}

static app_id_t app_ui_from_display_app(display_app_id_t app_id)
{
    switch (app_id) {
    case DISPLAY_APP_DEVICE:
        return APP_ID_DEVICE;
    case DISPLAY_APP_CALL:
        return APP_ID_CALL;
    case DISPLAY_APP_WECHAT:
        return APP_ID_WECHAT;
    case DISPLAY_APP_AI_CHAT:
        return APP_ID_AI_CHAT;
    case DISPLAY_APP_SYSTEM:
        return APP_ID_SYSTEM;
    case DISPLAY_APP_HOME:
    default:
        return APP_ID_HOME;
    }
}

static display_call_state_t app_ui_to_display_call_state(app_call_state_t state)
{
    switch (state) {
    case APP_CALL_STATE_OUTGOING:
        return DISPLAY_CALL_STATE_OUTGOING;
    case APP_CALL_STATE_INCOMING:
        return DISPLAY_CALL_STATE_INCOMING;
    case APP_CALL_STATE_CONNECTING:
        return DISPLAY_CALL_STATE_CONNECTING;
    case APP_CALL_STATE_IN_CALL:
        return DISPLAY_CALL_STATE_IN_CALL;
    case APP_CALL_STATE_ERROR:
        return DISPLAY_CALL_STATE_ERROR;
    case APP_CALL_STATE_IDLE:
    default:
        return DISPLAY_CALL_STATE_IDLE;
    }
}

static esp_err_t app_ui_on_wifi_connect(const char *ssid, const char *password, void *ctx)
{
    (void)ctx;
    return app_connect_wifi(ssid, password);
}

static esp_err_t app_ui_on_set_device_uuid(const char *uuid, void *ctx)
{
    (void)ctx;
    return app_update_device_uuid(uuid);
}

static esp_err_t app_ui_on_wifi_scan(void *ctx)
{
    (void)ctx;
    return app_request_wifi_scan();
}

static esp_err_t app_ui_on_ping_test(void *ctx)
{
    (void)ctx;
    return app_start_ping_test();
}

static esp_err_t app_ui_on_disconnect_rtc(void *ctx)
{
    (void)ctx;
    return app_disconnect_rtc();
}

static esp_err_t app_ui_on_hangup_call(void *ctx)
{
    (void)ctx;
    return app_hangup_call_async();
}

static esp_err_t app_ui_on_set_local_video_enabled(bool enabled, void *ctx)
{
    (void)ctx;
    return app_set_local_video_enabled(enabled);
}

static esp_err_t app_ui_on_set_local_audio_enabled(bool enabled, void *ctx)
{
    (void)ctx;
    return app_set_local_audio_enabled(enabled);
}

static esp_err_t app_ui_on_set_speaker_volume(uint8_t percent, void *ctx)
{
    (void)ctx;
    return app_request_speaker_volume(percent);
}

static esp_err_t app_ui_on_set_capture_gain(uint8_t percent, void *ctx)
{
    (void)ctx;
    return app_request_capture_gain(percent);
}

static esp_err_t app_ui_on_call_contact(const char *device_id,
                                        display_call_type_t call_type,
                                        void *ctx)
{
    (void)ctx;
    esp_err_t ret = app_enter_app(APP_ID_CALL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "enter call before outgoing call failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return app_call_contact(device_id,
                            call_type == DISPLAY_CALL_TYPE_VIDEO ?
                            APP_CALL_TYPE_VIDEO : APP_CALL_TYPE_AUDIO);
}

static esp_err_t app_ui_on_add_call_contact(const char *device_id, void *ctx)
{
    (void)ctx;
    return app_add_call_contact(device_id);
}

static esp_err_t app_ui_on_respond_call_contact(const char *device_id,
                                                bool accept,
                                                void *ctx)
{
    (void)ctx;
    return app_respond_call_contact(device_id, accept);
}

static esp_err_t app_ui_on_update_call_contact_remark(const char *device_id,
                                                      const char *remark,
                                                      void *ctx)
{
    (void)ctx;
    return app_update_call_contact_remark(device_id, remark);
}

static esp_err_t app_ui_on_delete_call_contact(const char *device_id, void *ctx)
{
    (void)ctx;
    return app_delete_call_contact(device_id);
}

static esp_err_t app_ui_on_refresh_call_contacts(void *ctx)
{
    (void)ctx;
    return app_refresh_call_contacts();
}

static esp_err_t app_ui_on_scan_contact(void *ctx)
{
    (void)ctx;
    return app_scan_contact();
}

static esp_err_t app_ui_on_start_contact_scan(display_scan_preview_cb_t preview_cb,
                                              display_contact_scan_result_cb_t result_cb,
                                              void *scan_ctx,
                                              void *ctx)
{
    (void)ctx;
    esp_err_t ret = app_enter_app(APP_ID_CALL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "enter call before contact scan failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return app_start_contact_scan(preview_cb,
                                  result_cb,
                                  scan_ctx);
}

static esp_err_t app_ui_on_stop_contact_scan(void *ctx)
{
    (void)ctx;

    if (app_ui_contact_scan_stop_task_is_running()) {
        return ESP_OK;
    }

    app_ui_set_contact_scan_stop_task_running(true);
    BaseType_t task_ret = app_ui_create_background_task(app_ui_stop_contact_scan_task,
                                                        "call_scan_stop",
                                                        4096,
                                                        NULL,
                                                        5);
    if (task_ret != pdPASS) {
        app_ui_set_contact_scan_stop_task_running(false);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t app_ui_on_start_tirtc_config_scan(display_scan_preview_cb_t preview_cb,
                                                   display_tirtc_config_scan_result_cb_t result_cb,
                                                   void *scan_ctx,
                                                   void *ctx)
{
    (void)ctx;

    return app_start_tirtc_config_scan(preview_cb,
                                       (app_tirtc_config_scan_result_cb_t)result_cb,
                                       scan_ctx);
}

static esp_err_t app_ui_on_stop_tirtc_config_scan(void *ctx)
{
    (void)ctx;

    if (app_ui_tirtc_config_scan_stop_task_is_running()) {
        return ESP_OK;
    }

    app_ui_set_tirtc_config_scan_stop_task_running(true);
    BaseType_t task_ret = app_ui_create_background_task(app_ui_stop_tirtc_config_scan_task,
                                                        "tirtc_scan_stop",
                                                        4096,
                                                        NULL,
                                                        5);
    if (task_ret != pdPASS) {
        app_ui_set_tirtc_config_scan_stop_task_running(false);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t app_ui_on_start_device_binding(void *ctx)
{
    (void)ctx;
    return app_start_device_binding();
}

static esp_err_t app_ui_on_reset_device_binding(void *ctx)
{
    (void)ctx;
    return app_reset_device_binding();
}

static esp_err_t app_ui_on_wechat_contact(const char *open_id, void *ctx)
{
    (void)ctx;
    return app_wechat_call_contact(open_id);
}

static esp_err_t app_ui_on_add_wechat_contact(const char *open_id, void *ctx)
{
    (void)ctx;
    return app_wechat_add_contact(open_id);
}

static esp_err_t app_ui_on_remove_wechat_contact(const char *open_id, void *ctx)
{
    (void)ctx;
    return app_wechat_remove_contact(open_id);
}

static esp_err_t app_ui_on_update_wechat_contact_remark(const char *open_id,
                                                        const char *remark,
                                                        void *ctx)
{
    (void)ctx;
    return app_wechat_update_contact_remark(open_id, remark);
}

static esp_err_t app_ui_on_scan_wechat_contact(void *ctx)
{
    (void)ctx;
    return app_scan_wechat_contact();
}

static esp_err_t app_ui_on_start_wechat_contact_scan(display_scan_preview_cb_t preview_cb,
                                                     display_wechat_contact_scan_result_cb_t result_cb,
                                                     void *scan_ctx,
                                                     void *ctx)
{
    (void)ctx;
    esp_err_t ret = app_enter_app(APP_ID_WECHAT);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "enter wechat before contact scan failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return app_start_wechat_contact_scan(preview_cb,
                                         (app_wechat_contact_scan_result_cb_t)result_cb,
                                         scan_ctx);
}

static esp_err_t app_ui_on_stop_wechat_contact_scan(void *ctx)
{
    (void)ctx;

    if (app_ui_wechat_contact_scan_stop_task_is_running()) {
        return ESP_OK;
    }

    app_ui_set_wechat_contact_scan_stop_task_running(true);
    BaseType_t task_ret = app_ui_create_background_task(app_ui_stop_wechat_contact_scan_task,
                                                        "wechat_scan_stop",
                                                        4096,
                                                        NULL,
                                                        5);
    if (task_ret != pdPASS) {
        app_ui_set_wechat_contact_scan_stop_task_running(false);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t app_ui_on_wechat_hangup_call(void *ctx)
{
    (void)ctx;
    return app_wechat_hangup_call();
}

static esp_err_t app_ui_on_wechat_accept_call(void *ctx)
{
    (void)ctx;
    return app_wechat_accept_call();
}

static esp_err_t app_ui_on_wechat_reject_call(void *ctx)
{
    (void)ctx;
    return app_wechat_reject_call();
}

static esp_err_t app_ui_on_accept_call(void *ctx)
{
    (void)ctx;
    return app_request_accept_call();
}

static esp_err_t app_ui_on_reject_call(void *ctx)
{
    (void)ctx;
    return app_reject_call();
}

static esp_err_t app_ui_on_start_ota(void *ctx)
{
    (void)ctx;
    return app_start_ota();
}

static esp_err_t app_ui_on_restart_for_ota(void *ctx)
{
    (void)ctx;
    app_restart_for_ota();
    return ESP_OK;
}

static esp_err_t app_ui_on_close_ai_chat(void *ctx)
{
    (void)ctx;
    return app_close_ai_chat();
}

static esp_err_t app_ui_on_start_ai_chat(void *ctx)
{
    (void)ctx;
    return app_request_start_ai_chat();
}

static esp_err_t app_ui_on_clear_ai_chat_messages(void *ctx)
{
    (void)ctx;
    return app_clear_ai_chat_messages();
}

static esp_err_t app_ui_on_set_ai_chat_avatar(uint8_t avatar, void *ctx)
{
    (void)ctx;
    return app_set_ai_chat_avatar(avatar);
}

static esp_err_t app_ui_on_set_tirtc_config_field(display_tirtc_config_field_t field,
                                                  const char *value,
                                                  void *ctx)
{
    (void)ctx;
    return app_update_rtc_config_field(app_ui_from_display_rtc_field(field), value);
}

static esp_err_t app_ui_on_set_tirtc_server_env(display_tirtc_server_env_t env, void *ctx)
{
    (void)ctx;
    return app_set_rtc_server_env(app_ui_from_display_rtc_env(env));
}

static esp_err_t app_ui_on_enter_app(display_app_id_t app_id, void *ctx)
{
    (void)ctx;
    return app_request_enter_app(app_ui_from_display_app(app_id));
}

static esp_err_t app_ui_on_return_home(void *ctx)
{
    (void)ctx;
    return app_request_return_home();
}

void app_ui_configure_display_actions(display_actions_t *actions)
{
    if (actions == NULL) {
        return;
    }

    *actions = (display_actions_t){
        .on_wifi_connect = app_ui_on_wifi_connect,
        .on_set_device_uuid = app_ui_on_set_device_uuid,
        .on_wifi_scan = app_ui_on_wifi_scan,
        .on_ping_test = app_ui_on_ping_test,
        .on_disconnect_rtc = app_ui_on_disconnect_rtc,
        .on_hangup_call = app_ui_on_hangup_call,
        .on_start_ota = app_ui_on_start_ota,
        .on_restart_for_ota = app_ui_on_restart_for_ota,
        .on_set_local_video_enabled = app_ui_on_set_local_video_enabled,
        .on_set_local_audio_enabled = app_ui_on_set_local_audio_enabled,
        .on_set_speaker_volume = app_ui_on_set_speaker_volume,
        .on_set_capture_gain = app_ui_on_set_capture_gain,
        .on_call_contact = app_ui_on_call_contact,
        .on_add_call_contact = app_ui_on_add_call_contact,
        .on_respond_call_contact = app_ui_on_respond_call_contact,
        .on_update_call_contact_remark = app_ui_on_update_call_contact_remark,
        .on_delete_call_contact = app_ui_on_delete_call_contact,
        .on_refresh_call_contacts = app_ui_on_refresh_call_contacts,
        .on_scan_contact = app_ui_on_scan_contact,
        .on_start_contact_scan = app_ui_on_start_contact_scan,
        .on_stop_contact_scan = app_ui_on_stop_contact_scan,
        .on_start_tirtc_config_scan = app_ui_on_start_tirtc_config_scan,
        .on_stop_tirtc_config_scan = app_ui_on_stop_tirtc_config_scan,
        .on_start_device_binding = app_ui_on_start_device_binding,
        .on_reset_device_binding = app_ui_on_reset_device_binding,
        .on_wechat_contact = app_ui_on_wechat_contact,
        .on_add_wechat_contact = app_ui_on_add_wechat_contact,
        .on_remove_wechat_contact = app_ui_on_remove_wechat_contact,
        .on_update_wechat_contact_remark = app_ui_on_update_wechat_contact_remark,
        .on_scan_wechat_contact = app_ui_on_scan_wechat_contact,
        .on_start_wechat_contact_scan = app_ui_on_start_wechat_contact_scan,
        .on_stop_wechat_contact_scan = app_ui_on_stop_wechat_contact_scan,
        .on_wechat_hangup_call = app_ui_on_wechat_hangup_call,
        .on_wechat_accept_call = app_ui_on_wechat_accept_call,
        .on_wechat_reject_call = app_ui_on_wechat_reject_call,
        .on_accept_call = app_ui_on_accept_call,
        .on_reject_call = app_ui_on_reject_call,
        .on_start_ai_chat = app_ui_on_start_ai_chat,
        .on_close_ai_chat = app_ui_on_close_ai_chat,
        .on_clear_ai_chat_messages = app_ui_on_clear_ai_chat_messages,
        .on_set_ai_chat_avatar = app_ui_on_set_ai_chat_avatar,
        .on_set_tirtc_config_field = app_ui_on_set_tirtc_config_field,
        .on_set_tirtc_server_env = app_ui_on_set_tirtc_server_env,
        .on_enter_app = app_ui_on_enter_app,
        .on_return_home = app_ui_on_return_home,
        .ctx = NULL,
    };
}

void app_ui_fill_display_status(display_status_t *status, void *ctx)
{
    app_snapshot_t *snapshot = app_ui_display_snapshot();
    uint16_t scan_count = 0;
    uint8_t call_contact_count = 0;
    uint8_t wechat_contact_count = 0;

    (void)ctx;

    if (status == NULL) {
        return;
    }

    memset(status, 0, sizeof(*status));
    if (snapshot == NULL) {
        ESP_LOGE(TAG, "display snapshot alloc failed");
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    app_get_snapshot(snapshot);

    status->network_connected = snapshot->network.connected;
    status->network_rssi = snapshot->network.rssi;
    strlcpy(status->network_ip_addr, snapshot->network.ip_addr, sizeof(status->network_ip_addr));
    strlcpy(status->network_ssid, snapshot->network.ssid, sizeof(status->network_ssid));
    strlcpy(status->saved_network_ssid,
            snapshot->network.saved_ssid,
            sizeof(status->saved_network_ssid));
    strlcpy(status->saved_network_password,
            snapshot->network.saved_password,
            sizeof(status->saved_network_password));
    status->network_connect_failed = snapshot->network.connect_failed;
    status->wifi_scan_in_progress = snapshot->network.scan_in_progress;
    scan_count = snapshot->network.scan_count > DISPLAY_WIFI_SCAN_MAX ?
        DISPLAY_WIFI_SCAN_MAX : snapshot->network.scan_count;
    status->wifi_scan_count = scan_count;
    for (uint16_t index = 0; index < scan_count; ++index) {
        strlcpy(status->wifi_scan_results[index].ssid,
                snapshot->network.scan_results[index].ssid,
                sizeof(status->wifi_scan_results[index].ssid));
        status->wifi_scan_results[index].rssi = snapshot->network.scan_results[index].rssi;
        status->wifi_scan_results[index].secure = snapshot->network.scan_results[index].secure;
        status->wifi_scan_results[index].channel = snapshot->network.scan_results[index].channel;
    }
    status->ping_running = snapshot->network.ping_running;
    status->ping_valid = snapshot->network.ping_valid;
    status->ping_transmitted = snapshot->network.ping_transmitted;
    status->ping_received = snapshot->network.ping_received;
    status->ping_latency_avg_ms = snapshot->network.ping_latency_avg_ms;
    status->ping_jitter_ms = snapshot->network.ping_jitter_ms;
    status->ping_loss_percent = snapshot->network.ping_loss_percent;
    status->test_running = snapshot->test.sender_running;
    strlcpy(status->test_status, snapshot->test.sender_status, sizeof(status->test_status));
    status->ota_state = (display_ota_state_t)snapshot->ota.state;
    status->ota_running = snapshot->ota.running;
    status->ota_progress_percent = snapshot->ota.progress_percent;
    status->ota_bytes_read = snapshot->ota.bytes_read;
    status->ota_total_size = snapshot->ota.total_size;
    status->ota_last_error = snapshot->ota.last_error;
    strlcpy(status->ota_current_version,
            snapshot->ota.current_version,
            sizeof(status->ota_current_version));
    strlcpy(status->ota_target_version,
            snapshot->ota.target_version,
            sizeof(status->ota_target_version));
    strlcpy(status->ota_url, snapshot->ota.url, sizeof(status->ota_url));
    strlcpy(status->ota_message, snapshot->ota.message, sizeof(status->ota_message));

    strlcpy(status->device_uuid, snapshot->device.uuid, sizeof(status->device_uuid));
    status->cpu_usage_percent = snapshot->device.cpu_usage_percent;
    status->device_door_open = snapshot->device.door_open;
    app_ui_fill_memory_status(status);

    status->rtc_connected = snapshot->rtc.connected;
    status->rtc_call_active = snapshot->rtc.call_active;
    status->rtc_incoming_call_pending = snapshot->rtc.incoming_call_pending;
    status->rtc_video_enabled = snapshot->controls.effective_video_enabled;
    status->rtc_audio_enabled = snapshot->controls.effective_audio_enabled;
    status->rtc_local_audio_send_enabled = snapshot->rtc.local_audio_send_enabled;
    status->rtc_state = snapshot->rtc.state;
    status->rtc_tx_video_frames = snapshot->rtc.tx_video_frames;
    status->rtc_rx_video_frames = snapshot->rtc.rx_video_frames;
    status->rtc_tx_audio_frames = snapshot->rtc.tx_audio_frames;
    status->rtc_rx_audio_frames = snapshot->rtc.rx_audio_frames;
    status->rtc_tx_video_fps = snapshot->rtc.tx_video_fps;
    status->rtc_rx_video_fps = snapshot->rtc.rx_video_fps;
    status->rtc_tx_audio_fps = snapshot->rtc.tx_audio_fps;
    status->rtc_rx_audio_fps = snapshot->rtc.rx_audio_fps;
    status->rtc_tx_video_width = snapshot->rtc.tx_video_width;
    status->rtc_tx_video_height = snapshot->rtc.tx_video_height;
    status->rtc_tx_video_target_fps = snapshot->rtc.tx_video_target_fps;
    status->rtc_tx_video_configured_bitrate_kbps = snapshot->rtc.tx_video_configured_bitrate_kbps;
    status->rtc_tx_video_measured_fps_x10 = snapshot->rtc.tx_video_measured_fps_x10;
    status->rtc_tx_video_measured_bitrate_kbps = snapshot->rtc.tx_video_measured_bitrate_kbps;
    status->rtc_tx_video_transport_bitrate_kbps =
        snapshot->rtc.tx_video_transport_bitrate_kbps;
    status->rtc_rx_video_transport_bitrate_kbps =
        snapshot->rtc.rx_video_transport_bitrate_kbps;
    strlcpy(status->tirtc_device_id,
            snapshot->rtc_config.device_id,
            sizeof(status->tirtc_device_id));
    strlcpy(status->tirtc_device_secret,
            snapshot->rtc_config.device_secret,
            sizeof(status->tirtc_device_secret));
    strlcpy(status->tirtc_token_subject,
            snapshot->rtc_config.token_subject,
            sizeof(status->tirtc_token_subject));
    strlcpy(status->tirtc_access_key_id,
            snapshot->rtc_config.access_key_id,
            sizeof(status->tirtc_access_key_id));
    strlcpy(status->tirtc_access_key_secret,
            snapshot->rtc_config.access_key_secret,
            sizeof(status->tirtc_access_key_secret));
    strlcpy(status->tirtc_access_url,
            snapshot->rtc_config.access_url,
            sizeof(status->tirtc_access_url));
    strlcpy(status->tirtc_server_api,
            snapshot->rtc_config.server_api,
            sizeof(status->tirtc_server_api));
    status->tirtc_server_env = app_ui_to_display_rtc_env(snapshot->rtc_config.server_env);
    status->binding_state = app_ui_to_display_binding_state(snapshot->binding.state);
    status->binding_running = snapshot->binding.running;
    strlcpy(status->binding_code, snapshot->binding.code, sizeof(status->binding_code));
    strlcpy(status->binding_message,
            snapshot->binding.message,
            sizeof(status->binding_message));
    status->call_state = app_ui_to_display_call_state(snapshot->call.state);
    status->call_type = snapshot->call.type == APP_CALL_TYPE_VIDEO ?
        DISPLAY_CALL_TYPE_VIDEO : DISPLAY_CALL_TYPE_AUDIO;
    status->call_contacts_ready = snapshot->call_contacts.ready;
    status->call_contacts_refreshing = snapshot->call_contacts.refreshing;
    status->call_last_error = snapshot->call.last_error;
    status->call_contacts_last_error = snapshot->call_contacts.last_error;
    strlcpy(status->call_peer_device_id,
            snapshot->call.peer_device_id,
            sizeof(status->call_peer_device_id));
    strlcpy(status->call_room_id,
            snapshot->call.room_id,
            sizeof(status->call_room_id));
    strlcpy(status->call_message,
            snapshot->call.message,
            sizeof(status->call_message));
    call_contact_count = snapshot->call_contacts.count > DISPLAY_CALL_CONTACT_MAX ?
        DISPLAY_CALL_CONTACT_MAX : snapshot->call_contacts.count;
    status->call_contact_count = call_contact_count;
    for (uint8_t index = 0; index < call_contact_count; ++index) {
        strlcpy(status->call_contacts[index].device_id,
                snapshot->call_contacts.contacts[index].device_id,
                sizeof(status->call_contacts[index].device_id));
        strlcpy(status->call_contacts[index].remark,
                snapshot->call_contacts.contacts[index].remark,
                sizeof(status->call_contacts[index].remark));
        status->call_contacts[index].online = snapshot->call_contacts.contacts[index].online;
        status->call_contacts[index].deletable =
            snapshot->call_contacts.contacts[index].deletable;
    }
    status->call_pending_contact_count =
        snapshot->call_contacts.pending_count > DISPLAY_CALL_CONTACT_MAX ?
        DISPLAY_CALL_CONTACT_MAX : snapshot->call_contacts.pending_count;
    for (uint8_t index = 0; index < status->call_pending_contact_count; ++index) {
        strlcpy(status->call_pending_contacts[index].device_id,
                snapshot->call_contacts.pending[index].device_id,
                sizeof(status->call_pending_contacts[index].device_id));
        strlcpy(status->call_pending_contacts[index].created_at,
                snapshot->call_contacts.pending[index].created_at,
                sizeof(status->call_pending_contacts[index].created_at));
    }
    status->wechat_incoming_call_pending = snapshot->wechat.incoming_call_pending;
    status->wechat_call_state = app_ui_to_display_wechat_call_state(snapshot->wechat.call_state);
    status->wechat_contacts_ready = snapshot->wechat.contacts_ready;
    status->wechat_contacts_server_synced = snapshot->wechat.contacts_server_synced;
    status->wechat_contacts_last_error = snapshot->wechat.contacts_last_error;
    wechat_contact_count = snapshot->wechat.count > DISPLAY_WECHAT_CONTACT_MAX ?
        DISPLAY_WECHAT_CONTACT_MAX : snapshot->wechat.count;
    status->wechat_contact_count = wechat_contact_count;
    for (uint8_t index = 0; index < wechat_contact_count; ++index) {
        strlcpy(status->wechat_contacts[index].open_id,
                snapshot->wechat.contacts[index].open_id,
                sizeof(status->wechat_contacts[index].open_id));
        strlcpy(status->wechat_contacts[index].remark,
                snapshot->wechat.contacts[index].remark,
                sizeof(status->wechat_contacts[index].remark));
    }

    status->audio_ready = snapshot->audio.ready;
    status->audio_speaker_enabled = snapshot->audio.speaker_enabled;
    status->audio_input_level = snapshot->audio.input_level;
    status->audio_output_level = snapshot->audio.output_level;
    status->audio_speaker_volume_percent = snapshot->audio.speaker_volume_percent;
    status->audio_capture_gain_percent = snapshot->audio.capture_gain_percent;

    status->ai_chat_state = snapshot->ai_chat.state;
    status->ai_chat_active = snapshot->ai_chat.active;
    status->ai_chat_listening = snapshot->ai_chat.listening;
    status->ai_chat_cloud_speaking = snapshot->ai_chat.cloud_speaking;
    status->ai_chat_video_active = snapshot->ai_chat.video_active;
    status->ai_chat_tx_audio_frames = snapshot->ai_chat.tx_audio_frames;
    status->ai_chat_tx_video_frames = snapshot->ai_chat.tx_video_frames;
    status->ai_chat_tx_video_failures = snapshot->ai_chat.tx_video_failures;
    status->ai_chat_rx_commands = snapshot->ai_chat.rx_commands;
    status->ai_chat_last_error = snapshot->ai_chat.last_error;
    strlcpy(status->ai_chat_asr_caption,
            snapshot->ai_chat.asr_caption,
            sizeof(status->ai_chat_asr_caption));
    strlcpy(status->ai_chat_tts_caption,
            snapshot->ai_chat.tts_caption,
            sizeof(status->ai_chat_tts_caption));
    status->ai_chat_message_count = snapshot->ai_chat.message_count > DISPLAY_AI_CHAT_MESSAGE_MAX ?
        DISPLAY_AI_CHAT_MESSAGE_MAX : snapshot->ai_chat.message_count;
    for (uint8_t index = 0; index < status->ai_chat_message_count; ++index) {
        status->ai_chat_messages[index].caption_type = snapshot->ai_chat.messages[index].caption_type;
        status->ai_chat_messages[index].utterance_id = snapshot->ai_chat.messages[index].utterance_id;
        strlcpy(status->ai_chat_messages[index].text,
                snapshot->ai_chat.messages[index].text,
                sizeof(status->ai_chat_messages[index].text));
    }
    strlcpy(status->ai_chat_status,
            snapshot->ai_chat.status,
            sizeof(status->ai_chat_status));
    status->ai_chat_avatar = snapshot->ai_chat.avatar;
}
