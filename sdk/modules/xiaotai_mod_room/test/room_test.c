// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/xmini_business/test_business.c::business
// Replaces legacy: tests/regression/test_room_token_lease.py
// Replaces legacy: tests/regression/test_voice_reliability.py::test_command_recovery
// Replaces legacy: tests/xmini/test_command.c::command
// Replaces legacy: tests/regression/test_p4_runtime_maintenance.py
// Replaces legacy: tests/xmini/test_platform_routes.c::platform_routes
// Replaces legacy: tests/regression/test_p4_room_ptt_feedback.py
// Replaces legacy: tests/regression/test_room_async_lease.py
// Replaces legacy: tests/regression/test_voice_format.py
// Replaces legacy: tests/regression/test_room.py
// Replaces legacy: tests/xmini/xmini_group/test_group.c::group
#include "fake_platform.h"
#include "fake_link.h"
#include "room_internal.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "xiaotai/at.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/framework.h"
#include "xiaotai/link.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"

static const char empty[] = "{\"code\":200,\"data\":{\"device_id\":\"self\",\"desired_state\":\"\",\"assignment_version\":0}}";
static const char assigned[] = "{\"code\":200,\"data\":{\"device_id\":\"self\",\"desired_state\":\"joined\","
    "\"room_id\":\"group_room_exact\",\"room_code\":\"001234\",\"assignment_version\":3,\"device_name\":\"test unit\"}}";
static const char left[] = "{\"code\":200,\"data\":{\"device_id\":\"self\",\"desired_state\":\"left\",\"assignment_version\":4}}";
static const char receipt[] = "{\"code\":200,\"data\":{}}";
static xiaotai_signal_t fence, release_owner;
static xiaotai_signal_t restarted;
static atomic_int_fast64_t clock_us;
static int64_t wall_ms;
static int64_t credential_alloc_delay_ms;
static xiaotai_room_result_t last_result;
static unsigned results, changes, endings, member_events, voice_events;
static int32_t voices;
static size_t tracked_bytes;
static void *tracked_allocation;
static unsigned tracked_allocations, tracked_frees;
static room_end_t last_ending;
static uint32_t ending_assignment;
static unsigned requests_at_end;
static const xiaotai_room_config_t *app_config;
static xiaotai_err_t app_config_status;
static bool with_at;
static struct { const char *input; size_t remaining, used; char output[4096]; } wire;

int64_t __wrap_xiaotai_time_us(void) { return atomic_load(&clock_us); }
int64_t __wrap_xiaotai_clock_unix_ms(void) { return wall_ms; }

/* The host port keeps detached task records even after pthread_exit. Keep
 * just those handles reachable across independent framework boots. */
static xiaotai_task_t retained_tasks[64];
static unsigned retained_count;
xiaotai_err_t __real_xiaotai_task_create(const xiaotai_task_config_t *cfg, xiaotai_task_t *task);
xiaotai_err_t __wrap_xiaotai_task_create(const xiaotai_task_config_t *cfg, xiaotai_task_t *task)
{
    const xiaotai_err_t err = __real_xiaotai_task_create(cfg, task);
    if (err == XIAOTAI_OK) {
        assert(retained_count < sizeof(retained_tasks) / sizeof(retained_tasks[0]));
        retained_tasks[retained_count++] = *task;
    }
    return err;
}

void *__real_xiaotai_mem_alloc(xiaotai_mem_kind_t kind, size_t bytes);
void __real_xiaotai_mem_free(void *ptr);
void *__wrap_xiaotai_mem_alloc(xiaotai_mem_kind_t kind, size_t bytes)
{
    if (bytes == XIAOTAI_LINK_PEER_MAX + XIAOTAI_LINK_TOKEN_MAX && credential_alloc_delay_ms) {
        atomic_fetch_add(&clock_us, credential_alloc_delay_ms * 1000);
        wall_ms += credential_alloc_delay_ms;
        credential_alloc_delay_ms = 0;
    }
    void *const result = __real_xiaotai_mem_alloc(kind, bytes);
    if (tracked_bytes && bytes == tracked_bytes) {
        assert(kind == XIAOTAI_MEM_BULK && !tracked_allocation && result);
        tracked_allocation = result;
        ++tracked_allocations;
    }
    return result;
}

void __wrap_xiaotai_mem_free(void *ptr)
{
    if (ptr && ptr == tracked_allocation) { tracked_allocation = NULL; ++tracked_frees; }
    __real_xiaotai_mem_free(ptr);
}

static void barrier(void *unused) { (void)unused; xiaotai_signal_give(&fence); }
static void restart_fixture(void) { xiaotai_signal_give(&restarted); }

static void flush(void)
{
    for (unsigned i = 0; i < 3; ++i) {
        assert(xiaotai_run(barrier, NULL) == XIAOTAI_OK);
        assert(xiaotai_signal_take(&fence, 3000));
    }
}

static void owner(void (*job)(void *), void *ctx)
{
    assert(xiaotai_run(job, ctx) == XIAOTAI_OK);
    flush();
}

static xiaotai_room_status_t status(void)
{
    xiaotai_room_status_t result;
    xiaotai_room_status(&result);
    return result;
}

static xiaotai_room_stats_t stats(void)
{
    xiaotai_room_stats_t result;
    xiaotai_room_stats(&result);
    return result;
}

static void report(const xiaotai_event_t *event, void *unused)
{
    (void)unused;
    if (!strcmp(event->name, XIAOTAI_EVENT_ROOM_STATE)) {
        assert(event->args.value >= XIAOTAI_ROOM_OFFLINE && event->args.value <= XIAOTAI_ROOM_BLOCKED);
        ++changes;
    } else if (!strcmp(event->name, XIAOTAI_EVENT_ROOM_MEMBERS)) {
        ++member_events;
    } else if (!strcmp(event->name, XIAOTAI_EVENT_ROOM_VOICE)) {
        ++voice_events;
        voices = event->args.value;
    } else {
        assert(event->data && event->size == sizeof(last_result));
        last_result = *(const xiaotai_room_result_t *)event->data;
        assert(event->args.value == last_result.error && event->args.text[0]);
        ++results;
    }
}

static xiaotai_err_t never_run(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)argc; (void)argv; (void)out;
    assert(false);
    return XIAOTAI_FAIL;
}

static xiaotai_err_t probe_init(void)
{
    assert(xiaotai_event_subscribe(XIAOTAI_EVENT_ROOM_STATE, report, NULL) == XIAOTAI_OK);
    assert(xiaotai_event_subscribe(XIAOTAI_EVENT_ROOM_RESULT, report, NULL) == XIAOTAI_OK);
    assert(xiaotai_event_subscribe(XIAOTAI_EVENT_ROOM_MEMBERS, report, NULL) == XIAOTAI_OK);
    assert(xiaotai_event_subscribe(XIAOTAI_EVENT_ROOM_VOICE, report, NULL) == XIAOTAI_OK);
    if (with_at) {
        static const xiaotai_at_verb_t conflict = {.word = "PTT", .run = never_run};
        static const xiaotai_at_verb_t bad = {.word = "BAD", .max_args = 1, .run = never_run, .console_arg = "0"};
        static const xiaotai_at_verb_t bad_arg = {.word = "BAD", .min_args = 1, .max_args = 1,
                                                  .run = never_run, .console_arg = "0 1"};
        assert(xiaotai_at_verbs_register("AT+ROOM", &conflict, 1) == XIAOTAI_ERR_INVALID_STATE);
        assert(xiaotai_at_verbs_register("AT+ROOM", &bad, 1) == XIAOTAI_ERR_INVALID_ARG);
        assert(xiaotai_at_verbs_register("AT+ROOM", &bad_arg, 1) == XIAOTAI_ERR_INVALID_ARG);
    }
    if (app_config) app_config_status = xiaotai_room_configure(app_config);
    return XIAOTAI_OK;
}

static size_t at_read(void *ctx, uint8_t *data, size_t capacity, uint32_t timeout_ms)
{
    (void)ctx;
    if (!wire.remaining) {
        atomic_fetch_add(&clock_us, (int64_t)(timeout_ms ? timeout_ms : 1U) * 1000);
        return 0;
    }
    const size_t n = wire.remaining < capacity ? wire.remaining : capacity;
    memcpy(data, wire.input, n);
    wire.input += n;
    wire.remaining -= n;
    return n;
}

static xiaotai_err_t at_write(void *ctx, const char *data, size_t size)
{
    (void)ctx;
    assert(!xiaotai_on_owner());
    assert(size >= 2 && !memcmp(data + size - 2, "\r\n", 2));
    assert(wire.used + size < sizeof(wire.output));
    memcpy(wire.output + wire.used, data, size);
    wire.used += size;
    wire.output[wire.used] = '\0';
    return XIAOTAI_OK;
}

static void boot(bool external, bool online, const xiaotai_room_config_t *glue, const xiaotai_room_config_t *app)
{
    fake_reset(external);
    fake_link = (fake_link_t){0};
    wall_ms = INT64_C(1700000100000);
    credential_alloc_delay_ms = 0;
    fixture.online = online;
    changes = results = endings = 0;
    member_events = voice_events = 0;
    app_config = app;
    atomic_store(&clock_us, 1000);
    assert(xiaotai_eventlog_init(NULL) == XIAOTAI_OK);
    assert(xiaotai_room_configure(glue) == XIAOTAI_OK);
    if (with_at) {
        static const xiaotai_at_io_t io = {.name = "fixture", .read = at_read, .write = at_write};
        const xiaotai_at_config_t cli = {.no_platform_console = true, .task_stack_bytes = XIAOTAI_AT_NO_TASK,
                                         .command_timeout_ms = 60};
        assert(xiaotai_at_configure(&cli) == XIAOTAI_OK);
        assert(xiaotai_at_attach(&io) == XIAOTAI_OK);
    }
    static const xiaotai_module_t room = {.name = "room", .init = xiaotai_module_room_init, .start = xiaotai_module_room_start};
    static const xiaotai_module_t probe = {.name = "probe", .init = probe_init};
    static const xiaotai_module_t at = {.name = "at", .init = xiaotai_engine_at_init, .start = xiaotai_engine_at_start};
    static const xiaotai_module_t *const modules[] = {&room, &probe, &at};
    static const xiaotai_priority_t priorities[] = {{"room", 2}, {"other", 8}, {NULL, 0}};
    const xiaotai_framework_config_t config = {.modules = modules, .module_count = with_at ? 3U : 2U,
        .available_needs = UINT32_MAX, .priorities = priorities, .restart = restart_fixture, .restart_delay_ms = 1U};
    assert(xiaotai_framework_start(&config) == XIAOTAI_OK);
    flush();
}

static void reset_room(void *unused) { (void)unused; xiaotai_room_reset(); }

static void shutdown(void)
{
    owner(reset_room, NULL);
    xiaotai_framework_reset();
    xiaotai_at_reset();
    xiaotai_eventlog_reset();
}

typedef struct { unsigned index, http; int32_t code; xiaotai_err_t error; const char *body; } reply_t;

static void send_reply(void *ctx)
{
    const reply_t *reply = ctx;
    fake_reply(reply->index, reply->error, reply->http, reply->code, reply->body);
}

static void respond(unsigned index, unsigned http, int32_t code, const char *body)
{
    reply_t reply = {.index = index, .http = http, .code = code, .body = body};
    owner(send_reply, &reply);
}

static void ok(unsigned index, const char *body) { respond(index, 200, 200, body); }

static void change_online(void *ctx)
{
    fixture.online = ctx != NULL;
    assert(xiaotai_event_emit(fixture.online ? XIAOTAI_EVENT_PLATFORM_ONLINE : XIAOTAI_EVENT_PLATFORM_OFFLINE,
                              NULL, NULL, 0) == XIAOTAI_OK);
}

static void advance(void *ctx)
{
    const uint32_t delta_ms = *(uint32_t *)ctx;
    atomic_fetch_add(&clock_us, (int64_t)delta_ms * 1000);
}

static void elapse(uint32_t ms) { owner(advance, &ms); }

static void expect_request(unsigned index, const char *suffix, const char *body)
{
    static const char prefix[] = "/v1/call/group/device/";
    assert(index < fixture.sent);
    recorded_request_t *entry = &fixture.requests[index];
    if (strcmp(entry->path + sizeof(prefix) - 1, suffix) || strcmp(entry->body, body ? body : ""))
        fprintf(stderr, "request %u: expected %s %s, got %s %s\n", index,
                suffix, body ? body : "", entry->path, entry->body);
    assert(!strncmp(entry->path, prefix, sizeof(prefix) - 1));
    assert(!strcmp(entry->path + sizeof(prefix) - 1, suffix));
    assert(!strcmp(entry->body, body ? body : ""));
    assert(entry->method == (body ? XIAOTAI_PLATFORM_POST : XIAOTAI_PLATFORM_GET));
}

static void expect_token_request(unsigned index)
{
    static const char prefix[] = "{\"room_id\":\"group_room_exact\",\"assignment_version\":3,\"session_id\":\"";
    assert(index < fixture.sent);
    const recorded_request_t *entry = &fixture.requests[index];
    assert(!strcmp(entry->path, "/v1/call/group/device/connect-token") &&
           entry->method == XIAOTAI_PLATFORM_POST && entry->flags == XIAOTAI_PLATFORM_RELEASE);
    assert(!strncmp(entry->body, prefix, sizeof(prefix) - 1U));
    const char *session = entry->body + sizeof(prefix) - 1U;
    assert(strspn(session, "0123456789abcdef") == 32U && !strcmp(session + 32U, "\"}"));
}

static void notify_twice(void *unused)
{
    (void)unused;
    fake_notify(fixture.binding);
    fake_notify(fixture.binding);
}

static void single_slot(void *unused) { (void)unused; fixture.limit = 1; }

static void end_connection(room_t *room, room_end_t reason)
{
    assert(room->view.generation == 17 && room->view.assigned);
    ++endings;
    last_ending = reason;
    ending_assignment = room->view.assignment_version;
    requests_at_end = fixture.sent;
}

static void simulate_active(void *unused)
{
    (void)unused;
    room_t *room = room_context();
    room->view.phase = XIAOTAI_ROOM_LISTENING;
    room->view.generation = 17;
    room->end_connection = end_connection;
    room_drive(room);
}

