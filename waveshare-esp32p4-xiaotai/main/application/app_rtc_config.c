#include "app_rtc_config.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "device.h"
#include "device_identity.h"
#include "esp_check.h"
#include "nvs.h"
#include "platform_nvs_async.h"
#include "platform_storage.h"
#include "thing_service_registry.h"

static const char *TAG = "app_rtc_config";

#define APP_RTC_NVS_NAMESPACE         "rtc_cfg"
#define APP_RTC_NVS_KEY_DEVICE_ID     "dev_id"
#define APP_RTC_NVS_KEY_DEVICE_SECRET "dev_secret"
#define APP_RTC_NVS_KEY_DEVICE_CREDENTIALS "dev_cred"
#define APP_RTC_NVS_KEY_TOKEN_SUBJECT "app_id"
#define APP_RTC_NVS_KEY_ACCESS_KEY    "ak_id"
#define APP_RTC_NVS_KEY_SECRET_KEY    "ak_secret"
#define APP_RTC_NVS_KEY_SERVER_ENV    "env"

#define APP_RTC_DEVICE_CREDENTIALS_MAGIC   0x52444346U
#define APP_RTC_DEVICE_CREDENTIALS_VERSION 1U

typedef struct {
    app_rtc_server_env_t env;
    const char *name;
    const char *access_url;
    const char *server_api;
} app_rtc_server_profile_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    char device_id[APP_RTC_CONFIG_TEXT_MAX];
    char device_secret[APP_RTC_CONFIG_TEXT_MAX];
} app_rtc_device_credentials_store_t;

static const app_rtc_server_profile_t s_server_profiles[] = {
    {
        .env = APP_RTC_SERVER_ENV_TEST,
        .name = "test",
        .access_url = "https://ep-test-tirtc.tange365.com",
        .server_api = "https://api-test-tirtc.tange365.com",
    },
    {
        .env = APP_RTC_SERVER_ENV_PRE,
        .name = "pre",
        .access_url = "https://ep-pre-tirtc.tange365.com",
        .server_api = "https://api-pre-tirtc.tange365.com",
    },
    {
        .env = APP_RTC_SERVER_ENV_PROD,
        .name = "prod",
        .access_url = "https://ep-tirtc.tange365.com",
        .server_api = "https://api-tirtc.tange365.com",
    },
};

static app_rtc_config_snapshot_t s_rtc_settings;
static bool s_rtc_settings_loaded;

static const app_rtc_server_profile_t *app_rtc_profile_for_env(app_rtc_server_env_t env)
{
    for (size_t index = 0; index < sizeof(s_server_profiles) / sizeof(s_server_profiles[0]); ++index) {
        if (s_server_profiles[index].env == env) {
            return &s_server_profiles[index];
        }
    }
    return &s_server_profiles[2];
}

static app_rtc_server_env_t app_rtc_env_from_name(const char *name)
{
    if (name == NULL) {
        return APP_RTC_SERVER_ENV_PROD;
    }
    for (size_t index = 0; index < sizeof(s_server_profiles) / sizeof(s_server_profiles[0]); ++index) {
        if (strcmp(name, s_server_profiles[index].name) == 0) {
            return s_server_profiles[index].env;
        }
    }
    return APP_RTC_SERVER_ENV_PROD;
}

static app_rtc_server_env_t app_rtc_env_from_access_url(const char *url)
{
    if (url == NULL) {
        return APP_RTC_SERVER_ENV_PROD;
    }
    for (size_t index = 0; index < sizeof(s_server_profiles) / sizeof(s_server_profiles[0]); ++index) {
        if (strcmp(url, s_server_profiles[index].access_url) == 0) {
            return s_server_profiles[index].env;
        }
    }
    return APP_RTC_SERVER_ENV_PROD;
}

static const char *app_rtc_env_name(app_rtc_server_env_t env)
{
    return app_rtc_profile_for_env(env)->name;
}

static void app_rtc_apply_server_profile(app_rtc_config_snapshot_t *settings)
{
    const app_rtc_server_profile_t *profile = NULL;

    if (settings == NULL) {
        return;
    }

    profile = app_rtc_profile_for_env(settings->server_env);
    strlcpy(settings->access_url, profile->access_url, sizeof(settings->access_url));
    strlcpy(settings->server_api, profile->server_api, sizeof(settings->server_api));
}

