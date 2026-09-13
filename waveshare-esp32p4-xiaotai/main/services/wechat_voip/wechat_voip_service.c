#include "wechat_voip_service.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "app_task_affinity.h"
#include "network.h"
#include "tirtc_session.h"
#include "wechat_voip_config.h"
#include "wechat_voip_session.h"
#include "wechat_voip_thing.h"

static const char *TAG = "wx_voip_service";

#define WECHAT_VOIP_SERVICE_TASK_STACK        (8 * 1024)
#define WECHAT_VOIP_SERVICE_TASK_PRIORITY     4
#define WECHAT_VOIP_SERVICE_POLL_MS           500
#define WECHAT_VOIP_RTC_PREPARE_RETRY_MS      5000
#define WECHAT_VOIP_START_RETRY_LOG_MS        5000
#define WECHAT_VOIP_MEDIA_STOP_WAIT_MS        500
#define WECHAT_VOIP_SESSION_RELEASE_WAIT_MS   1800

static TaskHandle_t s_service_task;
static bool s_registered;
static volatile bool s_ingress_enabled;
static volatile bool s_session_enabled;
static bool s_service_task_starting;
static portMUX_TYPE s_service_state_lock = portMUX_INITIALIZER_UNLOCKED;

esp_err_t wechat_voip_service_configure_media_lifecycle(
    const wechat_voip_media_lifecycle_t *lifecycle,
    void *ctx)
{
    return wechat_voip_media_configure_lifecycle(lifecycle, ctx);
}

esp_err_t wechat_voip_service_set_incoming_policy(
    wechat_voip_incoming_allowed_cb_t callback,
    void *ctx)
{
    return wechat_voip_thing_set_incoming_policy(callback, ctx);
}

static bool wechat_voip_service_on_command(tirtc_conn_t conn,
                                           uint32_t cmdw,
                                           const void *data,
                                           uint32_t data_len,
                                           void *ctx)
{
    (void)ctx;
    return wechat_voip_session_on_command(conn, cmdw, data, data_len);
}

static void wechat_voip_service_on_connection_error(tirtc_conn_t conn, int error, void *ctx)
{
    (void)ctx;
    (void)wechat_voip_session_on_conn_error(conn, error);
}

static void wechat_voip_service_on_disconnected(tirtc_conn_t conn, void *ctx)
{
    (void)ctx;
    (void)wechat_voip_session_on_disconnected(conn);
}

static void notify_service_task(void)
{
    TaskHandle_t task = NULL;

    taskENTER_CRITICAL(&s_service_state_lock);
    task = s_service_task;
    taskEXIT_CRITICAL(&s_service_state_lock);
    if (task != NULL) {
        xTaskNotifyGive(task);
    }
}

static void stop_media_and_wait(void)
{
    esp_err_t ret = wechat_voip_media_stop_wait(NULL, WECHAT_VOIP_MEDIA_STOP_WAIT_MS);

    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "wechat media stop wait failed: %s", esp_err_to_name(ret));
    }
}

static void wait_session_released(void)
{
    esp_err_t ret =
        wechat_voip_session_wait_until_released(
            WECHAT_VOIP_SESSION_RELEASE_WAIT_MS);

    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG,
                 "wechat session release wait failed: %s",
                 esp_err_to_name(ret));
    }
}

static void wechat_voip_service_task(void *ctx)
{
    (void)ctx;
    TickType_t last_start_log_tick = 0;
    TickType_t last_rtc_prepare_tick = 0;

    while (true) {
        if (APP_CONFIG_WECHAT_VOIP_ENABLE && s_ingress_enabled && network_is_connected()) {
            tirtc_session_stats_t rtc_stats = {0};
            esp_err_t thing_ret = wechat_voip_thing_start();
            esp_err_t rtc_ret = ESP_OK;

            /* An identity reset can race a channel start. Recheck ownership
             * after start so the old listener cannot be resurrected. */
            if (!s_ingress_enabled) {
                wechat_voip_thing_stop();
            }

            if (s_session_enabled) {
                tirtc_session_get_stats(&rtc_stats);
                if (rtc_stats.sdk_started) {
                    last_rtc_prepare_tick = 0;
                } else if (rtc_stats.state != TIRTC_SESSION_STATE_STARTING) {
                    TickType_t now = xTaskGetTickCount();
                    bool prepare_due =
                        last_rtc_prepare_tick == 0 ||
                        now - last_rtc_prepare_tick >=
                            pdMS_TO_TICKS(WECHAT_VOIP_RTC_PREPARE_RETRY_MS);
                    if (prepare_due) {
                        last_rtc_prepare_tick = now;
                        rtc_ret = tirtc_session_prepare_sdk();
                    }
                }
            } else {
                last_rtc_prepare_tick = 0;
            }

            if (rtc_ret != ESP_OK || thing_ret != ESP_OK) {
                TickType_t now = xTaskGetTickCount();
                if (last_start_log_tick == 0 ||
                    now - last_start_log_tick >=
                        pdMS_TO_TICKS(WECHAT_VOIP_START_RETRY_LOG_MS)) {
                    last_start_log_tick = now;
                    bool waiting =
                        (thing_ret == ESP_OK || thing_ret == ESP_ERR_INVALID_STATE) &&
                        (rtc_ret == ESP_OK || rtc_ret == ESP_ERR_INVALID_STATE);
                    if (waiting) {
                        ESP_LOGI(TAG,
                                 "wechat VoIP waiting: ingress=%s rtc=%s page=%d",
                                 esp_err_to_name(thing_ret),
                                 esp_err_to_name(rtc_ret),
                                 s_session_enabled ? 1 : 0);
                    } else {
                        ESP_LOGW(TAG,
                                 "wechat VoIP start failed: ingress=%s rtc=%s page=%d",
                                 esp_err_to_name(thing_ret),
                                 esp_err_to_name(rtc_ret),
                                 s_session_enabled ? 1 : 0);
                    }
                }
            }
        }

        wechat_voip_service_maintenance();
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WECHAT_VOIP_SERVICE_POLL_MS));
    }
}

