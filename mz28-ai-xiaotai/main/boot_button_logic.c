#include "boot_button_logic.h"

static boot_button_event_t click_event(uint8_t click_count)
{
    switch (click_count) {
    case 1:
        return BOOT_BUTTON_EVENT_SINGLE_CLICK;
    case 2:
        return BOOT_BUTTON_EVENT_DOUBLE_CLICK;
    case 4:
        return BOOT_BUTTON_EVENT_QUADRUPLE_CLICK;
    default:
        return BOOT_BUTTON_EVENT_NONE;
    }
}

void boot_button_logic_init(boot_button_logic_t *logic, bool pressed,
                            uint32_t now_ms)
{
    *logic = (boot_button_logic_t){
        .released_seen = !pressed,
        .pressed = pressed,
        .tracking = false,
        .pressed_at_ms = now_ms,
    };
}

boot_button_event_t boot_button_logic_update(boot_button_logic_t *logic,
                                              bool pressed, uint32_t now_ms,
                                              uint32_t long_press_ms,
                                              uint32_t multi_click_ms)
{
    bool click_expired = !logic->pressed && logic->click_count != 0U &&
                         (uint32_t)(now_ms - logic->click_released_at_ms) >=
                             multi_click_ms;
    boot_button_event_t expired_event = BOOT_BUTTON_EVENT_NONE;

    if (click_expired) {
        expired_event = click_event(logic->click_count);
        logic->click_count = 0U;
    }

    if (pressed && !logic->pressed) {
        logic->pressed = true;
        logic->tracking = logic->released_seen;
        logic->long_emitted = false;
        logic->pressed_at_ms = now_ms;
    } else if (!pressed && logic->pressed) {
        bool click = logic->tracking && !logic->long_emitted;
        logic->pressed = false;
        logic->tracking = false;
        logic->released_seen = true;
        if (click) {
            if (logic->click_count < 4U) {
                ++logic->click_count;
            }
            logic->click_released_at_ms = now_ms;
            if (logic->click_count == 4U) {
                logic->click_count = 0U;
                return BOOT_BUTTON_EVENT_QUADRUPLE_CLICK;
            }
        }
    }

    if (pressed && logic->tracking && !logic->long_emitted &&
        (uint32_t)(now_ms - logic->pressed_at_ms) >= long_press_ms) {
        logic->long_emitted = true;
        logic->click_count = 0U;
        return BOOT_BUTTON_EVENT_LONG_PRESS;
    }

    return expired_event;
}
