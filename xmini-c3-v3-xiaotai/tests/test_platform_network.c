// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../components/demo_platform/platform_network.c"

const char s_wifi_event_base = 0, s_ip_event_base = 0;
static unsigned s_connects, s_changes, s_writes, s_erases, s_disconnect_logs, s_offline_logs;
static bool s_ready, s_exclusive_ready, s_timer_active;
static esp_err_t s_connect_error, s_commit_error, s_open_error;
static struct { bool wifi, identity, flag; wifi_saved_t credentials; platform_identity_t id; } s_saved, s_staged;
static struct esp_timer { esp_timer_cb_t callback; } s_timer;
static uint64_t s_timer_us;
static int64_t s_now_us;
static uint32_t s_random;
static char s_log[256];

const char *esp_err_to_name(esp_err_t error) { (void)error; return "mock"; }
void test_network_log(char level, const char *tag, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(s_log, sizeof(s_log), format, args);
    va_end(args);
    assert(!strcmp(tag, "network") && !strstr(s_log, "test-old") && !strstr(s_log, "test-password"));
    if (strstr(s_log, "Wi-Fi disconnected:")) { assert(level == 'W'); s_disconnect_logs++; }
    if (strstr(s_log, "Wi-Fi offline:")) { assert(level == 'E'); s_offline_logs++; }
}
esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *handle)
{
    assert(args->callback == backoff_expired && args->dispatch_method == ESP_TIMER_TASK && !s_timer.callback);
    s_timer.callback = args->callback; *handle = &s_timer; return ESP_OK;
}
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us)
{
    assert(timer == &s_timer);
    if (s_timer_active) return ESP_ERR_INVALID_STATE;
    s_timer_active = true; s_timer_us = timeout_us; return ESP_OK;
}
esp_err_t esp_timer_stop(esp_timer_handle_t timer)
{
    assert(timer == &s_timer);
    if (!s_timer_active) return ESP_ERR_INVALID_STATE;
    s_timer_active = false; return ESP_OK;
}
int64_t esp_timer_get_time(void) { return s_now_us; }
uint32_t esp_random(void) { return s_random; }
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap)
{ memcpy(ap->ssid, "test-old", 9); memset(ap->bssid, 0xa5, sizeof(ap->bssid)); ap->primary = 6; ap->rssi = -55; return ESP_OK; }
void platform_set_phase(platform_phase_t phase, esp_err_t error, const char *code, int64_t expiry)
{ assert(phase == PLATFORM_PHASE_CONNECTING && error == ESP_OK && !code && !expiry); }
void platform_network_changed(bool ready) { s_ready = ready; s_changes++; }
bool platform_http_exclusive_ready(uint32_t ticket) { return s_exclusive_ready && ticket == 7; }
EventGroupHandle_t xEventGroupCreateStatic(StaticEventGroup_t *storage) { storage->bits = 0; return storage; }
EventBits_t xEventGroupGetBits(EventGroupHandle_t group) { return group->bits; }
EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits) { return group->bits |= bits; }
EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits)
{ EventBits_t previous = group->bits; group->bits &= ~bits; return previous; }
esp_err_t esp_event_loop_create_default(void) { return ESP_OK; }
esp_err_t esp_event_handler_register(esp_event_base_t base, int32_t id,
    void (*handler)(void *, esp_event_base_t, int32_t, void *), void *context)
{ assert((base == WIFI_EVENT || base == IP_EVENT) && id && handler == network_event && !context); return ESP_OK; }
esp_err_t esp_netif_init(void) { return ESP_OK; }
void *esp_netif_create_default_wifi_sta(void) { return (void *)1; }
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config) { assert(config); return ESP_OK; }
esp_err_t esp_netif_sntp_sync_wait(TickType_t ticks) { assert(ticks == 20000); return ESP_OK; }
void esp_netif_sntp_deinit(void) {}
esp_err_t esp_wifi_connect(void) { s_connects++; return s_connect_error; }
esp_err_t esp_wifi_init(const wifi_init_config_t *config) { assert(config->nvs_enable == 0); return ESP_OK; }
esp_err_t esp_wifi_set_storage(int storage) { assert(storage == WIFI_STORAGE_RAM); return ESP_OK; }
esp_err_t esp_wifi_set_mode(int mode) { assert(mode == WIFI_MODE_STA); return ESP_OK; }
esp_err_t esp_wifi_set_config(int interface, const wifi_config_t *config)
{ assert(interface == WIFI_IF_STA && config->sta.pmf_cfg.capable); return ESP_OK; }
esp_err_t esp_wifi_set_ps(int mode) { assert(mode == WIFI_PS_NONE); return ESP_OK; }
esp_err_t esp_wifi_start(void) { return ESP_OK; }
esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    if (s_open_error) return s_open_error;
    if (!strcmp(name, "nvs.net80211")) { assert(mode == NVS_READONLY); *handle = 2; }
    else { assert(!strcmp(name, "xmini")); *handle = 1; }
    s_staged = s_saved;
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *size)
{
    assert(handle == 1);
    const void *source;
    size_t length;
    bool found;
    if (!strcmp(key, "wifi")) { source = &s_saved.credentials; length = sizeof(s_saved.credentials); found = s_saved.wifi; }
    else { assert(!strcmp(key, "identity")); source = &s_saved.id; length = sizeof(s_saved.id); found = s_saved.identity; }
    if (!found) return ESP_ERR_NVS_NOT_FOUND;
    if (*size < length) return ESP_ERR_NVS_INVALID_LENGTH;
    *size = length; memcpy(value, source, length); return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t size)
{
    assert(handle == 1); s_writes++;
    if (!strcmp(key, "wifi")) { assert(size == sizeof(s_staged.credentials)); memcpy(&s_staged.credentials, value, size); s_staged.wifi = true; }
    else { assert(!strcmp(key, "identity") && size == sizeof(s_staged.id)); memcpy(&s_staged.id, value, size); s_staged.identity = true; }
    return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value)
{
    if (handle == 2) { assert(!strcmp(key, "ap.sndchan")); *value = 0; return ESP_OK; }
    assert(handle == 1 && !strcmp(key, "wifi_provision"));
    if (!s_saved.flag) return ESP_ERR_NVS_NOT_FOUND;
    *value = 1; return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{ assert(handle == 1 && !strcmp(key, "wifi_provision") && value == 1); s_staged.flag = true; s_writes++; return ESP_OK; }
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    assert(handle == 1); s_erases++;
    bool found;
    if (!strcmp(key, "wifi_provision")) { found = s_staged.flag; s_staged.flag = false; }
    else { assert(!strcmp(key, "identity")); found = s_staged.identity; s_staged.identity = false; }
    return found ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_commit(nvs_handle_t handle)
{ assert(handle == 1); if (!s_commit_error) s_saved = s_staged; return s_commit_error; }
void nvs_close(nvs_handle_t handle) { assert(handle == 1 || handle == 2); }

static void fire(void) { assert(s_timer_active); s_timer_active = false; s_timer.callback(NULL); }
static void disconnect(uint8_t reason, int8_t rssi)
{
    wifi_event_sta_disconnected_t event = {.ssid = "test-old", .ssid_len = 8, .bssid = {0xa5}, .reason = reason, .rssi = rssi};
    s_now_us += 1000000;
    network_event(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &event);
}
static platform_network_diag_t diag(void)
{ platform_network_diag_t value; assert(platform_network_diag(&value) == ESP_OK); return value; }
static void expect_backoff(uint32_t base)
{
    platform_network_diag_t value = diag();
    assert(s_timer_active && s_timer_us == (uint64_t)value.backoff_ms * 1000);
    assert(value.backoff_ms == base + s_random % (base / 4 + 1) && value.backoff_ms <= base + base / 4);
}

int main(void)
{
    platform_network_diag_t d;
    assert(platform_network_diag(NULL) == ESP_ERR_INVALID_ARG);
    assert(platform_network_diag(&d) == ESP_ERR_INVALID_STATE && !d.connected && !d.disconnects && !d.history_count);
    assert(platform_network_start("test-old", "test-password") == ESP_OK && s_timer.callback);
    assert(!platform_network_ready() && !platform_network_error() && !s_writes);
    network_event(NULL, WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
    assert(s_connects == 1);
    disconnect(WIFI_REASON_NO_AP_FOUND, -90);
    assert(strstr(s_log, "reason=201 rssi=-90 attempt=1 uptime_ms=1000"));
    disconnect(WIFI_REASON_AUTH_EXPIRE, -85);
    disconnect(WIFI_REASON_AUTH_EXPIRE, -80);
    d = diag();
    assert(s_connects == 4 && !platform_network_error() && !s_timer_active && !s_ready && s_changes == 3);
    assert(d.attempts == 3 && !d.backoff_ms && d.first_reason == 201 && d.first_rssi == -90 && d.first_uptime_ms == 1000);
    assert(d.last_reason == WIFI_REASON_AUTH_EXPIRE && d.last_rssi == -80 && !s_offline_logs);

    disconnect(WIFI_REASON_BEACON_TIMEOUT, -75);
    d = diag();
    assert(s_connects == 4 && platform_network_error() == ESP_ERR_WIFI_NOT_CONNECT && !platform_network_ready());
    assert(!s_ready && s_changes == 4 && s_offline_logs == 1 && s_timer_us == 2000000);
    expect_backoff(2000);
    assert(d.attempts == 4 && d.beacon_timeouts == 1 && d.first_reason == 201 && d.last_reason == 200 && d.last_rssi == -75);
    assert(!d.connected && d.error == ESP_ERR_WIFI_NOT_CONNECT);
    fire();
    assert(s_connects == 5 && platform_network_error() == ESP_ERR_WIFI_NOT_CONNECT && !diag().backoff_ms);
    s_random = UINT32_MAX;
    static const uint32_t doubling[] = {4000, 8000, 16000, 32000, 60000, 60000};
    for (unsigned i = 0; i < 6; ++i) {
        disconnect(WIFI_REASON_NO_AP_FOUND, -88);
        expect_backoff(doubling[i]);
        assert(diag().backoff_ms > doubling[i]);
        fire();
        assert(s_connects == 6 + i && platform_network_error() == ESP_ERR_WIFI_NOT_CONNECT);
    }
    d = diag();
    assert(d.attempts == 10 && d.disconnects == 10 && d.first_reason == 201 && d.first_uptime_ms == 1000);
    assert(d.history_count == 8 && d.history[0].reason == WIFI_REASON_AUTH_EXPIRE && d.history[0].rssi == -80);
    assert(d.history[0].uptime_ms == 3000 && d.history[1].reason == 200 && d.history[1].uptime_ms == 4000);
    assert(d.history[7].reason == 201 && d.history[7].rssi == -88 && d.history[7].uptime_ms == 10000);

    disconnect(WIFI_REASON_NO_AP_FOUND, -88);
    expect_backoff(60000);
    s_connect_error = ESP_ERR_WIFI_STATE;
    fire();
    assert(s_connects == 12 && platform_network_error() == ESP_ERR_WIFI_NOT_CONNECT);
    expect_backoff(60000);
    s_connect_error = ESP_OK;
    fire();
    assert(s_connects == 13 && !s_timer_active);
    static const uint32_t credential[] = {120000, 240000, 300000, 300000};
    for (unsigned i = 0; i < 4; ++i) {
        disconnect(i & 1 ? WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT : WIFI_REASON_AUTH_FAIL, -60);
        expect_backoff(credential[i]);
        fire();
    }
    assert(s_connects == 17);
    disconnect(WIFI_REASON_NO_AP_FOUND, -88);
    expect_backoff(60000);

    assert(platform_network_retry() == ESP_OK && s_connects == 18 && !s_timer_active && !platform_network_error());
    assert(!diag().backoff_ms);
    for (unsigned i = 0; i < 3; ++i) disconnect(WIFI_REASON_NO_AP_FOUND, -88);
    assert(s_connects == 21 && !s_timer_active && !platform_network_error());
    s_random = 0;
    disconnect(WIFI_REASON_NO_AP_FOUND, -88);
    expect_backoff(2000);
    d = diag();
    assert(s_connects == 21 && s_timer_us == 2000000 && s_offline_logs == 2);
    assert(d.attempts == 20 && d.first_reason == 201 && d.first_uptime_ms == 1000);

    unsigned changes = s_changes;
    network_event(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    d = diag();
    assert(platform_network_ready() && s_ready && s_changes == changes + 1 && !platform_network_error() && !s_timer_active);
    assert(strstr(s_log, "rssi=-55 channel=6 outage_disconnects=20"));
    assert(d.connected && !d.attempts && !d.backoff_ms && !d.error && d.disconnects == 20 && d.history_count == 8);
    assert(d.first_reason == 201);
    backoff_expired(NULL);
    assert(s_connects == 21);
    disconnect(WIFI_REASON_BEACON_TIMEOUT, -82);
    d = diag();
    assert(!d.connected && d.attempts == 1 && d.first_reason == 200 && d.first_rssi == -82 && d.first_uptime_ms == 21000);
    assert(d.disconnects == 21 && d.beacon_timeouts == 2 && s_connects == 22 && !s_timer_active && !platform_network_error());
    for (unsigned i = 0; i < 3; ++i) disconnect(WIFI_REASON_NO_AP_FOUND, -88);
    assert(s_connects == 24 && s_offline_logs == 3);
    expect_backoff(2000);
    assert(diag().first_reason == 200 && diag().last_reason == 201);

    network_event(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    assert(platform_network_ready() && !s_timer_active);
    s_connect_error = ESP_ERR_NO_MEM;
    disconnect(WIFI_REASON_AUTH_EXPIRE, -70);
    assert(!platform_network_ready() && platform_network_error() == ESP_ERR_NO_MEM && s_connects == 25);
    expect_backoff(2000);
    assert(platform_network_retry() == ESP_ERR_NO_MEM && platform_network_error() == ESP_ERR_NO_MEM && s_connects == 26);
    expect_backoff(2000);
    s_connect_error = ESP_OK;
    assert(platform_network_retry() == ESP_OK && !platform_network_error() && !s_timer_active && s_connects == 27);

    for (unsigned i = 0; i < 4; ++i) disconnect(WIFI_REASON_NO_AP_FOUND, -88);
    assert(s_connects == 30 && s_offline_logs == 4);
    expect_backoff(2000);
    network_event(NULL, WIFI_EVENT, WIFI_EVENT_STA_STOP, NULL);
    d = diag();
    assert(!s_timer_active && d.stopped && !d.backoff_ms);
    backoff_expired(NULL);
    disconnect(WIFI_REASON_ASSOC_LEAVE, -88);
    assert(s_connects == 30 && !s_timer_active && !diag().backoff_ms);
    network_event(NULL, WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
    assert(s_connects == 31 && !diag().stopped);
    disconnect(WIFI_REASON_NO_AP_FOUND, -88);
    expect_backoff(4000);
    s_connect_error = ESP_ERR_WIFI_NOT_STARTED;
    fire();
    assert(s_connects == 32 && !s_timer_active && !diag().backoff_ms);
    s_connect_error = ESP_OK;
    d = diag();
    assert(d.disconnects == 31 && s_disconnect_logs == d.disconnects && s_offline_logs == 4);

    assert(platform_save_wifi("test-old", "test-password") == ESP_OK);
    platform_identity_t id = {.device_id = "test-device", .device_key = "not-a-real-key"};
    assert(platform_identity_save(&id) == ESP_OK);
    unsigned writes = s_writes, erases = s_erases;
    assert(platform_request_wifi_provisioning(7) == ESP_ERR_INVALID_STATE);
    assert(s_writes == writes && s_erases == erases);
    s_exclusive_ready = true;
    assert(platform_request_wifi_provisioning(6) == ESP_ERR_INVALID_STATE);
    s_commit_error = ESP_FAIL;
    assert(platform_request_wifi_provisioning(7) == ESP_FAIL && !platform_wifi_provisioning_requested());
    s_commit_error = ESP_OK;
    assert(platform_request_wifi_provisioning(7) == ESP_OK && platform_wifi_provisioning_requested());
    assert(s_erases == erases && !strcmp(s_saved.credentials.ssid, "test-old"));
    assert(!memcmp(&s_saved.id, &id, sizeof(id)));
    s_commit_error = ESP_FAIL;
    assert(platform_save_wifi("test-new", "test-new-password") == ESP_FAIL);
    assert(platform_wifi_provisioning_requested() && !strcmp(s_saved.credentials.ssid, "test-old"));
    s_commit_error = ESP_OK;
    assert(platform_save_wifi("test-new", "test-new-password") == ESP_OK);
    assert(!platform_wifi_provisioning_requested() && !memcmp(&s_saved.id, &id, sizeof(id)));
    char ssid[33], password[65];
    assert(platform_load_wifi(ssid, sizeof(ssid), password, sizeof(password)) && !strcmp(ssid, "test-new"));
    s_open_error = ESP_ERR_NO_MEM;
    assert(platform_request_wifi_provisioning(7) == ESP_ERR_NO_MEM);
    assert(!strcmp(s_saved.credentials.ssid, "test-new") && !memcmp(&s_saved.id, &id, sizeof(id)));
    puts("platform network real source: fast reconnect/backoff/cap/stop safety, outage diagnostics, NVS-disabled init, ticket/flag/commit preservation PASS");
    return 0;
}
