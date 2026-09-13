#include "wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "nvs.h"

#include "platform_storage.h"

#if __has_include("esp_hosted_event.h")
#include "esp_hosted_event.h"
#define WIFI_HOSTED_EVENT_RECOVERY_SUPPORTED 1
#else
#define WIFI_HOSTED_EVENT_RECOVERY_SUPPORTED 0
#endif

static const char *TAG = "wifi_mgr";

#define WIFI_CONNECTED_BIT      BIT0
#define WIFI_FAIL_BIT           BIT1
#define WIFI_SCAN_DONE_BIT      BIT2
#define WIFI_HOSTED_UP_BIT      BIT3
#define WIFI_MAX_RETRIES        8
#define WIFI_WAIT_MS            22000
#define WIFI_BACKGROUND_RECOVERY_INTERVAL_MS 30000U
#define WIFI_HOSTED_HEARTBEAT_INTERVAL_SEC 5
#define WIFI_HOSTED_HEARTBEAT_TIMEOUT_MS 20000U
#define WIFI_HOSTED_RECOVERY_RETRY_MS 5000U
#define WIFI_HOSTED_RECOVERY_TASK_STACK_SIZE (6 * 1024)
#define WIFI_INVALID_RSSI       (-127)
#define WIFI_NVS_NAMESPACE      "wifi"
#define WIFI_NVS_KEY_SSID       "ssid"
#define WIFI_NVS_KEY_PASSWORD   "password"
#define WIFI_CHANNEL_HINT_MAX_AGE_MS 30000U
#define WIFI_STA_PROTOCOLS_HE20 ((uint8_t)(WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX))
#define WIFI_STA_PROTOCOLS_HT40 ((uint8_t)(WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N))
#define WIFI_STA_PREFER_11AX_HE20 1
#define WIFI_SCAN_WAIT_MS 8000U
#define WIFI_SCAN_START_SETTLE_MS 600U
#define WIFI_SCAN_ACTIVE_MIN_MS 80U
#define WIFI_SCAN_ACTIVE_MAX_MS 180U
#define WIFI_SCAN_PASSIVE_MS 360U
#define WIFI_MAX_TX_POWER_QDBM 84
#define WIFI_SCAN_TASK_PRIORITY 5
#define WIFI_SCAN_TASK_CORE 1

typedef enum {
    WIFI_HOSTED_RECOVERY_NONE = 0,
    WIFI_HOSTED_RECOVERY_TRANSPORT_FAILURE,
    WIFI_HOSTED_RECOVERY_TRANSPORT_DOWN,
    WIFI_HOSTED_RECOVERY_UNEXPECTED_CP_INIT,
    WIFI_HOSTED_RECOVERY_HEARTBEAT_TIMEOUT,
    WIFI_HOSTED_RECOVERY_DISCONNECT_RPC,
    WIFI_HOSTED_RECOVERY_CONNECT_RPC,
    WIFI_HOSTED_RECOVERY_CONFIG_RPC,
} wifi_hosted_recovery_reason_t;

static EventGroupHandle_t s_wifi_event_group;
static esp_netif_t *s_wifi_sta_netif;
static wifi_status_t s_wifi_status;
static wifi_scan_snapshot_t s_wifi_scan_snapshot;
static bool s_wifi_initialized;
static bool s_wifi_event_loop_ready;
static bool s_wifi_scan_in_progress;
static bool s_wifi_manual_scan_active;
static bool s_wifi_resume_connect_after_scan;
static bool s_wifi_pending_explicit;
static bool s_wifi_release_requested;
static bool s_wifi_reconfig_in_progress;
static bool s_wifi_hosted_runtime_initialized;
static bool s_wifi_hosted_recovery_requested;
static bool s_wifi_hosted_recovery_in_progress;
static bool s_wifi_hosted_heartbeat_configured;
static bool s_wifi_hosted_seen_cp_init;
static uint32_t s_wifi_hosted_last_heartbeat_ms;
static uint32_t s_wifi_hosted_recovery_due_ms;
static uint32_t s_wifi_hosted_recovery_attempts;
static esp_err_t s_wifi_hosted_recovery_error;
static wifi_hosted_recovery_reason_t s_wifi_hosted_recovery_reason;
static uint32_t s_wifi_connect_started_ms;
static TickType_t s_wifi_connect_started_tick;
static uint32_t s_wifi_background_recovery_due_ms;
static esp_timer_handle_t s_wifi_connect_timer;
static char s_wifi_saved_ssid[33];
static char s_wifi_saved_password[WIFI_PASSWORD_MAX_LEN + 1];
static char s_wifi_pending_ssid[33];
static char s_wifi_pending_password[WIFI_PASSWORD_MAX_LEN + 1];
static wifi_driver_config_t s_wifi_config = {
    .enabled = true,
};
static TaskHandle_t s_wifi_scan_task;
static TaskHandle_t s_wifi_connect_watchdog_task;
static portMUX_TYPE s_wifi_lock = portMUX_INITIALIZER_UNLOCKED;

#if WIFI_HOSTED_EVENT_RECOVERY_SUPPORTED
static esp_event_handler_instance_t s_wifi_hosted_event_instance;
#endif

typedef enum {
    WIFI_SCAN_RESUME_NONE = 0,
    WIFI_SCAN_RESUME_CONNECT,
    WIFI_SCAN_RESUME_TARGET_MISSING,
} wifi_scan_resume_action_t;

static const char *wifi_disconnect_reason_name(uint8_t reason);
static void wifi_connect_timeout_cb(void *ctx);
static void wifi_connect_watchdog_task(void *ctx);
static void wifi_scan_task(void *ctx);
static wifi_scan_resume_action_t wifi_finish_scan_state_locked(void);
static void wifi_resume_connect_if_needed(wifi_scan_resume_action_t action);
static void wifi_request_hosted_recovery(wifi_hosted_recovery_reason_t reason, esp_err_t error);
static esp_err_t wifi_recover_hosted_transport(wifi_hosted_recovery_reason_t reason,
                                               esp_err_t trigger_error,
                                               uint32_t attempt);

static uint32_t wifi_uptime_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static bool wifi_deadline_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return deadline_ms != 0U && (int32_t)(now_ms - deadline_ms) >= 0;
}

static const char *wifi_hosted_recovery_reason_name(wifi_hosted_recovery_reason_t reason)
{
    switch (reason) {
    case WIFI_HOSTED_RECOVERY_TRANSPORT_FAILURE:
        return "transport-failure";
    case WIFI_HOSTED_RECOVERY_TRANSPORT_DOWN:
        return "transport-down";
    case WIFI_HOSTED_RECOVERY_UNEXPECTED_CP_INIT:
        return "unexpected-cp-init";
    case WIFI_HOSTED_RECOVERY_HEARTBEAT_TIMEOUT:
        return "heartbeat-timeout";
    case WIFI_HOSTED_RECOVERY_DISCONNECT_RPC:
        return "disconnect-rpc";
    case WIFI_HOSTED_RECOVERY_CONNECT_RPC:
        return "connect-rpc";
    case WIFI_HOSTED_RECOVERY_CONFIG_RPC:
        return "config-rpc";
    case WIFI_HOSTED_RECOVERY_NONE:
    default:
        return "none";
    }
}

static void wifi_request_hosted_recovery(wifi_hosted_recovery_reason_t reason, esp_err_t error)
{
    bool scheduled = false;
    const uint32_t now_ms = wifi_uptime_ms();

    taskENTER_CRITICAL(&s_wifi_lock);
    if (s_wifi_initialized && s_wifi_config.enabled && !s_wifi_release_requested &&
        !s_wifi_hosted_recovery_in_progress) {
        if (!s_wifi_hosted_recovery_requested) {
            s_wifi_hosted_recovery_requested = true;
            s_wifi_hosted_recovery_reason = reason;
            s_wifi_hosted_recovery_error = error;
            s_wifi_hosted_recovery_due_ms = now_ms;
            scheduled = true;
        }
    }
    taskEXIT_CRITICAL(&s_wifi_lock);

    if (!scheduled) {
        return;
    }

    ESP_LOGW(TAG,
             "ESP-Hosted recovery scheduled: reason=%s error=%s",
             wifi_hosted_recovery_reason_name(reason),
             esp_err_to_name(error));
    if (s_wifi_connect_watchdog_task != NULL) {
        xTaskNotifyGive(s_wifi_connect_watchdog_task);
    }
}

#if WIFI_HOSTED_EVENT_RECOVERY_SUPPORTED
static void wifi_hosted_event_handler(void *arg,
                                      esp_event_base_t event_base,
                                      int32_t event_id,
                                      void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base != ESP_HOSTED_EVENT) {
        return;
    }

    if (event_id == ESP_HOSTED_EVENT_TRANSPORT_UP) {
        if (s_wifi_event_group != NULL) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_HOSTED_UP_BIT);
        }
        return;
    }

    if (event_id == ESP_HOSTED_EVENT_CP_HEARTBEAT) {
        const uint32_t now_ms = wifi_uptime_ms();
        taskENTER_CRITICAL(&s_wifi_lock);
        s_wifi_hosted_last_heartbeat_ms = now_ms;
        taskEXIT_CRITICAL(&s_wifi_lock);
        return;
    }

    if (event_id == ESP_HOSTED_EVENT_CP_INIT) {
        bool unexpected = false;
        taskENTER_CRITICAL(&s_wifi_lock);
        unexpected = s_wifi_hosted_seen_cp_init && s_wifi_initialized &&
                     !s_wifi_hosted_recovery_in_progress && !s_wifi_release_requested;
        s_wifi_hosted_seen_cp_init = true;
        taskEXIT_CRITICAL(&s_wifi_lock);
        if (unexpected) {
            wifi_request_hosted_recovery(WIFI_HOSTED_RECOVERY_UNEXPECTED_CP_INIT, ESP_FAIL);
        }
        return;
    }

    if (event_id == ESP_HOSTED_EVENT_TRANSPORT_FAILURE) {
        wifi_request_hosted_recovery(WIFI_HOSTED_RECOVERY_TRANSPORT_FAILURE, ESP_FAIL);
        return;
    }

    if (event_id == ESP_HOSTED_EVENT_TRANSPORT_DOWN) {
        bool expected = false;
        taskENTER_CRITICAL(&s_wifi_lock);
        expected = s_wifi_hosted_recovery_in_progress || s_wifi_release_requested;
        taskEXIT_CRITICAL(&s_wifi_lock);
        if (!expected) {
            wifi_request_hosted_recovery(WIFI_HOSTED_RECOVERY_TRANSPORT_DOWN, ESP_FAIL);
        }
    }
}
#endif

