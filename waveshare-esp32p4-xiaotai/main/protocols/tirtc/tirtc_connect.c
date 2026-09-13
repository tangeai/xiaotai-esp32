#include "tirtc_connect.h"

#include <stdint.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "tirtc_session_internal.h"
#include "tirtc_token.h"

static const char *TAG = "tirtc_connect";

#define TIRTC_CONNECT_TASK_STACK            (24 * 1024)
#define TIRTC_CONNECT_TIMEOUT_TASK_STACK    (6 * 1024)
#define TIRTC_CONNECT_TASK_PRIORITY         5
/* Rapid redial may overlap the previous transport teardown; preserve the SDK's full negotiation window. */
#define TIRTC_CONNECT_RESULT_TIMEOUT_MS     38000U

static portMUX_TYPE s_connect_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_tirtc_online;
static bool s_connecting;
static uint32_t s_connect_generation;
static EXT_RAM_BSS_ATTR tirtc_session_config_t s_connect_config;
static TIRTCCONNECTCALLBACK s_connect_callback;
static void *s_connect_user_data;
static bool s_connect_has_provided_token;
static EXT_RAM_BSS_ATTR char s_connect_provided_token[TIRTC_CONNECT_TOKEN_MAX_LEN];

static void tirtc_connect_task(void *arg);

static uint32_t tirtc_connect_next_generation_locked(void)
{
    s_connect_generation++;
    if (s_connect_generation == 0U) {
        s_connect_generation = 1U;
    }
    return s_connect_generation;
}

static bool tirtc_connect_is_current_locked(uint32_t generation)
{
    return s_tirtc_online && s_connecting && generation == s_connect_generation;
}

void tirtc_connect_on_tirtc_started(void)
{
    taskENTER_CRITICAL(&s_connect_lock);
    s_tirtc_online = true;
    taskEXIT_CRITICAL(&s_connect_lock);
    ESP_LOGI(TAG, "TiRTC connect runtime is ready");
}

bool tirtc_connect_is_connecting(void)
{
    bool connecting = false;

    taskENTER_CRITICAL(&s_connect_lock);
    connecting = s_tirtc_online && s_connecting;
    taskEXIT_CRITICAL(&s_connect_lock);

    return connecting;
}

void tirtc_connect_cancel(void)
{
    taskENTER_CRITICAL(&s_connect_lock);
    s_tirtc_online = false;
    s_connecting = false;
    tirtc_connect_next_generation_locked();
    s_connect_callback = NULL;
    s_connect_user_data = NULL;
    memset(&s_connect_config, 0, sizeof(s_connect_config));
    s_connect_has_provided_token = false;
    s_connect_provided_token[0] = '\0';
    taskEXIT_CRITICAL(&s_connect_lock);
}

bool tirtc_connect_abort_attempt(void)
{
    bool aborted = false;
    uint32_t generation = 0;

    taskENTER_CRITICAL(&s_connect_lock);
    if (s_tirtc_online && s_connecting) {
        generation = s_connect_generation;
        s_connecting = false;
        tirtc_connect_next_generation_locked();
        s_connect_callback = NULL;
        s_connect_user_data = NULL;
        memset(&s_connect_config, 0, sizeof(s_connect_config));
        s_connect_has_provided_token = false;
        s_connect_provided_token[0] = '\0';
        aborted = true;
    }
    taskEXIT_CRITICAL(&s_connect_lock);

    if (aborted) {
        ESP_LOGI(TAG,
                 "TiRTC active connect aborted: gen=%lu runtime_online=1",
                 (unsigned long)generation);
    }
    return aborted;
}

