// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Unsanitized POSIX host only. Measures stack bytes touched, including
 * context setup; run board stack checks separately. No recursive measure. */
bool xiaotai_host_stack_measure(void (*run)(void *ctx), void *ctx, size_t *used);
