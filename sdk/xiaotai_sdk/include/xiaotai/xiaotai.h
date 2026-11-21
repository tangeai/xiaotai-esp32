// SPDX-License-Identifier: MIT
#pragma once
/* XiaoTai SDK public API for products.
 *
 * A product is a board, the applications it runs and a few parameters. The
 * build selects applications, the base modules they need and the engines those
 * need; xiaotai_start() brings everything up through the runtime framework.
 *
 * Writing an application or a base module: see xiaotai/framework.h (intents,
 * events, timers, activities, settings, commands) and xiaotai/view.h
 * (screens). Only embedding capabilities in an existing firmware: call the
 * base modules' own APIs (xiaotai/modules/<name>.h) after xiaotai_start(). */
#include <stdbool.h>
#include <stdint.h>
#include "xiaotai/framework.h"
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XIAOTAI_SDK_API_VERSION 1

/* start_failure_restart_ms: stay up after a failed start, console answering. */
#define XIAOTAI_START_HALT (-1)
/* start_failure_restart_ms 0: restart this long after a failed start (the
 * failure line reaches the console first). */
#define XIAOTAI_START_RESTART_DELAY_MS 2000

/* What a product configures at run time. The build generates the product's
 * own value, xiaotai_product_config (xiaotai/selection.h), from its product
 * configuration; a firmware that embeds the SDK may pass its own. Everything
 * else a product chooses (its engines' storage layouts, retry policies,
 * tuning tables, buffer ceilings) is handed to each selected engine, module
 * and application by the generated product glue through its own
 * xiaotai_<name>_configure(). This component links no optional engine, so a
 * product without a display carries no UI code and a product without a
 * camera no video code. */
typedef struct {
    /* Application shown first; NULL: the first one in the build. */
    const char *home;
    /* Foreground activity priorities; they override the defaults the
     * selected modules declare, by name. NULL: the modules' defaults. */
    const xiaotai_priority_t *priorities;
    /* UI theme by name; NULL: the best match for the board's display. Passed
     * on to the UI system when the product has one, ignored otherwise. */
    const char *theme;
    /* Service discovery URL; NULL: the one in the product configuration. Passed
     * on to the platform core when the product has one, ignored otherwise. A
     * stored environment (xiaotai/platform.h) still replaces either. */
    const char *discovery_url;
    /* Pairs the priorities cannot express (xiaotai/framework.h); NULL: none. */
    const xiaotai_preemption_t *preemptions;
    /* After a failed start: 0 restarts the device after
     * XIAOTAI_START_RESTART_DELAY_MS (as the firmware of every family did when
     * a startup stage failed), a positive value after that many milliseconds,
     * XIAOTAI_START_HALT keeps it running with the console open for
     * diagnosis (xiaotai_start() then returns the failure). */
    int32_t start_failure_restart_ms;
    /* Opt in to local UI and audio with no network or storage writes when
     * cold NVS init reports a recoverable error. Other failures are unchanged. */
    bool local_on_storage_boot_failure;
    /* Hold one slot of the existing owner queue for terminal events. Default false. */
    bool reserve_owner_terminal;
    /* Optional local presentation after a failed framework start, once the
     * board has brought its parts up and no owner can use them. When set,
     * leave the original startup error visible and never restart unaided. */
    xiaotai_err_t (*startup_error_guide)(xiaotai_err_t error);
} xiaotai_config_t;

/* Starts the product, in this order:
 *   1. the generated product glue configures the selected engines, modules
 *      and applications (and opens the event log and the console, so a device
 *      that fails below still answers on its console);
 *   2. the board brings up what its parts share (xiaotai_hal_init) and logs
 *      "board=<model> revision=<revision>"; a board that fails here, or whose
 *      parts do not give the needs its manifest declares
 *      (xiaotai_board_declared_needs; XIAOTAI_ERR_INVALID_STATE, both masks
 *      logged), stops the start: nothing is initialized on hardware that is
 *      not there;
 *   3. the runtime framework initializes and starts the selected engines, base
 *      modules and applications against the needs the board satisfies.
 * By default returns XIAOTAI_OK once local parts run; going online continues
 * in the background. With XIAOTAI_FRAMEWORK_OWNER_ON_MAIN, the calling task
 * becomes the owner and a successful device start does not return. On a
 * failure it follows the selected startup error guide or
 * config->start_failure_restart_ms and
 * returns the failure only when it does not restart. Called from the
 * product's main task; a call during startup returns XIAOTAI_ERR_INVALID_STATE.
 * After a completed start, a second call returns its result without repeating
 * anything. config may be NULL for all defaults and is not kept
 * (its strings and tables must stay valid: static storage). */
xiaotai_err_t xiaotai_start(const xiaotai_config_t *config);

/* Host tests: stop the framework and forget the first start's result.
 * Do not reset during bootstrap; a borrowed caller can be reset after
 * system.started, even though xiaotai_start() has not returned. */
void xiaotai_reset(void);

#ifdef __cplusplus
}
#endif
