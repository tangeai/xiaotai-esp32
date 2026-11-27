// SPDX-License-Identifier: MIT
/* The network engine's platform on the host: the scripted driver (both Wi-Fi
 * kinds, one script), the fixed setup page, the receive-path trace as a
 * no-op, and the test controls of xiaotai_net_host.h. The in-memory page
 * server and socket of src/net_platform.h join with the portal and captive
 * name-server work packages. */
#include "xiaotai_net_host.h"

#include <pthread.h>
#include <string.h>
#include <time.h>
#include "internal.h"
#include "net_platform.h"
#include "xiaotai/port/os.h"

/* ------------------------------------------------------------ the script */

typedef struct {
    xiaotai_net_host_step_t steps[XIAOTAI_NET_HOST_STEPS_MAX];
    size_t head, count;
} step_fifo_t;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_changed = PTHREAD_COND_INITIALIZER;
static step_fifo_t s_steps[XIAOTAI_NET_HOST_CALLS];
static xiaotai_net_host_record_t s_records[XIAOTAI_NET_HOST_RECORDS_MAX];
static size_t s_record_count;
static size_t s_calls[XIAOTAI_NET_HOST_CALLS];
static bool s_holding;
static uint32_t s_releases;
static xiaotai_net_driver_emit_fn_t s_emit;
static void *s_emit_ctx;
static xiaotai_err_t s_mac_result = XIAOTAI_OK;
static uint8_t s_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
static xiaotai_err_t s_signal_result = XIAOTAI_OK;
static int8_t s_signal_dbm = -50;
static xiaotai_net_scan_record_t s_scan[64];
static size_t s_scan_count;
static bool s_scan_truncated;

static void deadline_after(struct timespec *at, uint32_t timeout_ms)
{
    clock_gettime(CLOCK_REALTIME, at);
    at->tv_sec += timeout_ms / 1000U;
    at->tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
    if (at->tv_nsec >= 1000000000L) {
        at->tv_sec += 1;
        at->tv_nsec -= 1000000000L;
    }
}

/* Records the call, takes its step (or the default), holds when asked, and
 * emits the step's events with the call's token. */
static xiaotai_err_t scripted(xiaotai_net_host_call_t call, const xiaotai_net_host_record_t *detail,
                              bool *had_step)
{
    xiaotai_net_host_record_t record = *detail;
    record.call = call;
    record.on_worker = net_on_worker();
    record.at_us = xiaotai_time_us();

    xiaotai_net_host_step_t step = {.result = XIAOTAI_OK};
    pthread_mutex_lock(&s_lock);
    step_fifo_t *fifo = &s_steps[call];
    const bool queued = fifo->count > 0U;
    if (queued) {
        step = fifo->steps[fifo->head];
        fifo->head = (fifo->head + 1U) % XIAOTAI_NET_HOST_STEPS_MAX;
        --fifo->count;
    }
    if (s_record_count < XIAOTAI_NET_HOST_RECORDS_MAX) s_records[s_record_count++] = record;
    ++s_calls[call];
    if (call == XIAOTAI_NET_HOST_START) {
        s_emit = detail->start.emit;
        s_emit_ctx = detail->start.emit_ctx;
    }
    pthread_cond_broadcast(&s_changed);
    if (step.hold) {
        const uint32_t release = s_releases;
        s_holding = true;
        pthread_cond_broadcast(&s_changed);
        while (s_releases == release) pthread_cond_wait(&s_changed, &s_lock);
        s_holding = false;
    }
    const xiaotai_net_driver_emit_fn_t emit = s_emit;
    void *const emit_ctx = s_emit_ctx;
    pthread_mutex_unlock(&s_lock);

    for (uint8_t i = 0; i < step.event_count && emit; ++i) {
        xiaotai_net_driver_event_t event = step.events[i];
        if (event.token == 0U && (call == XIAOTAI_NET_HOST_JOIN || call == XIAOTAI_NET_HOST_SCAN)) {
            event.token = detail->token;
        }
        emit(emit_ctx, &event);
    }
    if (had_step) *had_step = queued;
    return step.result;
}

static xiaotai_network_kind_t kind_of(void *self) { return (xiaotai_network_kind_t)(uintptr_t)self; }