static void assignment(void)
{
    boot(true, false, NULL, NULL);
    assert(!status().attending && status().phase == XIAOTAI_ROOM_OFFLINE && fixture.sent == 0);
    assert(xiaotai_room_create(NULL) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_room_join("12345", "0123") == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_room_create("01a3") == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_room_create("1") == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_room_attend(true) == XIAOTAI_OK);
    flush();
    assert(status().attending && fixture.sent == 0);
    owner(change_online, &fixture);
    expect_request(0, "assignment", NULL);
    assert(status().phase == XIAOTAI_ROOM_SYNCING);
    assert(xiaotai_room_create(NULL) == XIAOTAI_OK);
    flush();
    assert(last_result.action == XIAOTAI_ROOM_ACTION_CREATE && last_result.error == XIAOTAI_ERR_INVALID_STATE && fixture.sent == 1);
    respond(0, 401, 200, empty);
    assert(!status().assignment_known && status().code == 401 && status().error == XIAOTAI_FAIL);
    elapse(4999);
    assert(fixture.sent == 1);
    elapse(1);
    respond(1, 200, 40320, "{\"code\":40320}");
    assert(!status().assignment_known && status().code == 40320 && stats().sync_failures == 2);
    elapse(5000);
    ok(2, "{\"code\":200,\"data\":{\"device_id\":\"intruder\",\"desired_state\":\"left\",\"assignment_version\":7}}");
    assert(!status().assignment_known && status().error == XIAOTAI_FAIL);
    elapse(5000);
    ok(3, "{\"code\":200,\"data\":{\"device_id\":\"self\",\"desired_state\":\"left\",\"assignment_version\":4294967296}}");
    assert(!status().assignment_known);
    elapse(5000);
    ok(4, "{\"code\":200,\"data\":{\"device_id\":\"self\",\"desired_state\":\"joined\",\"room_id\":\"g\",\"room_code\":\"000001\",\"assignment_version\":0}}");
    assert(!status().assignment_known);
    elapse(5000);
    ok(5, empty);
    assert(status().assignment_known && !status().assigned && status().phase == XIAOTAI_ROOM_EMPTY);
    elapse(29999);
    assert(fixture.sent == 6);
    elapse(1);
    expect_request(6, "assignment", NULL);
    owner(single_slot, NULL);
    char password[] = "0123", room_code[] = "001234";
    assert(xiaotai_room_join(room_code, password) == XIAOTAI_OK);
    password[0] = room_code[0] = '9';
    flush();
    assert(fixture.sent == 7 && status().busy && !status().assigned);
    assert(xiaotai_room_create(NULL) == XIAOTAI_OK);
    flush();
    assert(last_result.error == XIAOTAI_ERR_BUSY);
    ok(6, assigned); /* late, logically retired by JOIN */
    assert(stats().stale == 1 && !status().assigned);
    expect_request(7, "join", "{\"room_code\":\"001234\",\"password\":\"0123\"}");
    ok(7, receipt);
    assert(!status().assigned && !status().assignment_known && status().busy);
    expect_request(8, "assignment", NULL);
    owner(notify_twice, NULL);
    assert(fixture.sent == 9);
    ok(8, empty);
    assert(stats().stale == 2 && fixture.sent == 10 && status().busy);
    ok(9, assigned);
    assert(status().assigned && status().assignment_known && !status().busy && status().assignment_version == 3);
    assert(!strcmp(status().room_code, "001234") && !strcmp(status().self_name, "test unit"));
    assert(last_result.action == XIAOTAI_ROOM_ACTION_JOIN && last_result.error == XIAOTAI_OK);
    assert(xiaotai_room_create(NULL) == XIAOTAI_OK);
    flush();
    assert(last_result.error == XIAOTAI_ERR_INVALID_STATE && last_result.code == XIAOTAI_ROOM_CODE_ALREADY_MEMBER);
    owner(notify_twice, NULL);
    expect_request(10, "assignment", NULL);
    ok(10, empty);
    ok(11, empty);
    assert(stats().assignments_stale == 1 && status().assignment_version == 3);
    elapse(4999);
    assert(fixture.sent == 12);
    elapse(1);
    ok(12, assigned);

    owner(simulate_active, NULL);
    owner(notify_twice, NULL);
    ok(13, left); /* discard old invalidated read, do not stop the live room */
    assert(endings == 0 && status().generation == 17);
    ok(14, left);
    assert(endings == 1 && last_ending == ROOM_END_ASSIGNMENT && ending_assignment == 3);
    assert(!status().assigned && status().assignment_version == 4);
    assert(!strcmp(status().self_name, "test unit")); /* missing field doesn't erase a known local name */
    owner(notify_twice, NULL);
    ok(15, empty); /* invalidated */
    ok(16, "{\"code\":200,\"data\":{\"device_id\":\"self\",\"desired_state\":\"joined\",\"room_id\":\"group_room_exact\",\"room_code\":\"001234\",\"assignment_version\":5}}");
    owner(simulate_active, NULL);
    owner(change_online, NULL);
    assert(endings == 2 && last_ending == ROOM_END_OFFLINE && status().assigned);
    assert(xiaotai_room_leave() == XIAOTAI_OK);
    assert(xiaotai_room_attend(false) == XIAOTAI_OK);
    flush();
    assert(!status().attending && status().busy && status().assigned && status().phase == XIAOTAI_ROOM_LEAVING);
    assert(fixture.sent == 17);
    owner(change_online, &fixture);
    expect_request(17, "leave", "{\"room_id\":\"group_room_exact\",\"assignment_version\":5}");
    ok(17, receipt);
    assert(status().assigned && status().busy);
    expect_request(18, "assignment", NULL);
    ok(18, "{\"code\":200,\"data\":{\"device_id\":\"self\",\"desired_state\":\"left\",\"assignment_version\":4294967295}}");
    assert(!status().assigned && !status().busy && status().assignment_version == UINT32_MAX);
    assert(last_result.action == XIAOTAI_ROOM_ACTION_LEAVE && last_result.error == XIAOTAI_OK);
    elapse(30000);
    assert(fixture.sent == 19 && changes > 5);
    assert(room_request_can_yield(ROOM_REQUEST_ASSIGNMENT, XIAOTAI_ROOM_ACTION_JOIN));
    assert(!room_request_can_yield(ROOM_REQUEST_TOKEN, XIAOTAI_ROOM_ACTION_JOIN));
    assert(!room_request_can_yield(ROOM_REQUEST_PRESENCE, XIAOTAI_ROOM_ACTION_CREATE));
    assert(room_request_can_yield(ROOM_REQUEST_TOKEN, XIAOTAI_ROOM_ACTION_LEAVE));
    assert(room_request_can_yield(ROOM_REQUEST_PRESENCE, XIAOTAI_ROOM_ACTION_LEAVE));
    shutdown();
}

static void blocked_owner(void *unused)
{
    (void)unused;
    xiaotai_signal_give(&fence);
    assert(xiaotai_signal_take(&release_owner, 3000));
}

static void replace_binding(void *unused)
{
    (void)unused;
    ++fixture.binding;
    fixture.device = "replacement";
    assert(xiaotai_event_emit(XIAOTAI_EVENT_PLATFORM_ONLINE, NULL, NULL, 0) == XIAOTAI_OK);
}

static void attend_stale(void)
{
    boot(false, true, NULL, NULL);
    assert(xiaotai_room_attend(true) == XIAOTAI_OK);
    flush();
    ok(0, empty);
    assert(xiaotai_run(blocked_owner, NULL) == XIAOTAI_OK && xiaotai_signal_take(&fence, 3000));
    assert(xiaotai_room_create("4321") == XIAOTAI_OK);
    assert(xiaotai_room_attend(false) == XIAOTAI_OK);
    assert(xiaotai_room_attend(true) == XIAOTAI_OK);
    xiaotai_signal_give(&release_owner);
    flush();
    assert(stats().stale == 1 && fixture.sent == 2 && !status().assignment_known);
    expect_request(1, "assignment", NULL);
    ok(1, assigned);
    owner(simulate_active, NULL);
    assert(xiaotai_room_attend(false) == XIAOTAI_OK);
    flush();
    assert(endings == 1 && last_ending == ROOM_END_ATTENDANCE && status().assigned && !status().attending);
    assert(status().wait == XIAOTAI_ROOM_WAIT_NOT_ATTENDING);
    elapse(30000);
    assert(fixture.sent == 2);
    assert(xiaotai_room_attend(true) == XIAOTAI_OK);
    flush();
    expect_request(2, "assignment", NULL);
    owner(replace_binding, NULL);
    expect_request(3, "assignment", NULL);
    assert(status().attending && !status().assigned && status().assignment_version == 0);
    ok(2, assigned); /* the superseded binding's callback must not wipe the new GET */
    ok(3, "{\"code\":200,\"data\":{\"device_id\":\"replacement\",\"desired_state\":\"left\",\"assignment_version\":1}}");
    assert(stats().stale == 2 && status().assignment_known && status().assignment_version == 1);
    assert(xiaotai_room_create(NULL) == XIAOTAI_OK);
    flush();
    expect_request(4, "create", "{\"password\":\"\"}");
    assert(xiaotai_room_attend(false) == XIAOTAI_OK);
    flush();
    ok(4, "{\"code\":200,\"data\":{\"device_id\":\"replacement\",\"desired_state\":\"joined\",\"assignment_version\":2,\"room_code\":\"001234\",\"room_id\":\"g\"}}");
    assert(!status().attending && status().assigned && status().assignment_version == 2 && fixture.sent == 5);
    assert(last_result.action == XIAOTAI_ROOM_ACTION_CREATE && last_result.error == XIAOTAI_OK);
    assert(xiaotai_room_attend(true) == XIAOTAI_OK);
    flush();
    ok(5, "{\"code\":200,\"data\":{\"device_id\":\"replacement\",\"desired_state\":\"joined\",\"assignment_version\":2,\"room_code\":\"001234\",\"room_id\":\"g\"}}");
    owner(simulate_active, NULL);
    assert(xiaotai_room_leave() == XIAOTAI_OK);
    flush();
    assert(endings == 2 && last_ending == ROOM_END_LEAVE && requests_at_end == 6);
    assert(status().assigned && status().generation == 0 && status().phase == XIAOTAI_ROOM_LEAVING);
    expect_request(6, "leave", "{\"room_id\":\"g\",\"assignment_version\":2}");
    ok(6, "{\"code\":200,\"data\":{\"device_id\":\"replacement\",\"desired_state\":\"left\",\"assignment_version\":3}}");
    assert(!status().assigned && status().assignment_version == 3);
    expect_request(7, "assignment", NULL); /* mutation success invalidates the attending read */
    shutdown();
}

typedef struct { uint32_t max, pool; bool manual, provisioning; uint16_t members; } expected_config_t;

static void check_config(void *ctx)
{
    const expected_config_t *expected = ctx;
    const xiaotai_room_config_t *cfg = &room_context()->config;
    assert(cfg->command_max_bytes == expected->max && cfg->command_pool_bytes == expected->pool);
    assert(cfg->manual_connect == expected->manual && cfg->settle_before_provisioning == expected->provisioning);
    assert(cfg->members_max == expected->members);
    assert(fixture.transport.command_pool_bytes == expected->pool && fixture.transport.links == 1);
    assert(fixture.platform.concurrent_requests == 2 && fixture.platform.request_body_bytes == 256);
    assert(fixture.platform.response_bytes == 4096 && fixture.platform.signal_bytes == 512);
    assert(fixture.media.uplink.encoding == XIAOTAI_CODEC_G711A && fixture.media.uplink.rate_hz == 8000);
    assert(fixture.media.downlink.channels == 1 && fixture.media.packet_ms == 20 && fixture.media.downlink_burst_ms == 200);
    assert(fixture.media.latency.max_ms == 560 && fixture.media.latency.surplus_ms == 320);
}

static void config(void)
{
    xiaotai_room_config_t invalid = {.command_max_bytes = 2047};
    assert(xiaotai_room_configure(&invalid) == XIAOTAI_ERR_INVALID_ARG);
    invalid.command_max_bytes = XIAOTAI_LINK_COMMAND_LIMIT_BYTES + 1U;
    assert(xiaotai_room_configure(&invalid) == XIAOTAI_ERR_INVALID_ARG);
    invalid = (xiaotai_room_config_t){.command_max_bytes = 12288, .command_pool_bytes = 8192};
    assert(xiaotai_room_configure(&invalid) == XIAOTAI_ERR_INVALID_ARG);
    invalid.command_pool_bytes = XIAOTAI_LINK_COMMAND_LIMIT_BYTES + 1U;
    assert(xiaotai_room_configure(&invalid) == XIAOTAI_ERR_INVALID_ARG);
    expected_config_t expected = {40960, 81920, false, false, 100};
    boot(true, true, NULL, NULL);
    owner(check_config, &expected);
    assert(xiaotai_room_configure(NULL) == XIAOTAI_ERR_INVALID_STATE);
    shutdown();
    boot(false, true, NULL, NULL);
    expected = (expected_config_t){12288, 16384, false, false, UINT16_MAX};
    owner(check_config, &expected);
    shutdown();
    const xiaotai_room_config_t xmini = {.command_max_bytes = 12288, .command_pool_bytes = 16384, .settle_before_provisioning = true};
    const xiaotai_room_config_t same = {.command_max_bytes = 12288, .command_pool_bytes = 16384, .manual_connect = true};
    boot(false, true, &xmini, &same);
    assert(app_config_status == XIAOTAI_OK);
    expected.manual = true;
    owner(check_config, &expected);
    shutdown();
    const xiaotai_room_config_t zero = {.manual_connect = true};
    boot(false, true, &xmini, &zero);
    assert(app_config_status == XIAOTAI_OK);
    owner(check_config, &expected);
    shutdown();
    const xiaotai_room_config_t different = {.command_pool_bytes = 32768, .manual_connect = true};
    boot(false, true, &xmini, &different);
    assert(app_config_status == XIAOTAI_ERR_INVALID_STATE);
    expected.manual = false;
    expected.provisioning = true;
    owner(check_config, &expected);
    shutdown();
    const xiaotai_room_config_t s3 = {.command_max_bytes = 32768};
    boot(true, true, &s3, NULL);
    expected = (expected_config_t){32768, 65536, false, false, 100};
    owner(check_config, &expected);
    shutdown();
    const xiaotai_room_config_t cap = {.command_max_bytes = XIAOTAI_LINK_COMMAND_LIMIT_BYTES};
    boot(true, true, &cap, NULL);
    expected.max = expected.pool = XIAOTAI_LINK_COMMAND_LIMIT_BYTES;
    owner(check_config, &expected);
    shutdown();
    const xiaotai_room_config_t share_only = {.command_pool_bytes = 4096};
    boot(true, true, &share_only, NULL);
    expected.max = expected.pool = 4096;
    owner(check_config, &expected);
    shutdown();
}

static const char token_ok[] = "{\"code\":200,\"data\":{\"peer_id\":\"peer\",\"token\":\"test-token\","
                               "\"heartbeat_seconds\":1,\"lease_seconds\":9}}";
static uint32_t activity, activity_gen;

static void current_activity(void *unused)
{
    (void)unused;
    activity = xiaotai_activity_current(NULL);
    activity_gen = xiaotai_activity_generation(activity);
}

static void enable_transport(void *unused)
{
    (void)unused;
    fake_link.online = fake_link.can_connect = true;
}

static void begin_token(bool downlink, const xiaotai_room_config_t *options)
{
    const xiaotai_room_config_t defaults = {.request_downlink = downlink, .command_max_bytes = 4096,
                                             .command_pool_bytes = 12000};
    boot(true, true, options ? options : &defaults, NULL);
    owner(enable_transport, NULL);
    assert(xiaotai_room_attend(true) == XIAOTAI_OK);
    flush();
    expect_request(0, "assignment", NULL);
    ok(0, assigned);
    assert(status().phase == XIAOTAI_ROOM_CONNECTING && fixture.sent == 2U);
    assert(!strcmp(fixture.requests[1].path, "/v1/call/group/device/connect-token"));
    assert(fixture.requests[1].flags == XIAOTAI_PLATFORM_RELEASE);
    const char *session = strstr(fixture.requests[1].body, "\"session_id\":\"");
    assert(session);
    session += strlen("\"session_id\":\"");
    assert(strspn(session, "0123456789abcdef") == 32U && session[32] == '"');
    owner(current_activity, NULL);
    assert(activity && !activity_gen);
}

static void open_job(void *ctx) { (void)ctx; fake_open(); }

typedef struct {
    const char *input, *output;
    unsigned rate, channels;
} join_reply_t;

static void join_response(void *ctx)
{
    const join_reply_t *reply = ctx;
    char payload[320];
    const int n = snprintf(payload, sizeof(payload),
        "{\"jsonrpc\":\"2.0\",\"id\":%u,\"result\":{\"session_id\":\"j\","
        "\"input_audio\":{\"codec\":\"%s\",\"sample_rate\":%u,\"channels\":%u},"
        "\"output_audio\":{\"codec\":\"%s\",\"sample_rate\":8000,\"channels\":1}}}",
        fake_link.generation, reply->input, reply->rate, reply->channels, reply->output);
    assert(n > 0 && (size_t)n < sizeof(payload));
    fake_command(payload);
}

static void join_job(void *ctx)
{
    join_reply_t reply = {ctx ? ctx : "g711a", "g711a", 8000, 1};
    join_response(&reply);
}

static void codec_alias(void)
{
    struct {
        join_reply_t reply;
        bool legacy_alias, accepted;
    } cases[] = {
        {{"g711a", "g711a", 8000, 1}, false, true},
        {{"alaw", "g711a", 8000, 1}, false, false},
        {{"g711a", "alaw", 8000, 1}, false, false},
        {{"alaw", "g711a", 8000, 1}, true, true},
        {{"g711a", "alaw", 8000, 1}, true, true},
        {{"alaw", "alaw", 8000, 1}, true, true},
        {{"opus", "g711a", 8000, 1}, true, false},
        {{"alaw", "pcm", 8000, 1}, true, false},
        {{"g711u", "alaw", 8000, 1}, true, false},
        {{"alaw", "alaw", 16000, 1}, true, false},
        {{"alaw", "alaw", 8000, 2}, true, false},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const xiaotai_room_config_t config = {.accept_alaw_join_reply = cases[i].legacy_alias};
        begin_token(false, &config);
        ok(1, token_ok);
        owner(open_job, NULL);
        const char *request = fake_link.commands[0];
        assert(strstr(request, "\"input_audio\":{\"codec\":\"g711a\",\"sample_rate\":8000,\"channels\":1}"));
        assert(strstr(request, "\"output_audio\":{\"codec\":\"g711a\",\"sample_rate\":8000,\"channels\":1}"));
        owner(join_response, &cases[i].reply);
        assert(fake_link.media_open == cases[i].accepted);
        assert(stats().join_failures == (cases[i].accepted ? 0U : 1U));
        if (cases[i].accepted) assert(status().phase == XIAOTAI_ROOM_LISTENING);
        shutdown();
    }
}

