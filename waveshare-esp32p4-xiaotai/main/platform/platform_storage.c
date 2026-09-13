#include "platform_storage.h"

#include "esp_check.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "platform_storage";

static bool s_storage_initialized;

esp_err_t platform_storage_init(void)
{
    if (s_storage_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase nvs failed");
        ret = nvs_flash_init();
    }

    if (ret == ESP_OK) {
        s_storage_initialized = true;
    }
    return ret;
}
