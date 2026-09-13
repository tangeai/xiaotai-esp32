#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define THING_SERVICE_ENDPOINT_MAX_LEN 256

typedef struct {
    const char *discovery_url;
    const char *device_api_base;
    const char *voip_api_base;
    const char *ai_api_base;
    const char *call_api_base;
    const char *mqtt_uri;
    const char *tirtc_endpoint;
} thing_service_registry_config_t;

esp_err_t thing_service_registry_init(const thing_service_registry_config_t *config);
esp_err_t thing_service_registry_refresh(void);
bool thing_service_registry_is_ready(void);
bool thing_service_registry_is_discovered(void);

/* Returned pointers remain stable for the process lifetime. */
const char *thing_service_registry_device_api_base(void);
const char *thing_service_registry_voip_api_base(void);
const char *thing_service_registry_ai_api_base(void);
const char *thing_service_registry_call_api_base(void);
const char *thing_service_registry_mqtt_uri(void);
const char *thing_service_registry_tirtc_endpoint(void);

#ifdef __cplusplus
}
#endif
