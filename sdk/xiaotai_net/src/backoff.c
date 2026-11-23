// SPDX-License-Identifier: MIT
#include "internal.h"

uint32_t net_backoff_wait_ms(const xiaotai_net_retry_policy_t *policy, uint32_t failures,
                             xiaotai_net_loss_t latest, uint32_t random, uint32_t *previous_base_ms)
{
    if (failures <= policy->fast_retries) return 0;
    const uint32_t step = failures - policy->fast_retries - 1U;
    uint32_t base;
    if (policy->step_count) {
        base = policy->steps_ms[step < policy->step_count ? step : policy->step_count - 1U];
    } else {
        const uint32_t cap = latest == XIAOTAI_NET_LOSS_CREDENTIALS || latest == XIAOTAI_NET_LOSS_SECURITY
            ? policy->max_credentials_ms : policy->max_ms;
        const bool remember = policy->grow_from_previous_base && previous_base_ms;
        const uint64_t doubled = remember
            ? (*previous_base_ms ? (uint64_t)*previous_base_ms * 2U : policy->first_ms)
            : (uint64_t)policy->first_ms << (step < 32U ? step : 32U);
        base = doubled < cap ? (uint32_t)doubled : cap;
        if (remember) *previous_base_ms = base;
    }
    const uint64_t room = (uint64_t)base * policy->jitter_percent / 100U;
    const uint64_t jitter = ((uint64_t)random * (room + 1U)) >> 32;
    const uint64_t delay = base + jitter;
    return delay > UINT32_MAX ? UINT32_MAX : (uint32_t)delay;
}

bool net_backoff_give_up(const xiaotai_net_retry_policy_t *policy, uint32_t failures)
{
    return policy->retries_max != 0U && failures > policy->retries_max;
}
