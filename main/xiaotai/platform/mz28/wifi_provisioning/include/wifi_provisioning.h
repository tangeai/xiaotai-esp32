#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef struct {
    char ssid[33];
    char password[65];
} wifi_provisioning_credentials_t;

/* Starts the XiaoTai SoftAP and captive portal at http://192.168.6.1. This is
 * a boot-mode service: after valid credentials obtain an IP, the application
 * persists them and restarts into the normal Wi-Fi/VoIP runtime. */
esp_err_t wifi_provisioning_start(void);

/* Nonblocking. Returns one verified credential pair after the station has
 * obtained an IP address. The caller owns and must clear the returned copy. */
bool wifi_provisioning_take_credentials(
    wifi_provisioning_credentials_t *credentials);

bool wifi_provisioning_active(void);
