// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    XMINI_KEY_NONE,
    XMINI_KEY_NEXT,
    XMINI_KEY_CONFIRM,
    XMINI_KEY_BACK,
} xmini_key_event_t;

typedef struct {
    bool raw_down;
    bool stable_down;
    bool consumed;
    uint8_t clicks;
    uint32_t changed_at;
    uint32_t pressed_at;
    uint32_t released_at;
    uint32_t epoch;
} xmini_key_t;

// Call every 10 ms. Epoch invalidates gestures when the displayed context changes.
xmini_key_event_t xmini_key_poll(xmini_key_t *key, bool down,
                                  uint32_t now_ms, uint32_t epoch);