static bool wifi_disconnect_is_authentication_failure(uint8_t reason)
{
    return reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_802_1X_AUTH_FAILED;
}

static void wifi_arm_background_recovery_locked(uint32_t now_ms, uint8_t reason)
{
    if (s_wifi_status.started && s_wifi_status.configured &&
        !s_wifi_release_requested && !wifi_disconnect_is_authentication_failure(reason)) {
        s_wifi_background_recovery_due_ms = now_ms + WIFI_BACKGROUND_RECOVERY_INTERVAL_MS;
    } else {
        s_wifi_background_recovery_due_ms = 0U;
    }
}

static void wifi_sync_pending_with_saved(void)
{
    if (s_wifi_pending_explicit) {
        return;
    }

    strlcpy(s_wifi_pending_ssid, s_wifi_saved_ssid, sizeof(s_wifi_pending_ssid));
    strlcpy(s_wifi_pending_password, s_wifi_saved_password, sizeof(s_wifi_pending_password));
}

static esp_err_t wifi_apply_fallback_dns(void)
{
    const char *address = s_wifi_config.fallback_dns_ipv4;
    esp_netif_dns_info_t dns = {0};

    if (address == NULL || address[0] == '\0') {
        return ESP_OK;
    }
    if (s_wifi_sta_netif == NULL || esp_netif_str_to_ip4(address, &dns.ip.u_addr.ip4) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    dns.ip.type = ESP_IPADDR_TYPE_V4;
    return esp_netif_set_dns_info(s_wifi_sta_netif, ESP_NETIF_DNS_FALLBACK, &dns);
}

static void wifi_format_dns_server(esp_netif_dns_type_t type, char *buffer, size_t buffer_size)
{
    esp_netif_dns_info_t dns = {0};

    if (buffer == NULL || buffer_size == 0U) {
        return;
    }
    strlcpy(buffer, "none", buffer_size);
    if (s_wifi_sta_netif == NULL ||
        esp_netif_get_dns_info(s_wifi_sta_netif, type, &dns) != ESP_OK ||
        dns.ip.type != ESP_IPADDR_TYPE_V4 ||
        dns.ip.u_addr.ip4.addr == 0U) {
        return;
    }
    snprintf(buffer, buffer_size, IPSTR, IP2STR(&dns.ip.u_addr.ip4));
}

static void wifi_log_dns_servers(void)
{
    char main_dns[16] = {0};
    char backup_dns[16] = {0};
    char fallback_dns[16] = {0};

    wifi_format_dns_server(ESP_NETIF_DNS_MAIN, main_dns, sizeof(main_dns));
    wifi_format_dns_server(ESP_NETIF_DNS_BACKUP, backup_dns, sizeof(backup_dns));
    wifi_format_dns_server(ESP_NETIF_DNS_FALLBACK, fallback_dns, sizeof(fallback_dns));
    ESP_LOGI(TAG,
             "wifi DNS ready: main=%s backup=%s fallback=%s",
             main_dns,
             backup_dns,
             fallback_dns);
}

static void wifi_load_initial_saved_config(void)
{
    if (s_wifi_saved_ssid[0] != '\0') {
        return;
    }

    if (!s_wifi_config.enabled || s_wifi_config.default_ssid == NULL ||
        s_wifi_config.default_ssid[0] == '\0') {
        wifi_sync_pending_with_saved();
        return;
    }

    strlcpy(s_wifi_saved_ssid, s_wifi_config.default_ssid, sizeof(s_wifi_saved_ssid));
    if (s_wifi_config.default_password != NULL) {
        strlcpy(s_wifi_saved_password,
                s_wifi_config.default_password,
                sizeof(s_wifi_saved_password));
    }
    wifi_sync_pending_with_saved();
}

static void wifi_note_connect_started(void)
{
    taskENTER_CRITICAL(&s_wifi_lock);
    s_wifi_connect_started_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    s_wifi_connect_started_tick = xTaskGetTickCount();
    taskEXIT_CRITICAL(&s_wifi_lock);

    if (s_wifi_connect_timer != NULL) {
        (void)esp_timer_stop(s_wifi_connect_timer);
        esp_err_t ret = esp_timer_start_once(s_wifi_connect_timer, WIFI_WAIT_MS * 1000ULL);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "wifi connect timeout timer start failed: %s", esp_err_to_name(ret));
        }
    }
}

static void wifi_cancel_connect_timeout(void)
{
    taskENTER_CRITICAL(&s_wifi_lock);
    s_wifi_connect_started_ms = 0;
    s_wifi_connect_started_tick = 0;
    taskEXIT_CRITICAL(&s_wifi_lock);

    if (s_wifi_connect_timer != NULL) {
        (void)esp_timer_stop(s_wifi_connect_timer);
    }
}

static void wifi_mark_connect_timeout_if_needed(void)
{
    bool timed_out = false;
    char current_ssid[sizeof(s_wifi_status.ssid)] = {0};
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    TickType_t now_tick = xTaskGetTickCount();

    taskENTER_CRITICAL(&s_wifi_lock);
    if (s_wifi_connect_started_ms != 0 &&
        s_wifi_connect_started_tick != 0 &&
        s_wifi_status.configured &&
        s_wifi_status.started &&
        !s_wifi_status.connected &&
        ((uint32_t)(now_ms - s_wifi_connect_started_ms) >= WIFI_WAIT_MS ||
         (uint32_t)(now_tick - s_wifi_connect_started_tick) >= pdMS_TO_TICKS(WIFI_WAIT_MS))) {
        s_wifi_connect_started_ms = 0;
        s_wifi_connect_started_tick = 0;
        s_wifi_status.retry_count = WIFI_MAX_RETRIES;
        s_wifi_status.disconnect_reason = WIFI_REASON_NO_AP_FOUND;
        s_wifi_status.rssi = WIFI_INVALID_RSSI;
        s_wifi_status.ip_addr[0] = '\0';
        strlcpy(current_ssid, s_wifi_status.ssid, sizeof(current_ssid));
        wifi_arm_background_recovery_locked(now_ms, WIFI_REASON_NO_AP_FOUND);
        timed_out = true;
    }
    taskEXIT_CRITICAL(&s_wifi_lock);

    if (timed_out) {
        ESP_LOGW(TAG,
                 "wifi connect timeout: ssid=%s reason=%u(%s)",
                 current_ssid,
                 (unsigned)WIFI_REASON_NO_AP_FOUND,
                 wifi_disconnect_reason_name(WIFI_REASON_NO_AP_FOUND));
        if (s_wifi_event_group != NULL) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    }
}

static void wifi_connect_timeout_cb(void *ctx)
{
    (void)ctx;
    wifi_mark_connect_timeout_if_needed();
}

static const char *wifi_bandwidth_name(wifi_bandwidth_t bandwidth)
{
    switch (bandwidth) {
    case WIFI_BW_HT20:
        return "HT20";
    case WIFI_BW_HT40:
        return "HT40";
    default:
        return "unknown";
    }
}

static const char *wifi_auth_mode_name(wifi_auth_mode_t authmode)
{
    switch (authmode) {
    case WIFI_AUTH_OPEN:
        return "open";
    case WIFI_AUTH_WEP:
        return "wep";
    case WIFI_AUTH_WPA_PSK:
        return "wpa";
    case WIFI_AUTH_WPA2_PSK:
        return "wpa2";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "wpa/wpa2";
    case WIFI_AUTH_WPA2_ENTERPRISE:
        return "wpa2-enterprise";
    case WIFI_AUTH_WPA3_PSK:
        return "wpa3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "wpa2/wpa3";
    case WIFI_AUTH_WAPI_PSK:
        return "wapi";
    default:
        return "unknown";
    }
}

static void wifi_log_connected_link(void)
{
    wifi_ap_record_t ap_info = {0};
    esp_err_t ret = esp_wifi_sta_get_ap_info(&ap_info);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "wifi link details unavailable: %s", esp_err_to_name(ret));
        return;
    }

    const char *phy = ap_info.phy_11ax ? "11ax" :
                      ap_info.phy_11n ? "11n" :
                      ap_info.phy_11g ? "11g" :
                      ap_info.phy_11b ? "11b" : "legacy";
    ESP_LOGI(TAG,
             "wifi link: channel=%u bandwidth=%s phy=%s auth=%s rssi=%d",
             (unsigned)ap_info.primary,
             wifi_bandwidth_name(ap_info.bandwidth),
             phy,
             wifi_auth_mode_name(ap_info.authmode),
             (int)ap_info.rssi);
}