static void tirtc_connect_finish_attempt(uint32_t generation, int error)
{
    TIRTCCONNECTCALLBACK callback = NULL;
    void *user_data = NULL;
    bool notify = false;
    bool online = false;

    taskENTER_CRITICAL(&s_connect_lock);
    if (tirtc_connect_is_current_locked(generation)) {
        s_connecting = false;
        callback = s_connect_callback;
        user_data = s_connect_user_data;
        s_connect_callback = NULL;
        s_connect_user_data = NULL;
        s_connect_has_provided_token = false;
        s_connect_provided_token[0] = '\0';
        online = s_tirtc_online;
        notify = online;
    }
    taskEXIT_CRITICAL(&s_connect_lock);

    ESP_LOGW(TAG,
             "TiRTC active connect attempt finished: gen=%lu error=%d %s notify=%d online=%d",
             (unsigned long)generation,
             error,
             error == 0 ? "OK" : TiRtcGetErrorStr(error),
             notify ? 1 : 0,
             online ? 1 : 0);
    if (notify && callback != NULL) {
        callback(error, NULL, user_data);
    }
}

static void tirtc_connect_result_cb(int error, tirtc_conn_t hconn, void *user_data)
{
    uint32_t generation = (uint32_t)(uintptr_t)user_data;
    TIRTCCONNECTCALLBACK callback = NULL;
    void *callback_user_data = NULL;
    bool notify = false;
    bool release_connection = false;
    bool online = false;
    bool connecting = false;
    uint32_t current_generation = 0;

    ESP_LOGI(TAG,
             "TiRTC active connect result callback: gen=%lu error=%d %s hconn=%p",
             (unsigned long)generation,
             error,
             error == 0 ? "OK" : TiRtcGetErrorStr(error),
             hconn);

    taskENTER_CRITICAL(&s_connect_lock);
    online = s_tirtc_online;
    connecting = s_connecting;
    current_generation = s_connect_generation;
    if (tirtc_connect_is_current_locked(generation)) {
        s_connecting = false;
        callback = s_connect_callback;
        callback_user_data = s_connect_user_data;
        s_connect_callback = NULL;
        s_connect_user_data = NULL;
        s_connect_has_provided_token = false;
        s_connect_provided_token[0] = '\0';
        notify = s_tirtc_online;
    } else {
        release_connection = (error == 0 && hconn != NULL);
    }
    taskEXIT_CRITICAL(&s_connect_lock);

    if (release_connection) {
        ESP_LOGW(TAG,
                 "release stale TiRTC active connection: hconn=%p gen=%lu current_gen=%lu online=%d connecting=%d",
                 hconn,
                 (unsigned long)generation,
                 (unsigned long)current_generation,
                 online ? 1 : 0,
                 connecting ? 1 : 0);
        (void)tirtc_session_disconnect_connection(hconn);
        return;
    }

    if (notify && callback != NULL) {
        callback(error, hconn, callback_user_data);
    } else if (error == 0 && hconn != NULL) {
        ESP_LOGW(TAG, "release TiRTC active connection after connect cancel: hconn=%p", hconn);
        (void)tirtc_session_disconnect_connection(hconn);
    }
}

static void tirtc_connect_timeout_task(void *arg)
{
    uint32_t generation = (uint32_t)(uintptr_t)arg;

    vTaskDelay(pdMS_TO_TICKS(TIRTC_CONNECT_RESULT_TIMEOUT_MS));

    if (tirtc_connect_is_connecting()) {
        bool current = false;

        taskENTER_CRITICAL(&s_connect_lock);
        current = tirtc_connect_is_current_locked(generation);
        taskEXIT_CRITICAL(&s_connect_lock);

        if (current) {
            ESP_LOGW(TAG,
                     "TiRTC active connect timeout: gen=%lu timeout_ms=%lu",
                     (unsigned long)generation,
                     (unsigned long)TIRTC_CONNECT_RESULT_TIMEOUT_MS);
            tirtc_connect_finish_attempt(generation, TIRTC_E_TIMEOUTED);
        }
    }

    vTaskDeleteWithCaps(NULL);
}

