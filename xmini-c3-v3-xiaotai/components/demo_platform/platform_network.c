#include "platform_internal.h"
#include <string.h>
#include <time.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs.h"
#define WIFI_FAST_RECONNECTS 3U
#define WIFI_BACKOFF_FIRST_MS 2000U
#define WIFI_BACKOFF_CAP_MS 60000U
#define WIFI_BACKOFF_CREDENTIAL_CAP_MS 300000U
static EventGroupHandle_t s_network;
static StaticEventGroup_t s_network_storage;
static esp_timer_handle_t s_backoff;
static portMUX_TYPE s_wifi_lock = portMUX_INITIALIZER_UNLOCKED;
/* history is a raw ring at (disconnects - 1) % size; diag() returns it ordered. */
static platform_network_diag_t s_diag;
static unsigned s_fast;
static uint32_t s_backoff_base_ms;
static atomic_int s_network_error;
static bool s_time_synced;

static void log_legacy_wifi_metadata(const char *stage)
{
    nvs_handle_t handle;
    uint8_t secondary = 0;
    esp_err_t err = nvs_open("nvs.net80211", NVS_READONLY, &handle);
    if (err == ESP_OK) {
        err = nvs_get_u8(handle, "ap.sndchan", &secondary);
        nvs_close(handle);
    }
    ESP_LOGI("network", "legacy Wi-Fi metadata: stage=%s result=%s ap_secondary=%u",
             stage, esp_err_to_name(err), (unsigned)secondary);
}
static bool wifi_stopped(esp_err_t err)
{
    return err == ESP_ERR_WIFI_NOT_INIT || err == ESP_ERR_WIFI_NOT_STARTED || err == ESP_ERR_WIFI_STOP_STATE;
}
static bool credential_reason(unsigned reason)
{
    return reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || reason == WIFI_REASON_802_1X_AUTH_FAILED ||
           reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_HANDSHAKE_TIMEOUT ||
           reason == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY ||
           reason == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD;
}
static void backoff_cancel(void)
{
    portENTER_CRITICAL(&s_wifi_lock);
    s_diag.backoff_ms = 0;
    portEXIT_CRITICAL(&s_wifi_lock);
    if (s_backoff) esp_timer_stop(s_backoff);
}
static void backoff_schedule(void)
{
    uint32_t jitter = esp_random(), delay = 0, cap = 0;
    portENTER_CRITICAL(&s_wifi_lock);
    unsigned attempts = s_diag.attempts;
    if (s_backoff && !s_diag.stopped) {
        cap = credential_reason(s_diag.last_reason) ? WIFI_BACKOFF_CREDENTIAL_CAP_MS : WIFI_BACKOFF_CAP_MS;
        uint32_t base = s_backoff_base_ms ? s_backoff_base_ms * 2 : WIFI_BACKOFF_FIRST_MS;
        s_backoff_base_ms = base > cap ? cap : base;
        delay = s_backoff_base_ms + jitter % (s_backoff_base_ms / 4 + 1);
    }
    s_diag.backoff_ms = delay;
    portEXIT_CRITICAL(&s_wifi_lock);
    if (!delay) return;
    esp_timer_stop(s_backoff);
    esp_err_t err = esp_timer_start_once(s_backoff, (uint64_t)delay * 1000);
    if (err == ESP_OK) {
        ESP_LOGW("network", "Wi-Fi reconnect scheduled: delay_ms=%lu cap_ms=%lu attempt=%u",
                 (unsigned long)delay, (unsigned long)cap, attempts);
    } else {
        backoff_cancel();
        ESP_LOGE("network", "Wi-Fi backoff timer failed: %s; explicit retry required", esp_err_to_name(err));
    }
}
/* esp_timer task: the only caller of delayed connects. */
static void backoff_expired(void *arg)
{
    (void)arg;
    portENTER_CRITICAL(&s_wifi_lock);
    bool due = s_diag.backoff_ms && !s_diag.stopped;
    s_diag.backoff_ms = 0;
    unsigned attempts = s_diag.attempts;
    portEXIT_CRITICAL(&s_wifi_lock);
    if (!due || platform_network_ready()) return;
    esp_err_t err = esp_wifi_connect();
    if (err == ESP_OK) return;
    ESP_LOGW("network", "Wi-Fi backoff connect failed: %s attempt=%u", esp_err_to_name(err), attempts);
    if (!wifi_stopped(err)) backoff_schedule();
}
static void network_disconnected(const wifi_event_sta_disconnected_t *event)
{
    uint8_t reason = event ? event->reason : 0;
    int8_t rssi = event ? event->rssi : 0;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    xEventGroupClearBits(s_network, 1);
    portENTER_CRITICAL(&s_wifi_lock);
    if (!s_diag.attempts) {
        s_diag.first_reason = reason;
        s_diag.first_rssi = rssi;
        s_diag.first_uptime_ms = now;
    }
    unsigned attempt = ++s_diag.attempts;
    s_diag.last_reason = reason;
    s_diag.last_rssi = rssi;
    s_diag.history[s_diag.disconnects++ % PLATFORM_NETWORK_HISTORY] =
        (platform_network_disconnect_t){.reason = reason, .rssi = rssi, .uptime_ms = now};
    if (s_diag.history_count < PLATFORM_NETWORK_HISTORY) ++s_diag.history_count;
    if (reason == WIFI_REASON_BEACON_TIMEOUT) ++s_diag.beacon_timeouts;
    bool fast = s_fast < WIFI_FAST_RECONNECTS, exhausted = s_fast == WIFI_FAST_RECONNECTS;
    if (s_fast <= WIFI_FAST_RECONNECTS) ++s_fast;
    uint8_t first_reason = s_diag.first_reason;
    portEXIT_CRITICAL(&s_wifi_lock);
    ESP_LOGW("network", "Wi-Fi disconnected: reason=%u rssi=%d attempt=%u uptime_ms=%lu",
             (unsigned)reason, (int)rssi, attempt, (unsigned long)now);
    esp_err_t err = ESP_ERR_WIFI_NOT_CONNECT;
    if (fast && (err = esp_wifi_connect()) != ESP_OK)
        ESP_LOGE("network", "reconnect failed: %s", esp_err_to_name(err));
    if (exhausted)
        ESP_LOGE("network", "Wi-Fi offline: first_reason=%u last_reason=%u; background reconnect continues, "
                 "correct network or explicitly retry", (unsigned)first_reason, (unsigned)reason);
    if (err != ESP_OK && !wifi_stopped(err)) backoff_schedule();
    atomic_store(&s_network_error, err);
    platform_network_changed(false);
}
static void network_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        portENTER_CRITICAL(&s_wifi_lock);
        s_diag.stopped = false;
        portEXIT_CRITICAL(&s_wifi_lock);
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            atomic_store(&s_network_error, err);
            platform_network_changed(false);
            ESP_LOGE("network", "initial connect failed: %s", esp_err_to_name(err));
            if (!wifi_stopped(err)) backoff_schedule();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_STOP) {
        portENTER_CRITICAL(&s_wifi_lock);
        s_diag.stopped = true;
        portEXIT_CRITICAL(&s_wifi_lock);
        backoff_cancel();
        ESP_LOGI("network", "Wi-Fi station stopped; reconnect suspended");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        network_disconnected(data);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        backoff_cancel();
        portENTER_CRITICAL(&s_wifi_lock);
        unsigned attempts = s_diag.attempts;
        s_diag.attempts = 0;
        s_fast = 0;
        s_backoff_base_ms = 0;
        portEXIT_CRITICAL(&s_wifi_lock);
        atomic_store(&s_network_error, ESP_OK);
        xEventGroupSetBits(s_network, 1);
        platform_network_changed(true);
        wifi_ap_record_t ap = {0};
        esp_err_t err = esp_wifi_sta_get_ap_info(&ap);
        ESP_LOGI("network", "Wi-Fi IP ready: rssi=%d channel=%u outage_disconnects=%u ap_info=%s",
                 (int)ap.rssi, (unsigned)ap.primary, attempts, esp_err_to_name(err));
    }
}
esp_err_t platform_network_start(const char *ssid, const char *password)
{
    platform_set_phase(PLATFORM_PHASE_CONNECTING, ESP_OK, NULL, 0);
    atomic_store(&s_network_error, ESP_OK);
    s_network = xEventGroupCreateStatic(&s_network_storage);
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK)
        return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK)
        return err;
    if (!esp_netif_create_default_wifi_sta())
        return ESP_ERR_NO_MEM;
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    /* init() loads and normalizes legacy Wi-Fi NVS before set_storage(RAM).
     * This product owns its credentials in xmini, never in nvs.net80211. */
    init.nvs_enable = 0;
    log_legacy_wifi_metadata("before_init");
    if ((err = esp_wifi_init(&init)) != ESP_OK)
        return err;
    log_legacy_wifi_metadata("after_init");
    const esp_timer_create_args_t backoff = {
        .callback = backoff_expired, .dispatch_method = ESP_TIMER_TASK, .name = "wifi_backoff"};
    if (!s_backoff && (err = esp_timer_create(&backoff, &s_backoff)) != ESP_OK)
        return err;
    if ((err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, network_event, NULL)) != ESP_OK ||
        (err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, network_event, NULL)) != ESP_OK)
        return err;
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, ssid, strlen(ssid));
    memcpy(config.sta.password, password, strlen(password));
    config.sta.pmf_cfg.capable = true;
    if ((err = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK ||
        (err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK ||
        (err = esp_wifi_set_config(WIFI_IF_STA, &config)) != ESP_OK ||
        (err = esp_wifi_set_ps(WIFI_PS_NONE)) != ESP_OK)
        return err;
    return esp_wifi_start();
}
esp_err_t platform_network_retry(void)
{
    backoff_cancel();
    portENTER_CRITICAL(&s_wifi_lock);
    s_fast = 0;
    s_backoff_base_ms = 0;
    portEXIT_CRITICAL(&s_wifi_lock);
    atomic_store(&s_network_error, ESP_OK);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) atomic_store(&s_network_error, err);
    platform_network_changed(false);
    if (err != ESP_OK) {
        ESP_LOGE("network", "explicit retry failed: %s", esp_err_to_name(err));
        if (!wifi_stopped(err)) backoff_schedule();
    }
    return err;
}
bool platform_network_ready(void)
{
    return s_network && (xEventGroupGetBits(s_network) & 1);
}
esp_err_t platform_network_error(void) { return atomic_load(&s_network_error); }
esp_err_t platform_network_diag(platform_network_diag_t *diag)
{
    if (!diag) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_wifi_lock);
    *diag = s_diag;
    for (unsigned i = 0; i < diag->history_count; ++i)
        diag->history[i] = s_diag.history[(s_diag.disconnects - diag->history_count + i) % PLATFORM_NETWORK_HISTORY];
    portEXIT_CRITICAL(&s_wifi_lock);
    diag->connected = platform_network_ready();
    diag->error = atomic_load(&s_network_error);
    return s_network ? ESP_OK : ESP_ERR_INVALID_STATE;
}
esp_err_t platform_time_sync(void)
{
    if (s_time_synced) return ESP_OK;
    int64_t previous = (int64_t)time(NULL);
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        2, ESP_SNTP_SERVER_LIST("ntp.aliyun.com", "pool.ntp.org"));
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK)
        return err;
    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(20000));
    s_time_synced = err == ESP_OK;
    ESP_LOGI("network", "time source=SNTP previous_utc=%lld utc=%lld result=%s",
             (long long)previous, (long long)time(NULL), esp_err_to_name(err));
    esp_netif_sntp_deinit();
    return err;
}
esp_err_t platform_identity_load(platform_identity_t *id)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("xmini", NVS_READONLY, &nvs);
    if (err != ESP_OK)
        return err;
    size_t size = sizeof(*id);
    err = nvs_get_blob(nvs, "identity", id, &size);
    nvs_close(nvs);
    if (err == ESP_OK && (size != sizeof(*id) || !memchr(id->device_id, 0, sizeof(id->device_id)) ||
                          !memchr(id->device_key, 0, sizeof(id->device_key)) || !id->device_id[0] ||
                          !id->device_key[0]))
        err = ESP_ERR_INVALID_STATE;
    return err;
}
esp_err_t platform_identity_save(const platform_identity_t *id)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("xmini", NVS_READWRITE, &nvs);
    if (err != ESP_OK)
        return err;
    err = nvs_set_blob(nvs, "identity", id, sizeof(*id));
    if (err == ESP_OK)
        err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}
