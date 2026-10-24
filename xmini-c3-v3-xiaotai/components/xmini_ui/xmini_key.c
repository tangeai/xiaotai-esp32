// SPDX-License-Identifier: MIT
#include "xmini_key.h"

#define DEBOUNCE_MS 30U
#define DOUBLE_CLICK_MS 350U
#define HOLD_MS 1200U

xmini_key_event_t xmini_key_poll(xmini_key_t *key, bool down,
                                  uint32_t now_ms, uint32_t epoch)
{
    if (key->epoch != epoch) {
        key->epoch = epoch;
        key->clicks = 0;
        key->consumed = key->stable_down || key->raw_down || down;
    }
    if (down != key->raw_down) {
        key->raw_down = down;
        key->changed_at = now_ms;
    }
    if (key->raw_down != key->stable_down &&
        now_ms - key->changed_at >= DEBOUNCE_MS) {
        key->stable_down = key->raw_down;
        if (key->stable_down) {
            key->pressed_at = now_ms;
        } else if (key->consumed) {
            key->consumed = false;
            key->clicks = 0;
        } else {
            key->released_at = now_ms;
            if (++key->clicks == 2) {
                key->clicks = 0;
                return XMINI_KEY_CONFIRM;
            }
        }
    }
    if (key->stable_down && !key->consumed &&
        now_ms - key->pressed_at >= HOLD_MS) {
        key->consumed = true;
        key->clicks = 0;
        return XMINI_KEY_BACK;
    }
    if (!key->raw_down && !key->stable_down && key->clicks == 1 &&
        now_ms - key->released_at >= DOUBLE_CLICK_MS) {
        key->clicks = 0;
        return XMINI_KEY_NEXT;
    }
    return XMINI_KEY_NONE;
}
