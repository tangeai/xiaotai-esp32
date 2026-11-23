// SPDX-License-Identifier: MIT
#ifndef XIAOTAI_TRANSPORT_DIAG_MINI_H
#define XIAOTAI_TRANSPORT_DIAG_MINI_H

#include <stdbool.h>
#include "xiaotai/framework.h"

void xt_diag_mini_enable(bool enabled);
void xt_diag_mini_report(const xiaotai_reply_t *reply);

#endif
