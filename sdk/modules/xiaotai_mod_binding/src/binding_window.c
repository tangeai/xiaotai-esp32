// SPDX-License-Identifier: MIT
/* The code's window: how long the grant may arrive after the guest session
 * was asked for. It is the smaller of the configured wait and the temporary
 * token's remaining life, fixed once when CONNECTING begins (a reconnection or
 * a clock correction never extends it), measured on the monotonic clock. A
 * token with less than a second to live is an old code the server replayed:
 * the report is sent again instead. */
#include "binding_internal.h"
#include "xiaotai/port/clock.h"
#include "xiaotai/port/os.h"

static uint32_t s_window_timer;

xiaotai_err_t binding_window_open(binding_attempt_t *attempt, bool *stale)
{
    *stale = false;
    const xiaotai_binding_config_t *config = binding_config();
    int64_t window_ms = (int64_t)config->wait_s * 1000;
    int64_t expiry_s = 0;
    if (xiaotai_platform_token_expiry(attempt->temp_token, &expiry_s) != XIAOTAI_OK) {
        if (config->guest_token_needs_expiry) return XIAOTAI_ERR_INVALID_RESPONSE;
    } else {
        const int64_t wall_ms = xiaotai_clock_unix_ms();
        const int64_t wall_s = wall_ms / 1000;
        if (expiry_s <= wall_s && config->guest_token_needs_expiry) return XIAOTAI_ERR_TIMEOUT;
        /* Bound the scale conversion by the configured window, not the absolute exp. */
        const int64_t left_ms = expiry_s <= wall_s ? 0 : expiry_s > wall_s + config->wait_s ? window_ms :
            (expiry_s - wall_s) * 1000 - wall_ms % 1000;
        if (left_ms < BINDING_STALE_BELOW_MS) {
            *stale = true;
            return XIAOTAI_OK;
        }
        if (left_ms < window_ms) window_ms = left_ms;
    }
    attempt->deadline_us = xiaotai_time_us() + window_ms * 1000;
    return XIAOTAI_OK;
}

uint32_t binding_window_left_ms(const binding_attempt_t *attempt)
{
    if (attempt == NULL || attempt->deadline_us == 0) return 0;
    int64_t left_us = attempt->deadline_us - xiaotai_time_us();
    if (left_us <= 0) return 0;
    return (uint32_t)((left_us + 999) / 1000);
}

uint32_t binding_window_seconds(int64_t deadline_us)
{
    if (deadline_us == 0) return 0;
    int64_t left_us = deadline_us - xiaotai_time_us();
    if (left_us <= 0) return 0;
    return (uint32_t)((left_us + 999999) / 1000000);
}

static void window_end(void *ctx)
{
    s_window_timer = 0;
    if (!binding_serial_current(ctx) || binding_phase() != XIAOTAI_BINDING_WAITING) return;
    binding_expire();
}

void binding_window_forget(void) { s_window_timer = 0; }

void binding_window_stop(void)
{
    xiaotai_timer_cancel(s_window_timer);
    s_window_timer = 0;
}

void binding_window_arm(void)
{
    binding_attempt_t *attempt = binding_attempt();
    binding_window_stop();
    if (attempt == NULL) return;
    if (xiaotai_timer_start(binding_window_left_ms(attempt), window_end, binding_serial_ctx(attempt->serial),
                            &s_window_timer) != XIAOTAI_OK) {
        /* Without its end the code would be shown for ever. */
        s_window_timer = 0;
        binding_fail(XIAOTAI_ERR_NO_MEM, "grant", XIAOTAI_PLATFORM_NO_CODE);
    }
}
