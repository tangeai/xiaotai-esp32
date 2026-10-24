// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef const void *esp_event_base_t;
extern const char s_wifi_event_base, s_ip_event_base;
#define WIFI_EVENT (&s_wifi_event_base)
#define IP_EVENT (&s_ip_event_base)
#define WIFI_EVENT_STA_START 1
#define WIFI_EVENT_STA_DISCONNECTED 2
#define IP_EVENT_STA_GOT_IP 3
#define WIFI_EVENT_STA_STOP 4
#define ESP_EVENT_ANY_ID (-1)
esp_err_t esp_event_loop_create_default(void);
esp_err_t esp_event_handler_register(esp_event_base_t base, int32_t id,
    void (*handler)(void *, esp_event_base_t, int32_t, void *), void *context);