static void tirtc_connect_task(void *arg)
{
    uint32_t generation = (uint32_t)(uintptr_t)arg;
    int64_t task_started_at_us = esp_timer_get_time();
    tirtc_session_config_t config = {0};
    char connect_token[TIRTC_CONNECT_TOKEN_MAX_LEN] = {0};
    bool current = false;
    bool use_provided_token = false;

    taskENTER_CRITICAL(&s_connect_lock);
    current = tirtc_connect_is_current_locked(generation);
    if (current) {
        config = s_connect_config;
        use_provided_token = s_connect_has_provided_token;
        if (use_provided_token) {
            strlcpy(connect_token, s_connect_provided_token, sizeof(connect_token));
        }
    }
    taskEXIT_CRITICAL(&s_connect_lock);

    if (!current) {
        ESP_LOGW(TAG, "TiRTC active connect canceled before token");
        vTaskDeleteWithCaps(NULL);
        return;
    }

    esp_err_t token_ret = ESP_OK;
    if (use_provided_token) {
        ESP_LOGI(TAG,
                  "TiRTC active connect parameters: source=provided-token gen=%lu remote_id_len=%u token_len=%u",
                  (unsigned long)generation,
                  (unsigned)strlen(config.remote_device_id),
                  (unsigned)strlen(connect_token));
    } else {
        ESP_LOGI(TAG,
                 "TiRTC active connect token request: gen=%lu local_id_len=%u remote_id_len=%u subject_len=%u",
                 (unsigned long)generation,
                 (unsigned)strlen(config.device_id),
                 (unsigned)strlen(config.remote_device_id),
                 (unsigned)strlen(config.token_subject));
        token_ret = tirtc_token_fetch_connect(&config,
                                              config.remote_device_id,
                                              connect_token,
                                              sizeof(connect_token));
    }

    taskENTER_CRITICAL(&s_connect_lock);
    current = tirtc_connect_is_current_locked(generation);
    taskEXIT_CRITICAL(&s_connect_lock);

    if (!current) {
        ESP_LOGW(TAG, "TiRTC active connect canceled after token");
        vTaskDeleteWithCaps(NULL);
        return;
    }

    if (token_ret != ESP_OK) {
        ESP_LOGE(TAG, "TiRTC active connect token failed: %s", esp_err_to_name(token_ret));
        tirtc_connect_finish_attempt(generation, TIRTC_E_INVALID_PARAMETER);
        vTaskDeleteWithCaps(NULL);
        return;
    }

    int connect_ret = TIRTC_E_BUSY;
    if (tirtc_session_take_sdk_api_lock(TIRTC_SESSION_SDK_API_LOCK_WAIT_TICKS)) {
        taskENTER_CRITICAL(&s_connect_lock);
        current = tirtc_connect_is_current_locked(generation);
        taskEXIT_CRITICAL(&s_connect_lock);

        if (current) {
            int64_t connect_started_at_us = esp_timer_get_time();
            ESP_LOGI(TAG,
                      "TiRTC active connect start: gen=%lu source=%s remote_id_len=%u queued_ms=%llu",
                      (unsigned long)generation,
                      use_provided_token ? "provided-token" : "issued-token",
                      (unsigned)strlen(config.remote_device_id),
                      (unsigned long long)((connect_started_at_us - task_started_at_us) / 1000LL));
            connect_ret = TiRtcConnect(config.remote_device_id,
                                       connect_token,
                                       tirtc_connect_result_cb,
                                       (void *)(uintptr_t)generation);
            ESP_LOGI(TAG,
                      "TiRtcConnect returned: gen=%lu source=%s ret=%d %s elapsed_ms=%llu",
                      (unsigned long)generation,
                      use_provided_token ? "provided-token" : "issued-token",
                      connect_ret,
                      connect_ret == 0 ? "OK" : TiRtcGetErrorStr(connect_ret),
                      (unsigned long long)((esp_timer_get_time() - connect_started_at_us) / 1000LL));
        }
        tirtc_session_give_sdk_api_lock();
    }

    if (connect_ret != 0) {
        ESP_LOGE(TAG, "TiRtcConnect failed: %d %s", connect_ret, TiRtcGetErrorStr(connect_ret));
        tirtc_connect_finish_attempt(generation, connect_ret);
    }

    vTaskDeleteWithCaps(NULL);
}