static void joined(void)
{
    ok(1, token_ok);
    owner(open_job, NULL);
    owner(join_job, NULL);
    assert(status().phase == XIAOTAI_ROOM_LISTENING);
    assert(fixture.sent == 3U && strstr(fixture.requests[2].body, "\"state\":\"joined\""));
    assert(fixture.requests[2].method == XIAOTAI_PLATFORM_POST &&
           !strcmp(fixture.requests[2].path, "/v1/call/group/device/presence"));
}

static void tick_job(void *ctx)
{
    advance(ctx);
    room_drive(room_context());
}

static void tick(uint32_t ms) { owner(tick_job, &ms); }

static void socket_stopped(void *unused) { (void)unused; fake_close(XIAOTAI_LINK_END_STOPPED); }
static void signal_job(void *message) { fake_command(message); }

static void connect(void)
{
    for (unsigned mode = 0; mode < 2U; ++mode) {
        begin_token(mode != 0U, NULL);
        joined();
        assert(fake_link.connects == 1U && !strcmp(fake_link.peer, "peer") && !strcmp(fake_link.token, "test-token"));
        const xiaotai_link_profile_t *profile = &fake_link.profile.profile;
        assert(fake_link.profile.kind == XIAOTAI_LINK_WHIP && fake_link.profile.timeout_ms == 9000U);
        assert(profile->audio.receive_request == (mode != 0U) && profile->audio.receive_stream == 1U);
        assert(profile->audio.send_stream == 1U && profile->audio.send_format.codec == XIAOTAI_LINK_CODEC_G711A);
        assert(profile->audio.receive_formats[0].rate_hz == 8000U && !profile->video.send);
        assert(profile->command_max_bytes == 4096U && profile->command_pool_bytes == fixture.transport.command_pool_bytes);
        assert(!strcmp(fake_link.steps, "JOMRP"));
        assert(strstr(fake_link.commands[0], "\"input_audio\":{\"codec\":\"g711a\"") &&
               !strstr(fake_link.commands[0], "opus"));
        assert(strstr(fake_link.commands[1], "\"mic_state\":\"off\""));
        assert(!fake_link.session.duplex && fake_link.session.generation == status().generation &&
               fake_link.session.downlink_burst_ms == 200 && fake_link.session.latency.max_ms == 560U);
        owner(current_activity, NULL);
        assert(activity_gen == fake_link.generation && !status().transmitting);
        ok(2, "{}");
        tick(1000U);
        assert(fixture.sent == 4U);
        tick(1200U);
        assert(strstr(fake_link.commands[2], "get_room_snapshot"));
        ok(3, "{}");
        assert(fixture.sent == 5U); /* response arrived after the next heartbeat was already due */
        const int64_t previous_lease = room_context()->link.lease_until;
        respond(4, 503U, 0, "{}");
        assert(status().phase == XIAOTAI_ROOM_LISTENING && stats().heartbeat_failures == 1U);
        assert(room_context()->link.lease_until == previous_lease);
        const uint32_t remain = (uint32_t)(previous_lease - room_now_ms());
        tick(remain);
        assert(status().assigned && status().generation == 0 && stats().lease_expired == 1U);
        assert(strstr(fake_link.steps, "GLXpD") && !fake_link.media_open);
        owner(current_activity, NULL);
        assert(!activity && !activity_gen);
        shutdown();
    }
    begin_token(false, NULL);
    joined();
    ok(2, "{}");
    owner(signal_job, "{\"method\":\"room_snapshot\",\"params\":{\"room_id\":\"namespace:group_room_exact\","
                      "\"self\":{\"device_id\":\"self\",\"participant_id\":\"me\"},\"participants\":[]}}");
    tick(2000U);
    assert(fake_link.commands_count == 2U); /* valid unsolicited snapshot prevented a request */
    owner(signal_job, "{\"method\":\"room_closed\",\"params\":{\"room_id\":\"wrong:group_room_exact\"}}");
    assert(status().phase == XIAOTAI_ROOM_LISTENING);
    owner(signal_job, "{\"method\":\"room_closed\",\"params\":{\"room_id\":\"namespace:group_room_exact\"}}");
    assert(!status().generation && status().code == 40400);
    assert(fixture.sent == 5U && fixture.requests[3].pending && fixture.requests[4].flags == XIAOTAI_PLATFORM_URGENT);
    ok(3, "{}"); /* old joined heartbeat settles its slot, it cannot clear 40400 */
    expect_request(5, "assignment", NULL);
    assert(status().code == 40400);
    shutdown();
}

static void retry(void)
{
    begin_token(false, NULL);
    joined();
    respond(2, 200U, 40921, "{}");
    assert(status().assigned && !status().generation && status().attempts == 1U);
    assert(fixture.sent == 5U);
    assert(fixture.requests[3].flags == XIAOTAI_PLATFORM_URGENT &&
           strstr(fixture.requests[3].body, "\"state\":\"connect_failed\""));
    expect_request(4, "assignment", NULL);
    respond(3, 200U, 40921, "{}");
    ok(4, assigned);
    assert(status().wait == XIAOTAI_ROOM_WAIT_RETRY && status().code == 40921);
    tick(3999U);
    assert(fixture.sent == 5U);
    tick(1U);
    assert(fixture.sent == 6U && strstr(fixture.requests[5].path, "connect-token"));
    assert(strcmp(fixture.requests[1].body, fixture.requests[5].body));
    respond(5, 200U, 40300, "{}");
    assert(status().phase == XIAOTAI_ROOM_BLOCKED && status().code == 40300);
    assert(fixture.sent == 7U && fixture.requests[6].flags == XIAOTAI_PLATFORM_URGENT);
    ok(6, "{}");
    tick(60000U);
    assert(fixture.sent == 7U);
    shutdown();
}

static unsigned token_requests(void)
{
    unsigned count = 0;
    for (unsigned i = 0; i < fixture.sent; ++i) {
        if (strstr(fixture.requests[i].path, "connect-token")) ++count;
    }
    return count;
}

static void settle_assignments(void)
{
    for (unsigned i = 0; i < fixture.sent; ++i) {
        if (fixture.requests[i].pending && strstr(fixture.requests[i].path, "/assignment")) ok(i, assigned);
    }
}

static void legacy_superseded_lease(void)
{
    static const xiaotai_room_config_t config = {.wait_superseded_lease = true};
    static const char conflict[] = "{\"code\":40921}";
    const char suffix[] = ",\"state\":\"connect_failed\"}";
    begin_token(false, &config);
    joined();
    respond(2, 200, 40921, conflict);
    const size_t tuple = strlen(fixture.requests[1].body) - 1U;
    assert(fixture.sent == 5U && fixture.requests[3].flags == XIAOTAI_PLATFORM_URGENT &&
           !strncmp(fixture.requests[1].body, fixture.requests[3].body, tuple) &&
           !strcmp(fixture.requests[3].body + tuple, suffix));
    settle_assignments();
    tick(4001U);
    assert(fixture.requests[3].pending && token_requests() == 1U && status().wait == XIAOTAI_ROOM_WAIT_RELEASE);
    respond(3, 503, 40921, conflict);
    assert(token_requests() == 1U && !stats().releases);
    tick(2000U);
    assert(!strcmp(fixture.requests[3].body, fixture.requests[5].body));
    respond(5, 200, 40921, conflict);
    assert(token_requests() == 2U && stats().releases == 1U);
    shutdown();

    begin_token(false, &config);
    joined();
    const uint32_t previous_lease = room_context()->link.lease_ms;
    respond(2, 200, 40921, conflict);
    settle_assignments();
    assert(previous_lease > 4001U);
    tick(previous_lease - 1U);
    settle_assignments();
    assert(token_requests() == 1U && !stats().releases);
    tick(1U);
    assert(token_requests() == 2U && !stats().releases);
    respond(3, 200, 40921, conflict);
    assert(token_requests() == 2U && !stats().releases && stats().stale > 0U);
    puts("Room 40921: delayed same-tuple ACK permits retry; absent ACK waits old lease");
    shutdown();
}

static void legacy_superseded_unknown(void)
{
    static const xiaotai_room_config_t config = {.wait_superseded_lease = true};
    static const char conflict[] = "{\"code\":40921}";
    begin_token(false, &config);
    respond(1, 200, 40921, conflict);
    assert(!fake_link.connects && token_requests() == 1U);
    respond(2, 200, 40921, conflict); /* Releases only this new request's id. */
    settle_assignments();
    tick(4000U);
    assert(token_requests() == 1U && status().wait == XIAOTAI_ROOM_WAIT_RELEASE);
    /* The page's leave/attend epoch resets ordinary backoff, not this lease. */
    assert(xiaotai_room_attend(false) == XIAOTAI_OK);
    flush();
    assert(xiaotai_room_attend(true) == XIAOTAI_OK);
    flush();
    settle_assignments();
    assert(token_requests() == 1U);
    tick(40999U);
    settle_assignments();
    assert(token_requests() == 1U && !fake_link.connects);
    tick(1U);
    assert(token_requests() == 2U && !fake_link.connects);
    puts("Room 40921: unknown initial reservation waits 45000 ms across a new Room attendance epoch");
    shutdown();
}

static void heartbeat_expiry_retry(void)
{
    begin_token(false, NULL);
    joined();
    const int64_t expires = room_context()->link.lease_until;
    respond(2, 503, 200, "{\"code\":200}");
    assert(status().phase == XIAOTAI_ROOM_LISTENING && room_context()->link.lease_until == expires);
    tick(2000U);
    expect_request(3, "presence", fixture.requests[2].body);
    respond(3, 503, 200, "{\"code\":200}");
    tick((uint32_t)(expires - room_now_ms()) - 1U);
    assert(status().phase == XIAOTAI_ROOM_LISTENING && fake_link.media_open && !stats().lease_expired);
    tick(1U);
    assert(!status().generation && !fake_link.media_open && fake_link.disconnects == 1U && stats().lease_expired == 1U);
    for (unsigned i = 4; i < fixture.sent; ++i) {
        if (fixture.requests[i].pending && strstr(fixture.requests[i].body, "\"state\":\"connect_failed\""))
            respond(i, 200, 200, receipt);
    }
    settle_assignments();
    tick(3999U);
    assert(token_requests() == 1U);
    tick(1U);
    assert(token_requests() == 2U && status().phase == XIAOTAI_ROOM_CONNECTING);
    puts("Room HTTP 503: failed renewals do not extend the lease; expiry releases then retries automatically");
    shutdown();
}

static void connecting_presence(void)
{
    static const xiaotai_room_config_t config = {.connecting_presence = true};
    for (unsigned mode = 0; mode < 3U; ++mode) {
        begin_token(false, mode ? &config : NULL);
        if (mode == 2U) fixture.connecting_admission_result = XIAOTAI_ERR_NO_MEM;
        ok(1, token_ok);
        assert(fake_link.connects == 1U && fixture.connecting_attempts == (mode != 0U) &&
               fake_link.connecting_attempts_at_connect == fixture.connecting_attempts);
        if (mode == 1U) {
            const size_t tuple = strlen(fixture.requests[1].body) - 1U;
            assert(fixture.requests[2].pending && strstr(fixture.requests[2].path, "/presence") &&
                   !strncmp(fixture.requests[1].body, fixture.requests[2].body, tuple) &&
                   !strcmp(fixture.requests[2].body + tuple, ",\"state\":\"connecting\"}"));
        } else assert(fixture.sent == 2U);
        owner(open_job, NULL);
        owner(join_job, NULL);
        assert(status().phase == XIAOTAI_ROOM_LISTENING && fake_link.media_open && !fake_link.disconnects);
        if (mode == 1U) respond(2, 503, 200, "{\"code\":200}");
        assert(status().phase == XIAOTAI_ROOM_LISTENING && stats().heartbeats == 1U &&
               !stats().heartbeat_failures && fixture.connecting_attempts == (mode != 0U));
        shutdown();
    }
    puts("Room connecting: once before RTC on opt-in; omitted by default; neither refused submit nor HTTP503 blocks the room");
}

static void release(void)
{
    begin_token(false, NULL);
    assert(xiaotai_room_connect() == XIAOTAI_OK);
    flush();
    assert(last_result.action == XIAOTAI_ROOM_ACTION_CONNECT && last_result.error == XIAOTAI_ERR_BUSY);
    assert(xiaotai_room_join("001234", NULL) == XIAOTAI_OK);
    flush();
    assert(last_result.action == XIAOTAI_ROOM_ACTION_JOIN && last_result.error == XIAOTAI_ERR_BUSY);
    assert(xiaotai_room_attend(false) == XIAOTAI_OK);
    flush();
    assert(fixture.sent == 2U && status().assigned && !status().generation);
    ok(1, token_ok);
    assert(!fake_link.connects && fixture.sent == 3U);
    assert(fixture.requests[2].flags == XIAOTAI_PLATFORM_URGENT &&
           strstr(fixture.requests[2].body, "\"state\":\"left\""));
    reply_t stale_session = {.index = 2U, .error = XIAOTAI_ERR_PLATFORM_STALE};
    owner(send_reply, &stale_session);
    assert(stats().release_failures == 1U);
    tick(1999U);
    assert(fixture.sent == 3U);
    tick(1U);
    assert(fixture.sent == 4U && !strcmp(fixture.requests[2].body, fixture.requests[3].body));
    respond(3, 200U, 40400, "{}");
    assert(stats().releases == 1U && status().assigned && status().wait == XIAOTAI_ROOM_WAIT_NOT_ATTENDING);
    shutdown();

    begin_token(false, NULL);
    fake_link.connect_error = XIAOTAI_ERR_NO_MEM;
    ok(1, token_ok);
    assert(!status().generation && stats().connect_failures == 1U &&
           fixture.requests[2].flags == XIAOTAI_PLATFORM_URGENT &&
           strstr(fixture.requests[2].body, "\"state\":\"connect_failed\""));
    shutdown();
}

static uint32_t other_activity, quiet_ticket;
static xiaotai_err_t quiet_result;

static void other_begin(void *unused)
{
    (void)unused;
    const xiaotai_activity_t competitor = {.name = "other", .resources = XIAOTAI_RESOURCE_AUDIO_IN};
    assert(xiaotai_activity_begin(&competitor, &other_activity) == XIAOTAI_OK);
}

static void other_end(void *unused) { (void)unused; xiaotai_activity_end(other_activity); }

static void quiet_begin(void *reason) { quiet_result = xiaotai_quiesce_begin(reason, &quiet_ticket); }
static void quiet_end(void *unused) { (void)unused; xiaotai_quiesce_end(quiet_ticket); }
static void restart_request(void *unused)
{
    (void)unused;
    assert(xiaotai_restart_request("intent", 0U) == XIAOTAI_OK);
}

static void preempt(void)
{
    begin_token(false, NULL);
    joined();
    owner(other_begin, NULL);
    assert(status().attending && status().assigned && !status().generation);
    assert(strstr(fake_link.steps, "GLXpD") && stats().preempted == 1U);
    assert(fixture.sent == 4U && strstr(fixture.requests[3].body, "\"state\":\"suspended\""));
    ok(2, "{}");
    ok(3, "{}");
    assert(status().wait == XIAOTAI_ROOM_WAIT_ACTIVITY && fixture.sent == 4U);
    owner(other_end, NULL);
    assert(fixture.sent == 5U && strstr(fixture.requests[4].path, "connect-token"));
    owner(quiet_begin, XIAOTAI_QUIESCE_COPROCESSOR);
    assert(quiet_result == XIAOTAI_OK && stats().preempted == 2U && !status().generation);
    ok(4, token_ok);
    assert(fixture.sent == 5U); /* retained release, but never send while quiet */
    owner(quiet_end, NULL);
    assert(fixture.sent == 7U && strstr(fixture.requests[5].body, "\"state\":\"suspended\""));
    expect_request(6, "assignment", NULL);
    shutdown();
}

static void held_room_refusal(xiaotai_err_t result, xiaotai_room_action_t action, unsigned expected_results)
{
    assert(result == XIAOTAI_ERR_INVALID_STATE);
    flush();
    assert(results == expected_results && last_result.action == action && last_result.error == result);
    assert(fixture.sent == 1U && !status().generation);
}

static void set_platform_limit(void *ctx) { fixture.limit = *(const unsigned *)ctx; }

/* The room-owner slice of test_p4_runtime_maintenance.py: keeping the room
 * visible and rendering the suspended message are presenter contracts. */
