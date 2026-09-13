#include "system_time.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "app_task_affinity.h"

static const char *TAG = "system_time";

#define TIME_SYNC_MIN_VALID_UNIX_TIME 1704067200LL
#define TIME_SYNC_TIMEOUT_MS          1500U
#define TIME_SYNC_RETRY_COUNT         8U
#define TIME_SYNC_RETRY_DELAY_MS      5000U
#define TIME_SYNC_SNTP_SERVER_COUNT   4U
#define TIME_SYNC_SNTP_SERVER_0       "ntp.aliyun.com"
#define TIME_SYNC_SNTP_SERVER_1       "ntp.tencent.com"
#define TIME_SYNC_SNTP_SERVER_2       "ntp.huaweicloud.com"
#define TIME_SYNC_SNTP_SERVER_3       "cn.pool.ntp.org"
/* Last-resort clock bootstrap only; no credentials or device identity are sent. */
#define TIME_SYNC_HTTP_DATE_URL       "http://ep-open.tangeopen.com/services"
#define TIME_SYNC_HTTP_TIMEOUT_MS     5000U
#define TIME_SYNC_TASK_STACK_SIZE     (4U * 1024U)
#define TIME_SYNC_TASK_PRIORITY       3U

typedef struct {
    char date[48];
} system_time_http_ctx_t;

static portMUX_TYPE s_time_sync_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_time_sync_running;
static bool s_time_sync_force_requested;
static system_time_sync_cb_t s_time_sync_cb;
static void *s_time_sync_cb_ctx;

static void system_time_sync_task(void *ctx);
static void system_time_notify_sync_done(esp_err_t result);

static int64_t system_time_days_from_civil(int year, unsigned month, unsigned day)
{
    year -= month <= 2U;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = (unsigned)(year - era * 400);
    const unsigned shifted_month = month > 2U ? month - 3U : month + 9U;
    const unsigned day_of_year = (153U * shifted_month + 2U) / 5U + day - 1U;
    const unsigned day_of_era =
        year_of_era * 365U + year_of_era / 4U - year_of_era / 100U + day_of_year;
    return (int64_t)era * 146097LL + (int64_t)day_of_era - 719468LL;
}

static int system_time_month_number(const char *month)
{
    static const char *const months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };

    for (size_t index = 0; index < sizeof(months) / sizeof(months[0]); ++index) {
        if (strcmp(month, months[index]) == 0) {
            return (int)index + 1;
        }
    }
    return 0;
}

static esp_err_t system_time_parse_http_date(const char *date, time_t *unix_time)
{
    char weekday[4] = {0};
    char month_name[4] = {0};
    char zone[4] = {0};
    int day = 0;
    int year = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;

    if (date == NULL || unix_time == NULL ||
        sscanf(date,
               "%3[^,], %d %3s %d %d:%d:%d %3s",
               weekday,
               &day,
               month_name,
               &year,
               &hour,
               &minute,
               &second,
               zone) != 8) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const int month = system_time_month_number(month_name);
    if (month == 0 || strcmp(zone, "GMT") != 0 || year < 2024 || year > 2100 ||
        day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 60) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const int64_t days = system_time_days_from_civil(year, (unsigned)month, (unsigned)day);
    const int64_t seconds = days * 86400LL + hour * 3600LL + minute * 60LL + second;
    if (seconds < TIME_SYNC_MIN_VALID_UNIX_TIME) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *unix_time = (time_t)seconds;
    return ESP_OK;
}

static esp_err_t system_time_http_event_handler(esp_http_client_event_t *event)
{
    system_time_http_ctx_t *http_ctx = event != NULL ? event->user_data : NULL;

    if (http_ctx != NULL && event->event_id == HTTP_EVENT_ON_HEADER &&
        event->header_key != NULL && event->header_value != NULL &&
        strcasecmp(event->header_key, "Date") == 0) {
        strlcpy(http_ctx->date, event->header_value, sizeof(http_ctx->date));
    }
    return ESP_OK;
}

static esp_err_t system_time_sync_from_http_date(void)
{
    system_time_http_ctx_t http_ctx = {0};
    esp_http_client_config_t config = {
        .url = TIME_SYNC_HTTP_DATE_URL,
        .method = HTTP_METHOD_HEAD,
        .event_handler = system_time_http_event_handler,
        .user_data = &http_ctx,
        .timeout_ms = TIME_SYNC_HTTP_TIMEOUT_MS,
        .buffer_size = 512,
        .buffer_size_tx = 256,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Connection", "close");
    esp_err_t ret = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (ret != ESP_OK) {
        return ret;
    }
    if (status < 200 || status >= 400 || http_ctx.date[0] == '\0') {
        return ESP_ERR_INVALID_RESPONSE;
    }

    time_t unix_time = 0;
    ret = system_time_parse_http_date(http_ctx.date, &unix_time);
    if (ret != ESP_OK) {
        return ret;
    }

    struct timeval value = {
        .tv_sec = unix_time,
        .tv_usec = 0,
    };
    if (settimeofday(&value, NULL) != 0) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "system time synchronized from HTTP Date: unix=%lld", (long long)unix_time);
    return ESP_OK;
}

bool system_time_has_valid_time(void)
{
    time_t now = 0;

    time(&now);
    return now >= (time_t)TIME_SYNC_MIN_VALID_UNIX_TIME;
}

void system_time_set_sync_cb(system_time_sync_cb_t cb, void *ctx)
{
    taskENTER_CRITICAL(&s_time_sync_lock);
    s_time_sync_cb = cb;
    s_time_sync_cb_ctx = ctx;
    taskEXIT_CRITICAL(&s_time_sync_lock);
}