static void wifi_connect_watchdog_task(void *ctx)
{
    (void)ctx;

    while (true) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        wifi_mark_connect_timeout_if_needed();

        bool recover = false;
        bool heartbeat_timed_out = false;
        bool hosted_recover = false;
        char current_ssid[sizeof(s_wifi_status.ssid)] = {0};
        wifi_hosted_recovery_reason_t hosted_reason = WIFI_HOSTED_RECOVERY_NONE;
        esp_err_t hosted_error = ESP_OK;
        uint32_t hosted_attempt = 0U;
        const uint32_t now_ms = wifi_uptime_ms();

        taskENTER_CRITICAL(&s_wifi_lock);
#if WIFI_HOSTED_EVENT_RECOVERY_SUPPORTED
        heartbeat_timed_out = s_wifi_hosted_heartbeat_configured &&
                              s_wifi_hosted_last_heartbeat_ms != 0U &&
                              !s_wifi_hosted_recovery_in_progress &&
                              !s_wifi_release_requested &&
                              (uint32_t)(now_ms - s_wifi_hosted_last_heartbeat_ms) >=
                                  WIFI_HOSTED_HEARTBEAT_TIMEOUT_MS;
#endif
        taskEXIT_CRITICAL(&s_wifi_lock);

        if (heartbeat_timed_out) {
            wifi_request_hosted_recovery(WIFI_HOSTED_RECOVERY_HEARTBEAT_TIMEOUT,
                                         ESP_ERR_TIMEOUT);
        }

        taskENTER_CRITICAL(&s_wifi_lock);
        if (s_wifi_hosted_recovery_requested && !s_wifi_hosted_recovery_in_progress &&
            wifi_deadline_reached(now_ms, s_wifi_hosted_recovery_due_ms)) {
            hosted_recover = true;
            s_wifi_hosted_recovery_requested = false;
            s_wifi_hosted_recovery_in_progress = true;
            hosted_reason = s_wifi_hosted_recovery_reason;
            hosted_error = s_wifi_hosted_recovery_error;
            hosted_attempt = ++s_wifi_hosted_recovery_attempts;
        }
        taskEXIT_CRITICAL(&s_wifi_lock);

        if (hosted_recover) {
            esp_err_t hosted_ret = wifi_recover_hosted_transport(hosted_reason,
                                                                  hosted_error,
                                                                  hosted_attempt);
            if (hosted_ret != ESP_OK) {
                const uint32_t retry_due_ms = wifi_uptime_ms() + WIFI_HOSTED_RECOVERY_RETRY_MS;
                taskENTER_CRITICAL(&s_wifi_lock);
                s_wifi_hosted_recovery_requested = true;
                s_wifi_hosted_recovery_reason = hosted_reason;
                s_wifi_hosted_recovery_error = hosted_ret;
                s_wifi_hosted_recovery_due_ms = retry_due_ms;
                taskEXIT_CRITICAL(&s_wifi_lock);
                ESP_LOGE(TAG,
                         "ESP-Hosted recovery failed: attempt=%lu ret=%s retry_ms=%u",
                         (unsigned long)hosted_attempt,
                         esp_err_to_name(hosted_ret),
                         (unsigned)WIFI_HOSTED_RECOVERY_RETRY_MS);
            }
            continue;
        }

        taskENTER_CRITICAL(&s_wifi_lock);
        if (s_wifi_status.started && s_wifi_status.configured &&
            !s_wifi_status.connected && !s_wifi_release_requested &&
            !s_wifi_reconfig_in_progress && !s_wifi_scan_in_progress &&
            !s_wifi_manual_scan_active &&
            wifi_deadline_reached(now_ms, s_wifi_background_recovery_due_ms)) {
            s_wifi_background_recovery_due_ms = 0U;
            strlcpy(current_ssid, s_wifi_status.ssid, sizeof(current_ssid));
            recover = true;
        }
        taskEXIT_CRITICAL(&s_wifi_lock);

        if (!recover) {
            continue;
        }

        ESP_LOGI(TAG,
                 "wifi background recovery begins: ssid=%s interval_ms=%u",
                 current_ssid,
                 (unsigned)WIFI_BACKGROUND_RECOVERY_INTERVAL_MS);
        esp_err_t ret = esp_wifi_connect();
        if (ret == ESP_OK) {
            wifi_note_connect_started();
            continue;
        }

        ESP_LOGW(TAG, "wifi background recovery start failed: %s", esp_err_to_name(ret));
        wifi_request_hosted_recovery(WIFI_HOSTED_RECOVERY_CONNECT_RPC, ret);
    }
}

static void wifi_apply_startup_sta_tuning(void)
{
    uint8_t target_protocols = WIFI_STA_PREFER_11AX_HE20 ? WIFI_STA_PROTOCOLS_HE20 : WIFI_STA_PROTOCOLS_HT40;
    wifi_bandwidth_t target_bandwidth = WIFI_STA_PREFER_11AX_HE20 ? WIFI_BW_HT20 : WIFI_BW_HT40;
    esp_err_t ret = esp_wifi_set_protocol(WIFI_IF_STA, target_protocols);
    if (ret != ESP_OK && target_protocols != WIFI_STA_PROTOCOLS_HT40) {
        ESP_LOGW(TAG,
                 "wifi 11ax protocol tuning failed: %s, fallback to 11b/g/n",
                 esp_err_to_name(ret));
        target_protocols = WIFI_STA_PROTOCOLS_HT40;
        target_bandwidth = WIFI_BW_HT40;
        ret = esp_wifi_set_protocol(WIFI_IF_STA, target_protocols);
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "wifi protocol tuning failed: %s", esp_err_to_name(ret));
    }

    ret = esp_wifi_set_bandwidth(WIFI_IF_STA, target_bandwidth);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "wifi bandwidth tuning failed target=%s: %s",
                 wifi_bandwidth_name(target_bandwidth),
                 esp_err_to_name(ret));
    }

    uint8_t protocols = 0;
    wifi_bandwidth_t bandwidth = WIFI_BW_HT20;
    esp_err_t proto_ret = esp_wifi_get_protocol(WIFI_IF_STA, &protocols);
    esp_err_t bw_ret = esp_wifi_get_bandwidth(WIFI_IF_STA, &bandwidth);
    ESP_LOGI(TAG,
             "wifi performance protocol stage: prefer=%s protocols=0x%02x%s bandwidth=%s%s",
             WIFI_STA_PREFER_11AX_HE20 ? "11ax/he20" : "11n/ht40",
             proto_ret == ESP_OK ? protocols : target_protocols,
             proto_ret == ESP_OK ? "" : "(unverified)",
             bw_ret == ESP_OK ? wifi_bandwidth_name(bandwidth) : wifi_bandwidth_name(target_bandwidth),
             bw_ret == ESP_OK ? "" : "(unverified)");
}

static void wifi_disable_power_save(void)
{
    esp_err_t ret = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "wifi power-save disable failed: %s", esp_err_to_name(ret));
    }

    ret = esp_wifi_set_max_tx_power(WIFI_MAX_TX_POWER_QDBM);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "wifi max tx power tuning failed: %s", esp_err_to_name(ret));
    }

    int8_t tx_power = 0;
    if (esp_wifi_get_max_tx_power(&tx_power) == ESP_OK) {
        ESP_LOGI(TAG,
                 "wifi performance runtime stage: ps=none max_tx_power=%d qdbm",
                 (int)tx_power);
    } else {
        ESP_LOGI(TAG,
                 "wifi performance runtime stage: ps=none max_tx_power=%d qdbm(unverified)",
                 WIFI_MAX_TX_POWER_QDBM);
    }
}

static bool wifi_has_saved_config(void)
{
    return s_wifi_saved_ssid[0] != '\0';
}

static bool wifi_has_pending_config(void)
{
    return s_wifi_pending_ssid[0] != '\0';
}