static void environment_room(void)
{
    const xiaotai_room_config_t product = {.talk_keepalive_ms = 750};
    boot(true, true, &product, NULL);
    assert(xiaotai_room_attend(true) == XIAOTAI_OK);
    flush();
    expect_request(0, "assignment", NULL);
    ok(0, assigned);
    assert(status().attending && status().assigned && status().assignment_known);
    assert(!status().generation && !fake_link.connects);

    quiet_ticket = 0;
    owner(quiet_begin, XIAOTAI_QUIESCE_ENVIRONMENT);
    assert(quiet_result == XIAOTAI_OK && quiet_ticket && xiaotai_quiesced());
    const unsigned before_results = results;
    held_room_refusal(xiaotai_room_create(NULL), XIAOTAI_ROOM_ACTION_CREATE, before_results + 1);
    held_room_refusal(xiaotai_room_join("009999", NULL), XIAOTAI_ROOM_ACTION_JOIN, before_results + 2);
    held_room_refusal(xiaotai_room_leave(), XIAOTAI_ROOM_ACTION_LEAVE, before_results + 3);
    held_room_refusal(xiaotai_room_refresh(), XIAOTAI_ROOM_ACTION_REFRESH, before_results + 4);

    owner(notify_twice, NULL); /* The owner still owes a fresh read. */
    for (unsigned i = 0; i < 40U; ++i) tick(1000U);
    assert(xiaotai_room_attend(false) == XIAOTAI_OK);
    flush();
    assert(!status().attending && fixture.sent == 1U);
    assert(xiaotai_room_attend(true) == XIAOTAI_OK);
    flush();
    const xiaotai_room_status_t held = status();
    assert(held.attending && held.assigned && !strcmp(held.room_id, "group_room_exact") && held.assignment_version == 3);
    assert(fixture.sent == 1U && !fake_link.connects && !endings);

    owner(quiet_end, NULL);
    assert(!xiaotai_quiesced() && fixture.sent == 2U && results == before_results + 4);
    expect_request(1, "assignment", NULL);
    ok(1, assigned);
    assert(status().assignment_known && status().attending && status().assignment_version == 3 && fixture.sent == 2U);

    unsigned full = 0;
    owner(set_platform_limit, &full);
    assert(xiaotai_room_leave() == XIAOTAI_OK);
    flush();
    assert(fixture.sent == 2U);
    const xiaotai_room_status_t leave_pending = status();
    quiet_ticket = 0;
    owner(quiet_begin, XIAOTAI_QUIESCE_ENVIRONMENT);
    const xiaotai_room_status_t refused = status();
    assert(quiet_result == XIAOTAI_ERR_BUSY && !quiet_ticket && !xiaotai_quiesced());
    assert(!memcmp(&leave_pending, &refused, sizeof(refused)) && fixture.sent == 2U);
    shutdown();
}

static void quiesce(void)
{
    static const char *const allowed[] = {XIAOTAI_QUIESCE_PROVISIONING, XIAOTAI_QUIESCE_BINDING,
                                          XIAOTAI_QUIESCE_COPROCESSOR, "network", "intent"};
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); ++i) {
        begin_token(false, NULL);
        joined();
        const xiaotai_room_status_t before = status();
        owner(quiet_begin, XIAOTAI_QUIESCE_ENVIRONMENT);
        const xiaotai_room_status_t after = status();
        assert(quiet_result == XIAOTAI_ERR_BUSY && !memcmp(&before, &after, sizeof(before)) && fixture.sent == 3U);
        owner(quiet_begin, (void *)allowed[i]);
        assert(quiet_result == XIAOTAI_OK && !status().generation && stats().preempted == 1U);
        assert(xiaotai_room_create(NULL) == XIAOTAI_ERR_INVALID_STATE);
        assert(xiaotai_room_join("001234", NULL) == XIAOTAI_ERR_INVALID_STATE);
        assert(xiaotai_room_leave() == XIAOTAI_ERR_INVALID_STATE);
        assert(xiaotai_room_refresh() == XIAOTAI_ERR_INVALID_STATE);
        ok(2, "{}");
        for (unsigned n = 0; n < 20; ++n) tick(1000U);
        assert(fixture.sent == 3U && !fake_link.media_open);
        assert(xiaotai_room_attend(false) == XIAOTAI_OK);
        assert(xiaotai_room_attend(true) == XIAOTAI_OK);
        flush();
        assert(status().attending && fixture.sent == 3U);
        owner(quiet_end, NULL);
        assert(fixture.sent == 5U);
        expect_request(4, "assignment", NULL);
        shutdown();
    }
    const xiaotai_room_config_t flags[] = {{.settle_before_provisioning = true}, {.settle_before_binding = true}};
    for (size_t i = 0; i < 2U; ++i) {
        begin_token(false, &flags[i]);
        owner(quiet_begin, (void *)allowed[i]);
        assert(quiet_result == XIAOTAI_ERR_BUSY && status().phase == XIAOTAI_ROOM_CONNECTING);
        owner(quiet_begin, (void *)allowed[1U - i]);
        assert(quiet_result == XIAOTAI_OK && !status().generation);
        shutdown();
    }

    boot(false, true, NULL, NULL);
    assert(xiaotai_room_attend(true) == XIAOTAI_OK);
    flush();
    xiaotai_room_status_t before = status();
    owner(quiet_begin, XIAOTAI_QUIESCE_ENVIRONMENT);
    xiaotai_room_status_t after = status();
    assert(quiet_result == XIAOTAI_ERR_BUSY && !memcmp(&before, &after, sizeof(before)) && fixture.sent == 1U);
    ok(0, assigned);
    assert(xiaotai_room_leave() == XIAOTAI_OK);
    flush();
    expect_request(1, "leave", "{\"room_id\":\"group_room_exact\",\"assignment_version\":3}");
    ok(1, receipt);
    expect_request(2, "assignment", NULL);
    ok(2, assigned); /* confirming is still fenced while no HTTP is in flight */
    before = status();
    owner(quiet_begin, XIAOTAI_QUIESCE_ENVIRONMENT);
    after = status();
    assert(quiet_result == XIAOTAI_ERR_BUSY && !memcmp(&before, &after, sizeof(before)) &&
           fixture.sent == 3U && before.phase == XIAOTAI_ROOM_LEAVING);
    owner(restart_request, NULL);
    assert(xiaotai_quiesced() && fixture.sent == 3U);
    tick(10U);
    assert(xiaotai_signal_take(&restarted, 5000U));
    shutdown();

    begin_token(false, NULL);
    joined();
    ok(2, "{}");
    owner(quiet_begin, XIAOTAI_QUIESCE_ENVIRONMENT); /* settled, even with media open */
    assert(quiet_result == XIAOTAI_OK && !status().generation);
    owner(quiet_end, NULL);
    assert(fixture.sent == 5U && fixture.requests[3].flags == XIAOTAI_PLATFORM_URGENT);
    respond(3, 503U, 0, "{}");
    ok(4, assigned);
    before = status();
    owner(quiet_begin, XIAOTAI_QUIESCE_ENVIRONMENT); /* release owed, no HTTP in flight */
    after = status();
    assert(quiet_result == XIAOTAI_ERR_BUSY && !memcmp(&before, &after, sizeof(before)) && fixture.sent == 5U);
    shutdown();

    environment_room();
}

static void tokens(void)
{
    static const char wrong_type[] = "{\"data\":{\"peer_id\":7,\"token\":\"invalid\","
                                     "\"heartbeat_seconds\":\"1\",\"lease_seconds\":9}}";
    static const char expiry_null[] = "{\"data\":{\"peer_id\":\"peer\",\"token\":\"t\","
                                      "\"heartbeat_seconds\":1,\"lease_seconds\":9,\"expires_at\":null}}";
    static const char expiry_known[] = "{\"data\":{\"peer_id\":\"peer\",\"token\":\"t\","
                                       "\"heartbeat_seconds\":1,\"lease_seconds\":9,\"expires_at\":1700000100}}";
    const struct { unsigned status; const char *body; xiaotai_err_t expected; bool clock; } bad[] = {
        {200, wrong_type, XIAOTAI_FAIL, true}, {201, token_ok, XIAOTAI_FAIL, true},
        {200, expiry_null, XIAOTAI_FAIL, true}, {200, expiry_known, XIAOTAI_ERR_INVALID_STATE, false},
        {200, expiry_known, XIAOTAI_ERR_TIMEOUT, true},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        begin_token(false, NULL);
        fixture.clock_synced = bad[i].clock;
        respond(1, bad[i].status, 200, bad[i].body);
        assert(!fake_link.connects && stats().token_failures == 1 && status().error == bad[i].expected);
        shutdown();
    }
    begin_token(false, NULL);
    reply_t timeout = {.index = 1U, .error = XIAOTAI_ERR_TIMEOUT};
    owner(send_reply, &timeout);
    assert(!fake_link.connects && status().error == XIAOTAI_ERR_TIMEOUT);
    shutdown();
    const char token_2038[] = "{\"data\":{\"peer_id\":\"peer\",\"token\":\"t\","
        "\"heartbeat_seconds\":1,\"lease_seconds\":9,\"expires_at\":2147483648}}";
    begin_token(false, NULL);
    fixture.clock_synced = true;
    ok(1, token_2038);
    assert(fake_link.connects == 1U);
    shutdown();
    const char token_next_second[] = "{\"data\":{\"peer_id\":\"peer\",\"token\":\"t\","
        "\"heartbeat_seconds\":1,\"lease_seconds\":9,\"expires_at\":1700000101}}";
    begin_token(false, NULL);
    fixture.clock_synced = true;
    credential_alloc_delay_ms = 1000;
    ok(1, token_next_second);
    assert(!credential_alloc_delay_ms && !fake_link.connects && status().error == XIAOTAI_ERR_TIMEOUT);
    shutdown();
    char peer[XIAOTAI_LINK_PEER_MAX + 1U], token[XIAOTAI_LINK_TOKEN_MAX + 1U], body[3500];
    const size_t peers[] = {149, 128, 129, 2047, 2048, 129};
    const size_t lengths[] = {5, 5, 5, 1023, 5, 1024};
    for (size_t i = 0; i < sizeof(peers) / sizeof(peers[0]); ++i) {
        memset(peer, 'p', peers[i]); peer[peers[i]] = '\0';
        memset(token, 't', lengths[i]); token[lengths[i]] = '\0';
        assert(snprintf(body, sizeof(body), "{\"data\":{\"peer_id\":\"%s\",\"token\":\"%s\","
               "\"heartbeat_seconds\":1,\"lease_seconds\":9}}", peer, token) < (int)sizeof(body));
        begin_token(false, NULL);
        ok(1, body);
        if (peers[i] < XIAOTAI_LINK_PEER_MAX && lengths[i] < XIAOTAI_LINK_TOKEN_MAX) {
            assert(fake_link.connects == 1U && !strcmp(fake_link.peer, peer) && !strcmp(fake_link.token, token));
        } else assert(!fake_link.connects && status().error == XIAOTAI_FAIL);
        shutdown();
    }
}

static void token_http_lease(void)
{
#define LEASE_BODY(code) "{\"code\":" code ",\"data\":{\"peer_id\":\"fixture-peer\",\"token\":\"fixture-token\", " \
    "\"heartbeat_seconds\":1,\"lease_seconds\":3"
    static const char lease[] = LEASE_BODY("200") "}}";
    static const char heartbeat[] = "{\"code\":200,\"data\":null}";
    const struct {uint32_t rtt, allocation; unsigned connects; bool allocation_used;} boundaries[] = {
        {2999, 0, 1, true}, {3000, 1, 0, false}, {3500, 1, 0, false}, {2999, 1, 0, true},
    };
    for (size_t i = 0; i < sizeof(boundaries) / sizeof(boundaries[0]); ++i) {
        begin_token(false, NULL);
        const int64_t sent_at = room_context()->link.sent_at;
        elapse(boundaries[i].rtt);
        credential_alloc_delay_ms = boundaries[i].allocation;
        ok(1, lease);
        assert(fake_link.connects == boundaries[i].connects);
        assert(credential_alloc_delay_ms == (boundaries[i].allocation_used ? 0 : boundaries[i].allocation));
        if (boundaries[i].connects) {
            assert(fake_link.profile.timeout_ms == 1U && room_context()->link.lease_until == sent_at + 3000);
            assert(!strcmp(fake_link.peer, "fixture-peer") && !strcmp(fake_link.token, "fixture-token"));
        } else assert(!status().generation && status().error == XIAOTAI_ERR_TIMEOUT && stats().token_failures == 1U);
        shutdown();
    }
    puts("token HTTP lease: 2999ms leaves 1ms for WHIP; 3000/3500ms and 2999+1ms allocation never open RTC");

    /* The fake supplies the platform's separately parsed business code. */
    const struct {unsigned http; int32_t code, reported; const char *body;} rejected[] = {
        {503, 200, 0, lease}, {201, 200, 0, lease},
        {200, XIAOTAI_PLATFORM_NO_CODE, 0, LEASE_BODY("200.5") "}}"},
        {200, XIAOTAI_PLATFORM_NO_CODE, 0, LEASE_BODY("200") "}}x"},
        {401, XIAOTAI_PLATFORM_NO_CODE, 401, NULL}, {200, 40320, 40320, "{\"code\":40320}"},
        {200, 200, 0, LEASE_BODY("200") ",\"expires_at\":\"1900000001\"}}"},
        {200, 200, 0, LEASE_BODY("200") ",\"expires_at\":1900000001.5}}"},
    };
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        begin_token(false, NULL);
        respond(1, rejected[i].http, rejected[i].code, rejected[i].body);
        assert(!fake_link.connects && !status().generation && stats().token_failures == 1U);
        assert(status().code == rejected[i].reported);
        shutdown();
    }
    begin_token(false, NULL);
    fixture.clock_synced = false;
    ok(1, LEASE_BODY("200") "}} \r\n\t");
    assert(fake_link.connects == 1U); /* Omitted expiry does not require a wall clock. */
    shutdown();
    begin_token(false, NULL);
    fixture.clock_synced = true;
    wall_ms = INT64_C(2147483648000);
    ok(1, LEASE_BODY("200") ",\"expires_at\":2147483649}}");
    assert(fake_link.connects == 1U);
    shutdown();
    puts("token HTTP admissibility: 503/201 and malformed code/body rejected; 401/40320 retained; legal whitespace and post-2038 expiry allowed");

    const uint32_t rtts[] = {100U, 2500U};
    for (size_t i = 0; i < sizeof(rtts) / sizeof(rtts[0]); ++i) {
        begin_token(false, NULL);
        ok(1, lease);
        owner(open_job, NULL);
        owner(join_job, NULL);
        assert(status().phase == XIAOTAI_ROOM_LISTENING && fixture.sent == 3U);
        const int64_t sent_at = room_context()->link.sent_at;
        elapse(rtts[i]);
        ok(2, heartbeat);
        assert(room_context()->link.lease_until == sent_at + 3000);
        assert(room_context()->link.heartbeat_at == sent_at + 1000);
        if (rtts[i] < 1000U) {
            tick(999U - rtts[i]);
            assert(fixture.sent == 3U);
            tick(1U);
        }
        assert(fixture.sent == 4U && strstr(fixture.requests[3].body, "\"state\":\"joined\""));
        assert(room_context()->link.sent_at == sent_at + (rtts[i] < 1000U ? 1000U : rtts[i]));
        respond(3, 503, 200, heartbeat);
        assert(status().phase == XIAOTAI_ROOM_LISTENING && stats().heartbeat_failures == 1U);
        assert(room_context()->link.lease_until == sent_at + 3000);
        shutdown();
    }
    puts("presence renewals: 100/2500ms replies anchor lease and next heartbeat to send time; HTTP 503 with code 200 does not renew");

    begin_token(false, NULL);
    assert(xiaotai_room_attend(false) == XIAOTAI_OK);
    flush();
    ok(1, lease);
    assert(!fake_link.connects && fixture.sent == 3U && room_context()->release_pending);
    respond(2, 503, 40921, "{\"code\":40921}");
    assert(room_context()->release_pending && !stats().releases && stats().release_failures == 1U);
    tick(1999U);
    assert(fixture.sent == 3U);
    tick(1U);
    assert(fixture.sent == 4U && !strcmp(fixture.requests[2].body, fixture.requests[3].body));
    respond(3, 200, 40921, "{\"code\":40921}");
    assert(!room_context()->release_pending && stats().releases == 1U && !fake_link.connects);
    shutdown();
    puts("release: body 40921 clears only HTTP 200; 503 retains the same tuple for retry");
#undef LEASE_BODY
}

