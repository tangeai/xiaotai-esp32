// SPDX-License-Identifier: MIT
/* Fakes of identity, platform core and network engine for the transport's
 * host tests (see fakes.h). */
#include "fakes.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "xiaotai/framework.h"
#include "xiaotai/media_uplink.h"
#include "xiaotai/transport.h"
#include "xiaotai_transport_host.h"

#define HOLD_RECORDS 64U
#define PENDING_SUSPENDS 16U

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

static xiaotai_identity_state_t s_identity_state;
static xiaotai_identity_t s_identity;

static xiaotai_err_t s_endpoint_result;
static char s_endpoint[XIAOTAI_PLATFORM_URL_MAX];
static xiaotai_platform_phase_t s_phase;
static bool s_gated;
static bool s_profile_ready;
static fake_hold_record_t s_holds[HOLD_RECORDS];
static size_t s_hold_count;
static uint32_t s_hold_next;
static uint32_t s_holds_active;
static bool s_hold_auto;
static struct {
    xiaotai_platform_done_fn_t done;
    void *ctx;
    bool used;
} s_suspends[PENDING_SUSPENDS];

static xiaotai_net_state_t s_net_state;
static bool s_mac_valid;
static bool s_provisioning;
static bool s_trace_active, s_trace_transport;
static bool s_media_accept;
static bool s_media_opus;
static fake_media_record_t s_media;
static const xiaotai_media_uplink_executor_t *s_uplink;
static xiaotai_media_uplink_work_t (*s_uplink_step)(void);

void fakes_reset(void)
{
    pthread_mutex_lock(&s_lock);
    s_identity_state = XIAOTAI_IDENTITY_ABSENT;
    memset(&s_identity, 0, sizeof(s_identity));
    s_endpoint_result = XIAOTAI_ERR_PLATFORM_OFFLINE;
    s_endpoint[0] = '\0';
    s_phase = XIAOTAI_PLATFORM_STOPPED;
    s_gated = false;
    s_profile_ready = false;
    memset(s_holds, 0, sizeof(s_holds));
    s_hold_count = 0;
    s_hold_next = 0;
    s_holds_active = 0;
    s_hold_auto = true;
    memset(s_suspends, 0, sizeof(s_suspends));
    s_net_state = XIAOTAI_NET_STOPPED;
    s_mac_valid = false;
    s_provisioning = false;
    s_trace_active = s_trace_transport = false;
    s_media_accept = true;
    s_media_opus = false;
    memset(&s_media, 0, sizeof(s_media));
    s_uplink = NULL;
    s_uplink_step = NULL;
    pthread_mutex_unlock(&s_lock);
}

void fake_media_accept(bool accept)
{
    pthread_mutex_lock(&s_lock);
    s_media_accept = accept;
    pthread_mutex_unlock(&s_lock);
}

void fake_media_opus(bool available)
{
    pthread_mutex_lock(&s_lock);
    s_media_opus = available;
    pthread_mutex_unlock(&s_lock);
}

bool xiaotai_media_can_receive(const xiaotai_media_format_t *format)
{
    pthread_mutex_lock(&s_lock);
    const bool available = s_media_opus && format && format->encoding == XIAOTAI_CODEC_OPUS &&
        format->rate_hz == 16000U && format->channels == 1U && format->packet_ms == 20U;
    pthread_mutex_unlock(&s_lock);
    return available;
}

fake_media_record_t fake_media_record(void)
{
    pthread_mutex_lock(&s_lock);
    const fake_media_record_t record = s_media;
    pthread_mutex_unlock(&s_lock);
    return record;
}

bool xiaotai_media_push(uint32_t generation, const xiaotai_media_packet_t *packet)
{
    pthread_mutex_lock(&s_lock);
    ++s_media.calls;
    s_media.generation = generation;
    s_media.frame = *packet;
    s_media.frame.data = NULL;
    s_media.first_byte = packet->size ? packet->data[0] : 0;
    const bool take = s_media_accept;
    pthread_mutex_unlock(&s_lock);
    return take;
}

