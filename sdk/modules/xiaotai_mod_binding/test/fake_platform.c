// SPDX-License-Identifier: MIT
/* Fakes for the binding module's host tests (see fake_platform.h). */
#include "fake_platform.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xiaotai/framework.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"

fake_request_t g_request;
fake_guest_t g_guest;
xiaotai_platform_status_t g_platform;
unsigned g_platform_retries;
xiaotai_platform_requirement_t g_requirement;
char g_trace[256];
int64_t g_unix_ms;
fake_identity_t g_identity;
xiaotai_net_status_t g_net;
bool g_provisioning_boot;
fake_mem_t g_mem;

static void trace(const char *word)
{
    strncat(g_trace, word, sizeof(g_trace) - strlen(g_trace) - 1U);
    strncat(g_trace, " ", sizeof(g_trace) - strlen(g_trace) - 1U);
}

static bool s_guest_ready;

/* ---------------------------------------------------------------- owner */

static xiaotai_signal_t s_sync;
static bool s_sync_ready;
static void sync_give(void *ctx) { (void)ctx; xiaotai_signal_give(&s_sync); }

bool fake_drain(void)
{
    if (!s_sync_ready) {
        (void)xiaotai_signal_init(&s_sync);
        s_sync_ready = true;
    }
    if (xiaotai_run(sync_give, NULL) != XIAOTAI_OK) return false;
    return xiaotai_signal_take(&s_sync, 3000);
}

void fake_drain_events(void)
{
    /* An owner job can emit a quiesce event, whose handler can in turn emit
     * binding.failed/announced after the second fence has been queued. */
    if (!fake_drain() || !fake_drain() || !fake_drain()) abort();
}

void fake_on_owner(void (*fn)(void *ctx), void *ctx)
{
    if (xiaotai_run(fn, ctx) != XIAOTAI_OK) abort();
    fake_drain_events();
}

/* ---------------------------------------------------------------- memory */

void *__real_xiaotai_mem_calloc(xiaotai_mem_kind_t kind, size_t count, size_t size);
void __real_xiaotai_mem_free(void *pointer);

void *__wrap_xiaotai_mem_calloc(xiaotai_mem_kind_t kind, size_t count, size_t size)
{
    void *pointer = __real_xiaotai_mem_calloc(kind, count, size);
    if (pointer != NULL && g_mem.watch_size != 0U && count == 1U && size == g_mem.watch_size) {
        ++g_mem.allocations;
        g_mem.kind = kind;
        g_mem.live = pointer;
    }
    return pointer;
}

void __wrap_xiaotai_mem_free(void *pointer)
{
    if (pointer != NULL && pointer == g_mem.live) {
        const unsigned char *bytes = pointer;
        bool zero = true;
        for (size_t i = 0; i < g_mem.watch_size; ++i) zero = zero && bytes[i] == 0U;
        g_mem.freed_wiped = zero;
        ++g_mem.frees;
        g_mem.live = NULL;
    }
    __real_xiaotai_mem_free(pointer);
}

/* ----------------------------------------------------------- owner queue */

bool g_run_fail_once;
unsigned g_run_failures;

xiaotai_err_t __real_xiaotai_run(void (*fn)(void *ctx), void *ctx);

xiaotai_err_t __wrap_xiaotai_run(void (*fn)(void *ctx), void *ctx)
{
    if (g_run_fail_once) {
        g_run_fail_once = false;
        ++g_run_failures;
        return XIAOTAI_ERR_NO_MEM;
    }
    return __real_xiaotai_run(fn, ctx);
}

/* ----------------------------------------------------------------- clock */

int64_t __wrap_xiaotai_clock_unix_ms(void) { return g_unix_ms; }

static _Atomic int64_t s_monotonic_us;
int64_t __real_xiaotai_time_us(void);

int64_t __wrap_xiaotai_time_us(void)
{
    int64_t at_us = atomic_load(&s_monotonic_us);
    return at_us != 0 ? at_us : __real_xiaotai_time_us();
}

void fake_clock_start(void) { atomic_store(&s_monotonic_us, 1); }