static void legacy_xmini_join(const char *token, bool signal_first)
{
    const xiaotai_room_config_t options = {.notify_deferred_no_mem = true, .accept_alaw_join_reply = false,
                                          .request_downlink = true, .talk_signal_before_gate = signal_first};
    boot(false, true, &options, NULL);
    owner(enable_transport, NULL);
    assert(xiaotai_room_attend(true) == XIAOTAI_OK);
    flush();
    expect_request(0, "assignment", NULL);
    ok(0, assigned);
    expect_token_request(1);
    ok(1, token);
    owner(open_job, NULL);
    owner(join_job, NULL);
    assert(fake_link.profile.profile.audio.receive_request && fake_link.media_open && !fake_link.gate_open);
    assert(status().phase == XIAOTAI_ROOM_LISTENING && fixture.sent == 3U);
    assert(!strcmp(fixture.requests[2].path, "/v1/call/group/device/presence") &&
           fixture.requests[2].method == XIAOTAI_PLATFORM_POST &&
           strstr(fixture.requests[2].body, "\"state\":\"joined\""));
}

static void legacy_xmini_heartbeat(void)
{
    static const char lease[] = "{\"code\":200,\"data\":{\"peer_id\":\"peer\",\"token\":\"test-token\","
                                "\"heartbeat_seconds\":1,\"lease_seconds\":3}}";
    static const char regular[] = "{\"code\":200,\"data\":{\"peer_id\":\"peer\",\"token\":\"test-token\","
                                  "\"heartbeat_seconds\":15,\"lease_seconds\":45}}";
    static const char heartbeat[] = "{\"code\":200,\"data\":null}";
    legacy_xmini_join(lease, false);
    const uint32_t generation = status().generation;
    const int64_t first = room_context()->link.sent_at;
    elapse(2500U);
    ok(2, heartbeat);
    assert(status().generation == generation && room_context()->link.lease_until == first + 3000);
    assert(fixture.sent == 4U && room_context()->link.sent_at == first + 2500);
    expect_request(3, "presence", fixture.requests[2].body);
    elapse(100U);
    ok(3, heartbeat);
    assert(room_context()->link.heartbeat_at == first + 3500 && room_context()->link.lease_until == first + 5500);
    tick(400U);
    assert(status().generation == generation && fake_link.media_open && !fake_link.disconnects && fixture.sent == 4U);
    puts("Xmini heartbeat: 2500 ms reply triggers immediate renewal; second reply keeps media beyond the first lease");
    shutdown();

    legacy_xmini_join(regular, false);
    const int64_t before = room_context()->link.lease_until;
    elapse(400U);
    respond(2, 503, 200, heartbeat);
    assert(status().phase == XIAOTAI_ROOM_LISTENING && fake_link.media_open && !fake_link.disconnects);
    assert(room_context()->link.lease_until == before && stats().heartbeat_failures == 1U);
    printf("Xmini heartbeat HTTP 503: lease unchanged, view_error=%d, failure_count=%u\n",
           status().error, stats().heartbeat_failures);
    tick(1999U);
    assert(fixture.sent == 3U);
    tick(1U);
    expect_request(3, "presence", fixture.requests[2].body);
    ok(3, heartbeat);
    assert(room_context()->link.lease_until == room_context()->link.sent_at + 45000);
    assert(status().phase == XIAOTAI_ROOM_LISTENING && fake_link.media_open && !fake_link.disconnects);
    shutdown();
}

static const char snapshot[];

static void legacy_xmini_ptt(void)
{
    legacy_xmini_join(token_ok, true);
    ok(2, "{}");
    owner(signal_job, (void *)snapshot);
    const uint32_t gen = status().generation;
    const uint16_t members = status().member_count;
    assert(members == 2U);

    bool pressed = false;
    fake_link.mic_command_error = -5;
    assert(xiaotai_room_toggle_talk(gen, &pressed) == XIAOTAI_OK && pressed);
    flush();
    assert(status().generation == gen && status().phase == XIAOTAI_ROOM_LISTENING && status().error == -5 &&
           !status().transmitting && !fake_link.gate_open_calls && fake_link.media_open && !fake_link.disconnects &&
           status().member_count == members && stats().signals_failed == 1U);

    fake_link.mic_command_error = XIAOTAI_OK;
    const unsigned begin = fake_link.step_count;
    assert(xiaotai_room_toggle_talk(gen, &pressed) == XIAOTAI_OK && pressed);
    flush();
    assert(fake_link.step_count == begin + 2U && fake_link.steps[begin] == 'O' && fake_link.steps[begin + 1U] == 'g');
    assert(status().generation == gen && status().phase == XIAOTAI_ROOM_SPEAKING && status().transmitting &&
           status().error == XIAOTAI_OK && fake_link.gate_open_calls == 1U && fake_link.gate_open);

    fake_link.mic_command_error = -5;
    const unsigned end = fake_link.step_count;
    assert(xiaotai_room_toggle_talk(gen, &pressed) == XIAOTAI_OK && !pressed && !fake_link.gate_open);
    flush();
    assert(fake_link.step_count == end + 2U && fake_link.steps[end] == 'G' && fake_link.steps[end + 1U] == 'O');
    assert(status().generation == gen && status().phase == XIAOTAI_ROOM_LISTENING && status().error == -5 &&
           !status().transmitting && fake_link.media_open && !fake_link.disconnects && stats().signals_failed == 2U);

    fake_link.mic_command_error = XIAOTAI_OK;
    assert(xiaotai_room_toggle_talk(gen, &pressed) == XIAOTAI_OK && pressed);
    flush();
    assert(status().generation == gen && status().transmitting && fake_link.gate_open_calls == 2U);
    assert(xiaotai_room_talk(gen, false) == XIAOTAI_OK);
    flush();
    const unsigned commands = fake_link.commands_count;

    assert(xiaotai_run(blocked_owner, NULL) == XIAOTAI_OK && xiaotai_signal_take(&fence, 3000));
    assert(xiaotai_room_toggle_talk(gen, &pressed) == XIAOTAI_OK && pressed && !fake_link.gate_open);
    assert(xiaotai_room_toggle_talk(gen, &pressed) == XIAOTAI_OK && !pressed && !fake_link.gate_open);
    xiaotai_signal_give(&release_owner);
    flush();
    assert(fake_link.commands_count == commands && fake_link.gate_open_calls == 2U && status().generation == gen &&
           !status().transmitting && !fake_link.disconnects);
    puts("Legacy PTT: failed edges keep the same room, next click retries, pending release never opens the gate");
    shutdown();
}

static void late_open(void *ctx) { advance(ctx); fake_open(); }
static void late_join(void *ctx) { advance(ctx); join_job(NULL); }

static void deadlines(void)
{
    static const char lease[] = "{\"data\":{\"peer_id\":\"p\",\"token\":\"t\","
                                "\"heartbeat_seconds\":1,\"lease_seconds\":3}}";
    for (uint32_t delay = 2999; delay <= 3001; ++delay) {
        const bool in_time = delay < 3000;
        begin_token(false, NULL);
        ok(1, lease);
        const int64_t lease_until = room_context()->link.lease_until;
        owner(late_open, &delay);
        if (in_time) {
            assert(!strcmp(fake_link.steps, "J"));
            assert(room_context()->link.deadline == lease_until);
        } else {
            assert(!strchr(fake_link.steps, 'J') && !status().generation && status().error == XIAOTAI_ERR_TIMEOUT);
        }
        shutdown();

        begin_token(false, NULL);
        ok(1, lease);
        owner(open_job, NULL);
        owner(late_join, &delay);
        if (in_time) {
            assert(fake_link.media_open && !strcmp(fake_link.steps, "JOMRP"));
            assert(status().generation && status().generation == fake_link.generation);
        } else {
            assert(!strchr(fake_link.steps, 'O') && !strchr(fake_link.steps, 'M') && !fake_link.permitted);
            assert(!status().generation && status().error == XIAOTAI_ERR_TIMEOUT);
        }
        shutdown();
    }
    begin_token(false, NULL);
    ok(1, token_ok);
    owner(open_job, NULL);
    owner(join_job, "opus");
    assert(!fake_link.media_open && stats().join_failures == 1);
    shutdown();
    begin_token(false, NULL);
    joined();
    owner(socket_stopped, NULL);
    assert(!status().generation && !fake_link.disconnects && status().attempts == 1U);
    shutdown();
}

static const char snapshot[] = "{\"method\":\"room_snapshot\",\"params\":{\"room_id\":\"namespace:group_room_exact\","
    "\"self\":{\"device_id\":\"self\",\"participant_id\":\"me\",\"device_name\":\"server name\"},\"participants\":["
    "{\"participant_id\":\"p1\",\"device_id\":\"remote\",\"device_name\":\"Remote\",\"mic_state\":\"on\"}]}}";
static const char remote_speaking[] = "{\"method\":\"participant_mic_state_changed\",\"params\":{"
    "\"room_id\":\"namespace:group_room_exact\",\"participant_id\":\"p1\",\"mic_state\":\"speaking\"}}";
static const char remote_leave[] = "{\"method\":\"participant_left\",\"params\":{"
    "\"room_id\":\"namespace:group_room_exact\",\"participant_id\":\"p1\"}}";
static const char remote_join[] = "{\"method\":\"participant_joined\",\"params\":{\"room_id\":\"namespace:group_room_exact\"}}";

static void active_room(uint16_t list_limit, uint16_t keepalive)
{
    const xiaotai_room_config_t options = {.members_max = list_limit, .talk_keepalive_ms = keepalive,
        .command_max_bytes = 4096, .command_pool_bytes = 12000};
    begin_token(false, &options);
    joined();
    ok(2, "{}");
}

static void dropped_job(void *unused) { (void)unused; fake_command_gen(fake_link.generation, NULL, true); }
static void stale_command_job(void *message) { fake_command_gen(fake_link.generation + 1U, message, false); }

typedef struct {
    uint32_t generation;
    xiaotai_link_command_t message;
} room_command_sample_t;

static void inject_command(void *ctx)
{
    const room_command_sample_t *sample = ctx;
    fake_link.handlers.command(fake_link.handlers.ctx, sample->generation, &sample->message);
}

static void command_recovery(void)
{
    static char exact[32769];
    const struct { size_t max; bool recover; } cases[] = {{32768, true}, {32768, false}, {12288, false}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
        const size_t max = cases[i].max;
        const xiaotai_room_config_t options = {.command_max_bytes = (uint32_t)max,
                                              .recover_oversized_command = cases[i].recover};
        begin_token(false, &options);
        joined();
        ok(2, "{}");
        const uint32_t gen = fake_link.generation;
        memcpy(exact, snapshot, sizeof(snapshot) - 1U);
        memset(exact + sizeof(snapshot) - 1U, ' ', max + 1U - sizeof(snapshot));
        exact[max] = '\0';
        owner(signal_job, exact);
        assert(stats().snapshots == 1U && status().members_ready && !stats().commands_dropped);
        owner(signal_job, "{broken JSON");
        assert(status().phase == XIAOTAI_ROOM_LISTENING && !fake_link.disconnects);
        room_command_sample_t sample = {gen, {.word = 0x2200U, .size = max, .dropped = true}};
        owner(inject_command, &sample); /* In-range copy/pool failure is not a length violation. */
        assert(stats().commands_dropped == 1U && status().phase == XIAOTAI_ROOM_LISTENING && !fake_link.disconnects);
        owner(signal_job, exact); /* Resets the consecutive drop counter, as before. */

        sample.message.size = max + 1U;
        sample.message.word = 0x2400U;
        owner(inject_command, &sample);
        sample.message.word = 0x2200U;
        ++sample.generation;
        owner(inject_command, &sample);
        sample.generation = gen;
        assert(!fake_link.disconnects && stats().commands_dropped == 1U);
        owner(inject_command, &sample);
        fprintf(stderr, "Room command: limit=%zu bytes=%zu recover=%u phase=%d drops=%u disconnects=%u\n",
                max, sample.message.size, (unsigned)cases[i].recover, status().phase,
                (unsigned)stats().commands_dropped, (unsigned)fake_link.disconnects);
        assert(stats().commands_dropped == 2U);
        if (cases[i].recover) {
            assert(fake_link.disconnects == 1U && !fake_link.media_open && !fake_link.permitted);
            assert(status().attending && status().assigned && !status().generation &&
                   status().attempts == 1U && status().error == XIAOTAI_ERR_TIMEOUT);
            owner(inject_command, &sample);
            owner(socket_stopped, NULL);
            assert(fake_link.disconnects == 1U && stats().commands_dropped == 2U && status().attempts == 1U);
            assert(fixture.sent == 5U && strstr(fixture.requests[3].body, "\"state\":\"connect_failed\""));
            expect_request(4, "assignment", NULL);
            ok(3, "{}");
            ok(4, assigned);
            tick(3999U);
            assert(status().wait == XIAOTAI_ROOM_WAIT_RETRY && fixture.sent == 5U);
            tick(1U);
            expect_token_request(5);
            ok(5, token_ok);
            owner(open_job, NULL);
            owner(join_job, NULL);
            assert(status().phase == XIAOTAI_ROOM_LISTENING && fake_link.disconnects == 1U &&
                   fake_link.generation != gen && fake_link.connects == 2U);
        } else {
            assert(!fake_link.disconnects && status().phase == XIAOTAI_ROOM_LISTENING);
        }
        shutdown();
    }
}

static void members(void)
{
    active_room(3, 0);
    assert(!status().members_ready);
    owner(signal_job, (void *)snapshot);
    xiaotai_room_status_t view = status();
    assert(view.members_ready && view.member_count == 2U && view.listed_count == 2U && !view.speaking_count &&
           !view.members_truncated && !strcmp(view.self_name, "server name"));
    assert(member_events == 1U && voice_events == 0U);
    xiaotai_room_member_t row;
    uint32_t revision;
    assert(xiaotai_room_members(0, &row, 1, &revision) == 1U && revision == view.members_revision);
    assert(!strcmp(row.name, "Remote") && row.mic == XIAOTAI_ROOM_MIC_ON && !row.self);
    assert(xiaotai_room_members(1, &row, 1, NULL) == 1U && row.self && !strcmp(row.participant_id, "me"));
    assert(!xiaotai_room_members(2, &row, 1, NULL));
    owner(signal_job, (void *)snapshot); /* same contents: no artificial revision or event */
    assert(status().members_revision == revision && member_events == 1U);
    owner(signal_job, (void *)remote_speaking);
    assert(status().speaking_count == 1U && member_events == 2U && voice_events == 1U && voices == 1);
    owner(signal_job, (void *)remote_speaking);
    assert(member_events == 2U && voice_events == 1U);
    owner(signal_job, (void *)remote_leave);
    assert(status().member_count == 1U && status().speaking_count == 0U && member_events == 3U &&
           voice_events == 2U && voices == 0);

    owner(signal_job, (void *)remote_join);
    owner(signal_job, (void *)remote_join);
    tick(199U);
    assert(fake_link.commands_count == 2U);
    tick(1U);
    assert(fake_link.commands_count == 3U && strstr(fake_link.commands[2], "get_room_snapshot"));
    owner(signal_job, (void *)remote_join);
    tick(499U);
    assert(fake_link.commands_count == 3U);
    tick(1U);
    assert(fake_link.commands_count == 4U);

    char large_field[2600];
    const int prefix = snprintf(large_field, sizeof(large_field), "{\"method\":\"room_snapshot\",\"params\":{"
        "\"room_id\":\"namespace:group_room_exact\",\"self\":{\"device_id\":\"self\",\"participant_id\":\"me\"},"
        "\"participants\":[{\"participant_id\":\"p\",\"x\":\"");
    assert(prefix > 0 && (size_t)prefix + 2048U + 7U < sizeof(large_field));
    memset(large_field + prefix, 'x', 2048U);
    strcpy(large_field + prefix + 2048, "\"}]}}");
    owner(signal_job, large_field);
    assert(status().members_ready && status().member_count == 2U && stats().snapshots == 3U);

    /* Non-snapshot size limit also applies to a tiny message with many tokens. */
    char large_count[300];
    const int head = snprintf(large_count, sizeof(large_count), "{\"method\":\"participant_joined\",\"params\":{"
        "\"room_id\":\"namespace:group_room_exact\",\"x\":[");
    char *tail = large_count + head;
    for (unsigned i = 0; i < 56U; ++i) { *tail++ = '0'; if (i < 55U) *tail++ = ','; }
    strcpy(tail, "]}}");
    size_t tokens;
    assert(xiaotai_json_parse(large_count, strlen(large_count), NULL, 0, &tokens, NULL) == XIAOTAI_OK && tokens == 65U);
    owner(signal_job, large_count);
    assert(status().phase == XIAOTAI_ROOM_LISTENING && stats().commands_dropped == 1U);
    owner(signal_job, (void *)snapshot); /* any normal command interrupts the consecutive failures */
    owner(dropped_job, NULL);
    assert(status().phase == XIAOTAI_ROOM_LISTENING && stats().commands_dropped == 2U);
    owner(signal_job, large_count);
    assert(status().phase == XIAOTAI_ROOM_BLOCKED && status().error == XIAOTAI_ERR_INVALID_SIZE &&
           stats().commands_dropped == 3U && fake_link.disconnects == 1U);
    shutdown();

    active_room(1, 0);
    owner(signal_job, (void *)snapshot);
    assert(status().member_count == 2U && status().listed_count == 1U && status().members_truncated &&
           stats().members_dropped == 1U);
    owner(signal_job, "{\"method\":\"room_snapshot\",\"params\":{\"room_id\":\"namespace:group_room_exact\","
        "\"self\":{\"participant_id\":\"me\",\"device_id\":\"self\"},\"participants\":["
        "{\"participant_id\":\"p\",\"device_name\":\"01234567890123456789012345678901234567890123456789012345678901234\"}]}}");
    assert(status().phase == XIAOTAI_ROOM_BLOCKED && status().error == XIAOTAI_ERR_INVALID_SIZE &&
           stats().snapshots_rejected == 1U);
    shutdown();

    active_room(3, 0);
    /* S3 and CoreS3 wire: the local ID can occur only in participants. */
    owner(signal_job, "{\"method\":\"room_snapshot\",\"params\":{\"room_id\":\"namespace:group_room_exact\","
        "\"self\":{\"device_id\":\"self\"},\"participants\":[{\"participant_id\":\"me\",\"device_id\":\"self\","
        "\"device_name\":\"local row\"},{\"participant_id\":\"p\",\"mic_state\":\"off\"}]}}");
    assert(status().members_ready && status().member_count == 2U && !strcmp(status().self_name, "local row"));
    assert(xiaotai_room_members(0, &row, 1, NULL) == 1U && row.self);
    owner(signal_job, "{\"method\":\"room_snapshot\",\"params\":{\"room_id\":\"namespace:group_room_exact\","
        "\"self\":{\"participant_id\":\"me\",\"device_id\":\"self\",\"device_name\":\"\"},\"participants\":[]}}");
    assert(status().members_ready && status().member_count == 1U && !status().self_name[0]);
    shutdown();

    begin_token(false, NULL);
    ok(1, "{\"code\":200,\"data\":{\"peer_id\":\"peer\",\"token\":\"test-token\","
          "\"heartbeat_seconds\":20,\"lease_seconds\":120}}");
    owner(open_job, NULL);
    owner(join_job, NULL);
    ok(2, "{}");
    owner(signal_job, (void *)snapshot);
    tick(44999U);
    assert(status().members_ready && fake_link.commands_count == 2U);
    tick(1U);
    assert(!status().members_ready && status().generation && fake_link.commands_count == 3U &&
           strstr(fake_link.commands[2], "get_room_snapshot"));
    owner(signal_job, (void *)snapshot);
    assert(status().members_ready && fake_link.commands_count == 3U);
    shutdown();
}