static xiaotai_err_t host_start(void *self, const xiaotai_network_t *part, const xiaotai_net_driver_start_t *start)
{
    const xiaotai_err_t err = xiaotai_require_internal_stack("wifi.start");
    if (err != XIAOTAI_OK) return err;
    xiaotai_net_host_record_t record = {.kind = kind_of(self), .part = part};
    if (start) record.start = *start;
    return scripted(XIAOTAI_NET_HOST_START, &record, NULL);
}

static xiaotai_err_t host_join(void *self, uint32_t token, const xiaotai_net_credentials_t *credentials,
                               uint32_t flags)
{
    const xiaotai_err_t err = xiaotai_require_internal_stack("wifi.join");
    if (err != XIAOTAI_OK) return err;
    xiaotai_net_host_record_t record = {.kind = kind_of(self), .token = token, .flags = flags};
    if (credentials) {
        record.has_credentials = true;
        record.credentials = *credentials;
    }
    return scripted(XIAOTAI_NET_HOST_JOIN, &record, NULL);
}

static xiaotai_err_t host_leave(void *self)
{
    const xiaotai_net_host_record_t record = {.kind = kind_of(self)};
    return scripted(XIAOTAI_NET_HOST_LEAVE, &record, NULL);
}

static xiaotai_err_t host_scan(void *self, uint32_t token)
{
    const xiaotai_net_host_record_t record = {.kind = kind_of(self), .token = token};
    return scripted(XIAOTAI_NET_HOST_SCAN, &record, NULL);
}

static xiaotai_err_t host_scan_cancel(void *self)
{
    const xiaotai_net_host_record_t record = {.kind = kind_of(self)};
    return scripted(XIAOTAI_NET_HOST_SCAN_CANCEL, &record, NULL);
}

static xiaotai_err_t host_scan_read(void *self, xiaotai_net_scan_record_t *records, size_t capacity,
                                    size_t *count, bool *truncated)
{
    const xiaotai_net_host_record_t record = {.kind = kind_of(self)};
    const xiaotai_err_t result = scripted(XIAOTAI_NET_HOST_SCAN_READ, &record, NULL);
    pthread_mutex_lock(&s_lock);
    const size_t copied = s_scan_count < capacity ? s_scan_count : capacity;
    if (records && copied) memcpy(records, s_scan, copied * sizeof(*records));
    if (count) *count = result == XIAOTAI_OK ? copied : 0U;
    if (truncated) *truncated = result == XIAOTAI_OK && (s_scan_truncated || s_scan_count > capacity);
    pthread_mutex_unlock(&s_lock);
    return result;
}

static xiaotai_err_t host_ap_start(void *self, const xiaotai_net_ap_config_t *config)
{
    const xiaotai_err_t err = xiaotai_require_internal_stack("wifi.ap_start");
    if (err != XIAOTAI_OK) return err;
    xiaotai_net_host_record_t record = {.kind = kind_of(self)};
    if (config) record.ap = *config;
    return scripted(XIAOTAI_NET_HOST_AP_START, &record, NULL);
}

static xiaotai_err_t host_ap_stop(void *self)
{
    const xiaotai_err_t err = xiaotai_require_internal_stack("wifi.ap_stop");
    if (err != XIAOTAI_OK) return err;
    const xiaotai_net_host_record_t record = {.kind = kind_of(self)};
    return scripted(XIAOTAI_NET_HOST_AP_STOP, &record, NULL);
}

static xiaotai_err_t host_signal(void *self, int8_t *dbm)
{
    const xiaotai_net_host_record_t record = {.kind = kind_of(self)};
    bool stepped = false;
    xiaotai_err_t result = scripted(XIAOTAI_NET_HOST_SIGNAL, &record, &stepped);
    pthread_mutex_lock(&s_lock);
    if (!stepped) result = s_signal_result;
    if (dbm && result == XIAOTAI_OK) *dbm = s_signal_dbm;
    pthread_mutex_unlock(&s_lock);
    return result;
}

static xiaotai_err_t host_mac(void *self, uint8_t mac[6])
{
    const xiaotai_net_host_record_t record = {.kind = kind_of(self)};
    bool stepped = false;
    xiaotai_err_t result = scripted(XIAOTAI_NET_HOST_MAC, &record, &stepped);
    pthread_mutex_lock(&s_lock);
    if (!stepped) result = s_mac_result;
    if (mac && result == XIAOTAI_OK) memcpy(mac, s_mac, 6);
    pthread_mutex_unlock(&s_lock);
    return result;
}

