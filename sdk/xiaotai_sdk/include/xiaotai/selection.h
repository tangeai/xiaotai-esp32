// SPDX-License-Identifier: MIT
#pragma once
/* What the build generates for one product, and xiaotai_start() consumes.
 *
 * The selection script (scripts/sdk_selection.py) resolves the product's
 * applications into base modules and engines and writes one C file that
 * defines everything below from the manifests, the board package's manifest
 * and the product configuration. Nothing here is written by hand, and nothing
 * else in the SDK names a selected component: an engine, module or
 * application that is not selected is not compiled and appears in none of
 * these.
 *
 * Host tests of xiaotai_start() define the same symbols themselves. */
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/framework.h"
#include "xiaotai/port/err.h"
#include "xiaotai/xiaotai.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Selected engines first (the event log, the console and identity before
 * every other one), then base modules, each after what it depends on;
 * NULL-terminated. */
extern const xiaotai_module_t *const xiaotai_selected_modules[];
extern const size_t xiaotai_selected_module_count;
/* Selected applications, in the product's order; NULL-terminated. */
extern const xiaotai_application_t *const xiaotai_selected_applications[];
extern const size_t xiaotai_selected_application_count;

/* The product's run-time configuration (home application, priorities and
 * preemption rules, start-failure policy). The product's main passes it to
 * xiaotai_start(); the generated xiaotai_product.h declares it too. */
extern const xiaotai_config_t xiaotai_product_config;

/* XIAOTAI_NEED_* the board package's manifest declares (board.json, by the
 * rules of sdk/xiaotai_hal/needs.json). xiaotai_start() refuses to start when
 * the parts the board creates give other needs (xiaotai_hal_needs()). */
extern const uint32_t xiaotai_board_declared_needs;

/* [glue] The product glue. Called once by xiaotai_start(), on the product's
 * main task, before the board is initialized and before the framework
 * starts. It calls every configure function declared in the "configure"
 * entries of the selected units' manifests (engines, then modules, then
 * applications, in initialization order) with the product's parameters; an
 * entry's "overrides" let a non-NULL member of config (theme, discovery_url)
 * replace the product's value. The event log and the console are configured
 * first and opened, so a later failure is still reported on the console.
 * It touches no part of the board and starts nothing else. Returns the first
 * error; xiaotai_start() then stops. config is never NULL. */
xiaotai_err_t xiaotai_selected_configure(const xiaotai_config_t *config);

#ifdef __cplusplus
}
#endif
