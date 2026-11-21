// SPDX-License-Identifier: MIT
/* Driver registry: one driver per network kind, registered by driver
 * components (and tests) in their init and by the engine's own init for the
 * kinds nobody took. The engine's start() binds the driver of the part in
 * use and closes registration. Written before start on the owner task (or
 * a test's main task before the framework runs), read afterwards. */
#include <stdatomic.h>
#include <string.h>
#include "internal.h"

/* Kinds of xiaotai_network_kind_t a driver may register for; the enum is
 * appended to as kinds are added, the table grows with it. */
#define DRIVER_KINDS 8U

static const xiaotai_net_driver_t *s_drivers[DRIVER_KINDS];
static const xiaotai_net_driver_t *_Atomic s_bound;
static atomic_bool s_closed;

/* Names show up in diagnostics lines: [a-z0-9_], 1..15 characters. */
static bool name_valid(const char *name)
{
    if (!name) return false;
    const size_t length = strnlen(name, 16U);
    if (length == 0U || length > 15U) return false;
    for (size_t i = 0; i < length; ++i) {
        const char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

xiaotai_err_t xiaotai_net_driver_register(const xiaotai_net_driver_t *driver)
{
    if (!driver || !driver->start || !driver->join || !driver->leave || !name_valid(driver->name)) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    if ((unsigned)driver->kind >= DRIVER_KINDS) return XIAOTAI_ERR_INVALID_ARG;
    const uint32_t capabilities = driver->capabilities;
    if ((capabilities & XIAOTAI_NET_DRIVER_SCAN) &&
        (!driver->scan || !driver->scan_cancel || !driver->scan_read)) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    if ((capabilities & XIAOTAI_NET_DRIVER_ACCESS_POINT) && (!driver->ap_start || !driver->ap_stop)) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    if ((capabilities & XIAOTAI_NET_DRIVER_SIGNAL) && !driver->signal) return XIAOTAI_ERR_INVALID_ARG;
    if (atomic_load(&s_closed)) return XIAOTAI_ERR_INVALID_STATE;
    if (s_drivers[driver->kind]) return XIAOTAI_ERR_INVALID_STATE;
    s_drivers[driver->kind] = driver;
    return XIAOTAI_OK;
}

const xiaotai_net_driver_t *xiaotai_net_driver_bound(void) { return atomic_load(&s_bound); }

const xiaotai_net_driver_t *net_driver_for(xiaotai_network_kind_t kind)
{
    if ((unsigned)kind >= DRIVER_KINDS) return NULL;
    return s_drivers[kind];
}

void net_drivers_bind(const xiaotai_net_driver_t *bound)
{
    atomic_store(&s_closed, true);
    atomic_store(&s_bound, bound);
}

void net_drivers_reset(void)
{
    memset(s_drivers, 0, sizeof(s_drivers));
    atomic_store(&s_bound, NULL);
    atomic_store(&s_closed, false);
}
