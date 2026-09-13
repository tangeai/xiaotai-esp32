#include "network.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "ping/ping_sock.h"
#include "wifi.h"

static const char *TAG = "network";

#define NETWORK_MONITOR_INTERVAL_MS 250U
#define NETWORK_MONITOR_STOP_WAIT_MS 300U
#define NETWORK_MONITOR_TASK_STACK_SIZE (5 * 1024)
#define NETWORK_QUALITY_PROBE_COUNT 20U
#define NETWORK_QUALITY_PROBE_INTERVAL_MS 200U
#define NETWORK_QUALITY_PROBE_TIMEOUT_MS 500U
#define NETWORK_QUALITY_PROBE_DATA_SIZE 32U

static network_state_t s_network_state;
static network_ping_status_t s_ping_status;
static network_state_cb_t s_state_cb;
static void *s_state_cb_ctx;
static TaskHandle_t s_monitor_task;
static esp_ping_handle_t s_ping_handle;
static bool s_ping_cancel_requested;
static bool s_monitor_stop_requested;
static uint64_t s_ping_time_sum_ms;
static uint32_t s_ping_success_count;
static uint32_t s_ping_previous_time_ms;
static uint64_t s_ping_jitter_sum_ms;
static uint32_t s_ping_jitter_sample_count;
static portMUX_TYPE s_network_lock = portMUX_INITIALIZER_UNLOCKED;

static uint32_t network_ping_loss_percent(uint32_t transmitted, uint32_t received)
{
    if (transmitted == 0U) {
        return 0U;
    }

    const uint32_t lost = transmitted > received ? transmitted - received : 0U;
    return (lost * 100U + transmitted / 2U) / transmitted;
}

static void network_ping_set_summary_locked(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(s_ping_status.summary, sizeof(s_ping_status.summary), fmt, args);
    va_end(args);
}

static bool network_resolve_ping_target(const char *target_host, ip_addr_t *target_addr)
{
    bool resolved = false;
    memset(target_addr, 0, sizeof(*target_addr));

#ifdef CONFIG_LWIP_IPV4
    struct in_addr sock_addr4;
    if (inet_pton(AF_INET, target_host, &sock_addr4) == 1) {
        inet_addr_to_ip4addr(ip_2_ip4(target_addr), &sock_addr4);
        return true;
    }
#endif

#ifdef CONFIG_LWIP_IPV6
    struct sockaddr_in6 sock_addr6;
    if (inet_pton(AF_INET6, target_host, &sock_addr6.sin6_addr) == 1) {
        return ipaddr_aton(target_host, target_addr) != 0;
    }
#endif

    struct addrinfo hint = {0};
    struct addrinfo *res = NULL;
    if (getaddrinfo(target_host, NULL, &hint, &res) != 0 || res == NULL) {
        if (res != NULL) {
            freeaddrinfo(res);
        }
        return false;
    }

#ifdef CONFIG_LWIP_IPV4
    if (res->ai_family == AF_INET) {
        struct in_addr addr4 = ((struct sockaddr_in *)(res->ai_addr))->sin_addr;
        inet_addr_to_ip4addr(ip_2_ip4(target_addr), &addr4);
        resolved = true;
    }
#endif
#ifdef CONFIG_LWIP_IPV6
    if (res->ai_family == AF_INET6) {
        struct in6_addr addr6 = ((struct sockaddr_in6 *)(res->ai_addr))->sin6_addr;
        inet6_addr_to_ip6addr(ip_2_ip6(target_addr), &addr6);
        resolved = true;
    }
#endif
    freeaddrinfo(res);
    return resolved;
}

