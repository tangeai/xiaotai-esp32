/* Scan driver calls belong to the existing Wi-Fi event owner. HTTP only queues
 * an intent and reads a completed snapshot. Never scan during a STA connection;
 * retries wait at most one bounded scan, not a parallel stream of scans. */
#define PORTAL_SCAN_MAX 24U
#define PORTAL_SCAN_TIMEOUT_US 5000000LL
enum { PORTAL_SCAN_IDLE, PORTAL_SCAN_QUEUED, PORTAL_SCAN_RUNNING,
       PORTAL_SCAN_READY, PORTAL_SCAN_BUSY, PORTAL_SCAN_ERROR };
static atomic_int s_scan_state;
static bool s_scan_active;
static bool s_scan_cancel_pending;
static int64_t s_scan_started_us;
static EXT_RAM_BSS_ATTR wifi_ap_record_t s_scan_records[PORTAL_SCAN_MAX];
static uint16_t s_scan_count;
static void wifi_control_event(int32_t event_id);

static void portal_scan_cancel(int state)
{
    if (s_scan_active) {
        /* scan_stop also emits SCAN_DONE. Consume that completion before a new
         * explicit scan, otherwise its late event could complete the new scan. */
        s_scan_cancel_pending = true;
        esp_err_t err = esp_wifi_scan_stop();
        if (err != ESP_OK) ESP_LOGW(TAG, "Wi-Fi scan stop failed: %s", esp_err_to_name(err));
        err = esp_wifi_clear_ap_list();
        if (err != ESP_OK) ESP_LOGW(TAG, "Wi-Fi scan clear failed: %s", esp_err_to_name(err));
        s_scan_active = false;
    }
    atomic_store(&s_scan_state, state);
    if (s_retry_due_us == 0 && s_retry_timer != NULL) (void)esp_timer_stop(s_retry_timer);
}

static void portal_scan_begin(void)
{
    if (atomic_load(&s_scan_state) != PORTAL_SCAN_QUEUED) return;
    if (!atomic_load(&s_provisioning) || s_connected || s_connecting ||
        s_scan_active || s_scan_cancel_pending) {
        atomic_store(&s_scan_state, PORTAL_SCAN_BUSY);
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (s_scan_started_us && now - s_scan_started_us < PORTAL_SCAN_TIMEOUT_US) {
        atomic_store(&s_scan_state, PORTAL_SCAN_BUSY);
        return;
    }
    wifi_scan_config_t config = {
        .show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = {.min = 0, .max = 120},
    };
    s_scan_started_us = now;
    s_scan_count = 0;
    esp_err_t err = esp_wifi_scan_start(&config, false);
    if (err == ESP_OK) {
        s_scan_active = true;
        atomic_store(&s_scan_state, PORTAL_SCAN_RUNNING);
        /* Reuse the retry timer as a bounded scan watchdog; no extra RTOS task. */
        err = esp_timer_start_periodic(s_retry_timer, 1000000);
        if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) return;
        portal_scan_cancel(PORTAL_SCAN_ERROR);
    } else {
        atomic_store(&s_scan_state, err == ESP_ERR_WIFI_STATE ? PORTAL_SCAN_BUSY : PORTAL_SCAN_ERROR);
    }
    ESP_LOGW(TAG, "Wi-Fi scan start failed: %s", esp_err_to_name(err));
}

static void portal_scan_done(const wifi_event_sta_scan_done_t *event)
{
    if (s_scan_cancel_pending) {
        s_scan_cancel_pending = false;
        return;
    }
    if (!s_scan_active) return;
    uint16_t count = PORTAL_SCAN_MAX;
    esp_err_t err = event != NULL && event->status == 0 ?
        esp_wifi_scan_get_ap_records(&count, s_scan_records) : ESP_FAIL;
    if (err != ESP_OK) {
        (void)esp_wifi_clear_ap_list();
        count = 0;
        ESP_LOGW(TAG, "Wi-Fi scan failed: %s", esp_err_to_name(err));
    }
    s_scan_active = false;
    s_scan_count = count;
    /* Release publishes records/count. Only a later serialized HTTP POST can
     * start writing this snapshot again; readers never touch an in-flight scan. */
    atomic_store(&s_scan_state, err == ESP_OK ? PORTAL_SCAN_READY : PORTAL_SCAN_ERROR);
    if (s_retry_due_us == 0) (void)esp_timer_stop(s_retry_timer);
    else wifi_control_event(WIFI_CONTROL_RETRY);
}

