// SPDX-License-Identifier: MIT
#pragma once
/* PW7 dormant proposal. A mailbox does not interrupt native DNS/TLS/socket IO.
 * The port serializes start/stop; only the confirmed MQTT task may take.
 * The caller still owns task lifetime and must join it before freeing this. */
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>

typedef struct { _Atomic uint32_t state; } xt_mqtt_owner_mailbox_t;
enum { XT_MQTT_MAILBOX_ACTIVE = 1U, XT_MQTT_MAILBOX_PENDING = 2U };

static inline void xt_mqtt_mailbox_init(xt_mqtt_owner_mailbox_t *box)
{
    atomic_init(&box->state, 0U);
}

/* A new epoch cannot inherit a pending wake from the stopped task. */
static inline uint32_t xt_mqtt_mailbox_start(xt_mqtt_owner_mailbox_t *box)
{
    const uint32_t old = atomic_load_explicit(&box->state, memory_order_acquire);
    if (old & XT_MQTT_MAILBOX_ACTIVE) return 0;
    uint32_t epoch = (old & ~3U) + 4U;
    if (!epoch) epoch = 4U;
    atomic_store_explicit(&box->state, epoch | XT_MQTT_MAILBOX_ACTIVE, memory_order_release);
    return epoch;
}

static inline void xt_mqtt_mailbox_stop(xt_mqtt_owner_mailbox_t *box)
{
    atomic_fetch_and_explicit(&box->state, ~3U, memory_order_acq_rel);
}

static inline bool xt_mqtt_mailbox_request(xt_mqtt_owner_mailbox_t *box)
{
    uint32_t state = atomic_load_explicit(&box->state, memory_order_acquire);
    while (state & XT_MQTT_MAILBOX_ACTIVE) {
        const uint32_t next = state | XT_MQTT_MAILBOX_PENDING;
        if (atomic_compare_exchange_weak_explicit(&box->state, &state, next,
                                                  memory_order_acq_rel, memory_order_acquire)) return true;
    }
    return false;
}

static inline bool xt_mqtt_mailbox_take(xt_mqtt_owner_mailbox_t *box, uint32_t epoch)
{
    uint32_t expected = epoch | XT_MQTT_MAILBOX_ACTIVE | XT_MQTT_MAILBOX_PENDING;
    return epoch && !(epoch & 3U) && atomic_compare_exchange_strong_explicit(
        &box->state, &expected, epoch | XT_MQTT_MAILBOX_ACTIVE, memory_order_acq_rel, memory_order_acquire);
}
