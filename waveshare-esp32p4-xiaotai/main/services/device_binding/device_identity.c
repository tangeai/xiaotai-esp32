#include "device_identity.h"

#include <stdio.h>
#include <string.h>

#include "esp_mac.h"

esp_err_t device_identity_get(device_binding_identity_t *identity)
{
    uint8_t mac[6] = {0};
    esp_err_t ret = ESP_OK;

    if (identity == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(identity, 0, sizeof(*identity));
    ret = esp_read_mac(mac, ESP_MAC_BASE);
    if (ret != ESP_OK) {
        return ret;
    }
    snprintf(identity->mac,
             sizeof(identity->mac),
             "%02X%02X%02X%02X%02X%02X",
             mac[0],
             mac[1],
             mac[2],
             mac[3],
             mac[4],
             mac[5]);

    return ESP_OK;
}
