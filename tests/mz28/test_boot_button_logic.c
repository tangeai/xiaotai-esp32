#include <assert.h>
#include <stdio.h>

#include "boot_button_logic.h"

static void test_short_press(void)
{
    boot_button_logic_t logic;
    boot_button_logic_init(&logic, false, 0);
    assert(boot_button_logic_update(&logic, true, 100, 7000, 350) ==
           BOOT_BUTTON_EVENT_NONE);
    assert(boot_button_logic_update(&logic, true, 1000, 7000, 350) ==
           BOOT_BUTTON_EVENT_NONE);
    assert(boot_button_logic_update(&logic, false, 1100, 7000, 350) ==
           BOOT_BUTTON_EVENT_NONE);
    assert(boot_button_logic_update(&logic, false, 1449, 7000, 350) ==
           BOOT_BUTTON_EVENT_NONE);
    assert(boot_button_logic_update(&logic, false, 1450, 7000, 350) ==
           BOOT_BUTTON_EVENT_SINGLE_CLICK);
}

static void test_double_click(void)
{
    boot_button_logic_t logic;
    boot_button_logic_init(&logic, false, 0);
    assert(boot_button_logic_update(&logic, true, 100, 7000, 350) ==
           BOOT_BUTTON_EVENT_NONE);
    assert(boot_button_logic_update(&logic, false, 160, 7000, 350) ==
           BOOT_BUTTON_EVENT_NONE);
    assert(boot_button_logic_update(&logic, true, 280, 7000, 350) ==
           BOOT_BUTTON_EVENT_NONE);
    assert(boot_button_logic_update(&logic, false, 340, 7000, 350) ==
           BOOT_BUTTON_EVENT_NONE);
    assert(boot_button_logic_update(&logic, false, 689, 7000, 350) ==
           BOOT_BUTTON_EVENT_NONE);
    assert(boot_button_logic_update(&logic, false, 690, 7000, 350) ==
           BOOT_BUTTON_EVENT_DOUBLE_CLICK);
}

static void test_quadruple_click(void)
{
    boot_button_logic_t logic;
    boot_button_logic_init(&logic, false, 0);
    const uint32_t press_at[] = {100, 240, 380, 520};
    const uint32_t release_at[] = {160, 300, 440, 580};
    for (size_t i = 0; i < 4; ++i) {
        assert(boot_button_logic_update(&logic, true, press_at[i], 7000,
                                        350) == BOOT_BUTTON_EVENT_NONE);
        boot_button_event_t event = boot_button_logic_update(
            &logic, false, release_at[i], 7000, 350);
        assert(event == (i == 3 ? BOOT_BUTTON_EVENT_QUADRUPLE_CLICK
                               : BOOT_BUTTON_EVENT_NONE));
    }
    assert(boot_button_logic_update(&logic, false, 1000, 7000, 350) ==
           BOOT_BUTTON_EVENT_NONE);
}

static void test_triple_click_is_ignored(void)
{
    boot_button_logic_t logic;
    boot_button_logic_init(&logic, false, 0);
    assert(boot_button_logic_update(&logic, true, 100, 7000, 350) ==
           BOOT_BUTTON_EVENT_NONE);
    assert(boot_button_logic_update(&logic, false, 150, 7000, 350) ==
           BOOT_BUTTON_EVENT_NONE);
    assert(boot_button_logic_update(&logic, true, 230, 7000, 350) ==
           BOOT_BUTTON_EVENT_NONE);
    assert(boot_button_logic_update(&logic, false, 280, 7000, 350) ==
           BOOT_BUTTON_EVENT_NONE);
    assert(boot_button_logic_update(&logic, true, 360, 7000, 350) ==
           BOOT_BUTTON_EVENT_NONE);
    assert(boot_button_logic_update(&logic, false, 410, 7000, 350) ==
           BOOT_BUTTON_EVENT_NONE);
    assert(boot_button_logic_update(&logic, false, 760, 7000, 350) ==
           BOOT_BUTTON_EVENT_NONE);
}

static void test_long_press_fires_once(void)
{
    boot_button_logic_t logic;
    boot_button_logic_init(&logic, false, 0);
    assert(boot_button_logic_update(&logic, true, 100, 7000, 350) ==
           BOOT_BUTTON_EVENT_NONE);
    assert(boot_button_logic_update(&logic, true, 7099, 7000, 350) ==
           BOOT_BUTTON_EVENT_NONE);
    assert(boot_button_logic_update(&logic, true, 7100, 7000, 350) ==
           BOOT_BUTTON_EVENT_LONG_PRESS);
    assert(boot_button_logic_update(&logic, true, 9000, 7000, 350) ==
           BOOT_BUTTON_EVENT_NONE);
    assert(boot_button_logic_update(&logic, false, 9010, 7000, 350) ==
           BOOT_BUTTON_EVENT_NONE);
}

static void test_long_press_cancels_pending_click(void)
{
    boot_button_logic_t logic;
    boot_button_logic_init(&logic, false, 0);
    assert(boot_button_logic_update(&logic, true, 100, 7000, 350) ==
           BOOT_BUTTON_EVENT_NONE);
    assert(boot_button_logic_update(&logic, false, 150, 7000, 350) ==
           BOOT_BUTTON_EVENT_NONE);
    assert(boot_button_logic_update(&logic, true, 250, 7000, 350) ==
           BOOT_BUTTON_EVENT_NONE);
    assert(boot_button_logic_update(&logic, true, 600, 7000, 350) ==
           BOOT_BUTTON_EVENT_NONE);
    assert(boot_button_logic_update(&logic, true, 7250, 7000, 350) ==
           BOOT_BUTTON_EVENT_LONG_PRESS);
    assert(boot_button_logic_update(&logic, false, 7300, 7000, 350) ==
           BOOT_BUTTON_EVENT_NONE);
}

static void test_held_during_boot_is_ignored(void)
{
    boot_button_logic_t logic;
    boot_button_logic_init(&logic, true, 0);
    assert(boot_button_logic_update(&logic, true, 8000, 7000, 350) ==
           BOOT_BUTTON_EVENT_NONE);
    assert(boot_button_logic_update(&logic, false, 8100, 7000, 350) ==
           BOOT_BUTTON_EVENT_NONE);
    assert(boot_button_logic_update(&logic, true, 8200, 7000, 350) ==
           BOOT_BUTTON_EVENT_NONE);
    assert(boot_button_logic_update(&logic, false, 8300, 7000, 350) ==
           BOOT_BUTTON_EVENT_NONE);
    assert(boot_button_logic_update(&logic, false, 8650, 7000, 350) ==
           BOOT_BUTTON_EVENT_SINGLE_CLICK);
}

int main(void)
{
    test_short_press();
    test_double_click();
    test_quadruple_click();
    test_triple_click_is_ignored();
    test_long_press_fires_once();
    test_long_press_cancels_pending_click();
    test_held_during_boot_is_ignored();
    puts("boot button logic: ok");
    return 0;
}