static void host_report(void *self, const xiaotai_reply_t *out)
{
    const xiaotai_net_host_record_t record = {.kind = kind_of(self)};
    (void)scripted(XIAOTAI_NET_HOST_REPORT, &record, NULL);
    if (out && out->write) out->write(out->ctx, "driver=host");
}

#define HOST_CAPABILITIES                                                                          \
    (XIAOTAI_NET_DRIVER_CREDENTIALS | XIAOTAI_NET_DRIVER_SCAN | XIAOTAI_NET_DRIVER_ACCESS_POINT | \
     XIAOTAI_NET_DRIVER_SIGNAL)

#define HOST_DRIVER(driver_kind, driver_name)                                                   \
    {                                                                                           \
        .kind = (driver_kind), .name = (driver_name), .capabilities = HOST_CAPABILITIES,       \
        .start = host_start, .join = host_join, .leave = host_leave, .scan = host_scan,        \
        .scan_cancel = host_scan_cancel, .scan_read = host_scan_read, .ap_start = host_ap_start, \
        .ap_stop = host_ap_stop, .signal = host_signal, .mac = host_mac, .report = host_report, \
        .self = (void *)(uintptr_t)(driver_kind),                                               \
    }

static const xiaotai_net_driver_t s_wifi = HOST_DRIVER(XIAOTAI_NETWORK_WIFI, "host_wifi");
static const xiaotai_net_driver_t s_wifi_coprocessor =
    HOST_DRIVER(XIAOTAI_NETWORK_WIFI_COPROCESSOR, "host_wifi_cp");

/* ------------------------------------------------------- platform seam */

static const char k_page[] = "<!doctype html><meta charset=\"utf-8\"><title>XiaoTai</title>";

const char *net_platform_page(size_t *size)
{
    if (size) *size = sizeof(k_page) - 1U;
    return k_page;
}

void net_platform_register_drivers(void)
{
    /* A kind already taken (a test registered its own driver) stays as it is. */
    (void)xiaotai_net_driver_register(&s_wifi);
    (void)xiaotai_net_driver_register(&s_wifi_coprocessor);
}

xiaotai_err_t net_platform_trace_install(void) { return XIAOTAI_OK; }

/* --------------------------------------------------------- test controls */

void xiaotai_net_host_reset(void)
{
    pthread_mutex_lock(&s_lock);
    memset(s_steps, 0, sizeof(s_steps));
    memset(s_records, 0, sizeof(s_records));
    memset(s_calls, 0, sizeof(s_calls));
    s_record_count = 0;
    s_emit = NULL;
    s_emit_ctx = NULL;
    s_mac_result = XIAOTAI_OK;
    const uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    memcpy(s_mac, mac, sizeof(s_mac));
    s_signal_result = XIAOTAI_OK;
    s_signal_dbm = -50;
    s_scan_count = 0;
    s_scan_truncated = false;
    ++s_releases;
    pthread_cond_broadcast(&s_changed);
    pthread_mutex_unlock(&s_lock);
}

const xiaotai_net_driver_t *xiaotai_net_host_driver(xiaotai_network_kind_t kind)
{
    if (kind == XIAOTAI_NETWORK_WIFI) return &s_wifi;
    if (kind == XIAOTAI_NETWORK_WIFI_COPROCESSOR) return &s_wifi_coprocessor;
    return NULL;
}

xiaotai_err_t xiaotai_net_host_step(xiaotai_net_host_call_t call, const xiaotai_net_host_step_t *step)
{
    if ((unsigned)call >= XIAOTAI_NET_HOST_CALLS || !step || step->event_count > XIAOTAI_NET_HOST_STEP_EVENTS) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&s_lock);
    step_fifo_t *fifo = &s_steps[call];
    xiaotai_err_t result = XIAOTAI_ERR_NO_MEM;
    if (fifo->count < XIAOTAI_NET_HOST_STEPS_MAX) {
        fifo->steps[(fifo->head + fifo->count) % XIAOTAI_NET_HOST_STEPS_MAX] = *step;
        ++fifo->count;
        result = XIAOTAI_OK;
    }
    pthread_mutex_unlock(&s_lock);
    return result;
}

