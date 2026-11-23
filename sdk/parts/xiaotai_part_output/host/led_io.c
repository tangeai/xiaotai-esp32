// SPDX-License-Identifier: MIT
#include "led_io.h"
#include "led_io_fake.h"
#include <string.h>

struct xiaotai_led_io {
    bool used;
    unsigned calls;
    size_t bytes;
    uint8_t frame[XT_LED_BYTES_MAX];
    xiaotai_err_t fail_next;
};

#define SIMULATED_LINES 128U
static xiaotai_led_io_t pins[SIMULATED_LINES];

xiaotai_err_t xiaotai_led_io_create(int pin, xiaotai_led_io_t **io)
{
    *io = NULL;
    if (pin < 0 || (unsigned)pin >= SIMULATED_LINES) return XIAOTAI_ERR_INVALID_ARG;
    if (pins[pin].used) return XIAOTAI_ERR_INVALID_STATE;
    pins[pin].used = true;
    *io = &pins[pin];
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_led_io_send(xiaotai_led_io_t *io, const uint8_t *data, size_t bytes)
{
    const xiaotai_err_t result = io->fail_next;
    io->fail_next = XIAOTAI_OK;
    ++io->calls;
    if (result == XIAOTAI_OK) {
        memcpy(io->frame, data, bytes);
        io->bytes = bytes;
    }
    return result;
}

void xiaotai_led_fake_fail(int pin, xiaotai_err_t err) { pins[pin].fail_next = err; }

size_t xiaotai_led_fake_frame(int pin, uint8_t *frame, unsigned *calls)
{
    const xiaotai_led_io_t *io = &pins[pin];
    *calls = io->calls;
    memcpy(frame, io->frame, io->bytes);
    return io->bytes;
}