esp_err_t wechat_voip_service_start_ingress(void)
{
    TaskHandle_t service_task = NULL;
    bool create_task = false;

    if (!APP_CONFIG_WECHAT_VOIP_ENABLE) {
        ESP_LOGI(TAG, "wechat VoIP disabled");
        return ESP_OK;
    }

    taskENTER_CRITICAL(&s_service_state_lock);
    s_ingress_enabled = true;
    service_task = s_service_task;
    if (service_task == NULL && !s_service_task_starting) {
        s_service_task_starting = true;
        create_task = true;
    }
    taskEXIT_CRITICAL(&s_service_state_lock);

    if (service_task != NULL) {
        xTaskNotifyGive(service_task);
        return ESP_OK;
    }
    if (!create_task) {
        return ESP_OK;
    }

    /*
     * This small task coordinates RTC and MQTT control paths, so its stack
     * stays in internal RAM. Media workers and large payloads remain in PSRAM.
     */
    TaskHandle_t created_task = NULL;
    BaseType_t task_ret = xTaskCreateWithCaps(wechat_voip_service_task,
                                              "wx_voip_svc",
                                              WECHAT_VOIP_SERVICE_TASK_STACK,
                                              NULL,
                                              WECHAT_VOIP_SERVICE_TASK_PRIORITY,
                                              &created_task,
                                              APP_TASK_STACK_CAPS_INTERNAL);
    taskENTER_CRITICAL(&s_service_state_lock);
    if (task_ret == pdPASS) {
        s_service_task = created_task;
    }
    s_service_task_starting = false;
    taskEXIT_CRITICAL(&s_service_state_lock);
    ESP_RETURN_ON_FALSE(task_ret == pdPASS,
                        ESP_ERR_NO_MEM,
                        TAG,
                        "create wechat voip service task failed");
    ESP_LOGI(TAG, "wechat VoIP device ingress started");
    return ESP_OK;
}

esp_err_t wechat_voip_service_start(void)
{
    esp_err_t ret = ESP_OK;

    if (!APP_CONFIG_WECHAT_VOIP_ENABLE) {
        ESP_LOGI(TAG, "wechat VoIP disabled");
        return ESP_OK;
    }

    if (!s_registered) {
        const tirtc_session_observer_t observer = {
            .on_command = wechat_voip_service_on_command,
            .on_connection_error = wechat_voip_service_on_connection_error,
            .on_disconnected = wechat_voip_service_on_disconnected,
        };
        ESP_RETURN_ON_ERROR(tirtc_session_register_observer(&observer, NULL),
                            TAG,
                            "register rtc observer failed");
        s_registered = true;
    }

    taskENTER_CRITICAL(&s_service_state_lock);
    s_session_enabled = true;
    taskEXIT_CRITICAL(&s_service_state_lock);

    ret = wechat_voip_service_start_ingress();
    if (ret != ESP_OK) {
        return ret;
    }

    /* First channel start refreshes automatically. Later page entries request
     * a fresh online contact snapshot without blocking the UI lifecycle. */
    ret = wechat_voip_thing_refresh_contacts_async();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "wechat contact refresh request failed: %s", esp_err_to_name(ret));
    }
    return ESP_OK;
}

void wechat_voip_service_stop(void)
{
    if (!APP_CONFIG_WECHAT_VOIP_ENABLE) {
        return;
    }

    taskENTER_CRITICAL(&s_service_state_lock);
    s_session_enabled = false;
    taskEXIT_CRITICAL(&s_service_state_lock);

    if (!wechat_voip_session_is_idle()) {
        wechat_voip_session_hangup();
    }
    (void)wechat_voip_thing_cancel_pending_call();
    /* Close an admission that raced the first session check. */
    if (!wechat_voip_session_is_idle()) {
        wechat_voip_session_hangup();
    }
    stop_media_and_wait();
    wait_session_released();
    notify_service_task();
}