static void app_rtc_fill_defaults(app_rtc_config_snapshot_t *settings)
{
    if (settings == NULL) {
        return;
    }

    memset(settings, 0, sizeof(*settings));
    strlcpy(settings->device_id, APP_CONFIG_RTC_DEVICE_ID, sizeof(settings->device_id));
    strlcpy(settings->device_secret, APP_CONFIG_RTC_DEVICE_SECRET_KEY, sizeof(settings->device_secret));
    strlcpy(settings->token_subject, APP_CONFIG_RTC_TOKEN_SUBJECT, sizeof(settings->token_subject));
    strlcpy(settings->access_key_id, APP_CONFIG_RTC_TOKEN_ACCESS_ID, sizeof(settings->access_key_id));
    strlcpy(settings->access_key_secret, APP_CONFIG_RTC_TOKEN_SECRET_KEY, sizeof(settings->access_key_secret));
    settings->server_env = app_rtc_env_from_access_url(APP_CONFIG_RTC_SERVICE_ENDPOINT);
    app_rtc_apply_server_profile(settings);
}

static esp_err_t app_rtc_load_string(nvs_handle_t nvs_handle, const char *key, char *value, size_t value_size)
{
    char loaded[APP_RTC_CONFIG_TEXT_MAX] = {0};
    size_t value_len = sizeof(loaded);
    esp_err_t ret = ESP_OK;

    if (key == NULL || value == NULL || value_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = nvs_get_str(nvs_handle, key, loaded, &value_len);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret == ESP_OK && loaded[0] != '\0') {
        strlcpy(value, loaded, value_size);
    }
    return ret;
}

static bool app_rtc_device_credentials_store_valid(const app_rtc_device_credentials_store_t *store)
{
    return store != NULL &&
           store->magic == APP_RTC_DEVICE_CREDENTIALS_MAGIC &&
           store->version == APP_RTC_DEVICE_CREDENTIALS_VERSION &&
           store->device_id[sizeof(store->device_id) - 1] == '\0' &&
           store->device_secret[sizeof(store->device_secret) - 1] == '\0';
}

static esp_err_t app_rtc_load_device_credentials(nvs_handle_t nvs_handle,
                                                 app_rtc_config_snapshot_t *settings)
{
    app_rtc_device_credentials_store_t store = {0};
    size_t store_len = sizeof(store);
    esp_err_t ret = ESP_OK;

    if (settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = nvs_get_blob(nvs_handle, APP_RTC_NVS_KEY_DEVICE_CREDENTIALS, &store, &store_len);
    if (ret == ESP_OK) {
        if (store_len == sizeof(store) && app_rtc_device_credentials_store_valid(&store)) {
            strlcpy(settings->device_id, store.device_id, sizeof(settings->device_id));
            strlcpy(settings->device_secret, store.device_secret, sizeof(settings->device_secret));
            return ESP_OK;
        }
        ESP_LOGW(TAG, "rtc credential blob invalid, falling back to legacy keys");
    } else if (ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "rtc credential blob load failed: %s", esp_err_to_name(ret));
    }

    ret = app_rtc_load_string(nvs_handle,
                              APP_RTC_NVS_KEY_DEVICE_ID,
                              settings->device_id,
                              sizeof(settings->device_id));
    if (ret != ESP_OK) {
        return ret;
    }
    return app_rtc_load_string(nvs_handle,
                               APP_RTC_NVS_KEY_DEVICE_SECRET,
                               settings->device_secret,
                               sizeof(settings->device_secret));
}