static esp_err_t wifi_load_saved_config(void)
{
    nvs_handle_t nvs_handle = 0;
    char saved_ssid[sizeof(s_wifi_saved_ssid)] = {0};
    char saved_password[sizeof(s_wifi_saved_password)] = {0};
    size_t ssid_len = sizeof(saved_ssid);
    size_t password_len = sizeof(saved_password);

    esp_err_t ret = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_get_str(nvs_handle, WIFI_NVS_KEY_SSID, saved_ssid, &ssid_len);
    if (ret != ESP_OK || saved_ssid[0] == '\0') {
        nvs_close(nvs_handle);
        return ret == ESP_OK ? ESP_ERR_NVS_NOT_FOUND : ret;
    }

    ret = nvs_get_str(nvs_handle, WIFI_NVS_KEY_PASSWORD, saved_password, &password_len);
    nvs_close(nvs_handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        saved_password[0] = '\0';
        ret = ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    strlcpy(s_wifi_saved_ssid, saved_ssid, sizeof(s_wifi_saved_ssid));
    strlcpy(s_wifi_saved_password, saved_password, sizeof(s_wifi_saved_password));
    s_wifi_pending_explicit = false;
    wifi_sync_pending_with_saved();
    return ESP_OK;
}

static esp_err_t wifi_save_saved_config(void)
{
    nvs_handle_t nvs_handle = 0;
    char ssid[sizeof(s_wifi_saved_ssid)] = {0};
    char password[sizeof(s_wifi_saved_password)] = {0};

    taskENTER_CRITICAL(&s_wifi_lock);
    strlcpy(ssid, s_wifi_saved_ssid, sizeof(ssid));
    strlcpy(password, s_wifi_saved_password, sizeof(password));
    taskEXIT_CRITICAL(&s_wifi_lock);

    if (ssid[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_str(nvs_handle, WIFI_NVS_KEY_SSID, ssid);
    if (ret == ESP_OK) {
        ret = nvs_set_str(nvs_handle, WIFI_NVS_KEY_PASSWORD, password);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);

    return ret;
}

static int wifi_compare_ap_records(const void *lhs, const void *rhs)
{
    const wifi_ap_record_t *left = (const wifi_ap_record_t *)lhs;
    const wifi_ap_record_t *right = (const wifi_ap_record_t *)rhs;

    if (left->rssi == right->rssi) {
        return strcmp((const char *)left->ssid, (const char *)right->ssid);
    }
    return right->rssi - left->rssi;
}

static uint8_t wifi_find_recent_channel_hint(const char *ssid)
{
    uint8_t channel = 0;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    if (ssid == NULL || ssid[0] == '\0') {
        return 0;
    }

    taskENTER_CRITICAL(&s_wifi_lock);
    if (!s_wifi_scan_snapshot.in_progress && s_wifi_scan_snapshot.count > 0 &&
        (uint32_t)(now_ms - s_wifi_scan_snapshot.last_scan_ms) <= WIFI_CHANNEL_HINT_MAX_AGE_MS) {
        for (uint16_t index = 0; index < s_wifi_scan_snapshot.count; ++index) {
            if (strcmp(s_wifi_scan_snapshot.results[index].ssid, ssid) == 0) {
                channel = s_wifi_scan_snapshot.results[index].channel;
                break;
            }
        }
    }
    taskEXIT_CRITICAL(&s_wifi_lock);

    return channel;
}

static void wifi_build_sta_config(wifi_config_t *wifi_cfg, const char *ssid, const char *password)
{
    uint8_t channel_hint = wifi_find_recent_channel_hint(ssid);

    *wifi_cfg = (wifi_config_t){
        .sta = {
            .scan_method = WIFI_FAST_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
            .threshold.authmode = WIFI_AUTH_OPEN,
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
        },
    };

    strlcpy((char *)wifi_cfg->sta.ssid, ssid != NULL ? ssid : "", sizeof(wifi_cfg->sta.ssid));
    strlcpy((char *)wifi_cfg->sta.password, password != NULL ? password : "", sizeof(wifi_cfg->sta.password));
    if (channel_hint > 0U) {
        wifi_cfg->sta.channel = channel_hint;
    }
}

static void wifi_post_hosted_netif_down(void)
{
    wifi_event_sta_disconnected_t disconnected = {0};
    disconnected.reason = WIFI_REASON_CONNECTION_FAIL;

    esp_err_t ret = esp_event_post(WIFI_EVENT,
                                   WIFI_EVENT_STA_DISCONNECTED,
                                   &disconnected,
                                   sizeof(disconnected),
                                   pdMS_TO_TICKS(250));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ESP-Hosted recovery disconnect event failed: %s", esp_err_to_name(ret));
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    ret = esp_event_post(WIFI_EVENT,
                         WIFI_EVENT_STA_STOP,
                         NULL,
                         0,
                         pdMS_TO_TICKS(250));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ESP-Hosted recovery stop event failed: %s", esp_err_to_name(ret));
    }
    vTaskDelay(pdMS_TO_TICKS(100));
}

static esp_err_t wifi_configure_hosted_heartbeat(void)
{
#if WIFI_HOSTED_EVENT_RECOVERY_SUPPORTED
    esp_err_t ret = esp_hosted_configure_heartbeat(true, WIFI_HOSTED_HEARTBEAT_INTERVAL_SEC);
    const uint32_t now_ms = wifi_uptime_ms();
    taskENTER_CRITICAL(&s_wifi_lock);
    s_wifi_hosted_heartbeat_configured = ret == ESP_OK;
    s_wifi_hosted_last_heartbeat_ms = ret == ESP_OK ? now_ms : 0U;
    taskEXIT_CRITICAL(&s_wifi_lock);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ESP-Hosted heartbeat setup failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG,
             "ESP-Hosted heartbeat monitor ready: interval=%us timeout=%ums",
             WIFI_HOSTED_HEARTBEAT_INTERVAL_SEC,
             (unsigned)WIFI_HOSTED_HEARTBEAT_TIMEOUT_MS);
#endif
    return ESP_OK;
}

static esp_err_t wifi_recover_hosted_transport(wifi_hosted_recovery_reason_t reason,
                                               esp_err_t trigger_error,
                                               uint32_t attempt)
{
    wifi_config_t wifi_cfg = {0};
    char pending_ssid[sizeof(s_wifi_pending_ssid)] = {0};
    char pending_password[sizeof(s_wifi_pending_password)] = {0};
    bool configured = false;
    bool auto_connect = false;
    esp_err_t ret = ESP_OK;

    taskENTER_CRITICAL(&s_wifi_lock);
    configured = s_wifi_pending_ssid[0] != '\0' || s_wifi_saved_ssid[0] != '\0';
    auto_connect = s_wifi_config.auto_connect;
    strlcpy(pending_ssid, s_wifi_pending_ssid, sizeof(pending_ssid));
    strlcpy(pending_password, s_wifi_pending_password, sizeof(pending_password));
    s_wifi_reconfig_in_progress = true;
    s_wifi_status.connected = false;
    s_wifi_status.rssi = WIFI_INVALID_RSSI;
    s_wifi_status.ip_addr[0] = '\0';
    s_wifi_background_recovery_due_ms = 0U;
    taskEXIT_CRITICAL(&s_wifi_lock);

    if (configured) {
        wifi_build_sta_config(&wifi_cfg, pending_ssid, pending_password);
    }

    wifi_cancel_connect_timeout();
    if (s_wifi_event_group != NULL) {
        xEventGroupClearBits(s_wifi_event_group,
                             WIFI_CONNECTED_BIT | WIFI_FAIL_BIT | WIFI_SCAN_DONE_BIT |
                                 WIFI_HOSTED_UP_BIT);
    }

    ESP_LOGW(TAG,
             "ESP-Hosted recovery begin: attempt=%lu reason=%s trigger=%s",
             (unsigned long)attempt,
             wifi_hosted_recovery_reason_name(reason),
             esp_err_to_name(trigger_error));

    wifi_post_hosted_netif_down();

    taskENTER_CRITICAL(&s_wifi_lock);
    s_wifi_status.started = false;
    s_wifi_scan_in_progress = false;
    s_wifi_scan_snapshot.in_progress = false;
    s_wifi_manual_scan_active = false;
    s_wifi_resume_connect_after_scan = false;
    s_wifi_hosted_heartbeat_configured = false;
    s_wifi_hosted_last_heartbeat_ms = 0U;
    taskEXIT_CRITICAL(&s_wifi_lock);

    if (s_wifi_sta_netif != NULL) {
        esp_netif_destroy_default_wifi(s_wifi_sta_netif);
        s_wifi_sta_netif = NULL;
    }

    if (s_wifi_hosted_runtime_initialized) {
        ret = (esp_err_t)esp_hosted_deinit();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "ESP-Hosted deinit failed: %s", esp_err_to_name(ret));
            goto failed;
        }
        s_wifi_hosted_runtime_initialized = false;
    }

    vTaskDelay(pdMS_TO_TICKS(100));
    ret = (esp_err_t)esp_hosted_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ESP-Hosted init failed: %s", esp_err_to_name(ret));
        goto failed;
    }
    s_wifi_hosted_runtime_initialized = true;

    taskENTER_CRITICAL(&s_wifi_lock);
    s_wifi_hosted_seen_cp_init = false;
    taskEXIT_CRITICAL(&s_wifi_lock);

#if WIFI_HOSTED_EVENT_RECOVERY_SUPPORTED
    ret = (esp_err_t)esp_hosted_connect_to_slave();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ESP-Hosted reconnect failed: %s", esp_err_to_name(ret));
        goto failed;
    }
    EventBits_t hosted_bits = xEventGroupWaitBits(s_wifi_event_group,
                                                   WIFI_HOSTED_UP_BIT,
                                                   pdFALSE,
                                                   pdFALSE,
                                                   pdMS_TO_TICKS(10000));
    if ((hosted_bits & WIFI_HOSTED_UP_BIT) == 0U) {
        ret = ESP_ERR_TIMEOUT;
        ESP_LOGE(TAG, "ESP-Hosted reconnect timed out waiting for transport up");
        goto failed;
    }

    /* The host transport can fail while the C6 Wi-Fi runtime remains alive.
     * Reconnect the transport first, then reset the remote Wi-Fi owner before
     * issuing another esp_wifi_init(). Otherwise a recoverable host-side fault
     * can turn into a duplicate remote initialization failure. */
    ret = esp_wifi_deinit();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGE(TAG,
                 "ESP-Hosted recovery remote Wi-Fi deinit failed: %s",
                 esp_err_to_name(ret));
        goto failed;
    }
#else
    ret = esp_hosted_slave_reset();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ESP-Hosted slave reset failed: %s", esp_err_to_name(ret));
        goto failed;
    }