static const char *portal_scan_status(int state)
{
    switch (state) {
    case PORTAL_SCAN_QUEUED: case PORTAL_SCAN_RUNNING: return "scanning";
    case PORTAL_SCAN_READY: return "ready";
    case PORTAL_SCAN_BUSY: return "busy";
    case PORTAL_SCAN_ERROR: return "error";
    default: return "idle";
    }
}

static esp_err_t portal_scan_post(httpd_req_t *request)
{
    if (!portal_socket_allowed(httpd_req_to_sockfd(request)))
        return httpd_resp_send_err(request, HTTPD_403_FORBIDDEN, "provisioning closed");
    if (request->content_len != 0)
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "scan body must be empty");
    int state = atomic_load(&s_scan_state);
    if (state != PORTAL_SCAN_RUNNING && state != PORTAL_SCAN_QUEUED &&
        atomic_compare_exchange_strong(&s_scan_state, &state, PORTAL_SCAN_QUEUED)) {
        esp_err_t err = esp_event_post(WIFI_MANAGER_CONTROL, WIFI_CONTROL_SCAN, NULL, 0, 0);
        if (err != ESP_OK) {
            atomic_store(&s_scan_state, PORTAL_SCAN_ERROR);
            return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "scan queue full");
        }
    }
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, "{\"accepted\":true}");
}

static bool portal_decode_ssid(const char *hex, char ssid[33])
{
    size_t length = strlen(hex);
    if (!length || length > 64U || (length & 1U)) return false;
    for (size_t i = 0; i < length / 2; ++i) {
        unsigned value = 0;
        for (unsigned n = 0; n < 2; ++n) {
            char c = hex[i * 2 + n];
            unsigned digit = c >= '0' && c <= '9' ? (unsigned)(c - '0') :
                c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10) :
                c >= 'A' && c <= 'F' ? (unsigned)(c - 'A' + 10) : 16U;
            if (digit >= 16U) return false;
            value = value * 16U + digit;
        }
        if (value == 0) return false;
        ssid[i] = (char)value;
    }
    ssid[length / 2] = 0;
    return true;
}

static bool portal_auth_supported(wifi_auth_mode_t auth)
{
    return auth == WIFI_AUTH_OPEN || auth == WIFI_AUTH_WPA2_PSK ||
        auth == WIFI_AUTH_WPA_WPA2_PSK || auth == WIFI_AUTH_WPA3_PSK ||
        auth == WIFI_AUTH_WPA2_WPA3_PSK;
}

static bool portal_saved(const wifi_history_t *history, const char *ssid, bool secure)
{
    for (unsigned i = 0; i < history->count; ++i)
        if (!strcmp(ssid, history->entries[i].ssid) &&
            (history->entries[i].password[0] != 0) == secure) return true;
    return false;
}