static esp_err_t app_rtc_save_string(const char *key, const char *value)
{
    if (key == NULL || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return platform_nvs_async_set_str_and_wait(APP_RTC_NVS_NAMESPACE, key, value);
}

static esp_err_t app_rtc_save_device_credentials(const char *device_id, const char *device_secret)
{
    app_rtc_device_credentials_store_t store = {
        .magic = APP_RTC_DEVICE_CREDENTIALS_MAGIC,
        .version = APP_RTC_DEVICE_CREDENTIALS_VERSION,
    };
    if (device_id == NULL || device_secret == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(device_id) >= sizeof(store.device_id) ||
        strlen(device_secret) >= sizeof(store.device_secret)) {
        return ESP_ERR_INVALID_SIZE;
    }
    strlcpy(store.device_id, device_id, sizeof(store.device_id));
    strlcpy(store.device_secret, device_secret, sizeof(store.device_secret));

    /* Device identity is acknowledged only after the new credential blob is durable. */
    esp_err_t ret = platform_nvs_async_set_blob_and_wait(APP_RTC_NVS_NAMESPACE,
                                                          APP_RTC_NVS_KEY_DEVICE_CREDENTIALS,
                                                          &store,
                                                          sizeof(store));
    if (ret == ESP_OK) {
        /* The blob is authoritative; legacy keys can be removed in queue order. */
        esp_err_t erase_id_ret = platform_nvs_async_erase_key(APP_RTC_NVS_NAMESPACE,
                                                               APP_RTC_NVS_KEY_DEVICE_ID);
        esp_err_t erase_secret_ret = platform_nvs_async_erase_key(APP_RTC_NVS_NAMESPACE,
                                                                   APP_RTC_NVS_KEY_DEVICE_SECRET);
        if (erase_id_ret != ESP_OK || erase_secret_ret != ESP_OK) {
            ESP_LOGW(TAG,
                     "rtc legacy credential cleanup queue failed: id=%s secret=%s",
                     esp_err_to_name(erase_id_ret),
                     esp_err_to_name(erase_secret_ret));
        }
        ESP_LOGD(TAG, "rtc credentials saved: device_id_len=%u", (unsigned)strlen(device_id));
    } else {
        ESP_LOGW(TAG, "rtc credentials save failed: ret=%s", esp_err_to_name(ret));
    }
    return ret;
}

static void app_rtc_load_settings(void)
{
    nvs_handle_t nvs_handle = 0;
    char env_name[8] = {0};
    esp_err_t ret = ESP_OK;

    if (s_rtc_settings_loaded) {
        return;
    }

    app_rtc_fill_defaults(&s_rtc_settings);
    ret = platform_storage_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "rtc settings storage init failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = nvs_open(APP_RTC_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret == ESP_OK) {
        ret = app_rtc_load_device_credentials(nvs_handle, &s_rtc_settings);
        if (ret != ESP_OK) {
            nvs_close(nvs_handle);
            ESP_LOGE(TAG, "rtc credentials load failed: %s", esp_err_to_name(ret));
            return;
        }
        ESP_LOGI(TAG,
                 "rtc credentials restored from nvs: device_id_len=%u configured=%d",
                 (unsigned)strlen(s_rtc_settings.device_id),
                 s_rtc_settings.device_id[0] != '\0' &&
                 s_rtc_settings.device_secret[0] != '\0' ? 1 : 0);
        (void)app_rtc_load_string(nvs_handle,
                                  APP_RTC_NVS_KEY_TOKEN_SUBJECT,
                                  s_rtc_settings.token_subject,
                                  sizeof(s_rtc_settings.token_subject));
        (void)app_rtc_load_string(nvs_handle,
                                  APP_RTC_NVS_KEY_ACCESS_KEY,
                                  s_rtc_settings.access_key_id,
                                  sizeof(s_rtc_settings.access_key_id));
        (void)app_rtc_load_string(nvs_handle,
                                  APP_RTC_NVS_KEY_SECRET_KEY,
                                  s_rtc_settings.access_key_secret,
                                  sizeof(s_rtc_settings.access_key_secret));
        if (app_rtc_load_string(nvs_handle, APP_RTC_NVS_KEY_SERVER_ENV, env_name, sizeof(env_name)) == ESP_OK &&
            env_name[0] != '\0') {
            s_rtc_settings.server_env = app_rtc_env_from_name(env_name);
        }
        nvs_close(nvs_handle);
    } else if (ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "rtc settings namespace open failed: %s", esp_err_to_name(ret));
        return;
    }

    app_rtc_apply_server_profile(&s_rtc_settings);
    ESP_LOGD(TAG,
             "rtc settings loaded: device_id_len=%u env=%s dev_secret_default=%d ak_default=%d sk_default=%d",
             (unsigned)strlen(s_rtc_settings.device_id),
             app_rtc_env_name(s_rtc_settings.server_env),
             strcmp(s_rtc_settings.device_secret, APP_CONFIG_RTC_DEVICE_SECRET_KEY) == 0,
             strcmp(s_rtc_settings.access_key_id, APP_CONFIG_RTC_TOKEN_ACCESS_ID) == 0,
             strcmp(s_rtc_settings.access_key_secret, APP_CONFIG_RTC_TOKEN_SECRET_KEY) == 0);
    s_rtc_settings_loaded = true;
}

