// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    XMINI_GUIDE_STARTUP, XMINI_GUIDE_WIFI_SETUP, XMINI_GUIDE_CONNECTING,
    XMINI_GUIDE_SERVICES, XMINI_GUIDE_BINDING, XMINI_GUIDE_VERIFYING,
    XMINI_GUIDE_ONLINE, XMINI_GUIDE_OFFLINE, XMINI_GUIDE_ERROR,
} xmini_guide_phase_t;

typedef struct {
    xmini_guide_phase_t phase;
    char ssid[33];
    char code[7];
    uint32_t expires_at_ms;
    unsigned error;
    bool startup_failed;
    bool retryable;
} xmini_guide_t;

bool xmini_guide_valid_code(const char *code);
unsigned xmini_guide_seconds_left(const xmini_guide_t *guide, uint32_t now_ms);
void xmini_guide_render(const xmini_guide_t *guide, uint32_t now_ms, uint8_t frame[1024]);
