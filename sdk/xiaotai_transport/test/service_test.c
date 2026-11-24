// SPDX-License-Identifier: MIT
#include "support.h"

typedef struct { atomic_uint calls; xiaotai_err_t result; int status; char body[32]; } answer_t;
static atomic_uint s_backend_calls;
static xiaotai_signal_t s_started, s_release;
static xiaotai_signal_t s_owner_started, s_owner_release;

static void block_owner(void *ctx)
{
    (void)ctx;
    xiaotai_signal_give(&s_owner_started);
    CHECK(xiaotai_signal_take(&s_owner_release, 2000));
}

static void filler(void *ctx) { (void)ctx; }

static xiaotai_err_t backend(void *ctx, const char *path, const char *json, int *status, char *body,
                              size_t capacity, size_t *size)
{
    (void)ctx;
    CHECK(xt_on_worker());
    const unsigned n = atomic_fetch_add(&s_backend_calls, 1U);
    if (n == 0) {
        xiaotai_signal_give(&s_started);
        CHECK(xiaotai_signal_take(&s_release, 2000));
        CHECK(!strcmp(path, "/first") && !strcmp(json, "{\"local\":1}"));
        CHECK(capacity >= sizeof("{\"ok\":1}"));
        memcpy(body, "{\"ok\":1}", sizeof("{\"ok\":1}"));
        *status = 0;
        *size = strlen(body);
        return XIAOTAI_OK;
    }
    CHECK(!strcmp(path, "/error") && json == NULL);
    *status = 410;
    return XIAOTAI_FAIL;
}

static void reply(void *ctx, xiaotai_err_t result, int status, const char *body, size_t size)
{
    answer_t *a = ctx;
    CHECK(xiaotai_on_owner() && !atomic_load(&a->calls));
    CHECK(size < sizeof(a->body));
    a->result = result;
    a->status = status;
    if (body) {
        CHECK(size == strlen(body));
        memcpy(a->body, body, size + 1);
    } else CHECK(!size);
    atomic_store(&a->calls, 1);
}

int main(void)
{
    answer_t a = {0}, b = {0}, c = {0};
    CHECK(xiaotai_signal_init(&s_started) == XIAOTAI_OK);
    CHECK(xiaotai_signal_init(&s_release) == XIAOTAI_OK);
    CHECK(xiaotai_signal_init(&s_owner_started) == XIAOTAI_OK);
    CHECK(xiaotai_signal_init(&s_owner_release) == XIAOTAI_OK);
    CHECK(xiaotai_transport_service_request("test", NULL, NULL, NULL) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_transport_service_request("/test", NULL, NULL, NULL) == XIAOTAI_ERR_INVALID_STATE);
    boot_online(NULL);
    char too_long[XIAOTAI_TRANSPORT_SERVICE_PATH_MAX + 1U];
    memset(too_long, '/', sizeof(too_long) - 1U);
    too_long[sizeof(too_long) - 1U] = 0;
    CHECK(xiaotai_transport_service_request(too_long, NULL, reply, &c) == XIAOTAI_ERR_INVALID_SIZE);
    char json_long[XIAOTAI_TRANSPORT_SERVICE_BODY_MAX + 1U];
    memset(json_long, 'x', sizeof(json_long) - 1U);
    json_long[sizeof(json_long) - 1U] = 0;
    CHECK(xiaotai_transport_service_request("/test", json_long, reply, &c) == XIAOTAI_ERR_INVALID_SIZE);
    xiaotai_mem_host_fail_nth(1);
    CHECK(xiaotai_transport_service_request("/test", NULL, reply, &c) == XIAOTAI_ERR_NO_MEM);
    xiaotai_transport_host_service(backend, NULL);

    char path[] = "/first", json[] = "{\"local\":1}";
    CHECK(xiaotai_transport_service_request(path, json, reply, &a) == XIAOTAI_OK);
    CHECK(xiaotai_signal_take(&s_started, 1000));
    memset(path, 'x', sizeof(path));
    memset(json, 'x', sizeof(json));
    CHECK(xiaotai_transport_service_request("/must-not-send", NULL, reply, &b) == XIAOTAI_OK);
    CHECK(xiaotai_transport_service_request("/full", NULL, reply, &c) == XIAOTAI_ERR_BUSY);
    uint32_t hold;
    CHECK(xiaotai_transport_hold(&hold) == XIAOTAI_OK);
    CHECK(xiaotai_run(block_owner, NULL) == XIAOTAI_OK);
    CHECK(xiaotai_signal_take(&s_owner_started, 1000));
    unsigned queued = 0;
    while (xiaotai_run(filler, NULL) == XIAOTAI_OK) ++queued;
    CHECK(queued > 0);
    xiaotai_signal_give(&s_release);
    const int64_t until = xiaotai_time_us() + 1000000;
    while (status_now().service_failures != 1 && xiaotai_time_us() < until) xiaotai_delay_ms(1);
    CHECK(status_now().service_failures == 1 && a.calls == 0 && b.calls == 0);
    xiaotai_delay_ms(30); /* The worker's owner post must fail while full. */
    xiaotai_signal_give(&s_owner_release);
    const int64_t delivered_by = xiaotai_time_us() + 1000000;
    while ((!a.calls || !b.calls) && xiaotai_time_us() < delivered_by) xiaotai_delay_ms(1);
    settle();
    CHECK(a.calls == 1 && a.result == XIAOTAI_OK && !a.status && !strcmp(a.body, "{\"ok\":1}"));
    CHECK(b.calls == 1 && b.result == XIAOTAI_ERR_INVALID_STATE && !b.status && !*b.body);
    CHECK(s_backend_calls == 1 && c.calls == 0);
    CHECK(status_now().service_requests == 2 && status_now().service_failures == 1);

    xiaotai_transport_release(hold);
    xiaotai_transport_host_stopped(0);
    settle();
    xiaotai_transport_host_started();
    settle();
    CHECK(xiaotai_transport_state() == XIAOTAI_TRANSPORT_ONLINE);
    CHECK(xiaotai_transport_service_request("/error", NULL, reply, &c) == XIAOTAI_OK);
    settle();
    CHECK(c.calls == 1 && c.result == XIAOTAI_FAIL && c.status == 410 && !*c.body);
    CHECK(status_now().service_requests == 3 && status_now().service_failures == 2);
    shutdown_all();
    xiaotai_signal_deinit(&s_started);
    xiaotai_signal_deinit(&s_release);
    xiaotai_signal_deinit(&s_owner_started);
    xiaotai_signal_deinit(&s_owner_release);
    return 0;
}
