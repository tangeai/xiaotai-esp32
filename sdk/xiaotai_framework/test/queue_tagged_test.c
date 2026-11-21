// SPDX-License-Identifier: MIT
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "internal.h"
#include "xiaotai/port/kv.h"
#include "xiaotai_mem_host.h"

enum { DEPTH = 32, WAIT_MS = 5000, PAYLOAD_BYTES = 13 };
static xiaotai_signal_t entered, release_owner, drained, stop_queued, app_ran;
static unsigned received, accepted, refused;
static size_t item_size;
static _Thread_local bool resetting, posting;
static xiaotai_queue_t *queue;
static int event_cookie, local_cookie, external_cookie, null_cookie;

xiaotai_err_t __real_xiaotai_queue_init(xiaotai_queue_t *, size_t, size_t, xiaotai_mem_kind_t);
xiaotai_err_t __wrap_xiaotai_queue_init(xiaotai_queue_t *q, size_t depth, size_t item,
                                    xiaotai_mem_kind_t storage)
{
    if (depth == DEPTH && storage == XIAOTAI_MEM_BULK) {
        assert(queue == NULL);
        item_size = item;
        queue = q;
    }
    return __real_xiaotai_queue_init(q, depth, item, storage);
}

bool __real_xiaotai_queue_send(xiaotai_queue_t *, const void *, uint32_t);
bool __wrap_xiaotai_queue_send(xiaotai_queue_t *q, const void *msg, uint32_t timeout)
{
    if (q != queue) return __real_xiaotai_queue_send(q, msg, timeout);
    if (posting) assert(timeout == 0);
    if (resetting) assert(timeout == 100);
    const bool ok = __real_xiaotai_queue_send(q, msg, timeout);
    if (posting) {
        if (ok) ++accepted;
        else ++refused;
    }
    if (resetting && ok) xiaotai_signal_give(&stop_queued);
    return ok;
}

static void fill_args(xiaotai_args_t *args, unsigned id)
{
    args->value = (int32_t)id;
    memset(args->text, (int)('A' + id % 26U), sizeof(args->text));
    args->text[sizeof(args->text) - 1] = '\0';
}

static unsigned check_args(const xiaotai_args_t *args)
{
    xiaotai_args_t expected;
    const unsigned id = (unsigned)args->value;
    fill_args(&expected, id);
    assert(memcmp(args->text, expected.text, sizeof(args->text)) == 0);
    return id;
}

static void record(unsigned id)
{
    assert(xiaotai_on_owner() && id == received);
    if (++received == DEPTH) xiaotai_signal_give(&drained);
}

static void on_event(const xiaotai_event_t *event, void *ctx)
{
    assert(fw_app_current() == 0 && !xiaotai_intent_from_external());
    if (ctx == &null_cookie) {
        assert(strcmp(event->name, "test.empty") == 0);
        assert(!event->data && event->size == 0 && event->args.value == 0);
        for (size_t i = 0; i < sizeof(event->args.text); ++i) assert(event->args.text[i] == '\0');
        record(28);
        return;
    }
    assert(ctx == &event_cookie && strcmp(event->name, "test.event") == 0);
    const unsigned id = check_args(&event->args);
    assert(event->data && event->size == PAYLOAD_BYTES);
    for (size_t i = 0; i < event->size; ++i) assert(((const unsigned char *)event->data)[i] == id + i);
    record(id);
}

static void on_intent(const xiaotai_args_t *args, void *ctx)
{
    assert(fw_app_current() == 0 && (ctx == &local_cookie || ctx == &external_cookie));
    assert(xiaotai_intent_from_external() == (ctx == &external_cookie));
    if (received == 29) {
        assert(ctx == &local_cookie && args->value == 0);
        for (size_t i = 0; i < sizeof(args->text); ++i) assert(args->text[i] == '\0');
        record(29);
    } else {
        record(check_args(args));
    }
}

static void app_job(void *ctx)
{
    assert(fw_app_current() == 0 && ctx == &app_ran);
    xiaotai_signal_give(&app_ran);
}

static void app_start(void) { assert(xiaotai_run(app_job, &app_ran) == XIAOTAI_OK); }

static xiaotai_err_t app_init(void)
{
    assert(xiaotai_event_subscribe("test.event", on_event, &event_cookie) == XIAOTAI_OK);
    assert(xiaotai_event_subscribe("test.empty", on_event, &null_cookie) == XIAOTAI_OK);
    assert(xiaotai_intent_handle_ex("test.local", on_intent, &local_cookie, XIAOTAI_INTENT_LOCAL) == XIAOTAI_OK);
    assert(xiaotai_intent_handle("test.external", on_intent, &external_cookie) == XIAOTAI_OK);
    return XIAOTAI_OK;
}

static void owner_work(void *ctx)
{
    assert(fw_app_current() == -1 && !xiaotai_intent_from_external());
    record(*(const unsigned *)ctx);
}

