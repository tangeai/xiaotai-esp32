#include "thing_service_registry.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "thing_http_client.h"

#include "app_memory_policy.h"

static const char *TAG = "thing_services";

#define THING_SERVICE_DISCOVERY_RESPONSE_MAX 2048
#define THING_SERVICE_DISCOVERY_MAX_ATTEMPTS     4U
#define THING_SERVICE_DISCOVERY_RETRY_INITIAL_MS 1000U
#define THING_SERVICE_DISCOVERY_RETRY_MAX_MS     4000U
#define THING_SERVICE_DISCOVERY_TIMEOUT_MS   5000U

typedef struct {
    char discovery_url[THING_SERVICE_ENDPOINT_MAX_LEN];
    char device_api_base[THING_SERVICE_ENDPOINT_MAX_LEN];
    char voip_api_base[THING_SERVICE_ENDPOINT_MAX_LEN];
    char ai_api_base[THING_SERVICE_ENDPOINT_MAX_LEN];
    char call_api_base[THING_SERVICE_ENDPOINT_MAX_LEN];
    char mqtt_uri[THING_SERVICE_ENDPOINT_MAX_LEN];
    char tirtc_endpoint[THING_SERVICE_ENDPOINT_MAX_LEN];
} thing_service_endpoints_t;

typedef struct {
    SemaphoreHandle_t lock;
    bool initialized;
    bool refreshing;
    bool attempted;
    bool discovered;
    esp_err_t last_result;
    thing_service_endpoints_t endpoints;
} thing_service_registry_runtime_t;

static EXT_RAM_BSS_ATTR thing_service_registry_runtime_t s_registry;

static bool thing_service_has_prefix(const char *value, const char *prefix)
{
    return value != NULL && prefix != NULL && strncmp(value, prefix, strlen(prefix)) == 0;
}

static bool thing_service_is_https_url(const char *value)
{
    return thing_service_has_prefix(value, "https://");
}

static bool thing_service_is_mqtts_uri(const char *value)
{
    return thing_service_has_prefix(value, "mqtts://");
}

static bool thing_service_http_status_is_recoverable(int status)
{
    return status == 408 || status == 425 || status == 429 ||
           (status >= 500 && status <= 599);
}

