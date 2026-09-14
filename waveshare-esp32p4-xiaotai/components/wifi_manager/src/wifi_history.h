#pragma once

#include "wifi_manager.h"

#define WIFI_HISTORY_CAPACITY 4U

typedef struct {
    uint8_t version;
    uint8_t count;
    wifi_manager_credentials_t entries[WIFI_HISTORY_CAPACITY];
} wifi_history_t;

/* Private to wifi_manager. Never serialize passwords to the portal. */
void wifi_history_init(const wifi_manager_credentials_t *active);
esp_err_t wifi_history_load(wifi_history_t *history);
esp_err_t wifi_history_remember_active(void);
esp_err_t wifi_history_forget_all(void);