esp_err_t system_time_request_sync(bool force_sync)
{
    BaseType_t task_ok = pdFALSE;

    if (!force_sync && system_time_has_valid_time()) {
        return ESP_OK;
    }

    taskENTER_CRITICAL(&s_time_sync_lock);
    if (force_sync) {
        s_time_sync_force_requested = true;
    }
    if (s_time_sync_running) {
        taskEXIT_CRITICAL(&s_time_sync_lock);
        return ESP_OK;
    }
    s_time_sync_running = true;
    taskEXIT_CRITICAL(&s_time_sync_lock);

    task_ok = xTaskCreateWithCaps(system_time_sync_task,
                                  "system_time_sync",
                                  TIME_SYNC_TASK_STACK_SIZE,
                                  NULL,
                                  TIME_SYNC_TASK_PRIORITY,
                                  NULL,
                                  APP_TASK_STACK_CAPS_BACKGROUND);
    if (task_ok != pdPASS) {
        taskENTER_CRITICAL(&s_time_sync_lock);
        s_time_sync_running = false;
        taskEXIT_CRITICAL(&s_time_sync_lock);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "system time sync scheduled%s", force_sync ? " (forced)" : "");
    return ESP_OK;
}

esp_err_t system_time_once(bool force_sync)
{
    esp_err_t ret = ESP_OK;
    time_t now = 0;
    esp_sntp_config_t sntp_config =
        ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(TIME_SYNC_SNTP_SERVER_COUNT,
                                               ESP_SNTP_SERVER_LIST(TIME_SYNC_SNTP_SERVER_0,
                                                                    TIME_SYNC_SNTP_SERVER_1,
                                                                    TIME_SYNC_SNTP_SERVER_2,
                                                                    TIME_SYNC_SNTP_SERVER_3));

    if (!force_sync && system_time_has_valid_time()) {
        return ESP_OK;
    }

    if (force_sync) {
        ESP_LOGI(TAG, "system time sync forced: primary=%s", TIME_SYNC_SNTP_SERVER_0);
    } else {
        ESP_LOGI(TAG, "system time invalid, syncing SNTP: primary=%s", TIME_SYNC_SNTP_SERVER_0);
    }

    esp_netif_sntp_deinit();
    ret = esp_netif_sntp_init(&sntp_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SNTP init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    for (uint32_t retry = 1; retry <= TIME_SYNC_RETRY_COUNT; ++retry) {
        ESP_LOGD(TAG,
                 "waiting for system time... (%lu/%lu)",
                 (unsigned long)retry,
                 (unsigned long)TIME_SYNC_RETRY_COUNT);

        ret = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(TIME_SYNC_TIMEOUT_MS));
        if (ret != ESP_ERR_TIMEOUT) {
            break;
        }
    }

    time(&now);
    esp_netif_sntp_deinit();

    if (ret == ESP_OK && system_time_has_valid_time()) {
        ESP_LOGI(TAG, "system time synchronized: unix=%lld", (long long)now);
        return ESP_OK;
    }

    ESP_LOGW(TAG,
             "SNTP unavailable: %s (%d), unix=%lld; trying HTTP Date",
             esp_err_to_name(ret),
             ret,
             (long long)now);
    esp_err_t http_ret = system_time_sync_from_http_date();
    if (http_ret == ESP_OK && system_time_has_valid_time()) {
        return ESP_OK;
    }

    ESP_LOGE(TAG,
             "system time sync failed: sntp=%s http=%s unix=%lld",
             esp_err_to_name(ret),
             esp_err_to_name(http_ret),
             (long long)now);
    return http_ret != ESP_OK ? http_ret : (ret == ESP_OK ? ESP_FAIL : ret);
}

static void system_time_notify_sync_done(esp_err_t result)
{
    system_time_sync_cb_t cb = NULL;
    void *cb_ctx = NULL;
    bool valid = system_time_has_valid_time();

    taskENTER_CRITICAL(&s_time_sync_lock);
    cb = s_time_sync_cb;
    cb_ctx = s_time_sync_cb_ctx;
    taskEXIT_CRITICAL(&s_time_sync_lock);

    if (cb != NULL) {
        cb(result, valid, cb_ctx);
    }
}

static void system_time_sync_task(void *ctx)
{
    (void)ctx;

    while (true) {
        bool force_sync = false;

        taskENTER_CRITICAL(&s_time_sync_lock);
        force_sync = s_time_sync_force_requested;
        s_time_sync_force_requested = false;
        taskEXIT_CRITICAL(&s_time_sync_lock);

        esp_err_t ret = system_time_once(force_sync);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "background system time sync failed: %s", esp_err_to_name(ret));
        }
        system_time_notify_sync_done(ret);

        taskENTER_CRITICAL(&s_time_sync_lock);
        if (ret == ESP_OK && !s_time_sync_force_requested) {
            s_time_sync_running = false;
            taskEXIT_CRITICAL(&s_time_sync_lock);
            break;
        }
        taskEXIT_CRITICAL(&s_time_sync_lock);

        if (ret != ESP_OK) {
            ESP_LOGI(TAG, "system time retry scheduled in %ums", (unsigned)TIME_SYNC_RETRY_DELAY_MS);
            vTaskDelay(pdMS_TO_TICKS(TIME_SYNC_RETRY_DELAY_MS));
        }
    }

    vTaskDeleteWithCaps(NULL);
}