static void network_ping_on_success(esp_ping_handle_t hdl, void *args)
{
    (void)args;
    uint32_t transmitted = 0;
    uint32_t received = 0;
    uint32_t time_gap_ms = 0;
    uint32_t loss_percent = 0;

    esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST, &transmitted, sizeof(transmitted));
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &received, sizeof(received));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &time_gap_ms, sizeof(time_gap_ms));

    taskENTER_CRITICAL(&s_network_lock);
    if (s_ping_handle != hdl || s_ping_cancel_requested) {
        taskEXIT_CRITICAL(&s_network_lock);
        return;
    }
    s_ping_status.transmitted = transmitted;
    s_ping_status.received = received;
    s_ping_status.last_time_ms = time_gap_ms;
    s_ping_time_sum_ms += time_gap_ms;
    /* Jitter is the mean absolute RTT change between consecutive replies. */
    if (s_ping_success_count > 0U) {
        const uint32_t jitter_sample = time_gap_ms >= s_ping_previous_time_ms
                                           ? time_gap_ms - s_ping_previous_time_ms
                                           : s_ping_previous_time_ms - time_gap_ms;
        s_ping_jitter_sum_ms += jitter_sample;
        s_ping_jitter_sample_count++;
        s_ping_status.jitter_ms =
            (uint32_t)(s_ping_jitter_sum_ms / s_ping_jitter_sample_count);
    }
    s_ping_previous_time_ms = time_gap_ms;
    if (s_ping_success_count == 0U || time_gap_ms < s_ping_status.min_time_ms) {
        s_ping_status.min_time_ms = time_gap_ms;
    }
    s_ping_success_count++;
    if (time_gap_ms > s_ping_status.max_time_ms) {
        s_ping_status.max_time_ms = time_gap_ms;
    }
    s_ping_status.avg_time_ms = (uint32_t)(s_ping_time_sum_ms / s_ping_success_count);
    loss_percent = network_ping_loss_percent(transmitted, received);
    s_ping_status.loss_percent = loss_percent;
    network_ping_set_summary_locked("RTT %lu ms Jitter %lu ms Loss %lu%%",
                                    (unsigned long)s_ping_status.avg_time_ms,
                                    (unsigned long)s_ping_status.jitter_ms,
                                    (unsigned long)loss_percent);
    taskEXIT_CRITICAL(&s_network_lock);
}

static void network_ping_on_timeout(esp_ping_handle_t hdl, void *args)
{
    (void)args;
    uint32_t transmitted = 0;
    uint32_t received = 0;
    uint32_t loss_percent = 0;

    esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST, &transmitted, sizeof(transmitted));
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &received, sizeof(received));

    taskENTER_CRITICAL(&s_network_lock);
    if (s_ping_handle != hdl || s_ping_cancel_requested) {
        taskEXIT_CRITICAL(&s_network_lock);
        return;
    }
    s_ping_status.transmitted = transmitted;
    s_ping_status.received = received;
    loss_percent = transmitted > 0U ? network_ping_loss_percent(transmitted, received) : 100U;
    s_ping_status.loss_percent = loss_percent;
    network_ping_set_summary_locked("RTT %lu ms Jitter %lu ms Loss %lu%%",
                                    (unsigned long)s_ping_status.avg_time_ms,
                                    (unsigned long)s_ping_status.jitter_ms,
                                    (unsigned long)loss_percent);
    taskEXIT_CRITICAL(&s_network_lock);
}

static void network_ping_on_end(esp_ping_handle_t hdl, void *args)
{
    (void)args;
    uint32_t transmitted = 0;
    uint32_t received = 0;

    esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST, &transmitted, sizeof(transmitted));
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &received, sizeof(received));

    taskENTER_CRITICAL(&s_network_lock);
    if (s_ping_handle != hdl) {
        taskEXIT_CRITICAL(&s_network_lock);
        esp_ping_delete_session(hdl);
        return;
    }
    s_ping_status.running = false;
    s_ping_status.valid = !s_ping_cancel_requested && transmitted > 0U;
    s_ping_status.transmitted = transmitted;
    s_ping_status.received = received;
    s_ping_status.loss_percent = network_ping_loss_percent(transmitted, received);
    if (s_ping_cancel_requested) {
        s_ping_status.last_error = ESP_ERR_INVALID_STATE;
        network_ping_set_summary_locked("Ping canceled");
    } else if (received > 0) {
        network_ping_set_summary_locked("RTT %lu ms Jitter %lu ms Loss %lu%%",
                                        (unsigned long)s_ping_status.avg_time_ms,
                                        (unsigned long)s_ping_status.jitter_ms,
                                        (unsigned long)s_ping_status.loss_percent);
    } else {
        s_ping_status.last_error = ESP_ERR_TIMEOUT;
        network_ping_set_summary_locked("RTT -- Jitter -- Loss %lu%%",
                                        (unsigned long)s_ping_status.loss_percent);
    }
    s_ping_handle = NULL;
    s_ping_cancel_requested = false;
    taskEXIT_CRITICAL(&s_network_lock);

    esp_ping_delete_session(hdl);
}