static void snapshot_many(char *buffer, size_t size, const char *method, const char *scope, unsigned members,
                          bool maximum_width)
{
    xiaotai_json_writer_t writer;
    xiaotai_json_writer_init(&writer, buffer, size);
    xiaotai_json_object_begin(&writer, NULL);
    xiaotai_json_put_string(&writer, "method", method);
    xiaotai_json_object_begin(&writer, "params");
    xiaotai_json_put_string(&writer, "room_id", scope);
    xiaotai_json_object_begin(&writer, "self");
    xiaotai_json_put_string(&writer, "participant_id", "me");
    xiaotai_json_put_string(&writer, "device_id", "self");
    xiaotai_json_object_end(&writer);
    xiaotai_json_array_begin(&writer, "participants");
    for (unsigned i = 0; i < members; ++i) {
        char id[97], device[65];
        if (maximum_width) {
            memset(id, 'p', sizeof(id) - 1U);
            memset(device, 'd', sizeof(device) - 1U);
            id[sizeof(id) - 1U] = device[sizeof(device) - 1U] = '\0';
            id[0] = device[0] = (char)('0' + i / 10U);
            id[1] = device[1] = (char)('0' + i % 10U);
        } else assert(snprintf(id, sizeof(id), "p%u", i) > 0);
        xiaotai_json_object_begin(&writer, NULL);
        xiaotai_json_put_string(&writer, "participant_id", id);
        if (maximum_width) {
            xiaotai_json_put_string(&writer, "device_id", device);
            xiaotai_json_put_string(&writer, "mic_state", "speaking");
        }
        xiaotai_json_object_end(&writer);
    }
    xiaotai_json_array_end(&writer);
    xiaotai_json_object_end(&writer);
    xiaotai_json_object_end(&writer);
    assert(xiaotai_json_writer_end(&writer, NULL) == XIAOTAI_OK);
}

// Exercise the S3 owner cases that are independent of the transport lease boundaries.
static void legacy_owner(bool retry_after_online)
{
    static const char denied[] = "{\"code\":40300}";
    boot(true, true, NULL, NULL);
    owner(enable_transport, NULL);
    assert(xiaotai_room_attend(true) == XIAOTAI_OK);
    flush();
    expect_request(0, "assignment", NULL);
    respond(0, 200, 40300, denied);
    assert(status().phase == XIAOTAI_ROOM_BLOCKED && status().code == 40300 &&
           !status().assignment_known && !status().generation && !fake_link.connects);
    tick(60000U);
    assert(fixture.sent == 1 && !fake_link.connects);
    assert(xiaotai_room_refresh() == XIAOTAI_OK);
    flush();
    expect_request(1, "assignment", NULL);
    ok(1, empty);
    assert(status().phase == XIAOTAI_ROOM_EMPTY && status().assignment_known && !status().code);
    shutdown();

    const xiaotai_room_config_t options = {.command_max_bytes = 32768, .recover_oversized_command = true,
        .accept_alaw_join_reply = true};
    begin_token(false, &options);
    joined();
    ok(2, "{}");
    static char full[32769];
    snapshot_many(full, sizeof(full), "room_snapshot", "namespace:group_room_exact", 99, true);
    assert(strlen(full) <= options.command_max_bytes);
    size_t tokens;
    assert(xiaotai_json_parse(full, strlen(full), NULL, 0, &tokens, NULL) == XIAOTAI_OK);
    const unsigned allocations = tracked_allocations, frees = tracked_frees;
    tracked_bytes = tokens * sizeof(xiaotai_json_token_t);
    owner(signal_job, full);
    tracked_bytes = 0;
    assert(tracked_allocations == allocations + 1 && tracked_frees == frees + 1 && !tracked_allocation);
    assert(status().phase == XIAOTAI_ROOM_LISTENING && status().member_count == 100 &&
           status().listed_count == 100 && status().speaking_count == 99 && !status().members_truncated);
    xiaotai_room_member_t last[3];
    assert(xiaotai_room_members(99, last, 3, NULL) == 1);
    printf("S3 legacy snapshot: %zu bytes with 96/64-byte IDs, count=100, page offset 99 gives one row\n", strlen(full));

    const uint32_t generation = status().generation;
    const uint32_t assignment_version = status().assignment_version;
    assert(xiaotai_room_talk(generation, true) == XIAOTAI_OK && fake_link.gate_open);
    flush();
    tick(1000U);
    assert(fixture.sent == 4 && !strcmp(fixture.requests[3].path, "/v1/call/group/device/presence") &&
           strstr(fixture.requests[3].body, "\"state\":\"joined\""));
    respond(3, 200, 40300, denied);
    assert(status().phase == XIAOTAI_ROOM_BLOCKED && status().code == 40300 && status().assigned &&
           !status().generation && !fake_link.gate_open && !fake_link.media_open && fake_link.disconnects == 1);
    assert(xiaotai_room_talk(generation, true) == XIAOTAI_ERR_INVALID_STATE);
    assert(fixture.sent == 5 && strstr(fixture.requests[4].body, "\"state\":\"connect_failed\""));
    ok(4, "{}");
    tick(60000U);
    assert(fixture.sent == 5 && fake_link.disconnects == 1);
    unsigned assignment_request = 5;
    if (retry_after_online) {
        owner(change_online, NULL);
        assert(status().wait == XIAOTAI_ROOM_WAIT_OFFLINE && status().code == 40300 &&
               status().assigned && status().assignment_version == assignment_version && !status().generation);
        tick(60000U);
        assert(fixture.sent == 5 && fake_link.connects == 1 && !fake_link.media_open && !fake_link.gate_open);
        owner(change_online, &fixture);
        expect_request(assignment_request, "assignment", NULL);
        respond(assignment_request++, 503, 0, "{}");
        printf("reconnect assignment HTTP 503: requests=%u cached_authoritative=%u\n",
               fixture.sent, (unsigned)status().assignment_known);
        fflush(stdout);
        assert(fixture.sent == assignment_request && status().assigned && !status().assignment_known &&
               status().assignment_version == assignment_version && fake_link.connects == 1);
        tick(4999U);
        assert(fixture.sent == assignment_request && !fake_link.media_open && !fake_link.gate_open);
        tick(1U);
    } else {
        assert(xiaotai_room_refresh() == XIAOTAI_OK);
        flush();
    }
    expect_request(assignment_request, "assignment", NULL);
    ok(assignment_request, assigned);
    printf("same assignment after %s: pending_requests=%u phase=%d wait=%d code=%d retry_latched=%u\n",
           retry_after_online ? "back online" : "explicit refresh", fixture.sent, status().phase, status().wait,
           status().code, (unsigned)(room_context()->link.retry_at == INT64_MAX));
    fflush(stdout);
    expect_token_request(assignment_request + 1U);
    ok(assignment_request + 1U, token_ok);
    owner(open_job, NULL);
    owner(join_job, NULL);
    assert(status().phase == XIAOTAI_ROOM_LISTENING && status().generation != generation &&
           status().assignment_version == assignment_version && !status().code &&
           fake_link.connects == 2 && !fake_link.gate_open);
    puts("S3 denied assignment and active presence: no retry storm, media retired, recovery gets a new generation");
    shutdown();
}

static void local_refresh_exclusive(void)
{
    const xiaotai_room_config_t options = {.members_max = 3, .talk_keepalive_ms = 750,
        .talk_keepalive_exclusive = true, .command_max_bytes = 4096, .command_pool_bytes = 12000};
    begin_token(false, &options);
    joined();
    ok(2, "{}");
    const uint32_t gen = status().generation;
    assert(gen && xiaotai_room_hold_local(gen, 1, true) == XIAOTAI_OK);
    flush();
    assert(status().transmitting && fake_link.gate_open);
    const unsigned closes = fake_link.gate_close_calls;
    tick(750U);
    assert(status().transmitting && fake_link.gate_open && fake_link.gate_close_calls == closes);
    const int64_t now = room_now_ms();
    assert(room_talk_due(room_context(), now) == now + 1);
    assert(xiaotai_room_hold_local_refresh(gen, 1) == XIAOTAI_OK);
    tick(750U);
    assert(status().transmitting && fake_link.gate_open && fake_link.gate_close_calls == closes);
    tick(1U);
    assert(!status().transmitting && !fake_link.gate_open && fake_link.gate_close_calls == closes + 1U &&
           stats().talk_keepalive_releases == 1U);
    assert(xiaotai_room_hold_local_refresh(gen, 1) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_room_hold_local(gen, 1, false) == XIAOTAI_OK && fake_link.gate_close_calls == closes + 1U);
    puts("exclusive local refresh: owner keeps 750 ms open, wakes at 751 ms, closes once");
    shutdown();
}

static void count_only(void)
{
    active_room(XIAOTAI_ROOM_NO_MEMBER_LIST, 0);
    assert(!room_context()->members.rows);
    size_t tokens = 0;
    assert(xiaotai_json_parse(snapshot, strlen(snapshot), NULL, 0, &tokens, NULL) == XIAOTAI_OK);
    tracked_bytes = tokens * sizeof(xiaotai_json_token_t);
    owner(signal_job, (void *)snapshot);
    tracked_bytes = 0;
    assert(tracked_allocations == 1U && tracked_frees == 1U && !tracked_allocation);
    assert(status().member_count == 2U && !status().listed_count && status().members_ready &&
           !status().members_truncated && !room_context()->members.rows);
    xiaotai_room_member_t row;
    assert(!xiaotai_room_members(0, &row, 1, NULL));
    owner(stale_command_job, (void *)snapshot);
    owner(signal_job, "{\"method\":\"room_closed\",\"params\":{\"room_id\":\"wrong:group_room_exact\"}}");
    assert(status().member_count == 2U && stats().snapshots == 1U && stats().stale == 2U);
    owner(signal_job, (void *)remote_speaking);
    assert(!status().members_ready && status().member_count == 2U && !status().speaking_count);
    tick(200U);
    assert(strstr(fake_link.commands[2], "get_room_snapshot"));
    owner(signal_job, "{\"method\":\"room_snapshot\",\"params\":{\"room_id\":\"namespace:group_room_exact\","
        "\"self\":{\"participant_id\":\"me\",\"device_id\":\"self\"},\"participants\":["
        "{\"participant_id\":\"p\"},{\"participant_id\":\"\\u0070\"}]}}");
    assert(status().phase == XIAOTAI_ROOM_BLOCKED && stats().snapshots_rejected == 1U);
    shutdown();

    active_room(XIAOTAI_ROOM_NO_MEMBER_LIST, 0);
    owner(signal_job, "{\"method\":\"room_snapshot\",\"params\":{\"room_id\":\"namespace:group_room_exact\","
        "\"self\":{\"participant_id\":\"other\",\"device_id\":\"not-self\"},\"participants\":[]}}");
    assert(status().phase == XIAOTAI_ROOM_BLOCKED && stats().snapshots_rejected == 1U && !status().members_ready);
    shutdown();

    active_room(XIAOTAI_ROOM_NO_MEMBER_LIST, 0);
    char many[4096];
    snapshot_many(many, sizeof(many), "room_snapshot", "namespace:group_room_exact", 99U, false);
    owner(signal_job, many);
    assert(status().phase == XIAOTAI_ROOM_LISTENING && status().member_count == 100U && !status().listed_count);
    snapshot_many(many, sizeof(many), "room_snapshot", "namespace:group_room_exact", 100U, false);
    owner(signal_job, many);
    assert(status().phase == XIAOTAI_ROOM_BLOCKED && stats().snapshots_rejected == 1U);
    shutdown();

    active_room(XIAOTAI_ROOM_NO_MEMBER_LIST, 0);
    snapshot_many(many, sizeof(many), "participant_joined", "namespace:group_room_exact", 101U, false);
    assert(xiaotai_json_parse(many, strlen(many), NULL, 0, &tokens, NULL) == XIAOTAI_OK);
    tracked_bytes = tokens * sizeof(xiaotai_json_token_t);
    const unsigned before = tracked_allocations;
    owner(signal_job, many);
    assert(status().phase == XIAOTAI_ROOM_LISTENING && !stats().snapshots_rejected && stats().commands_dropped == 1U &&
           tracked_allocations == before);
    tracked_bytes = 0;
    owner(signal_job, (void *)snapshot);
    snapshot_many(many, sizeof(many), "room_snapshot", "wrong:group_room_exact", 101U, false);
    assert(xiaotai_json_parse(many, strlen(many), NULL, 0, &tokens, NULL) == XIAOTAI_OK);
    tracked_bytes = tokens * sizeof(xiaotai_json_token_t);
    owner(signal_job, many);
    assert(status().phase == XIAOTAI_ROOM_LISTENING && status().member_count == 2U && stats().stale == 1U);
    assert(!stats().snapshots_rejected && tracked_allocations == before);
    tracked_bytes = 0;
    snapshot_many(many, sizeof(many), "room_snapshot", "namespace:group_room_exact", 101U, false);
    assert(xiaotai_json_parse(many, strlen(many), NULL, 0, &tokens, NULL) == XIAOTAI_OK);
    tracked_bytes = tokens * sizeof(xiaotai_json_token_t);
    owner(signal_job, many);
    assert(status().phase == XIAOTAI_ROOM_BLOCKED && stats().snapshots_rejected == 1U && tracked_allocations == before);
    tracked_bytes = 0;
    shutdown();

    /* The old G19 pair: the temporary disconnect preserves membership; only
     * authoritative leave changes it. Keep both HTTP fields in the assertion. */
    active_room(XIAOTAI_ROOM_NO_MEMBER_LIST, 0);
    assert(xiaotai_room_disconnect() == XIAOTAI_OK);
    flush();
    assert(status().assigned && status().assignment_version == 3U && !status().generation);
    assert(fixture.sent == 4U && strstr(fixture.requests[3].body, "\"state\":\"suspended\""));
    ok(3, "{}");
    assert(xiaotai_room_leave() == XIAOTAI_OK);
    flush();
    expect_request(4, "leave", "{\"room_id\":\"group_room_exact\",\"assignment_version\":3}");
    assert(status().assigned && status().assignment_version == 3U);
    ok(4, left);
    assert(!status().assigned && status().assignment_version == 4U);
    shutdown();
}