static esp_err_t portal_send_network(httpd_req_t *request, const char *ssid,
                                    int rssi, bool secure, bool saved, bool available,
                                    bool supported, bool *first)
{
    char hex[65], json[512];
    static const char digits[] = "0123456789abcdef";
    size_t length = strlen(ssid);
    for (size_t i = 0; i < length; ++i) {
        unsigned byte = (unsigned char)ssid[i];
        hex[i * 2] = digits[byte >> 4]; hex[i * 2 + 1] = digits[byte & 15];
    }
    hex[length * 2] = 0;
    /* Caller-owned cJSON nodes + PrintPreallocated keep all row scratch on
     * HTTPD's existing PSRAM stack. No global hooks or per-network heap nodes. */
    cJSON fields[] = {
        {.type = cJSON_String, .string = "ssid", .valuestring = (char *)ssid},
        {.type = cJSON_String, .string = "ssid_hex", .valuestring = hex},
        {.type = available ? cJSON_Number : cJSON_NULL, .string = "rssi", .valuedouble = rssi},
        {.type = secure ? cJSON_True : cJSON_False, .string = "secure"},
        {.type = saved ? cJSON_True : cJSON_False, .string = "saved"},
        {.type = available ? cJSON_True : cJSON_False, .string = "available"},
        {.type = supported ? cJSON_True : cJSON_False, .string = "supported"},
    };
    for (unsigned i = 0; i + 1 < sizeof(fields) / sizeof(fields[0]); ++i) fields[i].next = &fields[i + 1];
    cJSON object = {.type = cJSON_Object, .child = fields};
    if (!cJSON_PrintPreallocated(&object, json, sizeof(json), false)) return ESP_ERR_NO_MEM;
    if (!*first && httpd_resp_send_chunk(request, ",", 1) != ESP_OK) return ESP_FAIL;
    *first = false;
    return httpd_resp_sendstr_chunk(request, json);
}

static esp_err_t portal_networks_get(httpd_req_t *request)
{
    if (!portal_socket_allowed(httpd_req_to_sockfd(request)))
        return httpd_resp_send_err(request, HTTPD_403_FORBIDDEN, "provisioning closed");
    wifi_history_t history;
    esp_err_t history_err = wifi_history_load(&history);
    if (!portal_socket_allowed(httpd_req_to_sockfd(request))) {
        memset(&history, 0, sizeof(history));
        return httpd_resp_send_err(request, HTTPD_403_FORBIDDEN, "provisioning closed");
    }
    int state = atomic_load(&s_scan_state);
    uint16_t count = state == PORTAL_SCAN_READY ? s_scan_count : 0;
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    char header[96];
    snprintf(header, sizeof(header), "{\"state\":\"%s\",\"history_error\":%s,\"networks\":[",
             portal_scan_status(state), history_err == ESP_OK ? "false" : "true");
    esp_err_t err = httpd_resp_sendstr_chunk(request, header);
    bool first = true;
    for (unsigned i = 0; i < count && err == ESP_OK; ++i) {
        const wifi_ap_record_t *ap = &s_scan_records[i];
        if (!ap->ssid[0]) continue;
        bool duplicate = false;
        for (unsigned j = 0; j < i; ++j)
            if (!strncmp((const char *)ap->ssid, (const char *)s_scan_records[j].ssid, 33) &&
                (ap->authmode == WIFI_AUTH_OPEN) == (s_scan_records[j].authmode == WIFI_AUTH_OPEN) &&
                portal_auth_supported(ap->authmode) == portal_auth_supported(s_scan_records[j].authmode)) duplicate = true;
        if (duplicate) continue;
        char ssid[33]; memcpy(ssid, ap->ssid, 32); ssid[32] = 0;
        bool secure = ap->authmode != WIFI_AUTH_OPEN;
        err = portal_send_network(request, ssid, ap->rssi, secure,
            portal_saved(&history, ssid, secure), true, portal_auth_supported(ap->authmode), &first);
    }
    for (unsigned i = 0; i < history.count && err == ESP_OK; ++i) {
        const wifi_manager_credentials_t *entry = &history.entries[i];
        bool found = false;
        for (unsigned j = 0; j < count; ++j)
            if (!strncmp(entry->ssid, (const char *)s_scan_records[j].ssid, 33) &&
                (entry->password[0] == 0) == (s_scan_records[j].authmode == WIFI_AUTH_OPEN)) found = true;
        if (!found) err = portal_send_network(request, entry->ssid, 0,
            entry->password[0] != 0, true, false, true, &first);
    }
    memset(&history, 0, sizeof(history));
    if (err == ESP_OK) err = httpd_resp_sendstr_chunk(request, "]}");
    if (err == ESP_OK) err = httpd_resp_send_chunk(request, NULL, 0);
    return err;
}