static void advance_clock(void *ctx)
{
    int64_t at_us = *(const int64_t *)ctx;
    int64_t previous = atomic_load(&s_monotonic_us);
    if (previous == 0 || at_us <= previous) abort();
    atomic_store(&s_monotonic_us, at_us);
}

void fake_clock_advance_to(int64_t at_us)
{
    /* Advance in the owner job, not while it is blocked in queue_receive().
     * The real framework runs due timers before the first fence; the second
     * drains the events they emit before the test reads the result. */
    fake_on_owner(advance_clock, &at_us);
}

/* -------------------------------------------------------------- platform */

xiaotai_err_t xiaotai_platform_require(const xiaotai_platform_requirement_t *requirement)
{
    g_requirement = *requirement;
    return XIAOTAI_OK;
}

void xiaotai_platform_status(xiaotai_platform_status_t *status) { *status = g_platform; }

bool xiaotai_platform_environment_boot_failed(void) { return false; }

xiaotai_err_t xiaotai_platform_retry(void)
{
    ++g_platform_retries;
    trace("retry");
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_platform_request(const xiaotai_platform_request_t *request, xiaotai_platform_reply_fn_t reply,
                                       void *ctx)
{
    if (g_request.refuse != XIAOTAI_OK) return g_request.refuse;
    ++g_request.count;
    snprintf(g_request.service, sizeof(g_request.service), "%s", request->service);
    g_request.method = request->method;
    snprintf(g_request.path, sizeof(g_request.path), "%s", request->path);
    snprintf(g_request.body, sizeof(g_request.body), "%s", request->body != NULL ? request->body : "");
    g_request.timeout_ms = request->timeout_ms;
    g_request.auth = request->auth;
    g_request.flags = request->flags;
    g_request.at_us = xiaotai_time_us();
    g_request.reply = reply;
    g_request.ctx = ctx;
    return XIAOTAI_OK;
}

static xiaotai_platform_reply_t s_reply;
static char *s_reply_body;

static void deliver_reply(void *ctx)
{
    (void)ctx;
    xiaotai_platform_reply_fn_t reply = g_request.reply;
    g_request.reply = NULL;
    if (reply != NULL) reply(&s_reply, g_request.ctx);
}

void fake_reply(int http_status, int32_t code, const char *body)
{
    free(s_reply_body);
    s_reply_body = strdup(body != NULL ? body : "");
    s_reply = (xiaotai_platform_reply_t){.result = XIAOTAI_OK, .http_status = http_status, .code = code,
                                         .body = s_reply_body, .size = strlen(s_reply_body), .generation = 1};
    fake_on_owner(deliver_reply, NULL);
}

void fake_reply_error(xiaotai_err_t result)
{
    s_reply = (xiaotai_platform_reply_t){.result = result, .code = XIAOTAI_PLATFORM_NO_CODE, .generation = 1};
    fake_on_owner(deliver_reply, NULL);
}

const char *fake_report_body(const char *code, const char *token, const char *client_id)
{
    static char *body;
    free(body);
    size_t size = 256U + (code ? strlen(code) : 0U) + (token ? strlen(token) : 0U) +
                  (client_id ? strlen(client_id) : 0U);
    body = malloc(size);
    int length = snprintf(body, size, "{\"code\":200,\"msg\":\"ok\",\"data\":{");
    bool first = true;
    const char *names[] = {"code", "temp_token", "temp_client_id"};
    const char *values[] = {code, token, client_id};
    for (size_t i = 0; i < 3U; ++i) {
        if (values[i] == NULL) continue;
        length += snprintf(body + length, size - (size_t)length, "%s\"%s\":\"%s\"", first ? "" : ",", names[i],
                           values[i]);
        first = false;
    }
    snprintf(body + length, size - (size_t)length, "}}");
    return body;
}

#ifndef BINDING_REAL_JWT
xiaotai_err_t xiaotai_platform_token_expiry(const char *token, int64_t *exp_unix_s)
{
    if (token == NULL || strlen(token) >= XIAOTAI_PLATFORM_BEARER_MAX) return XIAOTAI_ERR_INVALID_ARG;
    if (strncmp(token, "exp.", 4) != 0) return XIAOTAI_ERR_NOT_FOUND;
    *exp_unix_s = strtoll(token + 4, NULL, 10);
    return *exp_unix_s > 0 ? XIAOTAI_OK : XIAOTAI_ERR_NOT_FOUND;
}
#endif

xiaotai_err_t xiaotai_platform_guest_open(const xiaotai_platform_guest_t *guest, uint32_t *id)
{
    if (g_guest.open_refuse != XIAOTAI_OK) return g_guest.open_refuse;
    if (g_guest.open) return XIAOTAI_ERR_BUSY;
    ++g_guest.opens;
    g_guest.open = true;
    s_guest_ready = false;
    g_guest.id += 7U;
    snprintf(g_guest.client_id, sizeof(g_guest.client_id), "%s", guest->client_id);
    snprintf(g_guest.token, sizeof(g_guest.token), "%s", guest->token);
    g_guest.guest = *guest;
    g_guest.guest.client_id = g_guest.client_id;
    g_guest.guest.token = g_guest.token;
    *id = g_guest.id;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_platform_guest_ack(uint32_t id, uint32_t timeout_ms, xiaotai_platform_done_fn_t done,
                                         void *ctx)
{
    if (!g_guest.open || id != g_guest.id) return XIAOTAI_ERR_PLATFORM_STALE;
    if (g_guest.ack_refuse != XIAOTAI_OK) return g_guest.ack_refuse;
    ++g_guest.acks;
    g_guest.ack_timeout_ms = timeout_ms;
    g_guest.ack_pending = true;
    g_guest.ack_with_identity = g_identity.state == XIAOTAI_IDENTITY_VALID;
    g_guest.ack_done = done;
    g_guest.ack_ctx = ctx;
    trace("ack");
    return XIAOTAI_OK;
}

bool xiaotai_platform_guest_subscribed(uint32_t id)
{
    return g_guest.open && g_guest.id == id && s_guest_ready;
}

void xiaotai_platform_guest_close(uint32_t id)
{
    if (!g_guest.open || id != g_guest.id) return;
    ++g_guest.closes;
    g_guest.open = false;
    g_guest.ack_pending = false;
    trace("close");
}

static void run_ready(void *ctx)
{
    (void)ctx;
    s_guest_ready = g_guest.open;
    if (g_guest.open && g_guest.guest.ready != NULL) g_guest.guest.ready(g_guest.id, g_guest.guest.ctx);
}

void fake_guest_ready(void) { fake_on_owner(run_ready, NULL); }

static void run_unready(void *ctx)
{
    (void)ctx;
    s_guest_ready = false;
    if (g_guest.open && g_guest.guest.unready != NULL) g_guest.guest.unready(g_guest.id, g_guest.guest.ctx);
}

void fake_guest_unready(void) { fake_on_owner(run_unready, NULL); }

static void run_ready_at(void *ctx)
{
    advance_clock(ctx);
    run_ready(NULL);
}

void fake_guest_ready_at(int64_t at_us) { fake_on_owner(run_ready_at, &at_us); }

static const char *s_message_type;
static const char *s_message_json;
static void run_message(void *ctx)
{
    (void)ctx;
    if (!g_guest.open || g_guest.guest.message == NULL) return;
    xiaotai_platform_signal_t signal = {.type = s_message_type, .channel = "", .json = s_message_json,
                                        .size = strlen(s_message_json), .generation = 1, .command = true};
    g_guest.guest.message(g_guest.id, &signal, g_guest.guest.ctx);
}

void fake_guest_message(const char *type, const char *json)
{
    s_message_type = type;
    s_message_json = json;
    fake_on_owner(run_message, NULL);
}

static xiaotai_err_t s_closed_reason;
static void run_closed(void *ctx)
{
    (void)ctx;
    if (!g_guest.open) return;
    g_guest.open = false;
    g_guest.ack_pending = false;
    if (g_guest.guest.closed != NULL) g_guest.guest.closed(g_guest.id, s_closed_reason, g_guest.guest.ctx);
}

void fake_guest_closed(xiaotai_err_t reason)
{
    s_closed_reason = reason;
    fake_on_owner(run_closed, NULL);
}

static xiaotai_err_t s_ack_result;
static void run_ack(void *ctx)
{
    (void)ctx;
    if (!g_guest.ack_pending) return;
    g_guest.ack_pending = false;
    g_guest.ack_done(s_ack_result, g_guest.ack_ctx);
}

void fake_guest_ack_result(xiaotai_err_t result)
{
    s_ack_result = result;
    fake_on_owner(run_ack, NULL);
}

/* -------------------------------------------------------------- identity */

void fake_identity_set(const char *device_id, const char *device_key)
{
    memset(&g_identity.identity, 0, sizeof(g_identity.identity));
    if (device_id == NULL) {
        g_identity.state = XIAOTAI_IDENTITY_ABSENT;
        return;
    }
    snprintf(g_identity.identity.device_id, sizeof(g_identity.identity.device_id), "%s", device_id);
    snprintf(g_identity.identity.device_key, sizeof(g_identity.identity.device_key), "%s", device_key);
    g_identity.state = XIAOTAI_IDENTITY_VALID;
}

xiaotai_identity_state_t xiaotai_identity_state(void) { return g_identity.state; }

xiaotai_err_t xiaotai_identity_get(xiaotai_identity_t *out)
{
    if (g_identity.state != XIAOTAI_IDENTITY_VALID) {
        memset(out, 0, sizeof(*out));
        return XIAOTAI_ERR_NOT_FOUND;
    }
    *out = g_identity.identity;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_identity_device_id(char *out, size_t capacity)
{
    if (g_identity.state != XIAOTAI_IDENTITY_VALID) return XIAOTAI_ERR_NOT_FOUND;
    if (strlen(g_identity.identity.device_id) >= capacity) return XIAOTAI_ERR_INVALID_SIZE;
    strcpy(out, g_identity.identity.device_id);
    return XIAOTAI_OK;
}

bool xiaotai_identity_matches(const char *device_id, const char *device_key)
{
    return g_identity.state == XIAOTAI_IDENTITY_VALID && strcmp(device_id, g_identity.identity.device_id) == 0 &&
           strcmp(device_key, g_identity.identity.device_key) == 0;
}

bool xiaotai_identity_valid(const xiaotai_identity_t *identity)
{
    return identity && identity->device_id[0] && identity->device_key[0] &&
           memchr(identity->device_id, 0, sizeof(identity->device_id)) &&
           memchr(identity->device_key, 0, sizeof(identity->device_key)) &&
           memchr(identity->client_id, 0, sizeof(identity->client_id));
}

xiaotai_err_t xiaotai_identity_erase(xiaotai_identity_done_fn_t done, void *ctx)
{
    if (g_identity.erase_refuse != XIAOTAI_OK) return g_identity.erase_refuse;
    ++g_identity.erases;
    g_identity.erase_pending = true;
    g_identity.done = done;
    g_identity.done_ctx = ctx;
    trace("erase");
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_identity_erase_for_restart(xiaotai_identity_done_fn_t done, void *ctx)
{
    const xiaotai_err_t err = xiaotai_identity_erase(done, ctx);
    if (err == XIAOTAI_OK) g_identity.restart_erase = true;
    return err;
}

void xiaotai_identity_publish_restart_erase(void)
{
    if (!g_identity.restart_erase_committed) abort();
    g_identity.restart_erase_committed = false;
    fake_identity_set(NULL, NULL);
    ++g_identity.generation;
}

void fake_identity_finish_erase(xiaotai_err_t result)
{
    if (!g_identity.erase_pending) abort();
    g_identity.erase_pending = false;
    if (result == XIAOTAI_OK) {
        g_identity.restart_erase_committed = true;
        if (!g_identity.restart_erase) xiaotai_identity_publish_restart_erase();
    }
    g_identity.restart_erase = false;
    g_identity.done(result, g_identity.done_ctx);
    fake_drain_events();
}

xiaotai_err_t xiaotai_identity_save(const xiaotai_identity_t *identity, xiaotai_identity_done_fn_t done, void *ctx)
{
    if (g_identity.save_refuse != XIAOTAI_OK) return g_identity.save_refuse;
    ++g_identity.saves;
    g_identity.saving = *identity;
    g_identity.save_pending = true;
    g_identity.done = done;
    g_identity.done_ctx = ctx;
    trace("save");
    return XIAOTAI_OK;
}

void fake_identity_finish_save(xiaotai_err_t result)
{
    if (!g_identity.save_pending) return;
    g_identity.save_pending = false;
    if (result == XIAOTAI_OK) {
        g_identity.identity = g_identity.saving;
        g_identity.state = XIAOTAI_IDENTITY_VALID;
        ++g_identity.generation;
    }
    g_identity.done(result, g_identity.done_ctx);
    fake_drain_events();
}

xiaotai_err_t xiaotai_identity_reload(xiaotai_identity_done_fn_t done, void *ctx)
{
    if (g_identity.reload_refuse != XIAOTAI_OK) return g_identity.reload_refuse;
    ++g_identity.reloads;
    g_identity.reload_pending = true;
    g_identity.done = done;
    g_identity.done_ctx = ctx;
    return XIAOTAI_OK;
}

void fake_identity_finish_reload(xiaotai_err_t result, xiaotai_identity_state_t state)
{
    if (!g_identity.reload_pending) return;
    g_identity.reload_pending = false;
    if (result == XIAOTAI_OK) g_identity.state = state;
    g_identity.done(result, g_identity.done_ctx);
    fake_drain_events();
}

xiaotai_err_t xiaotai_identity_fingerprint(const uint8_t mac[XIAOTAI_IDENTITY_MAC_BYTES], char *out, size_t capacity)
{
    if (capacity < XIAOTAI_IDENTITY_FINGERPRINT_MAX + 1U) return XIAOTAI_ERR_INVALID_SIZE;
    snprintf(out, capacity, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_identity_client_id(const uint8_t mac[XIAOTAI_IDENTITY_MAC_BYTES], char *out, size_t capacity)
{
    if (g_identity.state == XIAOTAI_IDENTITY_VALID && g_identity.identity.client_id[0] != '\0') {
        snprintf(out, capacity, "%s", g_identity.identity.client_id);
        return XIAOTAI_OK;
    }
    snprintf(out, capacity, "test-%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return XIAOTAI_OK;
}

uint32_t xiaotai_identity_generation(void) { return g_identity.generation; }

void xiaotai_identity_wipe(void *data, size_t size)
{
    volatile unsigned char *bytes = data;
    while (size-- > 0U) *bytes++ = 0U;
}

/* ------------------------------------------------------------------- net */

void xiaotai_net_status(xiaotai_net_status_t *status) { *status = g_net; }
bool xiaotai_net_provisioning_boot(void) { return g_provisioning_boot; }

/* ----------------------------------------------------------------- reset */

void fake_reset(void)
{
    memset(&g_request, 0, sizeof(g_request));
    memset(&g_guest, 0, sizeof(g_guest));
    memset(&g_identity, 0, sizeof(g_identity));
    memset(&g_net, 0, sizeof(g_net));
    memset(&g_platform, 0, sizeof(g_platform));
    memset(&g_requirement, 0, sizeof(g_requirement));
    g_trace[0] = '\0';
    g_platform_retries = 0;
    g_provisioning_boot = false;
    g_run_fail_once = false;
    g_run_failures = 0;
    g_identity.generation = 1;
    g_platform.phase = XIAOTAI_PLATFORM_NO_IDENTITY;
    g_platform.clock_synced = true;
    g_platform.discovered = true;
    g_net.state = XIAOTAI_NET_UP;
    g_net.generation = 1;
    g_net.mac_valid = true;
    const uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
    memcpy(g_net.mac, mac, sizeof(mac));
    g_unix_ms = 1800000000000LL;
    size_t watch = g_mem.watch_size;
    memset(&g_mem, 0, sizeof(g_mem));
    g_mem.watch_size = watch;
}