static void network_convert_from_wifi(network_state_t *dst)
{
    wifi_status_t wifi_status = {0};
    wifi_get_status(&wifi_status);

    dst->configured = wifi_status.configured;
    dst->started = wifi_status.started;
    dst->connected = wifi_status.connected;
    dst->retry_count = wifi_status.retry_count;
    dst->disconnect_reason = wifi_status.disconnect_reason;
    dst->rssi = wifi_status.rssi;
    strlcpy(dst->ssid, wifi_status.ssid, sizeof(dst->ssid));
    strlcpy(dst->ip_addr, wifi_status.ip_addr, sizeof(dst->ip_addr));
}

static bool network_state_equals(const network_state_t *lhs,
                                          const network_state_t *rhs)
{
    return lhs->configured == rhs->configured &&
           lhs->started == rhs->started &&
           lhs->connected == rhs->connected &&
           lhs->rssi == rhs->rssi &&
           lhs->retry_count == rhs->retry_count &&
           lhs->disconnect_reason == rhs->disconnect_reason &&
           strcmp(lhs->ssid, rhs->ssid) == 0 &&
           strcmp(lhs->ip_addr, rhs->ip_addr) == 0;
}

static bool network_should_log_state_change(const network_state_t *lhs,
                                                     const network_state_t *rhs)
{
    return lhs->connected != rhs->connected ||
           lhs->configured != rhs->configured ||
           lhs->started != rhs->started ||
           lhs->retry_count != rhs->retry_count ||
           lhs->disconnect_reason != rhs->disconnect_reason ||
           strcmp(lhs->ip_addr, rhs->ip_addr) != 0 ||
           strcmp(lhs->ssid, rhs->ssid) != 0;
}

static void network_publish_state(const network_state_t *state)
{
    network_state_cb_t cb = NULL;
    void *cb_ctx = NULL;

    taskENTER_CRITICAL(&s_network_lock);
    s_network_state = *state;
    cb = s_state_cb;
    cb_ctx = s_state_cb_ctx;
    taskEXIT_CRITICAL(&s_network_lock);

    if (cb != NULL) {
        cb(state, cb_ctx);
    }
}