bool xiaotai_net_host_wait_held(uint32_t timeout_ms)
{
    struct timespec until;
    deadline_after(&until, timeout_ms);
    pthread_mutex_lock(&s_lock);
    while (!s_holding && pthread_cond_timedwait(&s_changed, &s_lock, &until) == 0) {
    }
    const bool holding = s_holding;
    pthread_mutex_unlock(&s_lock);
    return holding;
}

void xiaotai_net_host_release(void)
{
    pthread_mutex_lock(&s_lock);
    ++s_releases;
    pthread_cond_broadcast(&s_changed);
    pthread_mutex_unlock(&s_lock);
}

size_t xiaotai_net_host_records(xiaotai_net_host_record_t *records, size_t capacity)
{
    pthread_mutex_lock(&s_lock);
    const size_t count = s_record_count;
    const size_t copied = count < capacity ? count : capacity;
    if (records && copied) memcpy(records, s_records, copied * sizeof(*records));
    pthread_mutex_unlock(&s_lock);
    return count;
}

bool xiaotai_net_host_wait_calls(xiaotai_net_host_call_t call, size_t count, uint32_t timeout_ms)
{
    if ((unsigned)call >= XIAOTAI_NET_HOST_CALLS) return false;
    struct timespec until;
    deadline_after(&until, timeout_ms);
    pthread_mutex_lock(&s_lock);
    while (s_calls[call] < count && pthread_cond_timedwait(&s_changed, &s_lock, &until) == 0) {
    }
    const bool reached = s_calls[call] >= count;
    pthread_mutex_unlock(&s_lock);
    return reached;
}

xiaotai_err_t xiaotai_net_host_emit(const xiaotai_net_driver_event_t *event)
{
    if (!event) return XIAOTAI_ERR_INVALID_ARG;
    pthread_mutex_lock(&s_lock);
    const xiaotai_net_driver_emit_fn_t emit = s_emit;
    void *const ctx = s_emit_ctx;
    pthread_mutex_unlock(&s_lock);
    if (!emit) return XIAOTAI_ERR_INVALID_STATE;
    emit(ctx, event);
    return XIAOTAI_OK;
}

void xiaotai_net_host_set_mac(xiaotai_err_t result, const uint8_t mac[6])
{
    pthread_mutex_lock(&s_lock);
    s_mac_result = result;
    if (mac) memcpy(s_mac, mac, sizeof(s_mac));
    pthread_mutex_unlock(&s_lock);
}

void xiaotai_net_host_set_signal(xiaotai_err_t result, int8_t dbm)
{
    pthread_mutex_lock(&s_lock);
    s_signal_result = result;
    s_signal_dbm = dbm;
    pthread_mutex_unlock(&s_lock);
}

void xiaotai_net_host_set_scan(const xiaotai_net_scan_record_t *records, size_t count, bool truncated)
{
    pthread_mutex_lock(&s_lock);
    if (count > sizeof(s_scan) / sizeof(s_scan[0])) count = sizeof(s_scan) / sizeof(s_scan[0]);
    if (records && count) memcpy(s_scan, records, count * sizeof(*records));
    s_scan_count = records ? count : 0U;
    s_scan_truncated = truncated;
    pthread_mutex_unlock(&s_lock);
}

void xiaotai_net_host_config(xiaotai_net_config_t *configured, xiaotai_net_config_t *effective)
{
    xiaotai_net_config_t given;
    net_config_configured(&given);
    if (configured) *configured = given;
    if (!effective) return;
    if (net_state) {
        *effective = net_state->config;
    } else {
        net_config_resolve(&given, effective);
    }
}

bool xiaotai_net_host_on_worker(void) { return net_on_worker(); }

void xiaotai_net_host_set_restart(void (*restart)(void)) { net_set_restart(restart); }

static xiaotai_err_t sync_call(net_call_t *call)
{
    call->size = 0;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_net_host_sync(uint32_t timeout_ms)
{
    return net_call(sync_call, 0, NULL, 0, NULL, 0, NULL, timeout_ms);
}

void xiaotai_net_host_tap(void (*tap)(const xiaotai_net_driver_event_t *event, void *ctx), void *ctx)
{
    net_set_driver_tap(tap, ctx);
}