xiaotai_err_t xiaotai_media_uplink_attach(const xiaotai_media_uplink_executor_t *executor)
{
    pthread_mutex_lock(&s_lock);
    s_uplink = executor;
    pthread_mutex_unlock(&s_lock);
    return XIAOTAI_OK;
}

void xiaotai_media_uplink_detach(const xiaotai_media_uplink_executor_t *executor)
{
    pthread_mutex_lock(&s_lock);
    if (s_uplink == executor) s_uplink = NULL;
    pthread_mutex_unlock(&s_lock);
}

xiaotai_media_uplink_work_t xiaotai_media_uplink_step(const xiaotai_media_uplink_executor_t *executor)
{
    pthread_mutex_lock(&s_lock);
    xiaotai_media_uplink_work_t (*run)(void) = s_uplink == executor ? s_uplink_step : NULL;
    pthread_mutex_unlock(&s_lock);
    return run ? run() : XIAOTAI_MEDIA_UPLINK_IDLE;
}

bool fake_media_uplink_wake(xiaotai_media_uplink_work_t (*step)(void))
{
    pthread_mutex_lock(&s_lock);
    const xiaotai_media_uplink_executor_t *executor = s_uplink;
    s_uplink_step = step;
    pthread_mutex_unlock(&s_lock);
    if (executor) executor->wake();
    return executor != NULL;
}

/* ---------------------------------------------------------------- identity */

void fake_identity_set(xiaotai_identity_state_t state, const char *device_id, const char *device_key,
                       const char *client_id)
{
    pthread_mutex_lock(&s_lock);
    s_identity_state = state;
    (void)snprintf(s_identity.device_id, sizeof(s_identity.device_id), "%s", device_id ? device_id : "");
    (void)snprintf(s_identity.device_key, sizeof(s_identity.device_key), "%s", device_key ? device_key : "");
    (void)snprintf(s_identity.client_id, sizeof(s_identity.client_id), "%s", client_id ? client_id : "");
    pthread_mutex_unlock(&s_lock);
}

xiaotai_identity_state_t xiaotai_identity_state(void)
{
    pthread_mutex_lock(&s_lock);
    const xiaotai_identity_state_t state = s_identity_state;
    pthread_mutex_unlock(&s_lock);
    return state;
}

xiaotai_err_t xiaotai_identity_get(xiaotai_identity_t *out)
{
    pthread_mutex_lock(&s_lock);
    const bool valid = s_identity_state == XIAOTAI_IDENTITY_VALID;
    if (valid) {
        *out = s_identity;
    } else {
        memset(out, 0, sizeof(*out));
    }
    pthread_mutex_unlock(&s_lock);
    return valid ? XIAOTAI_OK : XIAOTAI_ERR_NOT_FOUND;
}

xiaotai_err_t xiaotai_identity_client_id(const uint8_t mac[XIAOTAI_IDENTITY_MAC_BYTES], char *out, size_t capacity)
{
    pthread_mutex_lock(&s_lock);
    int written;
    if (s_identity.client_id[0]) {
        written = snprintf(out, capacity, "%s", s_identity.client_id);
    } else {
        written = snprintf(out, capacity, "test-%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4],
                           mac[5]);
    }
    pthread_mutex_unlock(&s_lock);
    return written >= 0 && (size_t)written < capacity ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_SIZE;
}

void xiaotai_identity_wipe(void *data, size_t size)
{
    volatile unsigned char *bytes = data;
    for (size_t i = 0; i < size; ++i) bytes[i] = 0;
}

/* ---------------------------------------------------------------- platform */

void fake_platform_endpoint(xiaotai_err_t result, const char *url)
{
    pthread_mutex_lock(&s_lock);
    s_endpoint_result = result;
    (void)snprintf(s_endpoint, sizeof(s_endpoint), "%s", url ? url : "");
    pthread_mutex_unlock(&s_lock);
}

