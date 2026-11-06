/* Private Wi-Fi history, separate from the authoritative next-boot credential.
 * All NVS calls below run on nvs_worker. One bounded blob fits its existing slot;
 * changing the selected network cannot destroy previously working credentials. */
#define WIFI_HISTORY_KEY "known"
#define WIFI_HISTORY_VERSION 1U
#define WIFI_HISTORY_MAX 5U
typedef struct {
    uint8_t version, count;
    wifi_manager_credentials_t entries[WIFI_HISTORY_MAX];
} wifi_history_t;
_Static_assert(sizeof(wifi_history_t) == 492, "Wi-Fi history layout changed");
_Static_assert(sizeof(wifi_history_t) <= NVS_WORKER_DATA_BYTES, "Wi-Fi history exceeds NVS slot");
/* Only the serial NVS worker uses this scratch; do not spend its internal stack
 * on a second full history or allocate from the internal heap at every IP event. */
static EXT_RAM_BSS_ATTR wifi_history_t s_history_work;

static bool wifi_history_valid(const wifi_history_t *history)
{
    if (history->version != WIFI_HISTORY_VERSION || history->count > WIFI_HISTORY_MAX) return false;
    for (unsigned i = 0; i < history->count; ++i) {
        const wifi_manager_credentials_t *entry = &history->entries[i];
        if (!memchr(entry->ssid, 0, sizeof(entry->ssid)) ||
            !memchr(entry->password, 0, sizeof(entry->password)) ||
            !wifi_manager_credentials_valid(entry->ssid, entry->password, NULL, 0)) return false;
        for (unsigned j = 0; j < i; ++j)
            if (!strcmp(entry->ssid, history->entries[j].ssid)) return false;
    }
    return true;
}

static esp_err_t load_history_job(void *data, size_t size)
{
    if (size != sizeof(wifi_history_t)) return ESP_ERR_INVALID_SIZE;
    wifi_history_t *history = data;
    memset(history, 0, sizeof(*history));
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        size_t bytes = sizeof(*history);
        err = nvs_get_blob(nvs, WIFI_HISTORY_KEY, history, &bytes);
        if (err == ESP_OK && (bytes != sizeof(*history) || !wifi_history_valid(history)))
            err = ESP_ERR_INVALID_RESPONSE;
        nvs_close(nvs);
    }
    if (err != ESP_OK) memset(history, 0, sizeof(*history));
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        history->version = WIFI_HISTORY_VERSION;
        err = ESP_OK;
    }
    return err;
}

static esp_err_t remember_network_job(void *data, size_t size)
{
    if (size != sizeof(wifi_manager_credentials_t)) return ESP_ERR_INVALID_SIZE;
    const wifi_manager_credentials_t *entry = data;
    if (!memchr(entry->ssid, 0, sizeof(entry->ssid)) ||
        !memchr(entry->password, 0, sizeof(entry->password)) ||
        !wifi_manager_credentials_valid(entry->ssid, entry->password, NULL, 0))
        return ESP_ERR_INVALID_ARG;
    wifi_history_t *history = &s_history_work;
    esp_err_t err = load_history_job(history, sizeof(*history));
    if (err != ESP_OK) return err; /* Do not overwrite a corrupt/newer history. */
    unsigned index = 0;
    while (index < history->count && strcmp(history->entries[index].ssid, entry->ssid)) ++index;
    if (index == 0 && history->count && !strcmp(history->entries[0].password, entry->password)) {
        memset(history, 0, sizeof(*history));
        return ESP_OK; /* Repeated IP events must not cause repeated Flash writes. */
    }
    if (index == history->count && history->count < WIFI_HISTORY_MAX) ++history->count;
    if (index >= WIFI_HISTORY_MAX) index = WIFI_HISTORY_MAX - 1U;
    memmove(&history->entries[1], &history->entries[0], index * sizeof(*entry));
    history->entries[0] = *entry;
    nvs_handle_t nvs = 0;
    err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, WIFI_HISTORY_KEY, history, sizeof(*history));
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    memset(history, 0, sizeof(*history));
    return err;
}

static esp_err_t wifi_history_load(wifi_history_t *history)
{
    memset(history, 0, sizeof(*history));
    esp_err_t err = nvs_worker_call(load_history_job, history, sizeof(*history), NVS_WORKER_WAIT_MS);
    if (err != ESP_OK) memset(history, 0, sizeof(*history));
    return err;
}

static esp_err_t wifi_history_password(const char *ssid, char password[65])
{
    wifi_history_t history;
    password[0] = 0;
    esp_err_t err = wifi_history_load(&history);
    if (err != ESP_OK) return err;
    err = ESP_ERR_NVS_NOT_FOUND;
    for (unsigned i = 0; i < history.count; ++i) {
        if (strcmp(ssid, history.entries[i].ssid)) continue;
        memcpy(password, history.entries[i].password, sizeof(history.entries[i].password));
        err = ESP_OK;
        break;
    }
    memset(&history, 0, sizeof(history));
    return err;
}
