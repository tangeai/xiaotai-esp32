// SPDX-License-Identifier: MIT
#include "clock_test.h"

#include "xiaotai/at.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/port/os.h"

unsigned clock_init_calls, clock_server_count;
char clock_tz[XIAOTAI_CLOCK_NAME_MAX];
char clock_hosts[XIAOTAI_CLOCK_SERVERS_MAX][XIAOTAI_CLOCK_NAME_MAX];
struct seen_events seen;

static xiaotai_signal_t signal;
static const char *platform_zone;
static bool platform_synced;

xiaotai_err_t __real_xiaotai_clock_init(const xiaotai_clock_config_t *config);
xiaotai_err_t __wrap_xiaotai_clock_init(const xiaotai_clock_config_t *config)
{
    ++clock_init_calls;
    clock_server_count = (unsigned)config->server_count;
    strcpy(clock_tz, config->timezone ? config->timezone : "");
    for (size_t i = 0; i < config->server_count; ++i) strcpy(clock_hosts[i], config->servers[i]);
    return __real_xiaotai_clock_init(config);
}

static xiaotai_err_t platform_init(void)
{
    static const char *const server[] = {"fixture.invalid"};
    const xiaotai_clock_config_t config = {.servers = server, .server_count = 1, .timezone = platform_zone};
    xiaotai_err_t err = xiaotai_clock_init(&config);
    if (err == XIAOTAI_OK && platform_synced) err = xiaotai_clock_set_unix_ms(xiaotai_clock_unix_ms());
    return err;
}

static void record_event(const xiaotai_event_t *event, void *unused)
{
    (void)unused;
    if (strcmp(event->name, XIAOTAI_EVENT_CLOCK_SYNCED) == 0) {
        assert(!event->data && !event->size && !event->args.value);
        ++seen.syncs;
    } else if (strcmp(event->name, XIAOTAI_EVENT_CLOCK_MINUTE) == 0) {
        assert(event->data && event->size == sizeof(seen.minute) && seen.syncs);
        seen.minute = *(const xiaotai_wallclock_t *)event->data;
        assert(event->args.value == seen.minute.local.hour * 60 + seen.minute.local.minute && seen.minute.synced);
        ++seen.minutes;
    } else if (strcmp(event->name, XIAOTAI_EVENT_CLOCK_DUE) == 0) {
        assert(event->data && event->size == sizeof(xiaotai_schedule_t));
        const xiaotai_schedule_t *due = event->data;
        assert(due->enabled && due->id == event->args.value && !strcmp(due->label, event->args.text));
        assert(seen.due < sizeof(seen.ids));
        seen.ids[seen.due++] = due->id;
    } else {
        assert(!event->size && event->args.value >= 0 && event->args.value <= (int)XIAOTAI_SCHEDULE_MAX);
        ++seen.schedules;
    }
}

static xiaotai_err_t probe_init(void)
{
    assert(xiaotai_event_subscribe(XIAOTAI_EVENT_CLOCK_SYNCED, record_event, NULL) == XIAOTAI_OK);
    assert(xiaotai_event_subscribe(XIAOTAI_EVENT_CLOCK_MINUTE, record_event, NULL) == XIAOTAI_OK);
    assert(xiaotai_event_subscribe(XIAOTAI_EVENT_CLOCK_DUE, record_event, NULL) == XIAOTAI_OK);
    return xiaotai_event_subscribe(XIAOTAI_EVENT_CLOCK_SCHEDULES, record_event, NULL);
}

static void noop(void *unused) { (void)unused; }
static void give(void *unused) { (void)unused; xiaotai_signal_give(&signal); }
static void restart(void) { assert(0); }

void run(void (*fn)(void *), void *ctx)
{
    assert(xiaotai_run(fn, ctx) == XIAOTAI_OK);
    for (unsigned i = 0; i < 3; ++i) {
        assert(xiaotai_run(give, NULL) == XIAOTAI_OK);
        assert(xiaotai_signal_take(&signal, 2000));
    }
}

void drain(void) { run(noop, NULL); }

void prepare(int64_t wall_ms)
{
    xiaotai_clock_host_reset();
    xiaotai_clock_host_set_wall_ms(wall_ms);
    xiaotai_clock_host_hold(true);
    assert(xiaotai_kv_init() == XIAOTAI_OK);
    assert(xiaotai_signal_init(&signal) == XIAOTAI_OK);
    seen = (struct seen_events){0};
    clock_init_calls = clock_server_count = 0;
    xiaotai_eventlog_init(NULL);
}

xiaotai_err_t start(const xiaotai_clock_module_config_t *config, const char *platform_tz, bool initially_synced)
{
    assert(xiaotai_clock_module_configure(config) == XIAOTAI_OK);
    platform_zone = platform_tz;
    platform_synced = initially_synced;
    static const xiaotai_module_t s_platform = {.name = "platform", .init = platform_init};
    static const xiaotai_module_t s_probe = {.name = "probe", .init = probe_init};
    static const xiaotai_module_t s_clock = {.name = "clock", .init = xiaotai_module_clock_init, .start = xiaotai_module_clock_start};
    static const xiaotai_module_t *const owned[] = {&s_platform, &s_probe, &s_clock};
    const xiaotai_framework_config_t framework = {
        .modules = owned + (platform_zone == NULL), .module_count = platform_zone ? 3 : 2, .restart = restart,
    };
    xiaotai_err_t err = xiaotai_framework_start(&framework);
    if (err == XIAOTAI_OK) drain();
    return err;
}

void stop(void)
{
    assert(xiaotai_kv_flush(2000) == XIAOTAI_OK);
    xiaotai_framework_reset();
    xiaotai_clock_module_reset();
    xiaotai_at_reset();
    xiaotai_signal_deinit(&signal);
}

typedef struct { int64_t mono_ms, wall_ms; } advance_t;
static void advance_owner(void *ctx)
{
    const advance_t *d = ctx;
    xiaotai_clock_host_advance_ms(d->wall_ms);
    xiaotai_os_host_advance_us(d->mono_ms * 1000);
}

void advance(int64_t mono_ms, int64_t wall_ms)
{
    advance_t d = {.mono_ms = mono_ms, .wall_ms = wall_ms};
    run(advance_owner, &d);
}

static void add_owner(void *ctx)
{
    xiaotai_schedule_t *entry = ctx;
    assert(xiaotai_schedule_add(entry, &entry->id) == XIAOTAI_OK);
}

uint8_t add(xiaotai_schedule_t entry) { run(add_owner, &entry); return entry.id; }

static void set_zone_owner(void *ctx)
{
    assert(xiaotai_setting_set_text(XIAOTAI_SETTING_CLOCK_TIMEZONE, ctx) == XIAOTAI_OK);
}

void set_zone(const char *text) { run(set_zone_owner, (void *)text); }

xiaotai_clock_module_stats_t stats(void)
{
    xiaotai_clock_module_stats_t out;
    xiaotai_clock_module_stats(&out);
    return out;
}

xiaotai_schedule_info_t get(uint8_t id)
{
    xiaotai_schedule_info_t out;
    assert(xiaotai_schedule_get(id, &out) == XIAOTAI_OK);
    return out;
}