static void app_fill_tirtc_license(rtc_transport_config_t *config)
{
    const char *device_id = NULL;
    int written = 0;

    if (config == NULL || config->device_license[0] != '\0' ||
        config->device_secret_key[0] == '\0') {
        return;
    }

    if (config->device_id[0] != '\0') {
        device_id = config->device_id;
    }

    if (device_id == NULL || device_id[0] == '\0') {
        config->device_license[0] = '\0';
        return;
    }

    written = snprintf(config->device_license,
                       sizeof(config->device_license),
                       "%s,%s",
                       device_id,
                       config->device_secret_key);
    if (written < 0 || written >= (int)sizeof(config->device_license)) {
        config->device_license[0] = '\0';
    }
}

esp_err_t app_build_rtc_transport_config(rtc_transport_config_t *config)
{
    app_rtc_config_snapshot_t settings = {0};
    device_binding_identity_t identity = {0};
    esp_err_t identity_ret = ESP_OK;

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    app_rtc_load_settings();
    settings = s_rtc_settings;

    memset(config, 0, sizeof(*config));
    config->enabled = APP_CONFIG_RTC_ENABLE != 0;
    config->default_session_mode = RTC_TRANSPORT_MODE_LISTEN;
    const char *service_endpoint = settings.access_url;
    if (settings.server_env == APP_RTC_SERVER_ENV_PROD &&
        thing_service_registry_is_discovered()) {
        service_endpoint = thing_service_registry_tirtc_endpoint();
    }
    strlcpy(config->service_endpoint, service_endpoint, sizeof(config->service_endpoint));
    strlcpy(config->device_id, settings.device_id, sizeof(config->device_id));
    strlcpy(config->device_secret_key, settings.device_secret, sizeof(config->device_secret_key));
    /* The physical client ID remains stable across bind and unbind cycles. */
    identity_ret = device_identity_get(&identity);
    if (identity_ret == ESP_OK && identity.mac[0] != '\0') {
        strlcpy(config->client_id, identity.mac, sizeof(config->client_id));
    } else {
        ESP_LOGW(TAG,
                 "rtc physical client id unavailable: %s",
                 esp_err_to_name(identity_ret));
    }
    strlcpy(config->remote_device_id, APP_CONFIG_RTC_REMOTE_DEVICE_ID, sizeof(config->remote_device_id));
    strlcpy(config->remote_device_secret_key,
            APP_CONFIG_RTC_REMOTE_DEVICE_SECRET_KEY,
            sizeof(config->remote_device_secret_key));
    strlcpy(config->token_access_id, settings.access_key_id, sizeof(config->token_access_id));
    strlcpy(config->token_secret_key, settings.access_key_secret, sizeof(config->token_secret_key));
    strlcpy(config->token_subject, settings.token_subject, sizeof(config->token_subject));
    config->token_ttl_seconds = APP_CONFIG_RTC_TOKEN_TTL_SECONDS;
    app_fill_tirtc_license(config);
    if (config->device_id[0] == '\0' || config->device_secret_key[0] == '\0') {
        config->enabled = false;
        ESP_LOGW(TAG, "rtc disabled until cloud device credentials are configured");
    } else if (config->client_id[0] == '\0') {
        config->enabled = false;
        ESP_LOGW(TAG, "rtc disabled until physical client id is available");
    }
    return ESP_OK;
}

void app_get_rtc_config_snapshot(app_rtc_config_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    app_rtc_load_settings();
    *snapshot = s_rtc_settings;
}