static void no_op(void *unused) { (void)unused; }

static void talk(void)
{
    active_room(3, 750U);
    owner(signal_job, (void *)snapshot);
    const uint32_t gen = status().generation;
    assert(xiaotai_room_talk(gen + 1U, true) == XIAOTAI_ERR_INVALID_STATE);
    fake_link.mic_muted = true;
    assert(xiaotai_room_talk(gen, true) == XIAOTAI_ERR_INVALID_STATE && !fake_link.gate_open_calls);
    fake_link.mic_muted = false;
    assert(xiaotai_room_talk(gen, true) == XIAOTAI_OK && fake_link.gate_open && !fake_link.gate_on_owner);
    flush();
    assert(status().phase == XIAOTAI_ROOM_SPEAKING && status().speaking_count == 1U && !voice_events &&
           strstr(fake_link.commands[2], "\"mic_state\":\"speaking\""));
    tick(400U);
    assert(xiaotai_room_talk(gen, true) == XIAOTAI_OK && fake_link.gate_open_calls == 1U);
    tick(749U);
    assert(fake_link.gate_open);
    tick(1U);
    assert(!fake_link.gate_open && fake_link.gate_tail_ms == 600U && stats().talk_keepalive_releases == 1U &&
           status().phase == XIAOTAI_ROOM_LISTENING && !status().speaking_count);

    assert(xiaotai_room_talk(gen, true) == XIAOTAI_OK);
    flush();
    assert(xiaotai_run(blocked_owner, NULL) == XIAOTAI_OK && xiaotai_signal_take(&fence, 3000));
    while (xiaotai_run(no_op, NULL) == XIAOTAI_OK) {}
    assert(xiaotai_room_talk(gen + 1U, false) == XIAOTAI_OK && !fake_link.gate_open &&
           fake_link.gate_tail_ms == 600U && !fake_link.gate_on_owner && !fake_link.mic_muted);
    assert(xiaotai_room_talk(gen, true) == XIAOTAI_ERR_NO_MEM && !fake_link.gate_open);
    xiaotai_signal_give(&release_owner);
    xiaotai_err_t queued = XIAOTAI_ERR_NO_MEM;
    for (unsigned i = 0; i < 300U && queued == XIAOTAI_ERR_NO_MEM; ++i) {
        queued = xiaotai_run(barrier, NULL);
        if (queued == XIAOTAI_ERR_NO_MEM) xiaotai_delay_ms(1);
    }
    assert(queued == XIAOTAI_OK && xiaotai_signal_take(&fence, 3000));
    tick(200U);
    assert(status().phase == XIAOTAI_ROOM_LISTENING && !status().transmitting && stats().talk_refused == 3U);
    assert(room_talk_due(room_context(), room_now_ms()) == INT64_MAX);
    assert(xiaotai_room_talk(gen, true) == XIAOTAI_OK);
    flush();
    fake_link.mic_command_error = XIAOTAI_FAIL;
    const uint32_t failures = stats().signals_failed;
    assert(xiaotai_room_talk(gen, false) == XIAOTAI_OK && !fake_link.gate_open);
    flush();
    assert(status().phase != XIAOTAI_ROOM_SPEAKING && !status().generation && !fake_link.media_open &&
           stats().signals_failed == failures + 1U);
    shutdown();

    active_room(3, 0);
    fake_link.mic_command_error = XIAOTAI_FAIL;
    assert(xiaotai_room_talk(status().generation, true) == XIAOTAI_OK);
    flush();
    assert(!fake_link.gate_open && status().phase != XIAOTAI_ROOM_SPEAKING && stats().signals_failed == 1U);
    shutdown();
}

static void local_hold(void)
{
    const uint16_t tails[] = {XIAOTAI_ROOM_NO_TAIL, 600U};
    for (size_t i = 0; i < sizeof(tails) / sizeof(*tails); ++i) {
        const xiaotai_room_config_t options = {.members_max = 3, .talk_tail_ms = tails[i],
            .command_max_bytes = 4096, .command_pool_bytes = 12000};
        begin_token(false, &options);
        joined();
        ok(2, "{}");
        const uint32_t gen = status().generation;
        assert(gen && xiaotai_room_hold_local(gen, 0, true) == XIAOTAI_ERR_INVALID_ARG);
        assert(xiaotai_room_hold_local(gen, UINT32_MAX, true) == XIAOTAI_OK);
        assert(fake_link.gate_open && !fake_link.gate_on_owner);
        flush();
        assert(status().transmitting);

        assert(xiaotai_run(blocked_owner, NULL) == XIAOTAI_OK && xiaotai_signal_take(&fence, 3000));
        while (xiaotai_run(no_op, NULL) == XIAOTAI_OK) {}
        assert(xiaotai_room_hold_local(gen + 1U, UINT32_MAX, false) == XIAOTAI_OK && fake_link.gate_open);
        assert(xiaotai_room_hold_local(gen, UINT32_MAX - 1U, false) == XIAOTAI_OK && fake_link.gate_open);
        assert(xiaotai_room_hold_local(gen, UINT32_MAX, false) == XIAOTAI_OK && !fake_link.gate_open &&
               !fake_link.gate_on_owner && fake_link.gate_tail_ms == (i ? 600U : 0U));
        assert(xiaotai_room_hold_local(gen, UINT32_MAX, true) == XIAOTAI_ERR_INVALID_STATE && !fake_link.gate_open);
        assert(xiaotai_room_hold_local(gen, 1, true) == XIAOTAI_ERR_NO_MEM && !fake_link.gate_open);
        xiaotai_signal_give(&release_owner);
        xiaotai_err_t queued = XIAOTAI_ERR_NO_MEM;
        for (unsigned step = 0; step < 300U && queued == XIAOTAI_ERR_NO_MEM; ++step) {
            queued = xiaotai_run(barrier, NULL);
            if (queued == XIAOTAI_ERR_NO_MEM) xiaotai_delay_ms(1);
        }
        assert(queued == XIAOTAI_OK && xiaotai_signal_take(&fence, 3000));
        tick(200U);
        assert(!status().transmitting && status().phase == XIAOTAI_ROOM_LISTENING &&
               strstr(fake_link.commands[fake_link.commands_count - 1U], "\"mic_state\":\"off\""));
        assert(xiaotai_room_hold_local(gen, 1, true) == XIAOTAI_ERR_INVALID_STATE);
        assert(xiaotai_room_hold_local(gen, 2, true) == XIAOTAI_OK && fake_link.gate_open);
        assert(xiaotai_room_hold_local(gen, UINT32_MAX, false) == XIAOTAI_OK && fake_link.gate_open);
        assert(xiaotai_room_hold_local(gen, 1, false) == XIAOTAI_OK && fake_link.gate_open);
        assert(xiaotai_room_hold_local(gen, 2, false) == XIAOTAI_OK && !fake_link.gate_open);
        assert(xiaotai_room_talk(gen, true) == XIAOTAI_OK && fake_link.gate_open);
        assert(xiaotai_room_hold_local(gen, 3, true) == XIAOTAI_ERR_BUSY);
        assert(xiaotai_room_hold_local(gen, 3, false) == XIAOTAI_OK && fake_link.gate_open);
        assert(xiaotai_room_talk(gen + 1U, false) == XIAOTAI_OK && !fake_link.gate_open);
        shutdown();
    }
}

static void local_refresh(void)
{
    active_room(3, 750U);
    const uint32_t gen = status().generation;
    assert(gen && xiaotai_room_hold_local_refresh(0, 1) == XIAOTAI_ERR_INVALID_ARG &&
           xiaotai_room_hold_local_refresh(gen, 0) == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_room_hold_local(gen, 1, true) == XIAOTAI_OK);
    flush();
    assert(status().transmitting && fake_link.gate_open);
    const unsigned opens = fake_link.gate_open_calls;
    const unsigned closes = fake_link.gate_close_calls;
    tick(749U);
    assert(xiaotai_room_hold_local_refresh(gen, 1) == XIAOTAI_OK);
    assert(fake_link.gate_open && fake_link.gate_open_calls == opens && fake_link.gate_close_calls == closes);
    tick(749U);
    assert(xiaotai_room_hold_local_refresh(gen, 2) == XIAOTAI_ERR_INVALID_STATE &&
           xiaotai_room_hold_local_refresh(gen + 1U, 1) == XIAOTAI_ERR_INVALID_STATE);
    assert(status().transmitting && fake_link.gate_open && fake_link.gate_open_calls == opens &&
           fake_link.gate_close_calls == closes);
    tick(1U);
    assert(!status().transmitting && !fake_link.gate_open && fake_link.gate_close_calls == closes + 1U &&
           stats().talk_keepalive_releases == 1U);
    assert(xiaotai_room_hold_local_refresh(gen, 1) == XIAOTAI_ERR_INVALID_STATE && !fake_link.gate_open &&
           fake_link.gate_close_calls == closes + 1U);
    puts("local refresh: accepted at 749 ms, wrong scope inert, owner expired at 750 ms");

    assert(xiaotai_room_hold_local(gen, 2, true) == XIAOTAI_OK);
    flush();
    assert(status().transmitting && fake_link.gate_open);
    assert(xiaotai_run(blocked_owner, NULL) == XIAOTAI_OK && xiaotai_signal_take(&fence, 3000));
    const unsigned late_closes = fake_link.gate_close_calls;
    const unsigned late_opens = fake_link.gate_open_calls;
    const unsigned late_commands = fake_link.commands_count;
    uint32_t elapsed = 750U;
    advance(&elapsed);
    assert(xiaotai_room_hold_local_refresh(gen, 2) == XIAOTAI_OK && fake_link.gate_open);
    elapsed = 751U;
    advance(&elapsed);
    assert(xiaotai_room_hold_local_refresh(gen, 1) == XIAOTAI_ERR_INVALID_STATE && fake_link.gate_open &&
           fake_link.gate_close_calls == late_closes);
    assert(xiaotai_room_hold_local_refresh(gen, 2) == XIAOTAI_ERR_INVALID_STATE && !fake_link.gate_open &&
           fake_link.gate_close_calls == late_closes + 1U && stats().talk_keepalive_releases == 2U);
    assert(xiaotai_room_hold_local_refresh(gen, 2) == XIAOTAI_ERR_INVALID_STATE &&
           xiaotai_room_hold_local(gen, 2, false) == XIAOTAI_OK && !fake_link.gate_open &&
           fake_link.gate_open_calls == late_opens && fake_link.gate_close_calls == late_closes + 1U);
    xiaotai_signal_give(&release_owner);
    flush();
    assert(!status().transmitting && status().phase == XIAOTAI_ROOM_LISTENING);
    assert(fake_link.gate_close_calls == late_closes + 1U);
    unsigned mic_off = 0;
    for (unsigned i = late_commands; i < fake_link.commands_count; ++i)
        if (strstr(fake_link.commands[i], "\"method\":\"set_mic_state\"") &&
            strstr(fake_link.commands[i], "\"mic_state\":\"off\"")) ++mic_off;
    assert(mic_off == 1U);
    puts("local refresh: blocked owner permits active 750 ms, closes once at 751 ms, no revival");

    assert(xiaotai_room_hold_local(gen, 3, true) == XIAOTAI_OK);
    flush();
    assert(status().transmitting && fake_link.gate_open);
    assert(xiaotai_run(blocked_owner, NULL) == XIAOTAI_OK && xiaotai_signal_take(&fence, 3000));
    const unsigned mute_closes = fake_link.gate_close_calls;
    fake_link.mic_muted = true;
    assert(xiaotai_room_hold_local_refresh(gen + 1U, 3) == XIAOTAI_ERR_INVALID_STATE &&
           xiaotai_room_hold_local_refresh(gen, 2) == XIAOTAI_ERR_INVALID_STATE &&
           fake_link.gate_open && fake_link.gate_close_calls == mute_closes);
    assert(xiaotai_room_hold_local_refresh(gen, 3) == XIAOTAI_ERR_INVALID_STATE && !fake_link.gate_open &&
           fake_link.gate_close_calls == mute_closes + 1U && fake_link.gate_tail_ms == 0 &&
           stats().talk_keepalive_releases == 2U);
    fake_link.mic_muted = false;
    assert(xiaotai_room_hold_local_refresh(gen, 3) == XIAOTAI_ERR_INVALID_STATE && !fake_link.gate_open);
    xiaotai_signal_give(&release_owner);
    flush();
    assert(!status().transmitting && fake_link.gate_close_calls == mute_closes + 1U);
    assert(xiaotai_room_hold_local(gen, 4, true) == XIAOTAI_OK && fake_link.gate_open);
    assert(xiaotai_room_hold_local_refresh(gen, 3) == XIAOTAI_ERR_INVALID_STATE &&
           xiaotai_room_hold_local(gen, 3, false) == XIAOTAI_OK && fake_link.gate_open &&
           fake_link.gate_close_calls == mute_closes + 1U);
    assert(xiaotai_room_hold_local(gen, 4, false) == XIAOTAI_OK && !fake_link.gate_open &&
           fake_link.gate_close_calls == mute_closes + 2U);
    flush();
    puts("local refresh: mute cancels only the matching ticket; stale refresh/release cannot touch replacement");
    shutdown();
}

static const char *at_command(const char *input)
{
    wire.input = input;
    wire.remaining = strlen(input);
    wire.used = 0;
    wire.output[0] = '\0';
    assert(xiaotai_at_service(20) == XIAOTAI_OK && !wire.remaining);
    return wire.output;
}

static void expect_at(const char *input, const char *expected)
{
    const char *out = at_command(input);
    if (strcmp(out, expected)) fprintf(stderr, "%s -> [%s] expected [%s]\n", input, out, expected);
    assert(!strcmp(out, expected));
}

static void check_help(const char *reply)
{
    FILE *manifest = fopen(XIAOTAI_TEST_ROOM_MANIFEST, "rb");
    assert(manifest);
    char json[3072];
    const size_t n = fread(json, 1, sizeof(json), manifest);
    assert(n && n < sizeof(json) && feof(manifest));
    fclose(manifest);
    xiaotai_json_token_t tokens[160];
    xiaotai_json_doc_t doc;
    assert(xiaotai_json_parse(json, n, tokens, 160, NULL, &doc) == XIAOTAI_OK);
    const int array = xiaotai_json_get(&doc, 0, "commands");
    assert(array >= 0 && tokens[array].type == XIAOTAI_JSON_ARRAY && tokens[array].children == 10);
    unsigned lines = 0;
    for (const char *row = reply; (row = strstr(row, "+HELP: AT+ROOM")); row += 1) ++lines;
    assert(lines == tokens[array].children);
    for (unsigned i = 0; i < lines; ++i) {
        const int row = xiaotai_json_at(&doc, array, i);
        assert(xiaotai_json_equals(&doc, xiaotai_json_get(&doc, row, "command"), "AT+ROOM"));
        assert(xiaotai_json_equals(&doc, xiaotai_json_get(&doc, row, "access"), i ? "action" : "read"));
        char word[24] = "", usage[80] = "", expected[160];
        const int verb = xiaotai_json_get(&doc, row, "verb"), use = xiaotai_json_get(&doc, row, "usage");
        if (verb >= 0) assert(xiaotai_json_string(&doc, verb, word, sizeof(word), NULL) == XIAOTAI_OK);
        if (use >= 0) assert(xiaotai_json_string(&doc, use, usage, sizeof(usage), NULL) == XIAOTAI_OK);
        const int len = snprintf(expected, sizeof(expected), "+HELP: AT+ROOM%s%s%s%s\r\n", word[0] ? " " : "",
                                 word, usage[0] ? " " : "", usage);
        assert(len > 0 && (size_t)len < sizeof(expected));
        const char *found = strstr(reply, expected);
        assert(found && !strstr(found + len, expected));
    }
}

static void open_cli(void)
{
    const char *ready = at_command("AT\r");
    assert(!strcmp(ready, "+READY: at=1 source=fixture locked=0\r\nOK\r\n"));
}

static void diag_write(void *ctx, const char *text)
{
    (void)ctx;
    assert(xiaotai_on_owner());
    assert(strlen(text) <= XIAOTAI_AT_OUT_LINE_MAX);
    const int used = snprintf(wire.output + wire.used, sizeof(wire.output) - wire.used, "%s\n", text);
    assert(used >= 0 && (size_t)used < sizeof(wire.output) - wire.used);
    wire.used += (size_t)used;
}

