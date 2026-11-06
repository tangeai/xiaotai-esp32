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
#include "nvs_store.h"

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
    size_t device_id_length = strnlen(config->device_id, sizeof(config->device_id));
    size_t secret_length = strnlen(config->device_secret, sizeof(config->device_secret));
    if (device_id_length == 0 || device_id_length >= sizeof(config->device_id)) {
        set_error(error, error_size, "device_id length must be 1..64 bytes");
        return false;
    }
    if (secret_length == 0 || secret_length >= sizeof(config->device_secret)) {
        set_error(error, error_size, "device_secret length must be 1..256 bytes");
        return false;
    }
    if (strnlen(config->client_id, sizeof(config->client_id)) >= sizeof(config->client_id)) {
        set_error(error, error_size, "client_id is not terminated");
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
    /* client_id 是向后兼容的可选字段；ID 和 secret 必须同时存在。 */
    size_t size = sizeof(config->device_id);
    esp_err_t err = nvs_store_read(TIRTC_NVS_NAMESPACE, "device_id",
        NVS_STORE_GET_STRING, config->device_id, &size, 5000);
    if (err == ESP_OK) {
        size = sizeof(config->device_secret);
        err = nvs_store_read(TIRTC_NVS_NAMESPACE, "secret",
            NVS_STORE_GET_STRING, config->device_secret, &size, 5000);
    }
    if (err == ESP_OK) {
        size = sizeof(config->client_id);
        err = nvs_store_read(TIRTC_NVS_NAMESPACE, "client_id",
            NVS_STORE_GET_STRING, config->client_id, &size, 5000);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            config->client_id[0] = '\0';
            err = ESP_OK;
        }
    }
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
    const nvs_store_op_t ops[] = {
        {NVS_STORE_STRING, "device_id", config->device_id, strlen(config->device_id) + 1},
        {NVS_STORE_STRING, "secret", config->device_secret, strlen(config->device_secret) + 1},
        {NVS_STORE_STRING, "client_id", config->client_id, strlen(config->client_id) + 1},
        {NVS_STORE_ERASE_KEY, "endpoint", NULL, 0},
    };
    /* The identity owner must see actual persistence, not enqueue success.
     * Preserve legacy keys; multi-key NVS writes do not promise rollback. */
    return nvs_store_execute(TIRTC_NVS_NAMESPACE, ops, 4, 5000);
}

esp_err_t runtime_config_clear_tirtc(void)
{
    const nvs_store_op_t op = {NVS_STORE_ERASE_ALL, NULL, NULL, 0};
    return nvs_store_execute(TIRTC_NVS_NAMESPACE, &op, 1, 5000);
}
