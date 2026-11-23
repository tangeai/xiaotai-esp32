// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>

/* Register production portal and captive DNS to local, socket-free I/O.
 * Must be called before the engine's init. */
void product_portal_prepare(void);
uint32_t product_portal_stack(void);
