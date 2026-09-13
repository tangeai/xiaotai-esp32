#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define WIFI_SCAN_RESULT_MAX 10
#define WIFI_PASSWORD_MAX_LEN 64

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t authmode;
    uint8_t channel;
    bool secure;
} wifi_scan_result_t;

typedef struct {
    bool in_progress;
    uint16_t count;
    uint32_t last_scan_ms;
    wifi_scan_result_t results[WIFI_SCAN_RESULT_MAX];
} wifi_scan_snapshot_t;

typedef struct {
    bool configured;
    bool started;
    bool connected;
    uint8_t retry_count;
    uint8_t disconnect_reason;
    int8_t rssi;
    char ssid[33];
    char ip_addr[16];
} wifi_status_t;

typedef struct {
    bool enabled;
    bool auto_connect;
    const char *default_ssid;
    const char *default_password;
    const char *fallback_dns_ipv4;
} wifi_driver_config_t;

esp_err_t wifi_prepare(const wifi_driver_config_t *config);
void wifi_release(void);
esp_err_t app_wifi_driver_connect(const char *ssid, const char *password);
esp_err_t wifi_request_scan(void);
void wifi_get_status(wifi_status_t *status);
void wifi_get_scan_snapshot(wifi_scan_snapshot_t *snapshot);
void wifi_get_saved_config(char *ssid, size_t ssid_size, char *password, size_t password_size);
