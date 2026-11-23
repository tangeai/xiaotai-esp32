// SPDX-License-Identifier: MIT
#include <assert.h>
#include <string.h>

#include "xiaotai/eventlog.h"
#include "xiaotai/framework.h"
#include "xiaotai/port/os.h"

static xiaotai_signal_t s_reported;
static uint32_t s_id;
static char s_report[256];

static void transition(void *ctx)
{
    (void)ctx;
    assert(xiaotai_activity_begin(&(xiaotai_activity_t){.name = "not_logged"}, &s_id) == XIAOTAI_OK);
    xiaotai_activity_end(s_id);
}

static void reply(void *ctx, const char *line)
{
    (void)ctx;
    assert(strlen(line) < sizeof(s_report));
    strcpy(s_report, line);
}

static void report(void *ctx)
{
    (void)ctx;
    xiaotai_diag_report("eventlog", &(xiaotai_reply_t){.write = reply});
    xiaotai_signal_give(&s_reported);
}

int main(void)
{
    assert(xiaotai_signal_init(&s_reported) == XIAOTAI_OK);
    static const xiaotai_module_t engine = {
        .name = "eventlog", .init = xiaotai_engine_eventlog_init, .start = xiaotai_engine_eventlog_start,
    };
    static const xiaotai_module_t *const modules[] = {&engine};
    assert(xiaotai_eventlog_init(&(xiaotai_eventlog_config_t){.reader_stack_bytes = XIAOTAI_EVENTLOG_NO_READER})
           == XIAOTAI_OK);
    assert(xiaotai_framework_start(&(xiaotai_framework_config_t){.modules = modules, .module_count = 1}) == XIAOTAI_OK);
    assert(xiaotai_run(transition, NULL) == XIAOTAI_OK);
    /* Both activity events are queued by the same owner job; the current
     * activity is already none by the time its start event is delivered. */
    for (unsigned i = 0; i < 1000 && xiaotai_eventlog_latest() < 3; ++i) xiaotai_delay_ms(1);
    xiaotai_eventlog_entry_t events[3];
    assert(xiaotai_eventlog_read(0, events, 3) == 3);
    for (size_t i = 0; i < 3; ++i) assert(strcmp(events[i].source, "framework") == 0);
    assert(strcmp(events[0].label, "started") == 0);
    assert(strcmp(events[1].label, "activity.started") == 0 && s_id != 0 && (uint32_t)events[1].value == s_id);
    assert(strcmp(events[2].label, "activity.ended") == 0 && events[2].value == 0);
    assert(xiaotai_run(report, NULL) == XIAOTAI_OK && xiaotai_signal_take(&s_reported, 1000));
    assert(strstr(s_report, "emitted=3 ") && !strstr(s_report, "not_logged"));
    xiaotai_framework_reset();
    xiaotai_eventlog_reset();
    xiaotai_signal_deinit(&s_reported);
    return 0;
}
