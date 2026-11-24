// SPDX-License-Identifier: MIT
#pragma once
#include "esp_event.h"
#include <stdbool.h>

#define ESP_ERR_WIFI_NOT_CONNECT 0x300f
#define ESP_ERR_WIFI_NOT_INIT 0x3001
#define ESP_ERR_WIFI_NOT_STARTED 0x3002
#define ESP_ERR_WIFI_STOP_STATE 0x3014
enum {
    WIFI_REASON_NO_AP_FOUND = 201, WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY = 210,
    WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD = 211, WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT = 15,
    WIFI_REASON_802_1X_AUTH_FAILED = 23, WIFI_REASON_AUTH_FAIL = 202, WIFI_REASON_HANDSHAKE_TIMEOUT = 204,
    WIFI_REASON_BEACON_TIMEOUT = 200,
};
enum {WIFI_EVENT_STA_START, WIFI_EVENT_STA_STOP, WIFI_EVENT_AP_START, WIFI_EVENT_AP_STOP,
      WIFI_EVENT_STA_CONNECTED, WIFI_EVENT_STA_DISCONNECTED, WIFI_EVENT_SCAN_DONE};
typedef enum {
    WIFI_AUTH_OPEN, WIFI_AUTH_WEP, WIFI_AUTH_WPA_PSK, WIFI_AUTH_WPA2_PSK, WIFI_AUTH_WPA_WPA2_PSK,
    WIFI_AUTH_ENTERPRISE, WIFI_AUTH_WPA3_PSK, WIFI_AUTH_WPA2_WPA3_PSK, WIFI_AUTH_WPA3_ENT_192,
    WIFI_AUTH_WPA3_ENTERPRISE, WIFI_AUTH_WPA2_WPA3_ENTERPRISE,
} wifi_auth_mode_t;
typedef enum {WIFI_MODE_NULL, WIFI_MODE_STA, WIFI_MODE_AP, WIFI_MODE_APSTA} wifi_mode_t;
typedef enum {WIFI_IF_STA, WIFI_IF_AP} wifi_interface_t;
typedef enum {WIFI_BW20, WIFI_BW40} wifi_bandwidth_t;
typedef enum {WIFI_PS_NONE, WIFI_PS_MIN_MODEM} wifi_ps_type_t;
typedef enum {WIFI_SECOND_CHAN_NONE} wifi_second_chan_t;
enum {WIFI_STORAGE_RAM, WIFI_SCAN_TYPE_ACTIVE};
typedef struct {int tx_buf_type, static_tx_buf_num, dynamic_tx_buf_num, cache_tx_buf_num, nvs_enable;} wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){0, 7, 9, 32, 1})
typedef union {
    struct {uint8_t ssid[32], password[64]; struct {wifi_auth_mode_t authmode;} threshold;
            struct {bool capable;} pmf_cfg;} sta;
    struct {uint8_t ssid[32], password[64], ssid_len, channel, max_connection; wifi_auth_mode_t authmode;} ap;
} wifi_config_t;
typedef struct {uint8_t ssid[32], ssid_len, channel;} wifi_event_sta_connected_t;
typedef struct {uint8_t reason; int8_t rssi;} wifi_event_sta_disconnected_t;
typedef struct {uint32_t status;} wifi_event_sta_scan_done_t;
typedef struct {uint8_t ssid[33], primary; int8_t rssi; wifi_auth_mode_t authmode;} wifi_ap_record_t;
typedef struct {bool show_hidden; int scan_type; struct {struct {unsigned min, max;} active;} scan_time;} wifi_scan_config_t;

esp_err_t esp_wifi_init(const wifi_init_config_t *);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_stop(void);
esp_err_t esp_wifi_deinit(void);
esp_err_t esp_wifi_set_storage(int);
esp_err_t esp_wifi_set_mode(wifi_mode_t);
esp_err_t esp_wifi_get_mode(wifi_mode_t *);
esp_err_t esp_wifi_get_protocol(wifi_interface_t, uint8_t *);
esp_err_t esp_wifi_set_protocol(wifi_interface_t, uint8_t);
esp_err_t esp_wifi_set_bandwidth(wifi_interface_t, wifi_bandwidth_t);
esp_err_t esp_wifi_get_bandwidth(wifi_interface_t, wifi_bandwidth_t *);
esp_err_t esp_wifi_set_ps(wifi_ps_type_t);
esp_err_t esp_wifi_get_ps(wifi_ps_type_t *);
esp_err_t esp_wifi_set_config(wifi_interface_t, wifi_config_t *);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_disconnect(void);
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *, bool);
esp_err_t esp_wifi_scan_stop(void);
esp_err_t esp_wifi_clear_ap_list(void);
esp_err_t esp_wifi_scan_get_ap_num(uint16_t *);
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *, wifi_ap_record_t *);
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *);
esp_err_t esp_wifi_get_mac(wifi_interface_t, uint8_t *);
esp_err_t esp_wifi_get_channel(uint8_t *, wifi_second_chan_t *);
