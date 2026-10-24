// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#define ESP_ERR_WIFI_NOT_INIT 0x3001
#define ESP_ERR_WIFI_NOT_STARTED 0x3002
#define ESP_ERR_WIFI_STATE 0x3006
#define ESP_ERR_WIFI_NOT_CONNECT 0x300f
#define ESP_ERR_WIFI_STOP_STATE 0x3014
#define WIFI_REASON_AUTH_EXPIRE 2
#define WIFI_REASON_ASSOC_LEAVE 8
#define WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT 15
#define WIFI_REASON_802_1X_AUTH_FAILED 23
#define WIFI_REASON_BEACON_TIMEOUT 200
#define WIFI_REASON_NO_AP_FOUND 201
#define WIFI_REASON_AUTH_FAIL 202
#define WIFI_REASON_HANDSHAKE_TIMEOUT 204
#define WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY 210
#define WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD 211
typedef struct { int nvs_enable; } wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){.nvs_enable = 1})
typedef struct { uint8_t ssid[32], ssid_len, bssid[6], reason; int8_t rssi; } wifi_event_sta_disconnected_t;
typedef struct { uint8_t bssid[6], ssid[33], primary; int8_t rssi; } wifi_ap_record_t;
typedef struct { struct { uint8_t ssid[32], password[64]; struct { bool capable; } pmf_cfg; } sta; } wifi_config_t;
#define WIFI_STORAGE_RAM 1
#define WIFI_MODE_STA 1
#define WIFI_IF_STA 1
#define WIFI_PS_NONE 0
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_init(const wifi_init_config_t *config);
esp_err_t esp_wifi_set_storage(int storage);
esp_err_t esp_wifi_set_mode(int mode);
esp_err_t esp_wifi_set_config(int interface, const wifi_config_t *config);
esp_err_t esp_wifi_set_ps(int mode);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap);
