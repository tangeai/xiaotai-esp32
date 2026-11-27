// SPDX-License-Identifier: MIT
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include "xiaotai/hal.h"
#include "xiaotai/port/os.h"
#include "xiaotai/selection.h"
#include "xiaotai/xiaotai.h"

static xiaotai_task_t s_caller;
static xiaotai_signal_t s_ready;
static atomic_bool s_returned;
static bool s_refuse = true;

xiaotai_err_t xiaotai_selected_configure(const xiaotai_config_t *config)
{
    assert(config && config->start_failure_restart_ms == XIAOTAI_START_HALT);
    return s_refuse ? XIAOTAI_ERR_INVALID_ARG : XIAOTAI_OK;
}

xiaotai_err_t xiaotai_hal_init(void)
{
    assert(xiaotai_task_is_current(&s_caller) && !xiaotai_on_owner());
    return XIAOTAI_OK;
}

uint32_t xiaotai_hal_needs(void) { return XIAOTAI_NEED_EXTERNAL_RAM; }
void xiaotai_hal_identity(xiaotai_board_identity_t *identity) { *identity = (xiaotai_board_identity_t){0}; }

static void owner_is_ready(void *ctx)
{
    (void)ctx;
    assert(xiaotai_framework_owner_on_caller() && xiaotai_task_is_current(&s_caller));
    xiaotai_signal_give(&s_ready);
}

static xiaotai_err_t init(void)
{
    assert(xiaotai_framework_owner_on_caller() && xiaotai_task_is_current(&s_caller));
    return XIAOTAI_OK;
}

static void start(void) { assert(xiaotai_run(owner_is_ready, NULL) == XIAOTAI_OK); }

static const xiaotai_module_t module = {.name = "caller_sdk", .init = init, .start = start};
const xiaotai_module_t *const xiaotai_selected_modules[] = {&module, NULL};
const size_t xiaotai_selected_module_count = 1;
const xiaotai_application_t *const xiaotai_selected_applications[] = {NULL};
const size_t xiaotai_selected_application_count = 0;
const xiaotai_config_t xiaotai_product_config = {.start_failure_restart_ms = XIAOTAI_START_HALT};
const uint32_t xiaotai_board_declared_needs = XIAOTAI_NEED_EXTERNAL_RAM;

static void *caller(void *arg)
{
    (void)arg;
    assert(xiaotai_task_borrow_current(&s_caller) == XIAOTAI_OK);
    assert(xiaotai_start(&xiaotai_product_config) == XIAOTAI_OK);
    assert(xiaotai_task_is_current(&s_caller));
    atomic_store(&s_returned, true);
    return NULL;
}

int main(void)
{
    assert(xiaotai_start(&xiaotai_product_config) == XIAOTAI_ERR_INVALID_ARG);
    xiaotai_reset();
    s_refuse = false;
    assert(xiaotai_signal_init(&s_ready) == XIAOTAI_OK);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, caller, NULL) == 0);
    assert(xiaotai_signal_take(&s_ready, 2000));
    assert(!atomic_load(&s_returned) && !xiaotai_on_owner());
    assert(xiaotai_start(&xiaotai_product_config) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_reset();
    assert(pthread_join(thread, NULL) == 0 && atomic_load(&s_returned));
    xiaotai_signal_deinit(&s_ready);
    return 0;
}
