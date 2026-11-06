#include "companion_bootstrap.h"

#include <stdio.h>
#include "eh_common_caps.h"
#include "eh_host_core.h"
#include "eh_host_mcu_transport_init_event.h"
#include "eh_host_sys.h"
#include "esp_log.h"
#include "starter_product.h"

static const char *TAG = "c61_bootstrap";

esp_err_t companion_bootstrap_run(bool manual_retry)
{
    starter_product_set_companion_identity(NULL);
    starter_product_set_companion_update(STARTER_COMPANION_UPDATE_CHECKING, 0, ESP_OK);
    int link_status = 0;
    if (manual_retry) {
        /* User-requested recovery before Wi-Fi starts. Re-enter the owner's
         * public lifecycle so its failed bring-up state is not bypassed. */
        link_status = eh_host_deinit();
    }
    if (link_status == 0) link_status = eh_host_init(NULL);
    if (link_status == 0) link_status = eh_host_connect_to_slave();
    if (link_status != 0) {
        ESP_LOGE(TAG, "C61 SPI link failed (%d); verify paired C61 firmware and wiring", link_status);
        starter_product_set_companion_update(STARTER_COMPANION_UPDATE_RECOVERY, 0, ESP_FAIL);
        return ESP_FAIL;
    }
    const unsigned chip = eh_host_mcu_transport_get_chip_id();
    char identity[80];
    snprintf(identity, sizeof(identity), "C61 chip=%02X init-fw=%08lX", chip,
             (unsigned long)eh_host_mcu_transport_get_fw_version());
    starter_product_set_companion_identity(identity);
    if (chip != EH_PRIV_FIRMWARE_CHIP_ESP32C61) {
        ESP_LOGE(TAG, "wrong coprocessor chip: 0x%02x (expected C61)", chip);
        starter_product_set_companion_update(STARTER_COMPANION_UPDATE_UNSUPPORTED, 0, ESP_ERR_INVALID_RESPONSE);
        return ESP_ERR_INVALID_RESPONSE;
    }
    starter_product_set_companion_update(STARTER_COMPANION_UPDATE_VERIFYING, 0, ESP_OK);
    eh_host_coprocessor_fwver_t version = {0};
    esp_err_t err = eh_host_sys_get_cp_fw_version(&version);
    if (err == ESP_OK) {
        snprintf(identity, sizeof(identity), "C61 SPI / Hosted %lu.%lu.%lu",
                 (unsigned long)version.major1, (unsigned long)version.minor1,
                 (unsigned long)version.patch1);
        starter_product_set_companion_identity(identity);
        /* Hosted 3.x MCU hosts retain RPC-V2 compatibility with 2.x CPs.
         * Keep the vendor 1V1 SPI image; do not accept unqualified versions. */
        const bool vendor_spi = version.major1 == 2 && version.minor1 == 12 && version.patch1 == 13;
        const bool project_spi = version.major1 == 3 && version.minor1 == 0 && version.patch1 == 7;
        if (!vendor_spi && !project_spi) {
            ESP_LOGE(TAG, "%s; supported C61 SPI profiles: vendor 2.12.13 or project 3.0.7", identity);
            err = ESP_ERR_INVALID_VERSION;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "C61 firmware check failed: %s; no automatic firmware update", esp_err_to_name(err));
        starter_product_set_companion_update(STARTER_COMPANION_UPDATE_UNSUPPORTED, 0, err);
        return err;
    }
    ESP_LOGI(TAG, "%s; read-only check passed", identity);
    starter_product_set_companion_update(STARTER_COMPANION_UPDATE_READY, 100, ESP_OK);
    return ESP_OK;
}