esp_err_t app_set_rtc_device_credentials(const char *device_id, const char *device_secret)
{
    char old_device_id[sizeof(s_rtc_settings.device_id)] = {0};
    char old_device_secret[sizeof(s_rtc_settings.device_secret)] = {0};

    if (device_id == NULL || device_id[0] == '\0' ||
        device_secret == NULL || device_secret[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(device_id) >= sizeof(s_rtc_settings.device_id) ||
        strlen(device_secret) >= sizeof(s_rtc_settings.device_secret)) {
        return ESP_ERR_INVALID_SIZE;
    }

    app_rtc_load_settings();
    strlcpy(old_device_id, s_rtc_settings.device_id, sizeof(old_device_id));
    strlcpy(old_device_secret, s_rtc_settings.device_secret, sizeof(old_device_secret));
    strlcpy(s_rtc_settings.device_id, device_id, sizeof(s_rtc_settings.device_id));
    strlcpy(s_rtc_settings.device_secret, device_secret, sizeof(s_rtc_settings.device_secret));

    esp_err_t ret = app_rtc_save_device_credentials(device_id, device_secret);
    if (ret != ESP_OK) {
        strlcpy(s_rtc_settings.device_id, old_device_id, sizeof(s_rtc_settings.device_id));
        strlcpy(s_rtc_settings.device_secret, old_device_secret, sizeof(s_rtc_settings.device_secret));
    }
    return ret;
}

esp_err_t app_clear_rtc_device_credentials(void)
{
    char old_device_id[sizeof(s_rtc_settings.device_id)] = {0};
    char old_device_secret[sizeof(s_rtc_settings.device_secret)] = {0};

    app_rtc_load_settings();
    strlcpy(old_device_id, s_rtc_settings.device_id, sizeof(old_device_id));
    strlcpy(old_device_secret, s_rtc_settings.device_secret, sizeof(old_device_secret));
    s_rtc_settings.device_id[0] = '\0';
    s_rtc_settings.device_secret[0] = '\0';

    esp_err_t ret = app_rtc_save_device_credentials("", "");
    if (ret != ESP_OK) {
        strlcpy(s_rtc_settings.device_id, old_device_id, sizeof(s_rtc_settings.device_id));
        strlcpy(s_rtc_settings.device_secret, old_device_secret, sizeof(s_rtc_settings.device_secret));
        return ret;
    }

    ESP_LOGD(TAG, "rtc credentials cleared");
    return ESP_OK;
}

esp_err_t app_set_rtc_config_field(app_rtc_config_field_t field, const char *value)
{
    char old_value[APP_RTC_CONFIG_TEXT_MAX] = {0};
    char *target = NULL;
    size_t target_size = 0;
    const char *nvs_key = NULL;
    bool device_credential_field = false;

    if (value == NULL || value[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    app_rtc_load_settings();
    switch (field) {
    case APP_RTC_CONFIG_FIELD_DEVICE_ID:
        target = s_rtc_settings.device_id;
        target_size = sizeof(s_rtc_settings.device_id);
        nvs_key = APP_RTC_NVS_KEY_DEVICE_ID;
        device_credential_field = true;
        break;
    case APP_RTC_CONFIG_FIELD_DEVICE_SECRET:
        target = s_rtc_settings.device_secret;
        target_size = sizeof(s_rtc_settings.device_secret);
        nvs_key = APP_RTC_NVS_KEY_DEVICE_SECRET;
        device_credential_field = true;
        break;
    case APP_RTC_CONFIG_FIELD_TOKEN_SUBJECT:
        target = s_rtc_settings.token_subject;
        target_size = sizeof(s_rtc_settings.token_subject);
        nvs_key = APP_RTC_NVS_KEY_TOKEN_SUBJECT;
        break;
    case APP_RTC_CONFIG_FIELD_ACCESS_KEY_ID:
        target = s_rtc_settings.access_key_id;
        target_size = sizeof(s_rtc_settings.access_key_id);
        nvs_key = APP_RTC_NVS_KEY_ACCESS_KEY;
        break;
    case APP_RTC_CONFIG_FIELD_ACCESS_KEY_SECRET:
        target = s_rtc_settings.access_key_secret;
        target_size = sizeof(s_rtc_settings.access_key_secret);
        nvs_key = APP_RTC_NVS_KEY_SECRET_KEY;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    if (strlen(value) >= target_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    strlcpy(old_value, target, sizeof(old_value));
    strlcpy(target, value, target_size);
    esp_err_t ret = ESP_OK;
    if (device_credential_field &&
        s_rtc_settings.device_id[0] != '\0' &&
        s_rtc_settings.device_secret[0] != '\0') {
        ret = app_rtc_save_device_credentials(s_rtc_settings.device_id, s_rtc_settings.device_secret);
    } else {
        ret = app_rtc_save_string(nvs_key, value);
    }
    if (ret != ESP_OK) {
        strlcpy(target, old_value, target_size);
    }
    return ret;
}

esp_err_t app_set_rtc_config_server_env(app_rtc_server_env_t env)
{
    const app_rtc_server_profile_t *profile = app_rtc_profile_for_env(env);

    app_rtc_load_settings();
    s_rtc_settings.server_env = profile->env;
    app_rtc_apply_server_profile(&s_rtc_settings);
    return app_rtc_save_string(APP_RTC_NVS_KEY_SERVER_ENV, app_rtc_env_name(profile->env));
}
