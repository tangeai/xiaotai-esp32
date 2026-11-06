#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BOOT_BUTTON_EVENT_NONE = 0,
    BOOT_BUTTON_EVENT_SINGLE_CLICK,
    BOOT_BUTTON_EVENT_DOUBLE_CLICK,
    BOOT_BUTTON_EVENT_QUADRUPLE_CLICK,
    BOOT_BUTTON_EVENT_LONG_PRESS,
} boot_button_event_t;

typedef struct {
    bool released_seen;
    bool pressed;
    bool tracking;
    bool long_emitted;
    uint8_t click_count;
    uint32_t pressed_at_ms;
    uint32_t click_released_at_ms;
} boot_button_logic_t;

void boot_button_logic_init(boot_button_logic_t *logic, bool pressed,
                            uint32_t now_ms);
boot_button_event_t boot_button_logic_update(boot_button_logic_t *logic,
                                              bool pressed, uint32_t now_ms,
                                              uint32_t long_press_ms,
                                              uint32_t multi_click_ms);