static void diag_report(void *unused)
{
    (void)unused;
    wire.used = 0;
    wire.output[0] = '\0';
    const xiaotai_reply_t reply = {.write = diag_write};
    xiaotai_diag_report("room", &reply);
}

static void check_secret(void)
{
    const char secret[] = "private-test-secret";
    begin_token(false, NULL);
    open_cli();
    ok(1, "{\"code\":200,\"data\":{\"peer_id\":[],\"token\":\"private-test-secret\","
          "\"heartbeat_seconds\":\"15\",\"lease_seconds\":45}}");
    assert(!fake_link.connects && stats().token_failures == 1U);
    assert(!strstr(at_command("AT+ROOM\r"), secret));
    owner(diag_report, NULL);
    char shape[120];
    (void)snprintf(shape, sizeof(shape), "+ROOM-TOKEN: peer_type=%u peer_bytes=0 token_type=%u token_bytes=19",
                   (unsigned)XIAOTAI_JSON_ARRAY, (unsigned)XIAOTAI_JSON_STRING);
    assert(strstr(wire.output, shape) && !strstr(wire.output, secret) && strstr(wire.output, "token_failures=1"));
    xiaotai_eventlog_entry_t entries[32];
    const size_t n = xiaotai_eventlog_read(0, entries, 32U);
    bool peer_type = false, peer_bytes = false, token_type = false, token_bytes = false, failure = false;
    for (size_t i = 0; i < n; ++i) {
        if (strcmp(entries[i].source, "room")) continue;
        assert(!strstr(entries[i].label, secret));
        if (!strcmp(entries[i].label, "token.peer_type")) peer_type = entries[i].value == XIAOTAI_JSON_ARRAY;
        if (!strcmp(entries[i].label, "token.peer_bytes")) peer_bytes = entries[i].value == 0;
        if (!strcmp(entries[i].label, "token.type")) token_type = entries[i].value == XIAOTAI_JSON_STRING;
        if (!strcmp(entries[i].label, "token.bytes")) token_bytes = entries[i].value == 19;
        if (!strcmp(entries[i].label, "connection.failed")) failure = entries[i].kind == XIAOTAI_EVENTLOG_ERR;
    }
    assert(peer_type && peer_bytes && token_type && token_bytes && failure);
    shutdown();
}

static void terminal_commands(void)
{
    with_at = true;
    boot(false, true, NULL, NULL);
    open_cli();
    expect_at("AT+ROOM\r", "+ROOM: state=0 code=- assigned=0 known=0 open=0 busy=0 members=0 online=0 "
                              "transmitting=0 gen=0 revision=0 error=0\r\nOK\r\n");
    check_help(at_command("AT+HELP\r"));
    expect_at("AT+ROOM PTT 1\r", "ERROR: INVALID_STATE\r\n");
    expect_at("AT+ROOM PTT 0\r", "OK\r\n");
    char boundary[XIAOTAI_COMMAND_LINE_MAX + 2U] = "AT+ROOM PTT 0";
    const size_t prefix = strlen(boundary), limit = XIAOTAI_COMMAND_LINE_MAX;
    memset(boundary + prefix, ' ', limit - prefix);
    boundary[limit - 1U] = '\r';
    boundary[limit] = '\0';
    expect_at(boundary, "OK\r\n");
    boundary[limit - 1U] = ' ';
    boundary[limit] = '\r';
    boundary[limit + 1U] = '\0';
    expect_at(boundary, "ERROR: INVALID_SIZE LINE_TOO_LONG\r\n");
    expect_at("AT+ROOM PTT 00\r", "ERROR: INVALID_ARG\r\n");
    expect_at("AT+ROOM PTT 0 extra\r", "ERROR: INVALID_ARG ARGUMENT_COUNT\r\n");
    expect_at("AT+ROOM STATUS\r", "ERROR: INVALID_ARG UNKNOWN_VERB\r\n");
    expect_at("AT+GROUP JOIN 012345\r", "ERROR: NOT_FOUND UNKNOWN_COMMAND\r\n");
    expect_at("AT+ROOM OPEN\r", "OK\r\n");
    flush();
    assert(status().attending);
    ok(0, empty);
    const char *const wrong[] = {"AT+ROOM JOIN 12345 1234\r", "AT+ROOM JOIN 012345 123\r", "AT+ROOM JOIN 012345 12345\r",
        "AT+ROOM JOIN 012345 12x4\r", "AT+ROOM JOIN 012345 -123\r", "AT+ROOM JOIN 012345 1234 extra\r",
        "AT+ROOM CREATE 123\r", "AT+ROOM CREATE 12345\r", "AT+ROOM CREATE 12a4\r", "AT+ROOM CREATE 1234 extra\r"};
    for (size_t i = 0; i < sizeof(wrong) / sizeof(*wrong); ++i)
        assert(!strncmp(at_command(wrong[i]), "ERROR: INVALID_ARG", sizeof("ERROR: INVALID_ARG") - 1U));
    flush();
    assert(fixture.sent == 1U && !status().busy);
    expect_at("AT+ROOM CLOSE\r", "OK\r\n");
    flush();
    assert(!status().attending);
    shutdown();

    const struct { const char *line, *path, *body; } valid[] = {
        {"AT+ROOM JOIN 012345 0000\r", "join", "{\"room_code\":\"012345\",\"password\":\"0000\"}"},
        {"AT+ROOM JOIN 012345\r", "join", "{\"room_code\":\"012345\",\"password\":\"\"}"},
        {"AT+ROOM CREATE 9876\r", "create", "{\"password\":\"9876\"}"},
        {"AT+ROOM CREATE\r", "create", "{\"password\":\"\"}"},
    };
    for (size_t i = 0; i < sizeof(valid) / sizeof(*valid); ++i) {
        boot(false, true, NULL, NULL);
        open_cli();
        expect_at("AT+ROOM OPEN\r", "OK\r\n");
        flush();
        ok(0, empty);
        expect_at(valid[i].line, "OK\r\n");
        flush();
        expect_request(1, valid[i].path, valid[i].body);
        ok(1, receipt);
        assert(!status().assigned);
        expect_request(2, "assignment", NULL);
        ok(2, assigned);
        assert(status().assigned && status().assignment_version == 3U);
        shutdown();
    }

    const xiaotai_room_config_t manual = {.manual_connect = true, .members_max = XIAOTAI_ROOM_NO_MEMBER_LIST};
    boot(false, true, &manual, NULL);
    open_cli();
    expect_at("AT+ROOM OPEN\r", "OK\r\n");
    flush();
    ok(0, assigned);
    assert(fixture.sent == 1U && status().wait == XIAOTAI_ROOM_WAIT_MANUAL);
    owner(enable_transport, NULL);
    expect_at("AT+ROOM CONNECT\r", "OK\r\n");
    flush();
    expect_request(1, "assignment", NULL);
    ok(1, assigned);
    expect_token_request(2);
    ok(2, token_ok);
    owner(open_job, NULL);
    owner(join_job, NULL);
    ok(3, "{}");
    owner(signal_job, (void *)snapshot);
    assert(strstr(at_command("AT+ROOM\r"), "members=0 online=2 transmitting=0"));
    owner(diag_report, NULL);
    assert(strstr(wire.output, "state=5 code=001234") && strstr(wire.output, "+ROOM-STATS: snapshots=1"));
    expect_at("AT+ROOM PTT 1\r", "OK\r\n");
    flush();
    assert(fake_link.gate_open && fake_link.gate_on_owner && status().transmitting);
    const size_t sent_before = fixture.sent;
    const size_t commands_before = fake_link.commands_count;
    expect_at("AT+ROOM PTT\r", "ERROR: INVALID_ARG ARGUMENT_COUNT\r\n");
    expect_at("AT+ROOM PTT 2\r", "ERROR: INVALID_ARG\r\n");
    expect_at("AT+ROOM PTT 0 extra\r", "ERROR: INVALID_ARG ARGUMENT_COUNT\r\n");
    expect_at("AT+ROOM LEAVE extra\r", "ERROR: INVALID_ARG ARGUMENT_COUNT\r\n");
    flush();
    assert(fixture.sent == sent_before && fake_link.commands_count == commands_before &&
           fake_link.gate_open && status().transmitting && status().assigned);
    expect_at("AT+ROOM PTT 0\r", "OK\r\n");
    assert(!fake_link.gate_open && fake_link.gate_tail_ms == 600U);
    tick(200U);
    assert(!status().transmitting && status().assigned);
    expect_at("AT+ROOM PTT 1\r", "OK\r\n");
    flush();
    assert(fake_link.gate_open && status().transmitting);
    assert(xiaotai_run(blocked_owner, NULL) == XIAOTAI_OK && xiaotai_signal_take(&fence, 3000));
    while (xiaotai_run(no_op, NULL) == XIAOTAI_OK) {}
    const xiaotai_reply_t raw = {.write = diag_write};
    assert(xiaotai_command_run("AT+ROOM PTT 0", &raw, 60) == XIAOTAI_ERR_NO_MEM && fake_link.gate_open);
    expect_at("AT+ROOM PTT 0 extra\r", "ERROR: NO_MEM\r\n");
    expect_at("AT+CLI OFF\r", "OK\r\n");
    expect_at("AT+ROOM PTT 0\r", "ERROR: INVALID_STATE CLI_DISABLED\r\n");
    assert(fake_link.gate_open);
    expect_at("AT+CLI ON\r", "OK\r\n");
    expect_at(" \tAT+ROOM  PTT\t0 \r", "OK\r\n");
    assert(!fake_link.gate_open && !fake_link.gate_on_owner && fake_link.gate_tail_ms == 600U);
    expect_at("AT+ROOM PTT 1\r", "ERROR: NO_MEM\r\n");
    assert(!fake_link.gate_open);
    xiaotai_signal_give(&release_owner);
    xiaotai_err_t queued = XIAOTAI_ERR_NO_MEM;
    for (unsigned i = 0; i < 300U && queued == XIAOTAI_ERR_NO_MEM; ++i) {
        queued = xiaotai_run(barrier, NULL);
        if (queued == XIAOTAI_ERR_NO_MEM) xiaotai_delay_ms(1);
    }
    assert(queued == XIAOTAI_OK && xiaotai_signal_take(&fence, 3000));
    tick(200U);
    assert(!status().transmitting);
    expect_at("AT+ROOM REFRESH\r", "OK\r\n");
    flush();
    expect_request(4, "assignment", NULL);
    assert(strstr(fake_link.commands[fake_link.commands_count - 1U], "get_room_snapshot"));
    ok(4, assigned);
    expect_at("AT+ROOM DISCONNECT\r", "OK\r\n");
    flush();
    assert(status().assigned && !status().generation);
    ok(5, "{}");
    expect_at("AT+ROOM LEAVE\r", "OK\r\n");
    flush();
    expect_request(6, "leave", "{\"room_id\":\"group_room_exact\",\"assignment_version\":3}");
    ok(6, left);
    assert(!status().assigned);
    puts("Room AT: normal PTT press/release, four malformed controls with no request, and assignment-versioned leave");
    shutdown();
    check_secret();
    with_at = false;
}

static void post(const char *name, int32_t value, const char *text)
{
    xiaotai_args_t args = {.value = value};
    if (text) {
        assert(strlen(text) < sizeof(args.text));
        strcpy(args.text, text);
    }
    assert(xiaotai_intent_post(name, &args) == XIAOTAI_OK);
    flush();
}

static void intents(void)
{
    boot(false, true, NULL, NULL);
    post(XIAOTAI_INTENT_ROOM_ATTEND, 1, NULL);
    assert(status().attending);
    ok(0, empty);
    post(XIAOTAI_INTENT_ROOM_CREATE, 0, "123");
    assert(fixture.sent == 1U && last_result.action == XIAOTAI_ROOM_ACTION_CREATE && last_result.error == XIAOTAI_ERR_INVALID_ARG);
    post(XIAOTAI_INTENT_ROOM_JOIN, 0, "012345  0000");
    assert(fixture.sent == 1U && last_result.action == XIAOTAI_ROOM_ACTION_JOIN && last_result.error == XIAOTAI_ERR_INVALID_ARG);
    xiaotai_args_t invalid = {0};
    memset(invalid.text, '0', sizeof(invalid.text));
    assert(xiaotai_intent_post(XIAOTAI_INTENT_ROOM_JOIN, &invalid) == XIAOTAI_OK);
    flush();
    assert(fixture.sent == 1U && last_result.error == XIAOTAI_ERR_INVALID_ARG);
    post(XIAOTAI_INTENT_ROOM_CREATE, 0, "0098");
    expect_request(1, "create", "{\"password\":\"0098\"}");
    respond(1, 200, 40320, "{}");
    post(XIAOTAI_INTENT_ROOM_JOIN, 0, "012345 0000");
    expect_request(2, "join", "{\"room_code\":\"012345\",\"password\":\"0000\"}");
    ok(2, receipt);
    expect_request(3, "assignment", NULL);
    ok(3, assigned);
    post(XIAOTAI_INTENT_ROOM_REFRESH, 0, NULL);
    expect_request(4, "assignment", NULL);
    ok(4, assigned);
    post(XIAOTAI_INTENT_ROOM_LEAVE, 0, NULL);
    expect_request(5, "leave", "{\"room_id\":\"group_room_exact\",\"assignment_version\":3}");
    ok(5, left);
    post(XIAOTAI_INTENT_ROOM_ATTEND, 0, NULL);
    assert(!status().assigned && !status().attending);
    shutdown();

    active_room(3, 0);
    post(XIAOTAI_INTENT_ROOM_TALK, 1, NULL);
    assert(fake_link.gate_open && status().transmitting);
    post(XIAOTAI_INTENT_ROOM_TALK, 0, NULL);
    assert(!fake_link.gate_open && !status().transmitting);
    post(XIAOTAI_INTENT_ROOM_DISCONNECT, 0, NULL);
    assert(status().assigned && !status().generation);
    ok(3, "{}");
    post(XIAOTAI_INTENT_ROOM_CONNECT, 0, NULL);
    expect_request(4, "assignment", NULL);
    ok(4, assigned);
    expect_token_request(5);
    shutdown();
}

int main(int argc, char **argv)
{
    assert(xiaotai_signal_init(&fence) == XIAOTAI_OK && xiaotai_signal_init(&release_owner) == XIAOTAI_OK &&
           xiaotai_signal_init(&restarted) == XIAOTAI_OK);
    if (argc == 1) assignment();
    else if (!strcmp(argv[1], "attend_stale")) attend_stale();
    else if (!strcmp(argv[1], "config")) config();
    else if (!strcmp(argv[1], "connect")) { connect(); tokens(); deadlines(); }
    else if (!strcmp(argv[1], "codec_alias")) codec_alias();
    else if (!strcmp(argv[1], "retry")) retry();
    else if (!strcmp(argv[1], "legacy_superseded_lease")) legacy_superseded_lease();
    else if (!strcmp(argv[1], "legacy_superseded_unknown")) legacy_superseded_unknown();
    else if (!strcmp(argv[1], "heartbeat_expiry_retry")) heartbeat_expiry_retry();
    else if (!strcmp(argv[1], "connecting_presence")) connecting_presence();
    else if (!strcmp(argv[1], "release")) release();
    else if (!strcmp(argv[1], "preempt")) preempt();
    else if (!strcmp(argv[1], "quiesce")) quiesce();
    else if (!strcmp(argv[1], "members")) members();
    else if (!strcmp(argv[1], "legacy_owner")) legacy_owner(false);
    else if (!strcmp(argv[1], "legacy_owner_reconnect")) legacy_owner(true);
    else if (!strcmp(argv[1], "command_recovery")) command_recovery();
    else if (!strcmp(argv[1], "talk")) talk();
    else if (!strcmp(argv[1], "local_hold")) local_hold();
    else if (!strcmp(argv[1], "local_refresh")) local_refresh();
    else if (!strcmp(argv[1], "local_refresh_exclusive")) local_refresh_exclusive();
    else if (!strcmp(argv[1], "count_only")) count_only();
    else if (!strcmp(argv[1], "at")) terminal_commands();
    else if (!strcmp(argv[1], "intents")) intents();
    else if (!strcmp(argv[1], "tokens")) tokens();
    else if (!strcmp(argv[1], "token_http_lease")) token_http_lease();
    else if (!strcmp(argv[1], "legacy_xmini_heartbeat")) legacy_xmini_heartbeat();
    else if (!strcmp(argv[1], "legacy_xmini_ptt")) legacy_xmini_ptt();
    else if (!strcmp(argv[1], "deadlines")) deadlines();
    else assert(false);
    xiaotai_signal_deinit(&fence);
    xiaotai_signal_deinit(&release_owner);
    xiaotai_signal_deinit(&restarted);
    return 0;
}
