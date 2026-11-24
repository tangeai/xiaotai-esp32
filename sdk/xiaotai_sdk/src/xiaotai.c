// SPDX-License-Identifier: MIT
/* xiaotai_start(): product glue, board, framework, in that order. This file
 * names no engine: what a product runs is in the generated tables
 * (xiaotai/selection.h). */
#include <stdbool.h>
#include <stdatomic.h>
#include "xiaotai/framework_config.h"
#include "xiaotai/hal.h"
#include "xiaotai/port/log.h"
#include "xiaotai/port/os.h"
#include "xiaotai/port/sys.h"
#include "xiaotai/selection.h"
#include "xiaotai/xiaotai.h"

static const char *TAG = "xiaotai";
static atomic_bool s_starting;
static atomic_bool s_started;
static _Atomic xiaotai_err_t s_result;

/* The first line of every start: which firmware this is. The eight digits of
 * the ELF digest are what a bench compares with its build record. */
static void announce(void)
{
    xiaotai_firmware_t firmware;
    xiaotai_firmware(&firmware);
    XIAOTAI_LOGI(TAG, "XiaoTai %s on %s, %s, elf=%.8s", firmware.version ? firmware.version : "?",
                 firmware.chip ? firmware.chip : "?", firmware.platform ? firmware.platform : "?",
                 firmware.elf_sha256[0] ? firmware.elf_sha256 : "unknown");
}

static xiaotai_err_t bring_up(const xiaotai_config_t *config, bool *board_ready)
{
    xiaotai_log_start();
    announce();
    xiaotai_err_t err = xiaotai_selected_configure(config);
    if (err != XIAOTAI_OK) {
        XIAOTAI_LOGE(TAG, "product configuration refused: %d", err);
        return err;
    }
    /* Before the owner task exists: a board that cannot bring up what its
     * parts share must not have engines opening those parts. */
    err = xiaotai_hal_init();
    if (err != XIAOTAI_OK) {
        XIAOTAI_LOGE(TAG, "board init failed: %d", err);
        return err;
    }
    xiaotai_board_identity_t identity = {0};
    xiaotai_hal_identity(&identity);
    XIAOTAI_LOGI(TAG, "board=%s revision=%s", identity.model ? identity.model : "",
                 identity.revision ? identity.revision : "");
    const uint32_t needs = xiaotai_hal_needs();
    if (needs != xiaotai_board_declared_needs) {
        XIAOTAI_LOGE(TAG, "board parts give needs 0x%lx, its manifest declares 0x%lx", (unsigned long)needs,
                     (unsigned long)xiaotai_board_declared_needs);
        return XIAOTAI_ERR_INVALID_STATE;
    }
    *board_ready = true;
    const xiaotai_framework_config_t framework = {
        .modules = xiaotai_selected_modules,
        .module_count = xiaotai_selected_module_count,
        .applications = xiaotai_selected_applications,
        .application_count = xiaotai_selected_application_count,
        .home = config->home,
        .priorities = config->priorities,
        .preemptions = config->preemptions,
        .available_needs = needs,
        .local_on_storage_boot_failure = config->local_on_storage_boot_failure,
        .reserve_owner_terminal = config->reserve_owner_terminal,
    };
    err = XIAOTAI_FRAMEWORK_OWNER_ON_MAIN ? xiaotai_framework_run_on_caller(&framework)
                                         : xiaotai_framework_start(&framework);
    if (err != XIAOTAI_OK) XIAOTAI_LOGE(TAG, "framework start failed: %d", err);
    return err;
}

xiaotai_err_t xiaotai_start(const xiaotai_config_t *config)
{
    static const xiaotai_config_t defaults;
    if (!config) config = &defaults;
    if (atomic_exchange(&s_starting, true)) return XIAOTAI_ERR_INVALID_STATE;
    if (atomic_load(&s_started)) {
        const xiaotai_err_t cached = atomic_load(&s_result);
        atomic_store(&s_starting, false);
        return cached;
    }
    atomic_store(&s_started, true);
    bool board_ready = false;
    const xiaotai_err_t result = bring_up(config, &board_ready);
    if (result != XIAOTAI_OK && config->startup_error_guide) {
        const xiaotai_err_t guide = board_ready && !xiaotai_framework_active()
                                       ? config->startup_error_guide(result) : XIAOTAI_ERR_INVALID_STATE;
        if (guide != XIAOTAI_OK) XIAOTAI_LOGE(TAG, "startup guide unavailable: %d (startup error %d)", guide, result);
        else XIAOTAI_LOGI(TAG, "startup guide shown (startup error %d)", result);
    }
    atomic_store(&s_result, result);
    atomic_store(&s_starting, false);
    if (result != XIAOTAI_OK && !config->startup_error_guide && config->start_failure_restart_ms != XIAOTAI_START_HALT) {
        const uint32_t delay = config->start_failure_restart_ms > 0 ? (uint32_t)config->start_failure_restart_ms
                                                                     : XIAOTAI_START_RESTART_DELAY_MS;
        XIAOTAI_LOGE(TAG, "start failed (%d): restarting in %lu ms", result, (unsigned long)delay);
        xiaotai_delay_ms(delay);
        /* Through a pointer: the port's restart never returns, a host test's
         * does, and the code after the call has to exist for it. */
        void (*volatile restart)(void) = xiaotai_restart;
        restart();
    }
    return result;
}

void xiaotai_reset(void)
{
    xiaotai_framework_reset();
    atomic_store(&s_result, XIAOTAI_OK);
    atomic_store(&s_started, false);
}