#endif

    s_wifi_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_wifi_sta_netif == NULL) {
        ret = ESP_ERR_NO_MEM;
        ESP_LOGE(TAG, "ESP-Hosted recovery STA netif allocation failed");
        goto failed;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&init_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ESP-Hosted recovery esp_wifi_init failed: %s", esp_err_to_name(ret));
        goto failed;
    }
    (void)wifi_configure_hosted_heartbeat();

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ESP-Hosted recovery Wi-Fi mode failed: %s", esp_err_to_name(ret));
        goto failed;
    }
    ret = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ESP-Hosted recovery Wi-Fi storage failed: %s", esp_err_to_name(ret));
        goto failed;
    }
    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ESP-Hosted recovery Wi-Fi config failed: %s", esp_err_to_name(ret));
        goto failed;
    }
    wifi_apply_startup_sta_tuning();
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ESP-Hosted recovery Wi-Fi start failed: %s", esp_err_to_name(ret));
        goto failed;
    }
    wifi_disable_power_save();

    taskENTER_CRITICAL(&s_wifi_lock);
    s_wifi_status.started = true;
    s_wifi_status.configured = configured;
    s_wifi_status.connected = false;
    s_wifi_status.retry_count = 0;
    s_wifi_status.disconnect_reason = 0;
    s_wifi_status.rssi = WIFI_INVALID_RSSI;
    s_wifi_status.ip_addr[0] = '\0';
    s_wifi_reconfig_in_progress = false;
    s_wifi_hosted_recovery_in_progress = false;
    s_wifi_hosted_recovery_reason = WIFI_HOSTED_RECOVERY_NONE;
    s_wifi_hosted_recovery_error = ESP_OK;
    s_wifi_hosted_recovery_attempts = 0U;
    taskEXIT_CRITICAL(&s_wifi_lock);

    if (configured && auto_connect) {
        ret = esp_wifi_connect();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "ESP-Hosted recovery Wi-Fi connect failed: %s", esp_err_to_name(ret));
            goto failed_after_state;
        }
        wifi_note_connect_started();
    }

    ESP_LOGI(TAG,
             "ESP-Hosted recovery complete: reconnect=%d stack_free=%u",
             configured && auto_connect,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    return ESP_OK;

failed_after_state:
    taskENTER_CRITICAL(&s_wifi_lock);
    s_wifi_hosted_recovery_in_progress = true;
    s_wifi_reconfig_in_progress = true;
    taskEXIT_CRITICAL(&s_wifi_lock);

failed:
    if (s_wifi_sta_netif != NULL) {
        esp_netif_destroy_default_wifi(s_wifi_sta_netif);
        s_wifi_sta_netif = NULL;
    }
    taskENTER_CRITICAL(&s_wifi_lock);
    s_wifi_status.started = false;
    s_wifi_status.connected = false;
    s_wifi_status.rssi = WIFI_INVALID_RSSI;
    s_wifi_status.ip_addr[0] = '\0';
    s_wifi_reconfig_in_progress = false;
    s_wifi_hosted_recovery_in_progress = false;
    taskEXIT_CRITICAL(&s_wifi_lock);
    return ret;
}

static void wifi_refresh_rssi(void)
{
    if (!s_wifi_initialized) {
        return;
    }

    taskENTER_CRITICAL(&s_wifi_lock);
    bool connected = s_wifi_status.connected;
    if (!connected) {
        s_wifi_status.rssi = WIFI_INVALID_RSSI;
        taskEXIT_CRITICAL(&s_wifi_lock);
        return;
    }
    taskEXIT_CRITICAL(&s_wifi_lock);

    wifi_ap_record_t ap_info = {0};
    esp_err_t ret = esp_wifi_sta_get_ap_info(&ap_info);

    taskENTER_CRITICAL(&s_wifi_lock);
    if (ret == ESP_OK) {
        s_wifi_status.rssi = ap_info.rssi;
    } else if (!s_wifi_status.connected) {
        s_wifi_status.rssi = WIFI_INVALID_RSSI;
    }
    taskEXIT_CRITICAL(&s_wifi_lock);
}

static bool wifi_scan_snapshot_has_ssid_locked(const char *ssid)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return false;
    }

    for (uint16_t index = 0; index < s_wifi_scan_snapshot.count; ++index) {
        if (strcmp(s_wifi_scan_snapshot.results[index].ssid, ssid) == 0) {
            return true;
        }
    }

    return false;
}

static wifi_scan_resume_action_t wifi_finish_scan_state_locked(void)
{
    bool wants_resume = s_wifi_resume_connect_after_scan &&
                        s_wifi_status.configured &&
                        !s_wifi_status.connected;
    bool target_available = wifi_scan_snapshot_has_ssid_locked(s_wifi_pending_ssid);
    wifi_scan_resume_action_t action = WIFI_SCAN_RESUME_NONE;

    if (wants_resume && target_available) {
        action = WIFI_SCAN_RESUME_CONNECT;
    } else if (wants_resume) {
        s_wifi_status.retry_count = WIFI_MAX_RETRIES;
        s_wifi_status.disconnect_reason = WIFI_REASON_NO_AP_FOUND;
        s_wifi_status.rssi = WIFI_INVALID_RSSI;
        s_wifi_status.ip_addr[0] = '\0';
        action = WIFI_SCAN_RESUME_TARGET_MISSING;
    }

    s_wifi_scan_in_progress = false;
    s_wifi_scan_snapshot.in_progress = false;
    s_wifi_manual_scan_active = false;
    s_wifi_resume_connect_after_scan = false;
    return action;
}

static void wifi_resume_connect_if_needed(wifi_scan_resume_action_t action)
{
    if (action == WIFI_SCAN_RESUME_NONE) {
        return;
    }

    if (action == WIFI_SCAN_RESUME_TARGET_MISSING) {
        char current_ssid[sizeof(s_wifi_status.ssid)] = {0};
        taskENTER_CRITICAL(&s_wifi_lock);
        strlcpy(current_ssid, s_wifi_status.ssid, sizeof(current_ssid));
        wifi_arm_background_recovery_locked(wifi_uptime_ms(), WIFI_REASON_NO_AP_FOUND);
        taskEXIT_CRITICAL(&s_wifi_lock);
        ESP_LOGW(TAG,
                 "wifi target not found: ssid=%s reason=%u(%s)",
                 current_ssid,
                 (unsigned)WIFI_REASON_NO_AP_FOUND,
                 wifi_disconnect_reason_name(WIFI_REASON_NO_AP_FOUND));
        if (s_wifi_event_group != NULL) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        return;
    }

    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "resume wifi connect after scan failed: %s", esp_err_to_name(ret));
        wifi_request_hosted_recovery(WIFI_HOSTED_RECOVERY_CONNECT_RPC, ret);
    } else {
        wifi_note_connect_started();
    }
}

static const char *wifi_disconnect_reason_name(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return "4way-timeout";
    case WIFI_REASON_802_1X_AUTH_FAILED:
        return "auth-8021x-failed";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "beacon-timeout";
    case WIFI_REASON_NO_AP_FOUND:
        return "no-ap-found";
    case WIFI_REASON_AUTH_FAIL:
        return "auth-failed";
    case WIFI_REASON_ASSOC_FAIL:
        return "assoc-failed";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "handshake-timeout";
    case WIFI_REASON_CONNECTION_FAIL:
        return "connection-failed";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        return "no-compatible-security";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "authmode-threshold";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "rssi-threshold";
    default:
        return "unknown";
    }
}

static const char *wifi_event_name(int32_t event_id)
{
    switch (event_id) {
    case WIFI_EVENT_STA_START:
        return "STA_START";
    case WIFI_EVENT_STA_STOP:
        return "STA_STOP";
    case WIFI_EVENT_STA_CONNECTED:
        return "STA_CONNECTED";
    case WIFI_EVENT_STA_DISCONNECTED:
        return "STA_DISCONNECTED";
    case WIFI_EVENT_SCAN_DONE:
        return "SCAN_DONE";
    default:
        return "OTHER";
    }
}

static esp_err_t wifi_start_blocking_scan(wifi_scan_config_t *scan_cfg, bool log_results)
{
    if (s_wifi_event_group == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xEventGroupClearBits(s_wifi_event_group, WIFI_SCAN_DONE_BIT);

    esp_err_t ret = esp_wifi_scan_start(scan_cfg, false);
    if (ret == ESP_ERR_WIFI_STATE) {
        vTaskDelay(pdMS_TO_TICKS(150));
        xEventGroupClearBits(s_wifi_event_group, WIFI_SCAN_DONE_BIT);
        ret = esp_wifi_scan_start(scan_cfg, false);
    }
    if (ret != ESP_OK && log_results) {
        ESP_LOGW(TAG, "wifi scan start failed: %s", esp_err_to_name(ret));
    }

    if (ret != ESP_OK) {
        return ret;
    }

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_SCAN_DONE_BIT,
                                           pdTRUE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(WIFI_SCAN_WAIT_MS));
    if ((bits & WIFI_SCAN_DONE_BIT) == 0) {
        (void)esp_wifi_scan_stop();
        ESP_LOGW(TAG, "wifi scan timeout: wait_ms=%u", (unsigned)WIFI_SCAN_WAIT_MS);
        return ESP_ERR_TIMEOUT;
    }

    return ret;
}

