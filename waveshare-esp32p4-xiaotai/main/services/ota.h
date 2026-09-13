#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define OTA_URL_MAX        256
#define OTA_MESSAGE_MAX    96
#define OTA_VERSION_MAX    32
#define OTA_DEVICE_ID_MAX  37
#define OTA_CHIP_MAX       16
#define OTA_REASON_MAX     32

typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_CHECKING,
    OTA_STATE_DOWNLOADING,
    OTA_STATE_VERIFYING,
    OTA_STATE_READY_TO_REBOOT,
    OTA_STATE_FAILED,
} ota_state_t;

typedef struct {
    ota_state_t state;
    bool running;
    uint8_t progress_percent;
    size_t bytes_read;
    size_t total_size;
    esp_err_t last_error;
    char current_version[OTA_VERSION_MAX];
    char target_version[OTA_VERSION_MAX];
    char url[OTA_URL_MAX];
    char message[OTA_MESSAGE_MAX];
} ota_snapshot_t;

typedef struct {
    const char *default_url;
} ota_config_t;

esp_err_t ota_init(const ota_config_t *config);
esp_err_t ota_start_default(const char *device_id);
esp_err_t ota_start(const char *base_url, const char *device_id);
void ota_get_snapshot(ota_snapshot_t *snapshot);
void ota_restart(void);
const char *ota_default_url(void);