esp_err_t platform_identity_clear_local(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("xmini", NVS_READWRITE, &nvs);
    if (err != ESP_OK)
        return err;
    err = nvs_erase_key(nvs, "identity");
    if (err == ESP_ERR_NVS_NOT_FOUND)
        err = ESP_OK;
    else if (err == ESP_OK)
        err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}
typedef struct {
    char ssid[33], password[65];
} wifi_saved_t;

#define WIFI_NVS_NAMESPACE "xmini"
#define WIFI_NVS_KEY "wifi"
#define WIFI_PROVISION_NVS_KEY "wifi_provision"

esp_err_t platform_save_wifi(const char *ssid, const char *password)
{
    if (!ssid || !password || !ssid[0] || strlen(ssid) > 32 || strlen(password) > 64)
        return ESP_ERR_INVALID_ARG;
    wifi_saved_t value = {0};
    strcpy(value.ssid, ssid);
    strcpy(value.password, password);
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, WIFI_NVS_KEY, &value, sizeof(value));
        if (err == ESP_OK) {
            esp_err_t erase_err = nvs_erase_key(nvs, WIFI_PROVISION_NVS_KEY);
            if (erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND)
                err = erase_err;
        }
        if (err == ESP_OK)
            err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    memset(&value, 0, sizeof(value));
    return err;
}
bool platform_load_wifi(char *ssid, size_t ssid_size, char *password, size_t password_size)
{
    nvs_handle_t nvs;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK)
        return false;
    wifi_saved_t value = {0};
    size_t size = sizeof(value);
    esp_err_t err = nvs_get_blob(nvs, WIFI_NVS_KEY, &value, &size);
    nvs_close(nvs);
    bool ok = err == ESP_OK && size == sizeof(value) && memchr(value.ssid, 0, sizeof(value.ssid)) &&
              memchr(value.password, 0, sizeof(value.password)) && strlen(value.ssid) < ssid_size &&
              strlen(value.password) < password_size;
    if (ok) {
        strcpy(ssid, value.ssid);
        strcpy(password, value.password);
    }
    memset(&value, 0, sizeof(value));
    return ok;
}

esp_err_t platform_request_wifi_provisioning(uint32_t ticket)
{
    if (!platform_http_exclusive_ready(ticket)) return ESP_ERR_INVALID_STATE;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK)
        return err;

    err = nvs_set_u8(nvs, WIFI_PROVISION_NVS_KEY, 1U);
    if (err == ESP_OK)
        err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

bool platform_wifi_provisioning_requested(void)
{
    nvs_handle_t nvs;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK)
        return false;
    uint8_t requested = 0U;
    esp_err_t err = nvs_get_u8(nvs, WIFI_PROVISION_NVS_KEY, &requested);
    nvs_close(nvs);
    return err == ESP_OK && requested == 1U;
}
