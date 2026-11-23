// SPDX-License-Identifier: MIT
#include "fakes.h"

#include <assert.h>
#include <string.h>
#include "xiaotai/modules/environment.h"
#include "xiaotai/port/os.h"

static xiaotai_signal_t synchronized;
static uint32_t quiet;
static unsigned snapshots;

void __real_xiaotai_net_status(xiaotai_net_status_t *status);
void __wrap_xiaotai_net_status(xiaotai_net_status_t *status)
{
    ++snapshots;
    __real_xiaotai_net_status(status);
}

static void barrier(void *ctx) { xiaotai_signal_give(ctx); }

static void flush(void)
{
    for (unsigned n = 0; n < 3U; ++n) {
        assert(xiaotai_run(barrier, &synchronized) == XIAOTAI_OK);
        assert(xiaotai_signal_take(&synchronized, 3000U));
    }
}

static void phase(xiaotai_environment_phase_t state)
{
    const xiaotai_args_t args = {.value = state};
    const xiaotai_environment_status_t status = {.phase = state,
        .environment = {.saved = "http://fixture.invalid/test"}};
    assert(xiaotai_event_emit(XIAOTAI_EVENT_ENVIRONMENT_CHANGED, &args, &status, sizeof(status)) == XIAOTAI_OK);
    flush();
}

static void begin(void *ctx) { assert(xiaotai_quiesce_begin(ctx, &quiet) == XIAOTAI_OK); }
static void end(void *ctx) { (void)ctx; xiaotai_quiesce_end(quiet); }

static void maintenance(const char *message)
{
    assert(companion.status.setup && !strcmp(companion.status.page, "maintenance"));
    assert(fixture.kind == XIAOTAI_VIEW_PROGRESS && !strcmp(fixture.text, message));
    assert(!fixture.action_count && !(fixture.flags & XIAOTAI_VIEW_MAY_SLEEP));
}

int main(void)
{
    static const xiaotai_module_t probe = {.name = "probe", .init = companion_fixture_init};
    static const xiaotai_module_t *const modules[] = {&probe};
    static const xiaotai_application_t app = {.name = "companion", .init = xiaotai_app_companion_init,
        .start = xiaotai_app_companion_start, .foreground = xiaotai_app_companion_foreground};
    static const xiaotai_application_t *const apps[] = {&app};
    const uint32_t needs = XIAOTAI_NEED_DISPLAY | XIAOTAI_NEED_TOUCH | XIAOTAI_NEED_KEY;
    fixture = (companion_test_fixture_t){.needs = needs, .navigation_pages = true,
        .net.state = XIAOTAI_NET_UP, .binding.phase = XIAOTAI_BINDING_BOUND, .platform.phase = XIAOTAI_PLATFORM_ONLINE};
    const xiaotai_framework_config_t fw = {.modules = modules, .module_count = 1U,
        .applications = apps, .application_count = 1U, .available_needs = needs};
    assert(xiaotai_signal_init(&synchronized) == XIAOTAI_OK);
    assert(xiaotai_framework_start(&fw) == XIAOTAI_OK);
    flush();
    assert(!companion.status.setup && !strcmp(companion.status.page, "home"));

    unsigned before = snapshots;
    phase(XIAOTAI_ENVIRONMENT_IDLE); /* No takeover on refusal. */
    phase(XIAOTAI_ENVIRONMENT_WAITING); /* No takeover on delayed progress. */
    assert(snapshots == before && !companion.status.setup);

    assert(xiaotai_run(begin, (void *)XIAOTAI_QUIESCE_ENVIRONMENT) == XIAOTAI_OK);
    flush();
    maintenance("正在切换服务环境");
    before = snapshots;
    const unsigned redraws = fixture.views;
    phase(XIAOTAI_ENVIRONMENT_WAITING);
    phase(XIAOTAI_ENVIRONMENT_ERROR_HELD);
    assert(snapshots == before + 2U && fixture.views == redraws);
    maintenance("正在切换服务环境");

    assert(xiaotai_run(end, NULL) == XIAOTAI_OK);
    flush();
    phase(XIAOTAI_ENVIRONMENT_IDLE);
    assert(!companion.status.setup && !strcmp(companion.status.page, "home"));

    assert(xiaotai_run(begin, (void *)XIAOTAI_QUIESCE_COPROCESSOR) == XIAOTAI_OK);
    flush();
    phase(XIAOTAI_ENVIRONMENT_IDLE);
    maintenance("正在维护联网模块");
    assert(xiaotai_run(end, NULL) == XIAOTAI_OK);
    flush();

    xiaotai_framework_reset();
    xiaotai_app_companion_reset();
    xiaotai_signal_deinit(&synchronized);
    return 0;
}
