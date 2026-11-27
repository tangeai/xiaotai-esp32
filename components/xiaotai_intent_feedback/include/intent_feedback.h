#pragma once

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>

// The input owner issues IDs in order; zero is reserved for untracked actions.
static inline uint32_t xiaotai_intent_next_id(uint32_t previous)
{
    return previous == UINT32_MAX ? 1U : previous + 1U;
}

// One business owner reports rejections in queue order. Only the latest
// rejection matters; an accepted enqueue is not a completed network action.
typedef _Atomic(uint32_t) xiaotai_intent_rejections_t;

static inline void xiaotai_intent_reject(xiaotai_intent_rejections_t *pending, uint32_t id)
{
    if (id) atomic_store_explicit(pending, id, memory_order_release);
}

static inline uint32_t xiaotai_intent_take_rejection(xiaotai_intent_rejections_t *pending)
{
    return atomic_exchange_explicit(pending, 0U, memory_order_acq_rel);
}

static inline bool xiaotai_intent_rejection_matches(uint32_t rejected, uint32_t current)
{
    return rejected != 0 && rejected == current;
}
