// SPDX-License-Identifier: MIT
/* Appended only to the locked new-I2C SCCB driver. Device entries are owned
 * by the camera; the controller and other devices always belong to the board. */
bool xiaotai_dvp_sccb_idle(void);
esp_err_t xiaotai_dvp_sccb_release_borrowed(int port);

bool xiaotai_dvp_sccb_idle(void)
{
    return !sccb_owns_i2c_port && device_count == 0;
}

esp_err_t xiaotai_dvp_sccb_release_borrowed(int port)
{
    if (sccb_owns_i2c_port || (device_count && sccb_i2c_port != port)) return ESP_ERR_INVALID_STATE;
    for (uint8_t i = 0; i < device_count; ++i) {
        /* A preceding close or the vendor's failed-init cleanup may have
         * removed only the prefix. Never try to delete that handle twice. */
        if (!devices[i].dev_handle) continue;
        const esp_err_t err = i2c_master_bus_rm_device(devices[i].dev_handle);
        if (err != ESP_OK) return err;
        devices[i] = (device_t){0};
    }
    device_count = 0;
    return ESP_OK;
}
