// SPDX-License-Identifier: MIT
#include "environment_internal.h"

/* ME-1 tests measure the coordinator, not allocations by the console's
 * global verb registry. mod_environment_at links the real adapter. */
xiaotai_err_t environment_at_register(void) { return XIAOTAI_OK; }
