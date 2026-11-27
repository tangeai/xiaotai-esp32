// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include "product_host.h"
#include "xiaotai_product.h"

#define PRODUCT_TEST_SSID "offline-product"
#define PRODUCT_TEST_PASSWORD "offline-password"

#ifdef __cplusplus
extern "C" {
#endif

/* Dispatch to the real owner and wait for the supplied invocation. */
void product_on_owner(void (*fn)(void *), void *ctx);
/* Prove that the started SDK still serves work on the selected owner. */
void product_check_owner(void);
void product_board_prepare(void);
/* Permit exactly one host reboot substitute for a deliberate restart case. */
void product_expect_restart(void);
bool product_restarted(void);
/* Design-review pictures (product_screens.c, tools/ui_preview.py). */
void product_screens_prepare(bool provisioned);
void product_screens_run(const char *scenario, bool provisioned);
void product_screens_gallery(void);

#ifdef __cplusplus
}
#endif