static void network_monitor_task(void *ctx)
{
    (void)ctx;

    while (true) {
        network_state_t current = {0};
        network_state_t previous = {0};
        bool stop_requested = false;

        taskENTER_CRITICAL(&s_network_lock);
        stop_requested = s_monitor_stop_requested;
        if (stop_requested) {
            s_monitor_stop_requested = false;
            s_monitor_task = NULL;
            taskEXIT_CRITICAL(&s_network_lock);
            vTaskDeleteWithCaps(NULL);
            return;
        }
        taskEXIT_CRITICAL(&s_network_lock);

        network_convert_from_wifi(&current);

        taskENTER_CRITICAL(&s_network_lock);
        previous = s_network_state;
        taskEXIT_CRITICAL(&s_network_lock);

        if (!network_state_equals(&current, &previous)) {
            bool log_change = network_should_log_state_change(&previous, &current);
            if (log_change) {
                ESP_LOGI(TAG,
                         "network changed: connected=%d ip=%s ssid=%s",
                         current.connected,
                         current.ip_addr,
                         current.ssid);
                network_publish_state(&current);
            } else {
                taskENTER_CRITICAL(&s_network_lock);
                s_network_state = current;
                taskEXIT_CRITICAL(&s_network_lock);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(NETWORK_MONITOR_INTERVAL_MS));
    }
}

esp_err_t network_prepare(const network_config_t *config)
{
    static const network_config_t default_config = {
        .enabled = true,
    };
    const network_config_t *effective_config = config != NULL ? config : &default_config;
    const wifi_driver_config_t wifi_config = {
        .enabled = effective_config->enabled,
        .auto_connect = effective_config->auto_connect,
        .default_ssid = effective_config->default_ssid,
        .default_password = effective_config->default_password,
        .fallback_dns_ipv4 = effective_config->fallback_dns_ipv4,
    };
    esp_err_t ret = wifi_prepare(&wifi_config);

    network_state_t current = {0};
    network_convert_from_wifi(&current);
    network_publish_state(&current);

    if (s_monitor_task == NULL) {
        taskENTER_CRITICAL(&s_network_lock);
        s_monitor_stop_requested = false;
        taskEXIT_CRITICAL(&s_network_lock);

        BaseType_t task_ok = xTaskCreatePinnedToCoreWithCaps(network_monitor_task,
                                                             "network_monitor",
                                                             NETWORK_MONITOR_TASK_STACK_SIZE,
                                                             NULL,
                                                             3,
                                                             &s_monitor_task,
                                                             0,
                                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (task_ok != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }

    return ret;
}

void network_release(void)
{
    bool monitor_task_active = false;
    uint32_t waited_ms = 0;

    network_cancel_ping();

    taskENTER_CRITICAL(&s_network_lock);
    if (s_monitor_task != NULL) {
        s_monitor_stop_requested = true;
    }
    taskEXIT_CRITICAL(&s_network_lock);

    do {
        taskENTER_CRITICAL(&s_network_lock);
        monitor_task_active = s_monitor_task != NULL;
        taskEXIT_CRITICAL(&s_network_lock);
        if (!monitor_task_active) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        waited_ms += 10;
    } while (waited_ms < NETWORK_MONITOR_STOP_WAIT_MS);

    if (monitor_task_active) {
        ESP_LOGW(TAG, "network monitor task did not stop in %lu ms",
                 (unsigned long)NETWORK_MONITOR_STOP_WAIT_MS);
    }

    wifi_release();

    taskENTER_CRITICAL(&s_network_lock);
    memset(&s_ping_status, 0, sizeof(s_ping_status));
    s_ping_time_sum_ms = 0;
    s_ping_success_count = 0;
    s_ping_previous_time_ms = 0;
    s_ping_jitter_sum_ms = 0;
    s_ping_jitter_sample_count = 0;
    taskEXIT_CRITICAL(&s_network_lock);

    network_state_t current = {0};
    network_convert_from_wifi(&current);
    network_publish_state(&current);
}

esp_err_t network_connect(const char *ssid, const char *password)
{
    esp_err_t ret = app_wifi_driver_connect(ssid, password);

    network_state_t current = {0};
    network_convert_from_wifi(&current);
    network_publish_state(&current);
    return ret;
}

esp_err_t network_request_scan(void)
{
    return wifi_request_scan();
}

void network_get_scan_results(network_scan_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    wifi_scan_snapshot_t wifi_snapshot = {0};
    wifi_get_scan_snapshot(&wifi_snapshot);

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->in_progress = wifi_snapshot.in_progress;
    snapshot->count = wifi_snapshot.count;
    snapshot->last_scan_ms = wifi_snapshot.last_scan_ms;
    for (uint16_t index = 0; index < wifi_snapshot.count && index < NETWORK_SCAN_RESULT_MAX; ++index) {
        strlcpy(snapshot->results[index].ssid, wifi_snapshot.results[index].ssid, sizeof(snapshot->results[index].ssid));
        snapshot->results[index].rssi = wifi_snapshot.results[index].rssi;
        snapshot->results[index].authmode = wifi_snapshot.results[index].authmode;
        snapshot->results[index].channel = wifi_snapshot.results[index].channel;
        snapshot->results[index].secure = wifi_snapshot.results[index].secure;
    }
}

esp_err_t network_start_ping(const char *target_host)
{
    ESP_RETURN_ON_FALSE(target_host != NULL && target_host[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "ping target is empty");
    ESP_RETURN_ON_FALSE(network_is_connected(), ESP_ERR_INVALID_STATE, TAG, "ping requires connected network");

    taskENTER_CRITICAL(&s_network_lock);
    if (s_ping_handle != NULL || s_ping_status.running) {
        network_ping_set_summary_locked("Ping already running");
        taskEXIT_CRITICAL(&s_network_lock);
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_ping_status, 0, sizeof(s_ping_status));
    s_ping_status.running = true;
    s_ping_status.last_error = ESP_OK;
    strlcpy(s_ping_status.target, target_host, sizeof(s_ping_status.target));
    network_ping_set_summary_locked("Ping resolving...");
    s_ping_cancel_requested = false;
    s_ping_time_sum_ms = 0;
    s_ping_success_count = 0;
    s_ping_previous_time_ms = 0;
    s_ping_jitter_sum_ms = 0;
    s_ping_jitter_sample_count = 0;
    taskEXIT_CRITICAL(&s_network_lock);

    ip_addr_t target_addr = {0};
    if (!network_resolve_ping_target(target_host, &target_addr)) {
        taskENTER_CRITICAL(&s_network_lock);
        s_ping_status.running = false;
        s_ping_status.valid = false;
        s_ping_status.last_error = ESP_FAIL;
        network_ping_set_summary_locked("Ping unavailable");
        taskEXIT_CRITICAL(&s_network_lock);
        ESP_LOGW(TAG, "ping start failed: target host is unreachable");
        return ESP_FAIL;
    }

    taskENTER_CRITICAL(&s_network_lock);
    network_ping_set_summary_locked("Ping running...");
    taskEXIT_CRITICAL(&s_network_lock);

    esp_ping_config_t ping_cfg = ESP_PING_DEFAULT_CONFIG();
    ping_cfg.count = NETWORK_QUALITY_PROBE_COUNT;
    ping_cfg.interval_ms = NETWORK_QUALITY_PROBE_INTERVAL_MS;
    ping_cfg.timeout_ms = NETWORK_QUALITY_PROBE_TIMEOUT_MS;
    ping_cfg.data_size = NETWORK_QUALITY_PROBE_DATA_SIZE;
    ping_cfg.target_addr = target_addr;

    esp_ping_callbacks_t ping_cbs = {
        .cb_args = NULL,
        .on_ping_success = network_ping_on_success,
        .on_ping_timeout = network_ping_on_timeout,
        .on_ping_end = network_ping_on_end,
    };

    esp_ping_handle_t ping_handle = NULL;
    esp_err_t ret = esp_ping_new_session(&ping_cfg, &ping_cbs, &ping_handle);
    if (ret != ESP_OK) {
        taskENTER_CRITICAL(&s_network_lock);
        s_ping_status.running = false;
        s_ping_status.last_error = ret;
        network_ping_set_summary_locked("Ping start failed");
        taskEXIT_CRITICAL(&s_network_lock);
        ESP_LOGW(TAG,
                 "ping session create failed: ret=%s internal_free=%u internal_largest=%u",
                 esp_err_to_name(ret),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        return ret;
    }

    taskENTER_CRITICAL(&s_network_lock);
    s_ping_handle = ping_handle;
    taskEXIT_CRITICAL(&s_network_lock);

    ret = esp_ping_start(ping_handle);
    if (ret != ESP_OK) {
        taskENTER_CRITICAL(&s_network_lock);
        s_ping_status.running = false;
        s_ping_status.last_error = ret;
        s_ping_handle = NULL;
        network_ping_set_summary_locked("Ping start failed");
        taskEXIT_CRITICAL(&s_network_lock);
        esp_ping_delete_session(ping_handle);
        return ret;
    }

    return ESP_OK;
}

void network_cancel_ping(void)
{
    esp_ping_handle_t ping_handle = NULL;

    taskENTER_CRITICAL(&s_network_lock);
    ping_handle = s_ping_handle;
    if (ping_handle == NULL && !s_ping_status.running) {
        taskEXIT_CRITICAL(&s_network_lock);
        return;
    }

    s_ping_status.running = false;
    s_ping_status.valid = false;
    s_ping_status.last_error = ESP_ERR_INVALID_STATE;
    network_ping_set_summary_locked("Ping canceled");
    if (ping_handle != NULL) {
        s_ping_cancel_requested = true;
    }
    taskEXIT_CRITICAL(&s_network_lock);

    if (ping_handle != NULL) {
        esp_err_t stop_ret = esp_ping_stop(ping_handle);
        if (stop_ret != ESP_OK) {
            ESP_LOGW(TAG, "ping cancel failed: %s", esp_err_to_name(stop_ret));
        }
    }
}

void network_get_ping_status(network_ping_status_t *status)
{
    if (status == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_network_lock);
    *status = s_ping_status;
    taskEXIT_CRITICAL(&s_network_lock);
}

void network_get_saved_config(char *ssid, size_t ssid_size, char *password, size_t password_size)
{
    wifi_get_saved_config(ssid, ssid_size, password, password_size);
}

void network_set_state_cb(network_state_cb_t cb, void *ctx)
{
    network_state_t snapshot = {0};

    taskENTER_CRITICAL(&s_network_lock);
    s_state_cb = cb;
    s_state_cb_ctx = ctx;
    snapshot = s_network_state;
    taskEXIT_CRITICAL(&s_network_lock);

    if (cb != NULL) {
        cb(&snapshot, ctx);
    }
}

void network_get_state(network_state_t *state)
{
    if (state == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_network_lock);
    *state = s_network_state;
    taskEXIT_CRITICAL(&s_network_lock);
}

bool network_is_connected(void)
{
    bool connected = false;

    taskENTER_CRITICAL(&s_network_lock);
    connected = s_network_state.connected;
    taskEXIT_CRITICAL(&s_network_lock);
    return connected;
}