void fake_platform_phase(xiaotai_platform_phase_t phase)
{
    pthread_mutex_lock(&s_lock);
    s_phase = phase;
    pthread_mutex_unlock(&s_lock);
}

void fake_platform_gated(bool gated)
{
    pthread_mutex_lock(&s_lock);
    s_gated = gated;
    pthread_mutex_unlock(&s_lock);
}

xiaotai_err_t xiaotai_platform_require_service(const char *service)
{
    return !strcmp(service, XIAOTAI_PLATFORM_SERVICE_TIRTC) ? XIAOTAI_OK : XIAOTAI_ERR_NOT_SUPPORTED;
}

bool xiaotai_platform_environment_boot_failed(void)
{
    return false;
}

bool xiaotai_platform_start_failed(void)
{
    return false;
}

xiaotai_err_t xiaotai_platform_endpoint(const char *service, char *out, size_t capacity)
{
    if (strcmp(service, XIAOTAI_PLATFORM_SERVICE_TIRTC) != 0) return XIAOTAI_ERR_NOT_SUPPORTED;
    pthread_mutex_lock(&s_lock);
    xiaotai_err_t result = s_endpoint_result;
    if (result == XIAOTAI_OK) {
        const int written = snprintf(out, capacity, "%s", s_endpoint);
        if (written < 0 || (size_t)written >= capacity) result = XIAOTAI_ERR_INVALID_SIZE;
    }
    pthread_mutex_unlock(&s_lock);
    return result;
}

void xiaotai_platform_status(xiaotai_platform_status_t *status)
{
    memset(status, 0, sizeof(*status));
    pthread_mutex_lock(&s_lock);
    status->phase = s_phase;
    status->gated = s_gated;
    status->discovered = s_endpoint_result != XIAOTAI_ERR_PLATFORM_OFFLINE;
    pthread_mutex_unlock(&s_lock);
}

void xiaotai_platform_profile_ready(bool ready)
{
    pthread_mutex_lock(&s_lock);
    s_profile_ready = ready;
    pthread_mutex_unlock(&s_lock);
}

bool fake_platform_profile_ready(void)
{
    pthread_mutex_lock(&s_lock);
    const bool ready = s_profile_ready;
    pthread_mutex_unlock(&s_lock);
    return ready;
}

static void record_hold(bool take, uint32_t hold)
{
    /* Read outside the fake's lock: the status takes the engine's own. */
    xiaotai_transport_status_t status;
    xiaotai_transport_status(&status);
    pthread_mutex_lock(&s_lock);
    if (s_hold_count < HOLD_RECORDS) {
        s_holds[s_hold_count] = (fake_hold_record_t){
            .take = take,
            .hold = hold,
            .at_ms = xiaotai_transport_host_clock(),
            .connect_reserve_held = status.connect_reserve_held,
            .start_reserve_held = status.start_reserve_held,
            .state = (int)status.state,
        };
    }
    ++s_hold_count;
    pthread_mutex_unlock(&s_lock);
}

static void deliver_suspended(void *ctx)
{
    const size_t index = (size_t)(uintptr_t)ctx;
    pthread_mutex_lock(&s_lock);
    const xiaotai_platform_done_fn_t done = s_suspends[index].done;
    void *done_ctx = s_suspends[index].ctx;
    s_suspends[index].used = false;
    pthread_mutex_unlock(&s_lock);
    if (done) done(XIAOTAI_OK, done_ctx);
}

xiaotai_err_t xiaotai_platform_signaling_hold(xiaotai_platform_done_fn_t suspended, void *ctx, uint32_t *hold)
{
    pthread_mutex_lock(&s_lock);
    const uint32_t id = ++s_hold_next;
    ++s_holds_active;
    size_t slot = PENDING_SUSPENDS;
    for (size_t i = 0; i < PENDING_SUSPENDS; ++i) {
        if (!s_suspends[i].used) {
            slot = i;
            s_suspends[i].used = true;
            s_suspends[i].done = suspended;
            s_suspends[i].ctx = ctx;
            break;
        }
    }
    const bool automatic = s_hold_auto;
    pthread_mutex_unlock(&s_lock);
    *hold = id;
    record_hold(true, id);
    if (automatic && slot < PENDING_SUSPENDS) (void)xiaotai_run(deliver_suspended, (void *)(uintptr_t)slot);
    return XIAOTAI_OK;
}

