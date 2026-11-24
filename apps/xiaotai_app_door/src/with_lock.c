// SPDX-License-Identifier: MIT
#include "door_internal.h"
#include "xiaotai/modules/lock.h"

static bool configured(void) { return door_config.lock < xiaotai_lock_count(); }

static xiaotai_err_t open_lock(void) { return xiaotai_lock_open(door_config.lock, 0); }

const door_feature_t door_with_lock = {.lock_configured = configured, .open_lock = open_lock};
