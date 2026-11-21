// SPDX-License-Identifier: MIT
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include "xiaotai/framework.h"
#include "xiaotai/port/os.h"

static xiaotai_task_t caller;
static xiaotai_signal_t initialized, delivered;
static atomic_bool returned;
static unsigned timer_runs, jobs;

static void tick(void *ctx)
{
    (void)ctx;
    assert(xiaotai_framework_owner_on_caller() && xiaotai_task_is_current(&caller));
    if (++timer_runs == 2) xiaotai_signal_give(&delivered);
    else {
        uint32_t id;
        assert(xiaotai_timer_start(0, tick, NULL, &id) == XIAOTAI_OK);
    }
}

static void handle_job(void *ctx)
{
    assert(xiaotai_framework_owner_on_caller() && xiaotai_task_is_current(&caller) && ctx == &jobs);
    ++jobs;
    xiaotai_signal_give(&delivered);
}

static xiaotai_err_t init(void)
{
    assert(xiaotai_framework_owner_on_caller() && xiaotai_task_is_current(&caller));
    return XIAOTAI_OK;
}

static void start(void)
{
    uint32_t id;
    assert(xiaotai_framework_owner_on_caller() && xiaotai_task_is_current(&caller));
    assert(xiaotai_timer_start(0, tick, NULL, &id) == XIAOTAI_OK);
    xiaotai_signal_give(&initialized);
}

static xiaotai_err_t fail(void)
{
    assert(xiaotai_on_owner());
    return XIAOTAI_ERR_NO_MEM;
}

static const xiaotai_module_t test_module = {.name = "caller_owner_test", .init = init, .start = start};
static const xiaotai_module_t failing_module = {.name = "caller_owner_fail", .init = fail};

static void *run(void *arg)
{
    assert(xiaotai_task_borrow_current(&caller) == XIAOTAI_OK);
    /* A preexisting notification belongs to the caller, not to the framework. */
    xiaotai_task_notify(&caller);
    assert(xiaotai_framework_run_on_caller(arg) == XIAOTAI_OK);
    assert(xiaotai_task_wait_notify(0));
    assert(xiaotai_task_is_current(&caller));
    atomic_store(&returned, true);
    return NULL;
}

int main(void)
{
    const xiaotai_module_t *failing[] = {&failing_module};
    assert(xiaotai_framework_run_on_caller(&(xiaotai_framework_config_t){
        .modules = failing, .module_count = 1}) == XIAOTAI_ERR_NO_MEM);
    assert(!xiaotai_on_owner());
    assert(xiaotai_signal_init(&initialized) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&delivered) == XIAOTAI_OK);
    const xiaotai_module_t *modules[] = {&test_module};
    xiaotai_framework_config_t configuration = {.modules = modules, .module_count = 1};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, run, &configuration) == 0);
    assert(xiaotai_signal_take(&initialized, 2000));
    assert(!xiaotai_on_owner() && !xiaotai_framework_owner_on_caller() && !atomic_load(&returned));
    assert(xiaotai_signal_take(&delivered, 2000));
    assert(xiaotai_run(handle_job, &jobs) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&delivered, 2000));
    assert(!atomic_load(&returned));
    xiaotai_framework_reset();
    assert(pthread_join(thread, NULL) == 0);
    assert(atomic_load(&returned) && timer_runs == 2 && jobs == 1);
    xiaotai_signal_deinit(&delivered);
    xiaotai_signal_deinit(&initialized);
    return 0;
}