static esp_err_t tirtc_connect_launch_tasks(uint32_t generation)
{
    BaseType_t task_ret = xTaskCreateWithCaps(tirtc_connect_task,
                                              "tirtc_connect",
                                              TIRTC_CONNECT_TASK_STACK,
                                              (void *)(uintptr_t)generation,
                                              TIRTC_CONNECT_TASK_PRIORITY,
                                              NULL,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (task_ret != pdPASS) {
        tirtc_connect_finish_attempt(generation, TIRTC_E_LACK_OF_RESOURCE);
        return ESP_ERR_NO_MEM;
    }

    task_ret = xTaskCreateWithCaps(tirtc_connect_timeout_task,
                                   "tirtc_conn_to",
                                   TIRTC_CONNECT_TIMEOUT_TASK_STACK,
                                   (void *)(uintptr_t)generation,
                                   TIRTC_CONNECT_TASK_PRIORITY,
                                   NULL,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (task_ret != pdPASS) {
        ESP_LOGW(TAG, "TiRTC active connect timeout monitor not created");
    }

    return ESP_OK;
}

esp_err_t tirtc_connect_start(const tirtc_session_config_t *config,
                              TIRTCCONNECTCALLBACK callback,
                              void *user_data)
{
    uint32_t generation = 0;

    if (config == NULL || callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->remote_device_id[0] == '\0') {
        ESP_LOGE(TAG, "TiRTC active connect remote device id is empty");
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_connect_lock);
    if (!s_tirtc_online) {
        taskEXIT_CRITICAL(&s_connect_lock);
        ESP_LOGW(TAG, "TiRTC is not online, active connect is unavailable");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_connecting) {
        taskEXIT_CRITICAL(&s_connect_lock);
        ESP_LOGW(TAG, "TiRTC active connect is already running");
        return ESP_ERR_INVALID_STATE;
    }

    s_connecting = true;
    generation = tirtc_connect_next_generation_locked();
    s_connect_config = *config;
    s_connect_callback = callback;
    s_connect_user_data = user_data;
    s_connect_has_provided_token = false;
    s_connect_provided_token[0] = '\0';
    taskEXIT_CRITICAL(&s_connect_lock);

    ESP_LOGI(TAG,
             "TiRTC active connect queued: gen=%lu local_id_len=%u remote_id_len=%u",
             (unsigned long)generation,
             (unsigned)strlen(config->device_id),
             (unsigned)strlen(config->remote_device_id));

    return tirtc_connect_launch_tasks(generation);
}
esp_err_t tirtc_connect_start_with_token(const char *remote_device_id,
                                         const char *connect_token,
                                         TIRTCCONNECTCALLBACK callback,
                                         void *user_data)
{
    uint32_t generation = 0;

    if (remote_device_id == NULL || remote_device_id[0] == '\0' ||
        connect_token == NULL || connect_token[0] == '\0' || callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(remote_device_id) >= sizeof(s_connect_config.remote_device_id) ||
        strlen(connect_token) >= sizeof(s_connect_provided_token)) {
        return ESP_ERR_INVALID_SIZE;
    }

    taskENTER_CRITICAL(&s_connect_lock);
    if (!s_tirtc_online) {
        taskEXIT_CRITICAL(&s_connect_lock);
        ESP_LOGW(TAG, "TiRTC is not online, token connect is unavailable");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_connecting) {
        taskEXIT_CRITICAL(&s_connect_lock);
        ESP_LOGW(TAG, "TiRTC active connect is already running");
        return ESP_ERR_INVALID_STATE;
    }

    s_connecting = true;
    generation = tirtc_connect_next_generation_locked();
    memset(&s_connect_config, 0, sizeof(s_connect_config));
    strlcpy(s_connect_config.remote_device_id, remote_device_id, sizeof(s_connect_config.remote_device_id));
    strlcpy(s_connect_provided_token, connect_token, sizeof(s_connect_provided_token));
    s_connect_has_provided_token = true;
    s_connect_callback = callback;
    s_connect_user_data = user_data;
    taskEXIT_CRITICAL(&s_connect_lock);

    ESP_LOGI(TAG,
             "TiRTC active connect queued with provided token: gen=%lu remote_id_len=%u token_len=%u",
             (unsigned long)generation,
             (unsigned)strlen(remote_device_id),
             (unsigned)strlen(connect_token));

    return tirtc_connect_launch_tasks(generation);
}
