// SPDX-License-Identifier: MIT
#pragma once
#include "internal.h"

static inline void net_store_wipe(void *data, size_t size)
{
    volatile unsigned char *bytes = data;
    while (size--) *bytes++ = 0;
}

/* [worker] No new history may be queued in this boot after an erase. */
void net_history_suspend(void);

/* [worker] Copies at most capacity saved names, never their credentials. */
xiaotai_err_t net_history_list(xiaotai_net_network_t *out, size_t capacity, size_t *count);
