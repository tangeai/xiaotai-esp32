// SPDX-License-Identifier: MIT
#include "led_io_fake.h"
#include "xiaotai_mem_host.h"
#include <assert.h>
#include <string.h>

typedef struct { bool active; unsigned writes; xiaotai_err_t result; } line_t;
static line_t relay, lamp;

static xiaotai_err_t drive(void *arg, bool active)
{
    line_t *line = arg;
    ++line->writes;
    if (line->result == XIAOTAI_OK) line->active = active;
    return line->result;
}

static const xiaotai_output_t *door, *light, *small, *full;

int main(void)
{
    const xiaotai_output_line_config_t d = {.kind = XIAOTAI_OUTPUT_RELAY, .role = "door",
        .line = {.set = drive, .self = &relay}};
    relay.active = true;
    assert(xiaotai_output_line_create(&d, &door) == XIAOTAI_OK);
    assert(door->desc.kind == XIAOTAI_OUTPUT_RELAY && door->desc.pixels == 1 && !door->write);
    assert(!relay.active && relay.writes == 1);
    assert(door->set(door->self, 20, 0, 0) == XIAOTAI_OK && relay.active);
    assert(door->set(door->self, 0, 30, 30) == XIAOTAI_OK && !relay.active);
    assert(xiaotai_led_strip_failures(door) == 0);
    const xiaotai_output_line_config_t l = {.kind = XIAOTAI_OUTPUT_LIGHT, .role = "status",
        .line = {.set = drive, .self = &lamp}, .initially_active = true};
    lamp.result = XIAOTAI_ERR_TIMEOUT;
    xiaotai_mem_host_clear();
    assert(xiaotai_output_line_create(&l, &light) == XIAOTAI_ERR_TIMEOUT && !light && !lamp.active);
    assert(xiaotai_mem_host_outstanding() == 0);
    lamp.result = XIAOTAI_OK;
    assert(xiaotai_output_line_create(&l, &light) == XIAOTAI_OK && lamp.active);

    const xiaotai_led_strip_config_t g = {.pin = 1, .pixels = 2, .role = "status", .brightness = 128};
    assert(xiaotai_led_strip_create(&g, &small) == XIAOTAI_OK && small->desc.kind == XIAOTAI_OUTPUT_RGB_LIGHT);
    uint8_t actual[XT_LED_BYTES_MAX];
    unsigned calls = 0;
    const uint8_t color[] = {255, 2, 5, 7, 13, 17};
    assert(small->write(small->self, color, 1) == XIAOTAI_ERR_INVALID_SIZE);
    assert(small->write(small->self, NULL, 2) == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_led_fake_frame(1, actual, &calls) == 0 && calls == 0);
    assert(small->write(small->self, color, 2) == XIAOTAI_OK);
    assert(xiaotai_led_fake_frame(1, actual, &calls) == 6 && calls == 1);
    const uint8_t expect[] = {1, 128, 3, 7, 4, 9};
    assert(memcmp(actual, expect, sizeof(expect)) == 0);
    xiaotai_led_fake_fail(1, XIAOTAI_ERR_TIMEOUT);
    assert(small->set(small->self, 0, 0, 0) == XIAOTAI_ERR_TIMEOUT);
    assert(xiaotai_led_strip_failures(small) == 1);
    assert(xiaotai_led_fake_frame(1, actual, &calls) == 6 && calls == 2);
    assert(memcmp(actual, expect, sizeof(expect)) == 0);

    const xiaotai_led_strip_config_t r = {.pin = 2, .pixels = 16, .role = "status", .order = XIAOTAI_LED_ORDER_RGB};
    assert(xiaotai_led_strip_create(&r, &full) == XIAOTAI_OK);
    assert(full->set(full->self, 25, 26, 255) == XIAOTAI_OK);
    assert(xiaotai_led_fake_frame(2, actual, &calls) == sizeof(actual) && calls == 1);
    for (size_t i = 0; i < sizeof(actual); i += 3U)
        assert(actual[i] == 25 && actual[i + 1U] == 26 && actual[i + 2U] == 255);
    const xiaotai_output_t *invalid = full;
    assert(xiaotai_led_strip_create(&r, &invalid) == XIAOTAI_ERR_INVALID_STATE && !invalid);
    assert(xiaotai_led_strip_create(&(xiaotai_led_strip_config_t){.pin = 0, .pixels = 17, .role = "status"},
                                   &invalid) == XIAOTAI_ERR_INVALID_ARG && !invalid);
    return 0;
}
