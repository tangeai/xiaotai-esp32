// SPDX-License-Identifier: MIT
#include <string.h>

#include "internal.h"
#include "xiaotai/framework.h"

typedef enum {SERVICE_READY, SERVICE_RUNNING, SERVICE_REPLY, SERVICE_POSTED} service_phase_t;
typedef struct {
    xiaotai_transport_reply_fn_t reply;
    void *ctx;
    uint32_t epoch;
    service_phase_t phase;
    xiaotai_err_t result;
    int status;
    size_t path_size, json_size, allocation_size, body_size;
    char data[];
} service_t;

static service_t *s_pending[XIAOTAI_TRANSPORT_SERVICE_PENDING_MAX];

static char *body(service_t *s) { return s->data + s->path_size + s->json_size; }

static void destroy(service_t *s)
{
    if (!s) return;
    const size_t size = s->allocation_size;
    volatile uint8_t *p = (volatile uint8_t *)s;
    for (size_t i = 0; i < size; ++i) p[i] = 0;
    xiaotai_mem_free(s);
}

static void detach_locked(service_t *s)
{
    for (unsigned i = 0; i < XIAOTAI_TRANSPORT_SERVICE_PENDING_MAX; ++i)
        if (s_pending[i] == s) s_pending[i] = NULL;
}

static void dispatch(void *ctx)
{
    service_t *s = ctx;
    xiaotai_critical_enter(&xt.lock);
    detach_locked(s);
    xiaotai_critical_exit(&xt.lock);
    s->reply(s->ctx, s->result, s->status, s->body_size ? body(s) : NULL, s->body_size);
    destroy(s);
}

static bool online(uint32_t epoch)
{
    return xiaotai_transport_state() == XIAOTAI_TRANSPORT_ONLINE && epoch == atomic_load(&xt.service_epoch) &&
           !atomic_load(&xt.worker_quit) && !atomic_load(&xt.hold_count) && !atomic_load(&xt.admission_closed);
}

static void run(uint32_t index, uint32_t unused)
{
    (void)unused;
    xiaotai_critical_enter(&xt.lock);
    service_t *s = s_pending[index];
    if (s) s->phase = SERVICE_RUNNING;
    xiaotai_critical_exit(&xt.lock);
    if (!s) return;

    s->result = XIAOTAI_ERR_INVALID_STATE;
    if (online(s->epoch) && !(atomic_load(&xt.upcalls) & XT_UP_STOPPED) &&
        atomic_load(&xt.restarts) == xt.restarts_seen) {
        const xiaotai_err_t err = xiaotai_rtc_service_request(s->data, s->json_size ? s->data + s->path_size : NULL,
            &s->status, body(s), XIAOTAI_TRANSPORT_SERVICE_BODY_MAX, &s->body_size);
        s->result = err == XIAOTAI_OK ? XIAOTAI_OK : XIAOTAI_FAIL;
        if (s->result != XIAOTAI_OK) s->body_size = 0;
    }
    if (s->result != XIAOTAI_OK) atomic_fetch_add(&xt.counters.service_failures, 1U);

    xiaotai_critical_enter(&xt.lock);
    if (s->reply) s->phase = SERVICE_REPLY;
    else detach_locked(s);
    const bool reap = !s->reply;
    xiaotai_critical_exit(&xt.lock);
    if (reap) destroy(s);
}

xiaotai_err_t xiaotai_transport_service_request(const char *path, const char *json,
                                                xiaotai_transport_reply_fn_t reply, void *ctx)
{
    if (!path || path[0] != '/') return XIAOTAI_ERR_INVALID_ARG;
    const size_t path_size = strnlen(path, XIAOTAI_TRANSPORT_SERVICE_PATH_MAX) + 1U;
    const size_t json_size = json ? strnlen(json, XIAOTAI_TRANSPORT_SERVICE_BODY_MAX) + 1U : 0U;
    if (path_size > XIAOTAI_TRANSPORT_SERVICE_PATH_MAX || json_size > XIAOTAI_TRANSPORT_SERVICE_BODY_MAX)
        return XIAOTAI_ERR_INVALID_SIZE;
    const uint32_t epoch = atomic_load(&xt.service_epoch);
    if (!online(epoch)) return XIAOTAI_ERR_INVALID_STATE;

    /* Reserve capacity before allocating, so many callers cannot transiently
     * exceed the same two-request memory budget. The worker only sees READY. */
    unsigned index = XIAOTAI_TRANSPORT_SERVICE_PENDING_MAX;
    xiaotai_critical_enter(&xt.lock);
    for (unsigned i = 0; i < XIAOTAI_TRANSPORT_SERVICE_PENDING_MAX; ++i) {
        if (!s_pending[i] && !xt.service_reserved[i]) {
            xt.service_reserved[i] = true;
            index = i;
            break;
        }
    }
    xiaotai_critical_exit(&xt.lock);
    if (index == XIAOTAI_TRANSPORT_SERVICE_PENDING_MAX) return XIAOTAI_ERR_BUSY;
    const size_t bytes = sizeof(service_t) + path_size + json_size + XIAOTAI_TRANSPORT_SERVICE_BODY_MAX;
    service_t *s = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, bytes);
    if (s) {
        *s = (service_t){.reply = reply, .ctx = ctx, .epoch = epoch, .phase = SERVICE_READY,
            .path_size = path_size, .json_size = json_size, .allocation_size = bytes};
        memcpy(s->data, path, path_size);
        if (json_size) memcpy(s->data + path_size, json, json_size);
        body(s)[0] = 0;
    }
    xiaotai_critical_enter(&xt.lock);
    xt.service_reserved[index] = false;
    s_pending[index] = s;
    xiaotai_critical_exit(&xt.lock);
    if (!s) return XIAOTAI_ERR_NO_MEM;
    const bool valid = online(epoch);
    if (valid && xt_worker_post(run, index, 0)) {
        atomic_fetch_add(&xt.counters.service_requests, 1U);
        return XIAOTAI_OK;
    }
    xiaotai_critical_enter(&xt.lock);
    detach_locked(s);
    xiaotai_critical_exit(&xt.lock);
    destroy(s);
    return valid ? XIAOTAI_ERR_BUSY : XIAOTAI_ERR_INVALID_STATE;
}

bool xt_service_replies(void)
{
    bool retry = false;
    for (unsigned i = 0; i < XIAOTAI_TRANSPORT_SERVICE_PENDING_MAX; ++i) {
        xiaotai_critical_enter(&xt.lock);
        service_t *s = s_pending[i];
        if (s && s->phase == SERVICE_REPLY) s->phase = SERVICE_POSTED;
        else s = NULL;
        xiaotai_critical_exit(&xt.lock);
        if (s && xiaotai_run(dispatch, s) != XIAOTAI_OK) {
            xiaotai_critical_enter(&xt.lock);
            s->phase = SERVICE_REPLY;
            xiaotai_critical_exit(&xt.lock);
            retry = true;
        }
    }
    return retry;
}

void xt_service_reset(void)
{
    /* Host teardown calls reset only after the owner and worker have stopped. */
    for (unsigned i = 0; i < XIAOTAI_TRANSPORT_SERVICE_PENDING_MAX; ++i) {
        destroy(s_pending[i]);
        s_pending[i] = NULL;
    }
}
