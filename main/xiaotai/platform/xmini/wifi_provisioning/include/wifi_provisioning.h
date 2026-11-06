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

/* Accepted is not verified: both HTTP and AT submit through this event-loop
 * path. Only take_credentials() returns a pair proven by STA_GOT_IP. */
esp_err_t wifi_provisioning_submit_credentials(const char *ssid, const char *password);

/* Nonblocking. Returns one verified credential pair after the station has
 * obtained an IP address. Submissions remain fenced until restart. The caller
 * owns the returned copy: retain it after a persistence failure for explicit
 * retry, then clear it after saving. Taking it does not mean it was saved. */
bool wifi_provisioning_take_credentials(
    wifi_provisioning_credentials_t *credentials);

bool wifi_provisioning_active(void);
typedef struct { char ssid[33]; bool connecting; bool failed; esp_err_t error; } wifi_provisioning_status_t;
wifi_provisioning_status_t wifi_provisioning_status(void);