void xiaotai_platform_signaling_release(uint32_t hold)
{
    pthread_mutex_lock(&s_lock);
    if (s_holds_active) --s_holds_active;
    pthread_mutex_unlock(&s_lock);
    record_hold(false, hold);
}

xiaotai_err_t xiaotai_platform_signaling_hold_startup(xiaotai_platform_done_fn_t suspended, void *ctx, uint32_t *hold)
{
    return xiaotai_platform_signaling_hold(suspended, ctx, hold);
}

size_t fake_platform_hold_records(fake_hold_record_t *out, size_t capacity)
{
    pthread_mutex_lock(&s_lock);
    for (size_t i = 0; out && i < s_hold_count && i < capacity && i < HOLD_RECORDS; ++i) out[i] = s_holds[i];
    const size_t count = s_hold_count;
    pthread_mutex_unlock(&s_lock);
    return count;
}

uint32_t fake_platform_holds_active(void)
{
    pthread_mutex_lock(&s_lock);
    const uint32_t active = s_holds_active;
    pthread_mutex_unlock(&s_lock);
    return active;
}

void fake_platform_hold_auto(bool automatic)
{
    pthread_mutex_lock(&s_lock);
    s_hold_auto = automatic;
    pthread_mutex_unlock(&s_lock);
}

void fake_platform_suspend(void)
{
    for (size_t i = 0; i < PENDING_SUSPENDS; ++i) {
        pthread_mutex_lock(&s_lock);
        const bool used = s_suspends[i].used;
        pthread_mutex_unlock(&s_lock);
        if (used) (void)xiaotai_run(deliver_suspended, (void *)(uintptr_t)i);
    }
}

/* ----------------------------------------------------------------- network */

void fake_net_set(xiaotai_net_state_t state, bool mac_valid)
{
    pthread_mutex_lock(&s_lock);
    s_net_state = state;
    s_mac_valid = mac_valid;
    pthread_mutex_unlock(&s_lock);
}

void fake_net_provisioning(bool provisioning)
{
    pthread_mutex_lock(&s_lock);
    s_provisioning = provisioning;
    pthread_mutex_unlock(&s_lock);
}

void fake_net_trace(bool active, bool transport)
{
    pthread_mutex_lock(&s_lock);
    s_trace_active = active;
    s_trace_transport = transport;
    pthread_mutex_unlock(&s_lock);
}

void xiaotai_net_status(xiaotai_net_status_t *status)
{
    memset(status, 0, sizeof(*status));
    pthread_mutex_lock(&s_lock);
    status->state = s_net_state;
    status->mac_valid = s_mac_valid;
    if (s_mac_valid) {
        static const uint8_t mac[6] = {0x02, 0x00, 0x00, 0x12, 0x34, 0x56};
        memcpy(status->mac, mac, sizeof(mac));
        status->generation = 1;
    }
    pthread_mutex_unlock(&s_lock);
}

bool xiaotai_net_provisioning_boot(void)
{
    pthread_mutex_lock(&s_lock);
    const bool provisioning = s_provisioning;
    pthread_mutex_unlock(&s_lock);
    return provisioning;
}

bool xiaotai_net_trace_active(void)
{
    pthread_mutex_lock(&s_lock);
    const bool active = s_trace_active;
    pthread_mutex_unlock(&s_lock);
    return active;
}

bool xiaotai_net_trace_transport_active(void)
{
    pthread_mutex_lock(&s_lock);
    const bool active = s_trace_active && s_trace_transport;
    pthread_mutex_unlock(&s_lock);
    return active;
}

void xiaotai_net_trace_media(bool video) { (void)video; }

void xiaotai_net_trace_transport(const xiaotai_net_trace_transport_t *sample) { (void)sample; }