static esp_err_t thing_service_copy(char *destination,
                                    size_t destination_size,
                                    const char *value)
{
    if (destination == NULL || destination_size == 0U || value == NULL || value[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(value) >= destination_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    strlcpy(destination, value, destination_size);
    return ESP_OK;
}

static const char *thing_service_json_string(const cJSON *root, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);

    return cJSON_IsString(item) && item->valuestring[0] != '\0' ? item->valuestring : NULL;
}

static esp_err_t thing_service_parse_response(const char *json,
                                              thing_service_endpoints_t *endpoints)
{
    cJSON *root = NULL;
    const char *device_api = NULL;
    const char *voip_api = NULL;
    const char *ai_api = NULL;
    const char *call_api = NULL;
    const char *mqtt_uri = NULL;
    const char *tirtc_endpoint = NULL;
    esp_err_t ret = ESP_OK;

    if (json == NULL || endpoints == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_Parse(json);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    device_api = thing_service_json_string(root, "device-srv");
    voip_api = thing_service_json_string(root, "voip-srv");
    ai_api = thing_service_json_string(root, "ai-srv");
    call_api = thing_service_json_string(root, "call-srv");
    mqtt_uri = thing_service_json_string(root, "mqtt-srv");
    tirtc_endpoint = thing_service_json_string(root, "tirtc-srv");

    if (!thing_service_is_https_url(device_api) ||
        !thing_service_is_https_url(voip_api) ||
        !thing_service_is_https_url(ai_api) ||
        !thing_service_is_mqtts_uri(mqtt_uri) ||
        (call_api != NULL && !thing_service_is_https_url(call_api)) ||
        (tirtc_endpoint != NULL && !thing_service_is_https_url(tirtc_endpoint))) {
        ESP_LOGE(TAG, "service discovery rejected insecure endpoint scheme");
        ret = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    ret = thing_service_copy(endpoints->device_api_base,
                             sizeof(endpoints->device_api_base),
                             device_api);
    if (ret == ESP_OK) {
        ret = thing_service_copy(endpoints->voip_api_base,
                                 sizeof(endpoints->voip_api_base),
                                 voip_api);
    }
    if (ret == ESP_OK) {
        ret = thing_service_copy(endpoints->ai_api_base,
                                 sizeof(endpoints->ai_api_base),
                                 ai_api);
    }
    if (ret == ESP_OK) {
        ret = thing_service_copy(endpoints->mqtt_uri,
                                 sizeof(endpoints->mqtt_uri),
                                 mqtt_uri);
    }
    if (ret == ESP_OK) {
        ret = thing_service_copy(endpoints->call_api_base,
                                 sizeof(endpoints->call_api_base),
                                 call_api != NULL ? call_api : device_api);
    }
    if (ret == ESP_OK && tirtc_endpoint != NULL) {
        ret = thing_service_copy(endpoints->tirtc_endpoint,
                                 sizeof(endpoints->tirtc_endpoint),
                                 tirtc_endpoint);
    }

cleanup:
    cJSON_Delete(root);
    return ret;
}

esp_err_t thing_service_registry_init(const thing_service_registry_config_t *config)
{
    if (config == NULL ||
        !thing_service_is_https_url(config->discovery_url) ||
        !thing_service_is_https_url(config->device_api_base) ||
        !thing_service_is_https_url(config->voip_api_base) ||
        !thing_service_is_https_url(config->ai_api_base) ||
        !thing_service_is_https_url(config->call_api_base) ||
        !thing_service_is_mqtts_uri(config->mqtt_uri) ||
        !thing_service_is_https_url(config->tirtc_endpoint)) {
        ESP_LOGE(TAG, "service registry requires HTTPS and MQTTS endpoints");
        return ESP_ERR_INVALID_ARG;
    }
    if (s_registry.lock == NULL) {
        s_registry.lock = xSemaphoreCreateMutexWithCaps(APP_SYNC_CAPS_CONTROL);
        if (s_registry.lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_registry.lock, portMAX_DELAY);
    memset(&s_registry.endpoints, 0, sizeof(s_registry.endpoints));
    esp_err_t ret = thing_service_copy(s_registry.endpoints.discovery_url,
                                       sizeof(s_registry.endpoints.discovery_url),
                                       config->discovery_url);
    if (ret == ESP_OK) {
        ret = thing_service_copy(s_registry.endpoints.device_api_base,
                                 sizeof(s_registry.endpoints.device_api_base),
                                 config->device_api_base);
    }
    if (ret == ESP_OK) {
        ret = thing_service_copy(s_registry.endpoints.voip_api_base,
                                 sizeof(s_registry.endpoints.voip_api_base),
                                 config->voip_api_base);
    }
    if (ret == ESP_OK) {
        ret = thing_service_copy(s_registry.endpoints.ai_api_base,
                                 sizeof(s_registry.endpoints.ai_api_base),
                                 config->ai_api_base);
    }
    if (ret == ESP_OK) {
        ret = thing_service_copy(s_registry.endpoints.call_api_base,
                                 sizeof(s_registry.endpoints.call_api_base),
                                 config->call_api_base);
    }
    if (ret == ESP_OK) {
        ret = thing_service_copy(s_registry.endpoints.mqtt_uri,
                                 sizeof(s_registry.endpoints.mqtt_uri),
                                 config->mqtt_uri);
    }
    if (ret == ESP_OK) {
        ret = thing_service_copy(s_registry.endpoints.tirtc_endpoint,
                                 sizeof(s_registry.endpoints.tirtc_endpoint),
                                 config->tirtc_endpoint);
    }
    s_registry.initialized = ret == ESP_OK;
    s_registry.refreshing = false;
    s_registry.attempted = false;
    s_registry.discovered = false;
    s_registry.last_result = ESP_ERR_INVALID_STATE;
    xSemaphoreGive(s_registry.lock);
    return ret;
}

esp_err_t thing_service_registry_refresh(void)
{
    thing_service_endpoints_t *candidate = NULL;
    char *response = NULL;
    char discovery_url[THING_SERVICE_ENDPOINT_MAX_LEN] = {0};
    int status = 0;
    esp_err_t ret = ESP_OK;

    if (s_registry.lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_registry.lock, portMAX_DELAY);
    if (!s_registry.initialized) {
        xSemaphoreGive(s_registry.lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_registry.discovered) {
        xSemaphoreGive(s_registry.lock);
        return ESP_OK;
    }
    if (s_registry.refreshing) {
        xSemaphoreGive(s_registry.lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_registry.refreshing = true;
    strlcpy(discovery_url, s_registry.endpoints.discovery_url, sizeof(discovery_url));
    xSemaphoreGive(s_registry.lock);

    candidate = app_memory_calloc_psram(1, sizeof(*candidate));
    response = app_memory_calloc_psram(1, THING_SERVICE_DISCOVERY_RESPONSE_MAX);
    if (candidate == NULL || response == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    xSemaphoreTake(s_registry.lock, portMAX_DELAY);
    *candidate = s_registry.endpoints;
    xSemaphoreGive(s_registry.lock);

    const thing_http_request_t request = {
        .url = discovery_url,
        .method = "GET",
        /*
         * Discovery has complete configured fallbacks. Keep each attempt
         * bounded so a half-ready phone hotspot cannot hold device identity
         * and RTC startup behind a single 10-second TLS connect.
         */
        .timeout_ms = THING_SERVICE_DISCOVERY_TIMEOUT_MS,
    };
    uint32_t retry_delay_ms = THING_SERVICE_DISCOVERY_RETRY_INITIAL_MS;
    for (uint8_t attempt = 1U; attempt <= THING_SERVICE_DISCOVERY_MAX_ATTEMPTS; ++attempt) {
        response[0] = '\0';
        status = 0;
        ret = thing_http_request_json(&request,
                                      response,
                                      THING_SERVICE_DISCOVERY_RESPONSE_MAX,
                                      &status);
        if (ret == ESP_OK && status == 200) {
            break;
        }
        bool recoverable = ret != ESP_OK ?
                               thing_http_error_is_recoverable(ret) :
                               thing_service_http_status_is_recoverable(status);
        if (!recoverable ||
            attempt == THING_SERVICE_DISCOVERY_MAX_ATTEMPTS) {
            break;
        }
        ESP_LOGW(TAG,
                 "service discovery retry: attempt=%u/%u wait_ms=%u ret=%s status=%d",
                 (unsigned)attempt,
                 (unsigned)THING_SERVICE_DISCOVERY_MAX_ATTEMPTS,
                 (unsigned)retry_delay_ms,
                 esp_err_to_name(ret),
                 status);
        vTaskDelay(pdMS_TO_TICKS(retry_delay_ms));
        if (retry_delay_ms < THING_SERVICE_DISCOVERY_RETRY_MAX_MS) {
            retry_delay_ms *= 2U;
            if (retry_delay_ms > THING_SERVICE_DISCOVERY_RETRY_MAX_MS) {
                retry_delay_ms = THING_SERVICE_DISCOVERY_RETRY_MAX_MS;
            }
        }
    }
    if (ret != ESP_OK) {
        goto cleanup;
    }
    if (status != 200) {
        ret = ESP_FAIL;
        goto cleanup;
    }
    ret = thing_service_parse_response(response, candidate);
    if (ret != ESP_OK) {
        goto cleanup;
    }

    xSemaphoreTake(s_registry.lock, portMAX_DELAY);
    s_registry.endpoints = *candidate;
    s_registry.discovered = true;
    xSemaphoreGive(s_registry.lock);
    ESP_LOGI(TAG,
             "service discovery ready: device=%s mqtt=%s tirtc=%s",
             candidate->device_api_base,
             candidate->mqtt_uri,
             candidate->tirtc_endpoint);

cleanup:
    xSemaphoreTake(s_registry.lock, portMAX_DELAY);
    s_registry.refreshing = false;
    s_registry.attempted = true;
    s_registry.last_result = ret;
    xSemaphoreGive(s_registry.lock);
    free(response);
    free(candidate);
    return ret;
}

bool thing_service_registry_is_ready(void)
{
    bool ready = false;

    if (s_registry.lock == NULL) {
        return false;
    }
    xSemaphoreTake(s_registry.lock, portMAX_DELAY);
    ready = s_registry.initialized && s_registry.attempted && !s_registry.refreshing;
    xSemaphoreGive(s_registry.lock);
    return ready;
}

bool thing_service_registry_is_discovered(void)
{
    bool discovered = false;

    if (s_registry.lock == NULL) {
        return false;
    }
    xSemaphoreTake(s_registry.lock, portMAX_DELAY);
    discovered = s_registry.discovered;
    xSemaphoreGive(s_registry.lock);
    return discovered;
}

const char *thing_service_registry_device_api_base(void)
{
    return s_registry.endpoints.device_api_base;
}

const char *thing_service_registry_voip_api_base(void)
{
    return s_registry.endpoints.voip_api_base;
}

const char *thing_service_registry_ai_api_base(void)
{
    return s_registry.endpoints.ai_api_base;
}

const char *thing_service_registry_call_api_base(void)
{
    return s_registry.endpoints.call_api_base;
}

const char *thing_service_registry_mqtt_uri(void)
{
    return s_registry.endpoints.mqtt_uri;
}

const char *thing_service_registry_tirtc_endpoint(void)
{
    return s_registry.endpoints.tirtc_endpoint;
}