static void hold(void *ctx)
{
    (void)ctx;
    assert(xiaotai_on_owner());
    xiaotai_signal_give(&entered);
    assert(xiaotai_signal_take(&release_owner, WAIT_MS));
}

static void wait_for_hold(void)
{
    assert(xiaotai_run(hold, NULL) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&entered, WAIT_MS));
}

static xiaotai_err_t emit(unsigned id)
{
    xiaotai_args_t args;
    fill_args(&args, id);
    unsigned char bytes[PAYLOAD_BYTES];
    for (size_t i = 0; i < sizeof(bytes); ++i) bytes[i] = (unsigned char)(id + i);
    char name[] = "test.event";
    const xiaotai_err_t err = xiaotai_event_emit(name, &args, bytes, sizeof(bytes));
    memset(name, '*', sizeof(name));
    memset(bytes, '*', sizeof(bytes));
    memset(&args, '*', sizeof(args));
    return err;
}

static void run_saturation(void)
{
    unsigned jobs[DEPTH];
    const int64_t idle = xiaotai_mem_host_outstanding();
    wait_for_hold();
    posting = true;
    for (unsigned id = 0; id < DEPTH; ++id) {
        xiaotai_args_t args;
        fill_args(&args, id);
        switch (id % 4U) {
        case 0:
            assert((id == 28 ? xiaotai_event_emit("test.empty", NULL, NULL, 0) : emit(id)) == XIAOTAI_OK);
            break;
        case 1:
            assert(xiaotai_intent_post("test.local", id == 29 ? NULL : &args) == XIAOTAI_OK);
            break;
        case 2:
            assert(xiaotai_intent_post_external("test.external", &args) == XIAOTAI_OK);
            break;
        case 3:
            jobs[id] = id;
            assert(xiaotai_run(owner_work, &jobs[id]) == XIAOTAI_OK);
            break;
        }
        memset(&args, '*', sizeof(args));
    }
    const int64_t full = xiaotai_mem_host_outstanding();
    assert(full == idle + 7 && accepted == DEPTH && refused == 0);
    assert(emit(DEPTH) == XIAOTAI_ERR_NO_MEM);
    assert(xiaotai_intent_post("test.local", NULL) == XIAOTAI_ERR_NO_MEM);
    assert(xiaotai_run(owner_work, &jobs[31]) == XIAOTAI_ERR_NO_MEM);
    assert(xiaotai_intent_post_external("test.local", NULL) == XIAOTAI_ERR_NOT_ALLOWED);
    assert(xiaotai_mem_host_outstanding() == full && accepted == DEPTH && refused == 3);
    posting = false;
    xiaotai_signal_give(&release_owner);
    assert(xiaotai_signal_take(&drained, WAIT_MS) && received == DEPTH);
    assert(xiaotai_mem_host_outstanding() == idle);
}

static void *reset(void *ctx)
{
    (void)ctx;
    resetting = true;
    xiaotai_framework_reset();
    return NULL;
}

int main(void)
{
    static const xiaotai_application_t app = {.name = "fixture", .init = app_init, .start = app_start};
    const xiaotai_application_t *const apps[] = {&app};
    const xiaotai_framework_config_t config = {.applications = apps, .application_count = 1,
                                               .queue_depth = DEPTH};
    assert(xiaotai_kv_init() == XIAOTAI_OK);
    const int64_t initial = xiaotai_mem_host_outstanding();
    xiaotai_signal_t *const signals[] = {&entered, &release_owner, &drained, &stop_queued, &app_ran};
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i) {
        assert(xiaotai_signal_init(signals[i]) == XIAOTAI_OK);
    }
    assert(xiaotai_framework_start(&config) == XIAOTAI_OK);
    assert(queue != NULL && item_size != 0);
    assert(xiaotai_signal_take(&app_ran, WAIT_MS));
    run_saturation();

    wait_for_hold();
    pthread_t thread;
    assert(pthread_create(&thread, NULL, reset, NULL) == 0);
    assert(xiaotai_signal_take(&stop_queued, WAIT_MS));
    const int64_t before_tail = xiaotai_mem_host_outstanding();
    unsigned trailing_job = 99;
    assert(emit(99) == XIAOTAI_OK && xiaotai_run(owner_work, &trailing_job) == XIAOTAI_OK);
    assert(xiaotai_mem_host_outstanding() == before_tail + 1);
    xiaotai_signal_give(&release_owner);
    assert(pthread_join(thread, NULL) == 0 && received == DEPTH);
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i) xiaotai_signal_deinit(signals[i]);
    assert(xiaotai_mem_host_outstanding() == initial);
    printf("host item=%zu depth=%u FIFO=%u refused=%u post_wait=0 reset_tail_freed=yes live_delta=0\n",
           item_size, DEPTH, received, refused);
    return 0;
}
