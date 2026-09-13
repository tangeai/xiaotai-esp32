#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define NETWORK_SCAN_RESULT_MAX  10
#define NETWORK_PASSWORD_MAX_LEN 64
#define NETWORK_PING_TARGET_MAX  64
#define NETWORK_PING_SUMMARY_MAX 96

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t authmode;
    uint8_t channel;
    bool secure;
} network_scan_result_t;

typedef struct {
    bool in_progress;
    uint16_t count;
    uint32_t last_scan_ms;
    network_scan_result_t results[NETWORK_SCAN_RESULT_MAX];
} network_scan_snapshot_t;

typedef struct {
    bool running;
    bool valid;
    uint32_t transmitted;
    uint32_t received;
    uint32_t last_time_ms;
    uint32_t min_time_ms;
    uint32_t avg_time_ms;
    uint32_t max_time_ms;
    uint32_t jitter_ms;
    uint32_t loss_percent;
    int last_error;
    char target[NETWORK_PING_TARGET_MAX];
    char summary[NETWORK_PING_SUMMARY_MAX];
} network_ping_status_t;

typedef struct {
    bool configured;
    bool started;
    bool connected;
    uint8_t retry_count;
    uint8_t disconnect_reason;
    int8_t rssi;
    char ssid[33];
    char ip_addr[16];
} network_state_t;

typedef void (*network_state_cb_t)(const network_state_t *state, void *ctx);

typedef struct {
    bool enabled;
    bool auto_connect;
    const char *default_ssid;
    const char *default_password;
    const char *fallback_dns_ipv4;
} network_config_t;

esp_err_t network_prepare(const network_config_t *config);
void network_release(void);
esp_err_t network_connect(const char *ssid, const char *password);
esp_err_t network_request_scan(void);
void network_get_scan_results(network_scan_snapshot_t *snapshot);
esp_err_t network_start_ping(const char *target_host);
void network_cancel_ping(void);
void network_get_ping_status(network_ping_status_t *status);
void network_get_saved_config(char *ssid, size_t ssid_size, char *password, size_t password_size);
void network_set_state_cb(network_state_cb_t cb, void *ctx);
void network_get_state(network_state_t *state);
bool network_is_connected(void);
