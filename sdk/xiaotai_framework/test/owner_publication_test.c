// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_runtime_init.py
// Replaces legacy: tests/test_boot_contract.py
#include <assert.h>
#include <stdatomic.h>
#include <string.h>

#include "xiaotai/framework.h"
#include "xiaotai/port/os.h"
#include "xiaotai_mem_host.h"

static xiaotai_signal_t s_child_entered, s_startup_job_done;
static atomic_bool s_at_start_gate;
static atomic_uint s_initializations;
static bool s_fail_create, s_fail_init;
static unsigned s_signal_failure, s_signal_inits, s_signal_releases;

xiaotai_err_t __real_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task);
bool __real_xiaotai_task_wait_notify(uint32_t timeout_ms);
xiaotai_err_t __real_xiaotai_signal_init(xiaotai_signal_t *signal);
void __real_xiaotai_signal_deinit(xiaotai_signal_t *signal);

static void unpublished_job(void *context)
{
    (void)context;
    assert(!"unpublished owner accepted work");
}

xiaotai_err_t __wrap_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task)
{
    if (strcmp(config->name, "xiaotai") != 0) return __real_xiaotai_task_create(config, task);
    if (s_fail_create) return XIAOTAI_ERR_NO_MEM;
    /* Match the ESP port: the task can run before create publishes its handle. */
    const unsigned before = atomic_load(&s_initializations);
    xiaotai_task_t deferred;
    const xiaotai_err_t err = __real_xiaotai_task_create(config, &deferred);
    if (err == XIAOTAI_OK) {
        assert(xiaotai_signal_take(&s_child_entered, 2000));
        assert(xiaotai_run(unpublished_job, NULL) == XIAOTAI_ERR_INVALID_STATE);
        assert(atomic_load(&s_initializations) == before);
        *task = deferred;
    }
    return err;
}

bool __wrap_xiaotai_task_wait_notify(uint32_t timeout_ms)
{
    atomic_store(&s_at_start_gate, true);
    xiaotai_signal_give(&s_child_entered);
    return __real_xiaotai_task_wait_notify(timeout_ms);
}

xiaotai_err_t __wrap_xiaotai_signal_init(xiaotai_signal_t *signal)
{
    if (s_signal_failure && ++s_signal_inits == s_signal_failure) return XIAOTAI_FAIL;
    return __real_xiaotai_signal_init(signal);
}

void __wrap_xiaotai_signal_deinit(xiaotai_signal_t *signal)
{
    if (s_signal_failure) ++s_signal_releases;
    __real_xiaotai_signal_deinit(signal);
}

static void startup_job(void *context)
{
    (void)context;
    assert(xiaotai_on_owner());
    xiaotai_signal_give(&s_startup_job_done);
}

static xiaotai_err_t initialize(void)
{
    atomic_fetch_add(&s_initializations, 1);
    if (!atomic_load(&s_at_start_gate)) xiaotai_signal_give(&s_child_entered);
    xiaotai_command_scope_t scope;
    const xiaotai_err_t err = xiaotai_command_scope_capture(&scope);
    if (err != XIAOTAI_OK) return err;
    if (s_fail_init) return XIAOTAI_FAIL;
    return xiaotai_run(startup_job, NULL);
}

static const xiaotai_module_t s_module = {.name = "startup", .init = initialize};
static const xiaotai_module_t *const s_modules[] = {&s_module};
static const xiaotai_framework_config_t s_config = {.modules = s_modules, .module_count = 1};

static void check_unpublished(void)
{
    assert(!xiaotai_module_present("startup"));
    assert(xiaotai_run(unpublished_job, NULL) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_mem_host_outstanding() == 0);
}

int main(int argc, char **argv)
{
    assert(xiaotai_signal_init(&s_child_entered) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&s_startup_job_done) == XIAOTAI_OK);
    xiaotai_mem_host_clear();
    if (argc == 1 || strcmp(argv[1], "failure") == 0) {
        const xiaotai_framework_config_t invalid = {.module_count = 1};
        assert(xiaotai_framework_start(NULL) == XIAOTAI_ERR_INVALID_ARG);
        assert(xiaotai_framework_start(&invalid) == XIAOTAI_ERR_INVALID_ARG);
        check_unpublished();
        for (unsigned nth = 1; nth <= 2; ++nth) {
            xiaotai_mem_host_fail_nth(nth);
            assert(xiaotai_framework_start(&s_config) == XIAOTAI_ERR_NO_MEM);
            xiaotai_mem_host_fail_nth(0);
            check_unpublished(); /* before reset, not after it */
        }
        for (unsigned nth = 1; nth <= 2; ++nth) {
            s_signal_failure = nth;
            s_signal_inits = s_signal_releases = 0;
            assert(xiaotai_framework_start(&s_config) == XIAOTAI_FAIL);
            assert(s_signal_inits == nth && s_signal_releases == nth - 1);
            s_signal_failure = 0;
            check_unpublished();
        }
        s_fail_create = true;
        assert(xiaotai_framework_start(&s_config) == XIAOTAI_ERR_NO_MEM);
        s_fail_create = false;
        check_unpublished();
        assert(atomic_load(&s_initializations) == 0);
    }
    if (argc == 1 || strcmp(argv[1], "early") == 0) {
        s_fail_init = true;
        assert(xiaotai_framework_start(&s_config) == XIAOTAI_FAIL);
        s_fail_init = false;
        atomic_store(&s_at_start_gate, false);
        assert(!xiaotai_module_present("startup"));
        assert(xiaotai_run(unpublished_job, NULL) == XIAOTAI_ERR_INVALID_STATE);
    }
    /* All failures may be retried at the framework boundary without reset. */
    assert(xiaotai_framework_start(&s_config) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&s_startup_job_done, 2000));
    const unsigned initialized = atomic_load(&s_initializations);
    assert(xiaotai_framework_start(&s_config) == XIAOTAI_ERR_INVALID_STATE);
    assert(atomic_load(&s_initializations) == initialized);
    xiaotai_framework_reset();
    xiaotai_signal_deinit(&s_startup_job_done);
    xiaotai_signal_deinit(&s_child_entered);
    return 0;
}
