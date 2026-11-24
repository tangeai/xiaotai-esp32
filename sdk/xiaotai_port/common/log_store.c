// SPDX-License-Identifier: MIT
/* Thresholds kept across restarts (xiaotai/port/log.h): one 16-bit value,
 * written by the storage worker, read once at start. Module thresholds and
 * third-party stacks' own log levels are never stored. */
#include <stdatomic.h>

#include "xiaotai/port/kv.h"
#include "xiaotai/port/log.h"

#define LOG_SPACE "xt_log"
#define LOG_KEY "levels"
/* app in the low byte, system in the high byte; NOTHING: no stored thresholds. */
#define NOTHING 0xffffU
#define LOAD_TIMEOUT_MS 500U

static atomic_uint s_stored = NOTHING;

static xiaotai_err_t store(unsigned value)
{
    const xiaotai_err_t err = xiaotai_kv_set_int_async(LOG_SPACE, LOG_KEY, XIAOTAI_KV_U16, (int64_t)value);
    if (err == XIAOTAI_OK) atomic_store(&s_stored, value);
    return err;
}

xiaotai_err_t xiaotai_log_save(void)
{
    return store((unsigned)xiaotai_log_level() | ((unsigned)xiaotai_log_system_level() << 8));
}

xiaotai_err_t xiaotai_log_forget(void) { return store(NOTHING); }

bool xiaotai_log_saved(uint8_t *app, uint8_t *system)
{
    const unsigned value = atomic_load(&s_stored);
    if (value == NOTHING) return false;
    if (app) *app = (uint8_t)(value & 0xffU);
    if (system) *system = (uint8_t)(value >> 8);
    return true;
}

void xiaotai_log_load(void)
{
    int64_t value = NOTHING;
    if (xiaotai_kv_get_int(LOG_SPACE, LOG_KEY, XIAOTAI_KV_U16, &value, LOAD_TIMEOUT_MS) != XIAOTAI_OK) return;
    if (value < 0 || value >= (int64_t)NOTHING) return;
    /* A value another build stored may be out of this build's range: each
     * threshold is applied only when this build accepts it. */
    const uint8_t app = (uint8_t)((unsigned)value & 0xffU);
    const uint8_t system = (uint8_t)((unsigned)value >> 8);
    const bool app_ok = xiaotai_log_set_level(app) == XIAOTAI_OK;
    const bool system_ok = xiaotai_log_set_system_level(system) == XIAOTAI_OK;
    if (app_ok && system_ok) atomic_store(&s_stored, (unsigned)value);
}
