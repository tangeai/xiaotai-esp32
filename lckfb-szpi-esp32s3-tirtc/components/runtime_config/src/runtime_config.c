/*
 * TiRTC 设备凭证的 NVS adapter。
 *
 * 三个字段作为一组提交；读取任一必需字段失败时清空整个输出，避免调用者误用
 * 半组配置。这里使用普通 NVS，量产工程应由组合根启用 NVS 加密或安全芯片。
 */
#include "runtime_config.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "nvs.h"

#define TIRTC_NVS_NAMESPACE "tirtc_cfg"

static void set_error(char *error, size_t error_size, const char *message)
{
    if (error != NULL && error_size > 0) {
        (void)snprintf(error, error_size, "%s", message);
    }
}

bool runtime_config_tirtc_valid(const runtime_tirtc_config_t *config,
                                char *error,
                                size_t error_size)
{
    if (config == NULL) {
        set_error(error, error_size, "config is null");
        return false;
    }
    size_t device_id_length = strlen(config->device_id);
    size_t secret_length = strlen(config->device_secret);
    if (device_id_length == 0 || device_id_length >= sizeof(config->device_id)) {
        set_error(error, error_size, "device_id length must be 1..64 bytes");
        return false;
    }
    if (secret_length == 0 || secret_length >= sizeof(config->device_secret)) {
        set_error(error, error_size, "device_secret length must be 1..256 bytes");
        return false;
    }
    set_error(error, error_size, "");
    return true;
}

esp_err_t runtime_config_load_tirtc(runtime_tirtc_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(config, 0, sizeof(*config));
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(TIRTC_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    /* client_id 是向后兼容的可选字段；ID 和 secret 必须同时存在。 */
    size_t size = sizeof(config->device_id);
    err = nvs_get_str(nvs, "device_id", config->device_id, &size);
    if (err == ESP_OK) {
        size = sizeof(config->device_secret);
        err = nvs_get_str(nvs, "secret", config->device_secret, &size);
    }
    if (err == ESP_OK) {
        size = sizeof(config->client_id);
        err = nvs_get_str(nvs, "client_id", config->client_id, &size);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            config->client_id[0] = '\0';
            err = ESP_OK;
        }
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        memset(config, 0, sizeof(*config));
    }
    return err;
}

esp_err_t runtime_config_save_tirtc(const runtime_tirtc_config_t *config)
{
    if (!runtime_config_tirtc_valid(config, NULL, 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(TIRTC_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) err = nvs_set_str(nvs, "device_id", config->device_id);
    if (err == ESP_OK) err = nvs_set_str(nvs, "secret", config->device_secret);
    if (err == ESP_OK) err = nvs_set_str(nvs, "client_id", config->client_id);
    if (err == ESP_OK) {
        esp_err_t erase_err = nvs_erase_key(nvs, "endpoint");
        if (erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND) {
            err = erase_err;
        }
    }
    /* nvs_commit 是整组配置对后续启动可见的提交点。 */
    if (err == ESP_OK) err = nvs_commit(nvs);
    if (nvs != 0) nvs_close(nvs);
    return err;
}

esp_err_t runtime_config_clear_tirtc(void)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(TIRTC_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) err = nvs_erase_all(nvs);
    if (err == ESP_OK) err = nvs_commit(nvs);
    if (nvs != 0) nvs_close(nvs);
    return err;
}