static esp_err_t wifi_scan_now(bool log_results)
{
    wifi_scan_config_t scan_cfg = {
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .show_hidden = false,
        .scan_time.active.min = WIFI_SCAN_ACTIVE_MIN_MS,
        .scan_time.active.max = WIFI_SCAN_ACTIVE_MAX_MS,
    };
    wifi_scan_config_t passive_scan_cfg = {
        .scan_type = WIFI_SCAN_TYPE_PASSIVE,
        .show_hidden = false,
        .scan_time.passive = WIFI_SCAN_PASSIVE_MS,
    };
    wifi_ap_record_t ap_records[WIFI_SCAN_RESULT_MAX] = {0};
    uint16_t ap_count = 0;
    uint16_t fetch_count = WIFI_SCAN_RESULT_MAX;
    wifi_scan_resume_action_t resume_connect = WIFI_SCAN_RESUME_NONE;

    taskENTER_CRITICAL(&s_wifi_lock);
    s_wifi_scan_in_progress = true;
    s_wifi_scan_snapshot.in_progress = true;
    s_wifi_manual_scan_active = true;
    s_wifi_resume_connect_after_scan = s_wifi_status.configured && !s_wifi_status.connected;
    taskEXIT_CRITICAL(&s_wifi_lock);

    esp_err_t ret = wifi_start_blocking_scan(&scan_cfg, log_results);
    if (ret != ESP_OK) {
        taskENTER_CRITICAL(&s_wifi_lock);
        resume_connect = wifi_finish_scan_state_locked();
        taskEXIT_CRITICAL(&s_wifi_lock);
        wifi_resume_connect_if_needed(resume_connect);
        return ret;
    }

    ret = esp_wifi_scan_get_ap_num(&ap_count);
    if (ret != ESP_OK) {
        taskENTER_CRITICAL(&s_wifi_lock);
        resume_connect = wifi_finish_scan_state_locked();
        taskEXIT_CRITICAL(&s_wifi_lock);
        wifi_resume_connect_if_needed(resume_connect);
        if (log_results) {
            ESP_LOGW(TAG, "wifi scan ap count failed: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    if (ap_count == 0) {
        ret = wifi_start_blocking_scan(&passive_scan_cfg, log_results);
        if (ret == ESP_OK) {
            ret = esp_wifi_scan_get_ap_num(&ap_count);
        }
    }

    if (ret != ESP_OK) {
        taskENTER_CRITICAL(&s_wifi_lock);
        resume_connect = wifi_finish_scan_state_locked();
        taskEXIT_CRITICAL(&s_wifi_lock);
        wifi_resume_connect_if_needed(resume_connect);
        if (log_results) {
            ESP_LOGW(TAG, "wifi passive scan failed: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    if (ap_count == 0) {
        taskENTER_CRITICAL(&s_wifi_lock);
        s_wifi_scan_snapshot.count = 0;
        s_wifi_scan_snapshot.last_scan_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        resume_connect = wifi_finish_scan_state_locked();
        taskEXIT_CRITICAL(&s_wifi_lock);
        wifi_resume_connect_if_needed(resume_connect);
        return ESP_OK;
    }

    if (ap_count < fetch_count) {
        fetch_count = ap_count;
    }

    ret = esp_wifi_scan_get_ap_records(&fetch_count, ap_records);
    if (ret != ESP_OK) {
        taskENTER_CRITICAL(&s_wifi_lock);
        resume_connect = wifi_finish_scan_state_locked();
        taskEXIT_CRITICAL(&s_wifi_lock);
        wifi_resume_connect_if_needed(resume_connect);
        if (log_results) {
            ESP_LOGW(TAG, "wifi scan readback failed: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    qsort(ap_records, fetch_count, sizeof(ap_records[0]), wifi_compare_ap_records);

    taskENTER_CRITICAL(&s_wifi_lock);
    memset(&s_wifi_scan_snapshot.results, 0, sizeof(s_wifi_scan_snapshot.results));
    s_wifi_scan_snapshot.count = 0;
    s_wifi_scan_snapshot.last_scan_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    for (uint16_t index = 0; index < fetch_count; ++index) {
        bool duplicate_ssid = false;

        if (ap_records[index].ssid[0] == '\0') {
            continue;
        }

        for (uint16_t existing = 0; existing < s_wifi_scan_snapshot.count; ++existing) {
            if (strcmp(s_wifi_scan_snapshot.results[existing].ssid, (const char *)ap_records[index].ssid) == 0) {
                duplicate_ssid = true;
                break;
            }
        }
        if (duplicate_ssid) {
            continue;
        }

        wifi_scan_result_t *dst = &s_wifi_scan_snapshot.results[s_wifi_scan_snapshot.count++];
        strlcpy(dst->ssid, (const char *)ap_records[index].ssid, sizeof(dst->ssid));
        dst->rssi = ap_records[index].rssi;
        dst->authmode = (uint8_t)ap_records[index].authmode;
        dst->channel = ap_records[index].primary;
        dst->secure = ap_records[index].authmode != WIFI_AUTH_OPEN;

        if (s_wifi_scan_snapshot.count >= WIFI_SCAN_RESULT_MAX) {
            break;
        }
    }
    resume_connect = wifi_finish_scan_state_locked();
    taskEXIT_CRITICAL(&s_wifi_lock);
    wifi_resume_connect_if_needed(resume_connect);

    (void)log_results;
    return ESP_OK;
}

static bool wifi_is_configured(void)
{
    return wifi_has_pending_config() || wifi_has_saved_config();
}

static void wifi_copy_status(wifi_status_t *status)
{
    wifi_mark_connect_timeout_if_needed();

    taskENTER_CRITICAL(&s_wifi_lock);
    *status = s_wifi_status;
    taskEXIT_CRITICAL(&s_wifi_lock);
}

static void wifi_event_handler(void *arg,
                                        esp_event_base_t event_base,
                                        int32_t event_id,
                                        void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        taskENTER_CRITICAL(&s_wifi_lock);
        s_wifi_status.started = true;
        bool should_connect = wifi_has_pending_config();
        char current_ssid[sizeof(s_wifi_status.ssid)] = {0};
        strlcpy(current_ssid, s_wifi_status.ssid, sizeof(current_ssid));
        taskEXIT_CRITICAL(&s_wifi_lock);

        ESP_LOGI(TAG, "wifi event: %s configured=%d ssid=%s", wifi_event_name(event_id), should_connect, current_ssid);
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        if (s_wifi_event_group != NULL) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_SCAN_DONE_BIT);
        }
        ESP_LOGI(TAG, "wifi event: %s", wifi_event_name(event_id));
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disconnected = (const wifi_event_sta_disconnected_t *)event_data;
        uint8_t retry_count = 0;
        bool should_reconnect = false;
        bool manual_scan_active = false;
        bool configured = false;
        bool release_requested = false;
        bool reconfig_in_progress = false;
        char current_ssid[sizeof(s_wifi_status.ssid)] = {0};

        wifi_cancel_connect_timeout();
        taskENTER_CRITICAL(&s_wifi_lock);
        s_wifi_status.connected = false;
        s_wifi_status.disconnect_reason = disconnected->reason;
        s_wifi_status.rssi = WIFI_INVALID_RSSI;
        s_wifi_status.ip_addr[0] = '\0';
        manual_scan_active = s_wifi_manual_scan_active || s_wifi_scan_in_progress;
        configured = s_wifi_status.configured;
        release_requested = s_wifi_release_requested || !s_wifi_status.started;
        reconfig_in_progress = s_wifi_reconfig_in_progress;
        strlcpy(current_ssid, s_wifi_status.ssid, sizeof(current_ssid));
        if (!release_requested && !reconfig_in_progress && s_wifi_status.configured && !manual_scan_active &&
            !wifi_disconnect_is_authentication_failure(disconnected->reason) &&
            s_wifi_status.retry_count < WIFI_MAX_RETRIES) {
            if (s_wifi_status.retry_count < UINT8_MAX) {
                s_wifi_status.retry_count++;
            }
            retry_count = s_wifi_status.retry_count;
            should_reconnect = true;
            s_wifi_background_recovery_due_ms = 0U;
        } else if (!release_requested && !reconfig_in_progress && s_wifi_status.configured &&
                   !manual_scan_active) {
            if (wifi_disconnect_is_authentication_failure(disconnected->reason)) {
                s_wifi_status.retry_count = WIFI_MAX_RETRIES;
            }
            retry_count = s_wifi_status.retry_count;
            wifi_arm_background_recovery_locked(wifi_uptime_ms(), disconnected->reason);
        }
        taskEXIT_CRITICAL(&s_wifi_lock);

        if (manual_scan_active) {
            return;
        }

        if (release_requested) {
            ESP_LOGI(TAG, "wifi disconnected for release: ssid=%s", current_ssid);
            return;
        }

        if (reconfig_in_progress) {
            ESP_LOGI(TAG, "wifi disconnected for reconfig: ssid=%s", current_ssid);
            return;
        }

        if (configured) {
            ESP_LOGW(TAG,
                     "wifi disconnected: ssid=%s reason=%u(%s) retry=%u/%u",
                     current_ssid,
                     (unsigned)disconnected->reason,
                     wifi_disconnect_reason_name(disconnected->reason),
                     (unsigned)retry_count,
                     (unsigned)WIFI_MAX_RETRIES);
        }

        if (should_reconnect) {
            ESP_LOGD(TAG,
                     "wifi reconnect scheduled: attempt=%u reason=%u",
                     (unsigned)retry_count,
                     (unsigned)disconnected->reason);
            esp_err_t ret = esp_wifi_connect();
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "wifi reconnect failed: %s", esp_err_to_name(ret));
                wifi_request_hosted_recovery(WIFI_HOSTED_RECOVERY_CONNECT_RPC, ret);
            } else {
                wifi_note_connect_started();
            }
            return;
        }

        if (configured) {
            ESP_LOGW(TAG,
                     "wifi reconnect stopped: attempts=%u reason=%u",
                     (unsigned)WIFI_MAX_RETRIES,
                     (unsigned)disconnected->reason);
            if (!wifi_disconnect_is_authentication_failure(disconnected->reason)) {
                ESP_LOGI(TAG,
                         "wifi background recovery scheduled: interval_ms=%u",
                         (unsigned)WIFI_BACKGROUND_RECOVERY_INTERVAL_MS);
            }
        }
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        return;
    }

    if (event_base == WIFI_EVENT) {
        ESP_LOGD(TAG, "wifi event: %s(%ld)", wifi_event_name(event_id), (long)event_id);
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *got_ip = (const ip_event_got_ip_t *)event_data;
        char connected_ssid[sizeof(s_wifi_status.ssid)] = {0};
        char connected_ip[sizeof(s_wifi_status.ip_addr)] = {0};

        esp_err_t dns_ret = wifi_apply_fallback_dns();
        if (dns_ret != ESP_OK) {
            ESP_LOGW(TAG,
                     "wifi fallback DNS apply failed: address=%s ret=%s",
                     s_wifi_config.fallback_dns_ipv4 != NULL ? s_wifi_config.fallback_dns_ipv4 : "",
                     esp_err_to_name(dns_ret));
        }
        wifi_log_dns_servers();

        wifi_cancel_connect_timeout();
        taskENTER_CRITICAL(&s_wifi_lock);
        strlcpy(s_wifi_saved_ssid, s_wifi_pending_ssid, sizeof(s_wifi_saved_ssid));
        strlcpy(s_wifi_saved_password, s_wifi_pending_password, sizeof(s_wifi_saved_password));
        s_wifi_pending_explicit = false;
        s_wifi_status.connected = true;
        s_wifi_status.retry_count = 0;
        s_wifi_background_recovery_due_ms = 0U;
        snprintf(s_wifi_status.ip_addr,
                 sizeof(s_wifi_status.ip_addr),
                 IPSTR,
                 IP2STR(&got_ip->ip_info.ip));
        strlcpy(connected_ssid, s_wifi_status.ssid, sizeof(connected_ssid));
        strlcpy(connected_ip, s_wifi_status.ip_addr, sizeof(connected_ip));
        taskEXIT_CRITICAL(&s_wifi_lock);
        wifi_refresh_rssi();
        wifi_log_connected_link();

        esp_err_t save_ret = wifi_save_saved_config();
        if (save_ret != ESP_OK) {
            ESP_LOGW(TAG, "save wifi credentials failed: %s", esp_err_to_name(save_ret));
        }
        wifi_sync_pending_with_saved();

        ESP_LOGI(TAG, "wifi connected: ssid=%s ip=%s", connected_ssid, connected_ip);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_nvs_init(void)
{
    return platform_storage_init();
}

static void wifi_scan_task(void *ctx)
{
    (void)ctx;

    while (true) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        bool can_scan = s_wifi_status.started;

        if (!can_scan) {
            taskENTER_CRITICAL(&s_wifi_lock);
            s_wifi_scan_in_progress = false;
            s_wifi_scan_snapshot.in_progress = false;
            taskEXIT_CRITICAL(&s_wifi_lock);
            continue;
        }
        (void)wifi_scan_now(false);
    }
}

esp_err_t wifi_prepare(const wifi_driver_config_t *config)
{
    bool started = false;

    if (config != NULL) {
        s_wifi_config = *config;
    }

    if (s_wifi_initialized) {
        if (!s_wifi_config.enabled) {
            return ESP_OK;
        }

        taskENTER_CRITICAL(&s_wifi_lock);
        started = s_wifi_status.started;
        taskEXIT_CRITICAL(&s_wifi_lock);
        if (started) {
            return ESP_OK;
        }

        wifi_sync_pending_with_saved();
        wifi_config_t wifi_cfg = {0};
        if (wifi_is_configured()) {
            wifi_build_sta_config(&wifi_cfg, s_wifi_pending_ssid, s_wifi_pending_password);
        }

        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG, "wifi config restore failed");
        wifi_apply_startup_sta_tuning();
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi restart failed");
        wifi_disable_power_save();

        taskENTER_CRITICAL(&s_wifi_lock);
        s_wifi_release_requested = false;
        s_wifi_reconfig_in_progress = false;
        s_wifi_background_recovery_due_ms = 0U;
        s_wifi_status.started = true;
        s_wifi_status.configured = wifi_is_configured();
        s_wifi_status.connected = false;
        s_wifi_status.retry_count = 0;
        s_wifi_status.disconnect_reason = 0;
        s_wifi_status.rssi = WIFI_INVALID_RSSI;
        s_wifi_status.ip_addr[0] = '\0';
        strlcpy(s_wifi_status.ssid, s_wifi_pending_ssid, sizeof(s_wifi_status.ssid));
        taskEXIT_CRITICAL(&s_wifi_lock);

        if (!wifi_is_configured()) {
            ESP_LOGI(TAG, "wifi restarted: no credentials, scan mode");
            return ESP_OK;
        }

        if (!s_wifi_config.auto_connect) {
            ESP_LOGI(TAG, "wifi restarted: credentials saved ssid=%s auto_connect=0", s_wifi_status.ssid);
            return ESP_OK;
        }

        ESP_LOGI(TAG, "wifi reconnecting: ssid=%s", s_wifi_status.ssid);
        esp_err_t connect_ret = esp_wifi_connect();
        if (connect_ret != ESP_OK) {
            ESP_LOGW(TAG, "wifi reconnect start failed: %s", esp_err_to_name(connect_ret));
            if (s_wifi_event_group != NULL) {
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
            return connect_ret;
        }
        wifi_note_connect_started();
        return ESP_OK;
    }

    bool seed_default_credentials = false;

    esp_log_level_set("wifi", ESP_LOG_ERROR);
    wifi_load_initial_saved_config();

    if (!s_wifi_config.enabled) {
        ESP_LOGW(TAG, "wifi disabled by configuration");
        taskENTER_CRITICAL(&s_wifi_lock);
        memset(&s_wifi_status, 0, sizeof(s_wifi_status));
        s_wifi_status.rssi = WIFI_INVALID_RSSI;
        taskEXIT_CRITICAL(&s_wifi_lock);
        s_wifi_initialized = true;
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(wifi_nvs_init(), TAG, "nvs init failed");

    if (!s_wifi_pending_explicit) {
        esp_err_t load_ret = wifi_load_saved_config();
        if (load_ret != ESP_OK && load_ret != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "load saved wifi credentials failed: %s", esp_err_to_name(load_ret));
        } else if (load_ret == ESP_ERR_NVS_NOT_FOUND && wifi_has_saved_config()) {
            seed_default_credentials = true;
        }
    }

    if (seed_default_credentials) {
        esp_err_t save_ret = wifi_save_saved_config();
        if (save_ret != ESP_OK) {
            ESP_LOGW(TAG, "seed default wifi credentials failed: %s", esp_err_to_name(save_ret));
        }
    }

    wifi_sync_pending_with_saved();

    taskENTER_CRITICAL(&s_wifi_lock);
    s_wifi_status.configured = wifi_is_configured();
    s_wifi_status.started = false;
    s_wifi_status.connected = false;
    s_wifi_status.retry_count = 0;
    s_wifi_status.disconnect_reason = 0;
    s_wifi_status.rssi = WIFI_INVALID_RSSI;
    s_wifi_status.ip_addr[0] = '\0';
    strlcpy(s_wifi_status.ssid, s_wifi_pending_ssid, sizeof(s_wifi_status.ssid));
    s_wifi_release_requested = false;
    s_wifi_reconfig_in_progress = false;
    s_wifi_background_recovery_due_ms = 0U;
    taskEXIT_CRITICAL(&s_wifi_lock);

    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret == ESP_OK) {
        s_wifi_event_loop_ready = true;
    } else if (ret == ESP_ERR_INVALID_STATE) {
        s_wifi_event_loop_ready = true;
    } else {
        return ret;
    }

    s_wifi_event_group = xEventGroupCreateWithCaps(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_wifi_event_group != NULL, ESP_ERR_NO_MEM, TAG, "wifi event group alloc failed");

#if WIFI_HOSTED_EVENT_RECOVERY_SUPPORTED
    if (s_wifi_hosted_event_instance == NULL) {
        ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(ESP_HOSTED_EVENT,
                                                                ESP_EVENT_ANY_ID,
                                                                wifi_hosted_event_handler,
                                                                NULL,
                                                                &s_wifi_hosted_event_instance),
                            TAG,
                            "register ESP-Hosted event handler failed");
    }
#endif

    if (s_wifi_connect_timer == NULL) {
        const esp_timer_create_args_t timer_args = {
            .callback = wifi_connect_timeout_cb,
            .arg = NULL,
            .name = "wifi_connect_timeout",
        };
        ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_wifi_connect_timer),
                            TAG,
                            "wifi connect timeout timer create failed");
    }

    s_wifi_sta_netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(s_wifi_sta_netif != NULL,
                        ESP_ERR_NO_MEM,
                        TAG,
                        "wifi sta netif create failed");

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "esp_wifi_init failed");
    s_wifi_hosted_runtime_initialized = true;
    (void)wifi_configure_hosted_heartbeat();
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL),
                        TAG,
                        "register wifi event handler failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL),
                        TAG,
                        "register ip event handler failed");

    wifi_config_t wifi_cfg = {0};
    if (wifi_is_configured()) {
        wifi_build_sta_config(&wifi_cfg, s_wifi_pending_ssid, s_wifi_pending_password);
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi mode set failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "wifi storage set failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG, "wifi config set failed");
    wifi_apply_startup_sta_tuning();
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");
    wifi_disable_power_save();

    taskENTER_CRITICAL(&s_wifi_lock);
    s_wifi_status.started = true;
    taskEXIT_CRITICAL(&s_wifi_lock);
    s_wifi_initialized = true;

    if (s_wifi_scan_task == NULL) {
        BaseType_t task_ok = xTaskCreatePinnedToCoreWithCaps(wifi_scan_task,
                                                             "wifi_scan",
                                                             4 * 1024,
                                                             NULL,
                                                             WIFI_SCAN_TASK_PRIORITY,
                                                             &s_wifi_scan_task,
                                                             WIFI_SCAN_TASK_CORE,
                                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_RETURN_ON_FALSE(task_ok == pdPASS, ESP_ERR_NO_MEM, TAG, "wifi scan task alloc failed");
    }

    if (s_wifi_connect_watchdog_task == NULL) {
        BaseType_t watchdog_ok = xTaskCreatePinnedToCoreWithCaps(wifi_connect_watchdog_task,
                                                                 "wifi_watchdog",
                                                                 WIFI_HOSTED_RECOVERY_TASK_STACK_SIZE,
                                                                 NULL,
                                                                 WIFI_SCAN_TASK_PRIORITY,
                                                                 &s_wifi_connect_watchdog_task,
                                                                 WIFI_SCAN_TASK_CORE,
                                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_RETURN_ON_FALSE(watchdog_ok == pdPASS,
                            ESP_ERR_NO_MEM,
                            TAG,
                            "wifi watchdog task alloc failed");
    }

    if (!wifi_is_configured()) {
        ESP_LOGI(TAG, "wifi ready: no credentials, scan mode");
        return ESP_OK;
    }

    if (!s_wifi_config.auto_connect) {
        ESP_LOGI(TAG, "wifi ready: credentials saved ssid=%s auto_connect=0", s_wifi_status.ssid);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "wifi connecting: ssid=%s", s_wifi_status.ssid);
    esp_err_t connect_ret = esp_wifi_connect();
    if (connect_ret != ESP_OK) {
        ESP_LOGW(TAG, "wifi connect start failed: %s", esp_err_to_name(connect_ret));
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        wifi_request_hosted_recovery(WIFI_HOSTED_RECOVERY_CONNECT_RPC, connect_ret);
        return connect_ret;
    }
    wifi_note_connect_started();
    return ESP_OK;
}

void wifi_release(void)
{
    bool started = false;

    if (!s_wifi_initialized || !s_wifi_config.enabled) {
        return;
    }

    wifi_cancel_connect_timeout();

    taskENTER_CRITICAL(&s_wifi_lock);
    started = s_wifi_status.started;
    s_wifi_release_requested = true;
    s_wifi_background_recovery_due_ms = 0U;
    s_wifi_scan_in_progress = false;
    s_wifi_scan_snapshot.in_progress = false;
    s_wifi_manual_scan_active = false;
    s_wifi_resume_connect_after_scan = false;
    s_wifi_reconfig_in_progress = false;
    s_wifi_status.connected = false;
    s_wifi_status.retry_count = 0;
    s_wifi_status.disconnect_reason = 0;
    s_wifi_background_recovery_due_ms = 0U;
    s_wifi_status.rssi = WIFI_INVALID_RSSI;
    s_wifi_status.ip_addr[0] = '\0';
    taskEXIT_CRITICAL(&s_wifi_lock);

    if (!started) {
        return;
    }

    (void)esp_wifi_scan_stop();
    esp_err_t disconnect_ret = esp_wifi_disconnect();
    if (disconnect_ret != ESP_OK && disconnect_ret != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGW(TAG, "wifi release disconnect failed: %s", esp_err_to_name(disconnect_ret));
    }

    esp_err_t stop_ret = esp_wifi_stop();
    if (stop_ret != ESP_OK && stop_ret != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(TAG, "wifi release stop failed: %s", esp_err_to_name(stop_ret));
    }

    taskENTER_CRITICAL(&s_wifi_lock);
    s_wifi_status.started = false;
    s_wifi_status.connected = false;
    s_wifi_status.rssi = WIFI_INVALID_RSSI;
    s_wifi_status.ip_addr[0] = '\0';
    s_wifi_reconfig_in_progress = false;
    taskEXIT_CRITICAL(&s_wifi_lock);
    ESP_LOGI(TAG, "wifi released");
}

esp_err_t app_wifi_driver_connect(const char *ssid, const char *password)
{
    if (!s_wifi_config.enabled) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_RETURN_ON_FALSE(ssid != NULL, ESP_ERR_INVALID_ARG, TAG, "ssid is required");
    ESP_RETURN_ON_FALSE(strlen(ssid) > 0, ESP_ERR_INVALID_ARG, TAG, "ssid is empty");

    char next_ssid[sizeof(s_wifi_pending_ssid)] = {0};
    char next_password[sizeof(s_wifi_pending_password)] = {0};
    strlcpy(next_ssid, ssid, sizeof(next_ssid));
    if (password != NULL && password[0] != '\0') {
        strlcpy(next_password, password, sizeof(next_password));
    } else {
        taskENTER_CRITICAL(&s_wifi_lock);
        if (s_wifi_saved_ssid[0] != '\0' && strcmp(next_ssid, s_wifi_saved_ssid) == 0) {
            strlcpy(next_password, s_wifi_saved_password, sizeof(next_password));
        }
        taskEXIT_CRITICAL(&s_wifi_lock);
    }

    taskENTER_CRITICAL(&s_wifi_lock);
    bool was_connected = s_wifi_status.connected;
    bool was_connecting = s_wifi_status.started &&
                          !s_wifi_status.connected &&
                          s_wifi_connect_started_ms != 0 &&
                          s_wifi_connect_started_tick != 0;
    bool same_connect_request = was_connecting &&
                                strcmp(next_ssid, s_wifi_pending_ssid) == 0 &&
                                strcmp(next_password, s_wifi_pending_password) == 0;
    bool started = s_wifi_status.started;
    if (same_connect_request) {
        strlcpy(s_wifi_status.ssid, s_wifi_pending_ssid, sizeof(s_wifi_status.ssid));
        taskEXIT_CRITICAL(&s_wifi_lock);
        ESP_LOGI(TAG, "wifi connect already in progress: ssid=%s", next_ssid);
        return ESP_OK;
    }
    strlcpy(s_wifi_pending_ssid, next_ssid, sizeof(s_wifi_pending_ssid));
    strlcpy(s_wifi_pending_password, next_password, sizeof(s_wifi_pending_password));
    s_wifi_pending_explicit = true;
    s_wifi_status.configured = true;
    s_wifi_status.connected = false;
    s_wifi_status.retry_count = 0;
    s_wifi_status.disconnect_reason = 0;
    s_wifi_background_recovery_due_ms = 0U;
    s_wifi_connect_started_ms = 0;
    s_wifi_connect_started_tick = 0;
    s_wifi_status.rssi = WIFI_INVALID_RSSI;
    s_wifi_status.ip_addr[0] = '\0';
    strlcpy(s_wifi_status.ssid, s_wifi_pending_ssid, sizeof(s_wifi_status.ssid));
    s_wifi_reconfig_in_progress = was_connected || was_connecting;
    taskEXIT_CRITICAL(&s_wifi_lock);

    if (!started) {
        return wifi_prepare(&s_wifi_config);
    }

    if (s_wifi_event_group != NULL) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    }

    wifi_config_t wifi_cfg = {0};
    wifi_build_sta_config(&wifi_cfg, next_ssid, next_password);

    if (was_connected || was_connecting) {
        esp_err_t disconnect_ret = esp_wifi_disconnect();
        if (disconnect_ret != ESP_OK && disconnect_ret != ESP_ERR_WIFI_NOT_CONNECT) {
            taskENTER_CRITICAL(&s_wifi_lock);
            s_wifi_reconfig_in_progress = false;
            taskEXIT_CRITICAL(&s_wifi_lock);
            ESP_LOGW(TAG, "wifi disconnect before reconnect failed: %s", esp_err_to_name(disconnect_ret));
            wifi_request_hosted_recovery(WIFI_HOSTED_RECOVERY_DISCONNECT_RPC, disconnect_ret);
            return disconnect_ret;
        }
        if (was_connecting) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
    esp_err_t set_ret = ESP_FAIL;
    for (uint8_t attempt = 0; attempt < 5; ++attempt) {
        set_ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
        if (set_ret == ESP_OK || set_ret != ESP_ERR_WIFI_STATE) {
            break;
        }
        ESP_LOGW(TAG,
                 "wifi config update waits for sta idle: attempt=%u err=%s",
                 (unsigned)(attempt + 1),
                 esp_err_to_name(set_ret));
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    if (set_ret != ESP_OK) {
        taskENTER_CRITICAL(&s_wifi_lock);
        s_wifi_reconfig_in_progress = false;
        taskEXIT_CRITICAL(&s_wifi_lock);
        ESP_LOGE(TAG, "wifi config update failed: %s", esp_err_to_name(set_ret));
        wifi_request_hosted_recovery(WIFI_HOSTED_RECOVERY_CONFIG_RPC, set_ret);
        return set_ret;
    }

    esp_err_t connect_ret = esp_wifi_connect();
    if (connect_ret != ESP_OK) {
        taskENTER_CRITICAL(&s_wifi_lock);
        s_wifi_reconfig_in_progress = false;
        taskEXIT_CRITICAL(&s_wifi_lock);
        ESP_LOGW(TAG, "wifi connect start failed: %s", esp_err_to_name(connect_ret));
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        wifi_request_hosted_recovery(WIFI_HOSTED_RECOVERY_CONNECT_RPC, connect_ret);
        return connect_ret;
    }
    taskENTER_CRITICAL(&s_wifi_lock);
    s_wifi_reconfig_in_progress = false;
    taskEXIT_CRITICAL(&s_wifi_lock);
    ESP_LOGI(TAG, "wifi connect requested: ssid=%s", next_ssid);
    wifi_note_connect_started();
    return ESP_OK;
}

esp_err_t wifi_request_scan(void)
{
    if (!s_wifi_config.enabled) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (!s_wifi_initialized || s_wifi_scan_task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_cancel_connect_timeout();

    taskENTER_CRITICAL(&s_wifi_lock);
    bool should_notify = !s_wifi_scan_in_progress;
    if (should_notify) {
        s_wifi_scan_in_progress = true;
        s_wifi_scan_snapshot.in_progress = true;
    }
    taskEXIT_CRITICAL(&s_wifi_lock);

    if (should_notify) {
        xTaskNotifyGive(s_wifi_scan_task);
    }
    return ESP_OK;
}

void wifi_get_status(wifi_status_t *status)
{
    if (status == NULL) {
        return;
    }

    /*
     * On ESP32-P4 + ESP-Hosted, esp_wifi_sta_get_ap_info() is a remote RPC.
     * Keep the hot status path cached; querying it from the monitor loop can
     * flood the hosted transport and starve normal Wi-Fi traffic.
     */
    wifi_copy_status(status);
}

void wifi_get_scan_snapshot(wifi_scan_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_wifi_lock);
    *snapshot = s_wifi_scan_snapshot;
    taskEXIT_CRITICAL(&s_wifi_lock);
}

void wifi_get_saved_config(char *ssid, size_t ssid_size, char *password, size_t password_size)
{
    taskENTER_CRITICAL(&s_wifi_lock);
    if (ssid != NULL && ssid_size > 0) {
        strlcpy(ssid, s_wifi_saved_ssid, ssid_size);
    }
    if (password != NULL && password_size > 0) {
        strlcpy(password, s_wifi_saved_password, password_size);
    }
    taskEXIT_CRITICAL(&s_wifi_lock);
}
