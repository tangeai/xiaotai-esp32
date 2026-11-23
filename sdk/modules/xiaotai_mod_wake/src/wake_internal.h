// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/modules/wake.h"
#include "xiaotai/wake.h"

xiaotai_err_t local_wake_at_register(void);
const char *local_wake_outcome_name(xiaotai_local_wake_outcome_t outcome);
xiaotai_local_wake_outcome_t local_wake_inject(const xiaotai_wake_detection_t *phrase);
xiaotai_err_t local_wake_gate_error(void);
