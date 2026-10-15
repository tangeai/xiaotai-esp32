#include "platform_internal.h"
#include <string.h>
#include <time.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs.h"
static EventGroupHandle_t s_network;
static StaticEventGroup_t s_network_storage;
static unsigned s_retries;
static void network_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_network, 1);
        if (s_retries++ < 10)
            esp_wifi_connect();
        else
            ESP_LOGE("network", "Wi-Fi retry limit reached; check credentials/AP and restart");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retries = 0;
        xEventGroupSetBits(s_network, 1);
        ESP_LOGI("network", "Wi-Fi IP ready");
    }
}
esp_err_t platform_network_start(const char *ssid, const char *password)
{
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
    if ((err = esp_wifi_init(&init)) != ESP_OK)
        return err;
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, network_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, network_event, NULL));
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, ssid, strlen(ssid));
    memcpy(config.sta.password, password, strlen(password));
    config.sta.pmf_cfg.capable = true;
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    return esp_wifi_start();
}
bool platform_network_ready(void)
{
    return s_network && (xEventGroupGetBits(s_network) & 1);
}
esp_err_t platform_time_sync(void)
{
    if (time(NULL) > 1700000000)
        return ESP_OK;
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        2, ESP_SNTP_SERVER_LIST("ntp.aliyun.com", "pool.ntp.org"));
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK)
        return err;
    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(20000));
    esp_netif_sntp_deinit();
    return err;
}
esp_err_t platform_identity_load(platform_identity_t *id)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("voip_demo", NVS_READONLY, &nvs);
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
    esp_err_t err = nvs_open("voip_demo", NVS_READWRITE, &nvs);
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
    esp_err_t err = nvs_open("voip_demo", NVS_READWRITE, &nvs);
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

#define WIFI_NVS_NAMESPACE "voip_demo"
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

esp_err_t platform_request_wifi_provisioning(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK)
        return err;

    err = nvs_erase_key(nvs, WIFI_NVS_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND)
        err = ESP_OK;
    if (err == ESP_OK)
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
