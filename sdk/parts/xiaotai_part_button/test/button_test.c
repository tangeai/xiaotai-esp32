// SPDX-License-Identifier: MIT
// Replaces legacy: tests/mz28/test_boot_button_logic.c::boot_button_logic
// Replaces legacy: tests/regression/test_s3_user_button.py
#include "xiaotai/parts/button.h"
#include "xiaotai_bus_host.h"
#include "xiaotai_mem_host.h"
#include <assert.h>

typedef struct { bool active, fail; unsigned reads; } fake_line_t;

static xiaotai_err_t read_line(void *arg, bool *active)
{
    fake_line_t *line = arg;
    ++line->reads;
    *active = line->active;
    return line->fail ? XIAOTAI_FAIL : XIAOTAI_OK;
}

static const xiaotai_input_t *button, *contact;

static void quiet(const xiaotai_input_t *input, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) assert(input->poll(input->self) == 0);
}

static void alternate_gpio(void)
{
    const xiaotai_gpio_config_t gpio = {.pin = 9, .pull_down = true};
    xiaotai_line_t line;
    assert(xiaotai_line_gpio_input(&gpio, &line) == XIAOTAI_OK);
    const xiaotai_bus_host_pin_t pin = xiaotai_bus_host_pin(gpio.pin);
    assert(pin.configured && !pin.output && pin.pull_down && !pin.pull_up);

    const xiaotai_button_config_t config = {.role = "main", .line = line,
                                           .debounce_ms = 50 + XIAOTAI_INPUT_POLL_MS};
    const xiaotai_input_t *key;
    assert(xiaotai_button_create(&config, &key) == XIAOTAI_OK);
    xiaotai_bus_host_pin_drive(gpio.pin, true);
    assert(key->open(key->self) == XIAOTAI_OK);
    for (unsigned tick = 1; tick <= 104; ++tick) {
        const bool pressed = tick < 12 || tick == 22 || (tick >= 26 && tick < 47) ||
                             tick == 48 || (tick >= 60 && tick < 72) ||
                             (tick >= 84 && tick < 96);
        xiaotai_bus_host_pin_drive(gpio.pin, pressed);
        const uint32_t expected = tick == 32 || tick == 66 || tick == 90 ? XIAOTAI_INPUT_PRESSED :
                                  tick == 55 || tick == 78 || tick == 102 ? XIAOTAI_INPUT_RELEASED : 0;
        assert(key->poll(key->self) == expected);
    }
}

int main(void)
{
    alternate_gpio();
    xiaotai_debounce_t db = {0};
    assert(xiaotai_debounce_step(&db, false, 10, 50) == 0);
    assert(xiaotai_debounce_step(&db, true, 10, 50) == 0);
    assert(xiaotai_debounce_step(&db, true, 40, 50) == 0);
    assert(xiaotai_debounce_step(&db, false, 10, 50) == 0); /* Bounce abandons candidate. */
    assert(xiaotai_debounce_step(&db, true, 10, 50) == 0);
    assert(xiaotai_debounce_step(&db, true, 49, 50) == 0);
    assert(xiaotai_debounce_step(&db, true, 1, 50) == XIAOTAI_INPUT_PRESSED);
    assert(xiaotai_debounce_step(&db, true, 65535, 50) == 0);
    assert(xiaotai_debounce_step(&db, false, 10, 65535) == 0);
    assert(xiaotai_debounce_step(&db, false, 65534, 65535) == 0);
    assert(xiaotai_debounce_step(&db, false, 20, 65535) == XIAOTAI_INPUT_RELEASED);

    static fake_line_t key = {.active = true};
    const xiaotai_button_config_t c = {.role = "main", .line = {.get = read_line, .self = &key}, .debounce_ms = 30};
    assert(xiaotai_button_create(&c, &button) == XIAOTAI_OK);
    assert(button->desc.kind == XIAOTAI_INPUT_BUTTON &&
           button->desc.events == (XIAOTAI_INPUT_PRESSED | XIAOTAI_INPUT_RELEASED));
    quiet(button, 2);
    assert(key.reads == 0); /* Create does not touch the line. */
    assert(button->open(button->self) == XIAOTAI_OK);
    quiet(button, 8000U / XIAOTAI_INPUT_POLL_MS); /* Held past the MZ28 long-press deadline at boot. */
    key.active = false;
    quiet(button, 5); /* No startup RELEASED event without its matching press. */
    key.active = true;
    quiet(button, 3);
    assert(button->poll(button->self) == XIAOTAI_INPUT_PRESSED);
    key.active = false;
    quiet(button, 2);
    key.fail = true;
    quiet(button, 3);
    key.fail = false;
    quiet(button, 3);
    assert(button->poll(button->self) == XIAOTAI_INPUT_RELEASED);

    static fake_line_t door = {.active = true};
    const xiaotai_button_config_t d = {.role = "door_contact", .line = {.get = read_line, .self = &door},
                                      .contact = true};
    assert(xiaotai_button_create(&d, &contact) == XIAOTAI_OK);
    assert(contact->desc.kind == XIAOTAI_INPUT_CONTACT);
    door.fail = true;
    assert(contact->open(contact->self) == XIAOTAI_FAIL);
    quiet(contact, 2);
    assert(door.reads == 1);
    door.fail = false;
    assert(contact->open(contact->self) == XIAOTAI_OK);
    quiet(contact, 4);
    assert(contact->poll(contact->self) == XIAOTAI_INPUT_PRESSED);
    quiet(contact, 1);
    door.active = false;
    quiet(contact, 5);
    assert(contact->poll(contact->self) == XIAOTAI_INPUT_RELEASED);
    const xiaotai_input_t *invalid = contact;
    assert(xiaotai_button_create(&(xiaotai_button_config_t){.role = "main"}, &invalid) == XIAOTAI_ERR_INVALID_ARG);
    assert(invalid == NULL);
    xiaotai_mem_host_fail_nth(1);
    assert(xiaotai_button_create(&c, &invalid) == XIAOTAI_ERR_NO_MEM && invalid == NULL);

    /* A contact that drops out under the finger: up 60 ms is still one press,
     * a release counts after 80 ms; a press still counts after 30 ms. */
    static fake_line_t worn = {0};
    const xiaotai_button_config_t w = {.role = "main", .line = {.get = read_line, .self = &worn},
                                       .debounce_ms = 30, .release_ms = 80};
    const xiaotai_input_t *slow;
    assert(xiaotai_button_create(&w, &slow) == XIAOTAI_OK && slow->open(slow->self) == XIAOTAI_OK);
    worn.active = true;
    quiet(slow, 3);
    assert(slow->poll(slow->self) == XIAOTAI_INPUT_PRESSED);
    worn.active = false;
    quiet(slow, 6);
    worn.active = true;
    quiet(slow, 5);
    worn.active = false;
    quiet(slow, 8);
    assert(slow->poll(slow->self) == XIAOTAI_INPUT_RELEASED);
    worn.active = true;
    quiet(slow, 3);
    assert(slow->poll(slow->self) == XIAOTAI_INPUT_PRESSED);
    return 0;
}
