// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdatomic.h>
#include <string.h>

#include "xiaotai/framework.h"
#include "xiaotai/port/os.h"

static xiaotai_signal_t s_first_waiting, s_first_complete, s_give, s_unblock;
static atomic_bool s_delay_completion;
static unsigned s_called;

void __real_xiaotai_signal_give(xiaotai_signal_t *signal);
void __wrap_xiaotai_signal_give(xiaotai_signal_t *signal)
{
    if (atomic_exchange(&s_delay_completion, false)) {
        assert(xiaotai_on_owner());
        __real_xiaotai_signal_give(&s_first_complete);
        assert(xiaotai_signal_take(&s_give, 2000));
    }
    __real_xiaotai_signal_give(signal);
}

static xiaotai_err_t handler(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    assert(argc == 2 && xiaotai_on_owner());
    ++s_called;
    if (!strcmp(argv[1], "first")) {
        __real_xiaotai_signal_give(&s_first_waiting);
        assert(xiaotai_signal_take(&s_unblock, 2000));
        atomic_store(&s_delay_completion, true);
        return XIAOTAI_ERR_NOT_FOUND;
    }
    assert(strcmp(argv[1], "second") == 0);
    out->write(out->ctx, "finished");
    return XIAOTAI_OK;
}

static xiaotai_err_t init(void) { return xiaotai_command_register("AT+FIXTURE", NULL, handler); }

static void reply(void *ctx, const char *text)
{
    assert(strcmp(text, "finished") == 0);
    ++*(unsigned *)ctx;
}

int main(void)
{
    assert(xiaotai_signal_init(&s_first_waiting) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&s_first_complete) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&s_give) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&s_unblock) == XIAOTAI_OK);
    static const xiaotai_module_t mod = {.name = "fixture", .init = init};
    static const xiaotai_module_t *const modules[] = {&mod};
    assert(xiaotai_framework_start(&(xiaotai_framework_config_t){.modules = modules, .module_count = 1}) == XIAOTAI_OK);
    unsigned replies = 0;
    const xiaotai_reply_t out = {.write = reply, .ctx = &replies};
    assert(xiaotai_command_run("AT+FIXTURE first", &out, 20) == XIAOTAI_ERR_TIMEOUT);
    assert(xiaotai_signal_take(&s_first_waiting, 1000));
    __real_xiaotai_signal_give(&s_unblock);
    assert(xiaotai_signal_take(&s_first_complete, 1000));
    /* First handler returned; the completion is still behind the gate. */
    assert(xiaotai_command_run("AT+FIXTURE second", &out, 20) == XIAOTAI_ERR_BUSY);
    __real_xiaotai_signal_give(&s_give);
    assert(xiaotai_command_run("AT+FIXTURE second", &out, 1000) == XIAOTAI_OK);
    assert(replies == 1 && s_called == 2);
    xiaotai_framework_reset();
    xiaotai_signal_deinit(&s_first_waiting);
    xiaotai_signal_deinit(&s_first_complete);
    xiaotai_signal_deinit(&s_give);
    xiaotai_signal_deinit(&s_unblock);
    return 0;
}