void wechat_voip_service_suspend_ingress(void)
{
    if (!APP_CONFIG_WECHAT_VOIP_ENABLE) {
        return;
    }

    taskENTER_CRITICAL(&s_service_state_lock);
    s_session_enabled = false;
    s_ingress_enabled = false;
    taskEXIT_CRITICAL(&s_service_state_lock);

    /*
     * Drain the identity-scoped dispatcher before closing RTC. Otherwise a
     * queued message from the previous identity can recreate a call session.
     */
    wechat_voip_thing_stop();
    if (!wechat_voip_session_is_idle()) {
        wechat_voip_session_hangup();
    }
    stop_media_and_wait();
    wait_session_released();
    notify_service_task();
}

esp_err_t wechat_voip_service_answer(void)
{
    if (!APP_CONFIG_WECHAT_VOIP_ENABLE || !s_session_enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    return wechat_voip_session_answer();
}

esp_err_t wechat_voip_service_reject_or_hangup(void)
{
    if (!APP_CONFIG_WECHAT_VOIP_ENABLE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!wechat_voip_session_is_idle()) {
        wechat_voip_session_hangup();
        return ESP_OK;
    }
    if (wechat_voip_thing_cancel_pending_call()) {
        return ESP_OK;
    }
    /* call_incoming may move the request into the session between checks. */
    if (!wechat_voip_session_is_idle()) {
        wechat_voip_session_hangup();
        return ESP_OK;
    }
    return ESP_ERR_INVALID_STATE;
}

esp_err_t wechat_voip_service_request_call(const char *open_id,
                                           wechat_voip_call_media_t call_media)
{
    if (!APP_CONFIG_WECHAT_VOIP_ENABLE || !s_session_enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    return wechat_voip_thing_request_call(open_id, call_media);
}

esp_err_t wechat_voip_service_refresh_contacts_async(void)
{
    if (!APP_CONFIG_WECHAT_VOIP_ENABLE) {
        return ESP_ERR_INVALID_STATE;
    }
    return wechat_voip_thing_refresh_contacts_async();
}

esp_err_t wechat_voip_service_update_contact_remark(const char *open_id,
                                                    const char *remark)
{
    if (!APP_CONFIG_WECHAT_VOIP_ENABLE || !s_session_enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    return wechat_voip_thing_update_contact_remark_async(open_id, remark);
}

bool wechat_voip_service_is_enabled(void)
{
    return APP_CONFIG_WECHAT_VOIP_ENABLE != 0;
}

bool wechat_voip_service_is_connected(void)
{
    return wechat_voip_service_is_enabled() &&
           wechat_voip_thing_is_connected();
}

esp_err_t wechat_voip_service_add_contact(const char *open_id)
{
    if (!APP_CONFIG_WECHAT_VOIP_ENABLE) {
        return ESP_ERR_INVALID_STATE;
    }
    return wechat_voip_thing_add_contact(open_id);
}

esp_err_t wechat_voip_service_remove_contact(const char *open_id)
{
    if (!APP_CONFIG_WECHAT_VOIP_ENABLE) {
        return ESP_ERR_INVALID_STATE;
    }
    return wechat_voip_thing_remove_contact(open_id);
}

bool wechat_voip_service_has_incoming_call(void)
{
    if (!APP_CONFIG_WECHAT_VOIP_ENABLE) {
        return false;
    }
    return wechat_voip_session_has_incoming_call();
}

wechat_voip_call_state_t wechat_voip_service_get_call_state(void)
{
    if (!APP_CONFIG_WECHAT_VOIP_ENABLE) {
        return WECHAT_VOIP_CALL_STATE_IDLE;
    }

    switch (wechat_voip_session_get_state()) {
    case WECHAT_VOIP_SESSION_STATE_RINGING:
        return WECHAT_VOIP_CALL_STATE_INCOMING;
    case WECHAT_VOIP_SESSION_STATE_CONNECTING:
    case WECHAT_VOIP_SESSION_STATE_AWAITING_CONNECTED:
        return WECHAT_VOIP_CALL_STATE_CONNECTING;
    case WECHAT_VOIP_SESSION_STATE_IN_CALL:
        return WECHAT_VOIP_CALL_STATE_IN_CALL;
    case WECHAT_VOIP_SESSION_STATE_CLOSING:
        return WECHAT_VOIP_CALL_STATE_CLOSING;
    case WECHAT_VOIP_SESSION_STATE_IDLE:
    default:
        break;
    }

    if (wechat_voip_thing_request_call_cancelling()) {
        return WECHAT_VOIP_CALL_STATE_CLOSING;
    }
    if (wechat_voip_thing_request_call_busy()) {
        return WECHAT_VOIP_CALL_STATE_CALLING;
    }
    return WECHAT_VOIP_CALL_STATE_IDLE;
}

void wechat_voip_service_maintenance(void)
{
    wechat_voip_session_maintenance();
    wechat_voip_thing_maintenance();
}

void wechat_voip_service_get_contacts(wechat_voip_contacts_snapshot_t *snapshot)
{
    wechat_voip_thing_get_contacts(snapshot);
}
