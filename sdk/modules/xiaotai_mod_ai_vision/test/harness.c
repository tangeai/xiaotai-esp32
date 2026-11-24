// SPDX-License-Identifier: MIT
/* Harness of the AI vision module's host tests (see harness.h). */
#include "harness.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "xiaotai/eventlog.h"
#include "xiaotai/port/os.h"
#include "xiaotai/video_link.h"
#include "xiaotai_kv_host.h"

fake_video_t fake_video;
fake_transport_t fake_transport;
char trace[2048];
int state_events[STATE_EVENTS_MAX];
size_t state_event_count;
xiaotai_ai_vision_status_t last_state_payload;

void trace_clear(void) { trace[0] = '\0'; }

int trace_count(const char *word)
{
    int count = 0;
    const size_t length = strlen(word);
    for (const char *at = strstr(trace, word); at; at = strstr(at + length, word)) ++count;
    return count;
}

static void note(const char *format, ...)
{
    char text[64];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    strncat(trace, text, sizeof(trace) - strlen(trace) - 1);
    strncat(trace, " ", sizeof(trace) - strlen(trace) - 1);
}

/* ------------------------------------------------------- fake video engine */

static bool fake_ready(void *ctx, uint32_t generation)
{
    (void)ctx;
    (void)generation;
    return false;
}

static xiaotai_err_t fake_send(void *ctx, uint32_t generation, const xiaotai_video_packet_t *packet,
                               uint32_t backlog_limit_bytes)
{
    (void)ctx;
    (void)generation;
    (void)packet;
    (void)backlog_limit_bytes;
    return XIAOTAI_ERR_INVALID_STATE;
}

const xiaotai_video_link_t fake_transport_link = {.ready = fake_ready, .send = fake_send};

void fakes_reset(void)
{
    memset(&fake_video, 0, sizeof(fake_video));
    fake_video.capabilities.uplink_codecs =
        XIAOTAI_VIDEO_CODEC_BIT(XIAOTAI_VIDEO_CODEC_JPEG) | XIAOTAI_VIDEO_CODEC_BIT(XIAOTAI_VIDEO_CODEC_H264);
    fake_video.capabilities.camera_width = 320;
    fake_video.capabilities.camera_height = 240;
    memset(&fake_transport, 0, sizeof(fake_transport));
}

void xiaotai_video_capabilities(xiaotai_video_capabilities_t *capabilities)
{
    ++fake_video.capability_reads;
    *capabilities = fake_video.capabilities;
}

xiaotai_err_t xiaotai_video_resolve(const xiaotai_video_profile_t *profile, size_t camera, uint16_t *width,
                                    uint16_t *height)
{
    ++fake_video.resolves;
    fake_video.resolved = *profile;
    fake_video.resolved_camera = camera;
    if (fake_video.resolve_result != XIAOTAI_OK) return fake_video.resolve_result;
    *width = profile->width;
    *height = profile->height;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_video_require_uplink(const xiaotai_video_profile_t *profile)
{
    ++fake_video.requires;
    fake_video.required = *profile;
    note("require");
    return fake_video.require_result;
}

xiaotai_err_t xiaotai_video_open(const xiaotai_video_session_t *session)
{
    ++fake_video.opens;
    fake_video.session = *session;
    if (session->uplink) fake_video.opened = *session->uplink;
    fake_video.opened_downlink = session->downlink != NULL;
    note("open:%lu", (unsigned long)session->generation);
    return fake_video.open_result;
}

void xiaotai_video_close(uint32_t generation)
{
    ++fake_video.closes;
    note("close:%lu", (unsigned long)generation);
}

const xiaotai_video_link_t *xiaotai_video_transport_link(void) { return &fake_transport_link; }

/* -------------------------------------------------------- fake transport */

xiaotai_err_t xiaotai_link_permit(uint32_t generation, xiaotai_link_media_kind_t kind, bool allowed)
{
    ++fake_transport.permits;
    note("permit:%lu:%d:%d", (unsigned long)generation, (int)kind, allowed ? 1 : 0);
    return allowed ? fake_transport.grant_result : XIAOTAI_OK;
}

/* ------------------------------------------------------------ host module */

static void on_state(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    const xiaotai_ai_vision_status_t *status = event->data;
    int value = (int)event->args.value;
    if (!status || event->size != sizeof(*status) || (int)status->state != value) {
        value = -1;
    } else {
        last_state_payload = *status;
    }
    if (state_event_count < STATE_EVENTS_MAX) state_events[state_event_count++] = value;
    note("state:%d", value);
}

static void on_ai_revoked(uint32_t id, xiaotai_revoke_t reason, void *ctx)
{
    (void)id;
    (void)reason;
    (void)ctx;
    note("revoked");
}

static xiaotai_err_t host_init(void) { return xiaotai_event_subscribe(XIAOTAI_EVENT_AI_VISION_STATE, on_state, NULL); }

static const xiaotai_module_t s_host = {.name = "host", .init = host_init};
static const xiaotai_module_t s_vision = {.name = "ai_vision", .init = xiaotai_module_ai_vision_init};
static const xiaotai_module_t *const s_with_vision[] = {&s_vision, &s_host};
static const xiaotai_module_t *const s_without_vision[] = {&s_host};
static const xiaotai_priority_t s_priorities[] = {{"ai", 2}, {"call", 3}, {NULL, 0}};

static void restart_fn(void) {}

static int start(const xiaotai_module_t *const *modules, size_t count)
{
    (void)xiaotai_eventlog_init(NULL);
    const xiaotai_framework_config_t config = {
        .modules = modules, .module_count = count, .priorities = s_priorities,
        .available_needs = 0xFFFFFFFFU, .restart = restart_fn,
    };
    const xiaotai_err_t err = xiaotai_framework_start(&config);
    if (err != XIAOTAI_OK) {
        fprintf(stderr, "framework start failed: %d\n", err);
        return 1;
    }
    drain();
    return 0;
}

static void reset_records(bool clear_storage)
{
    trace_clear();
    state_event_count = 0;
    memset(&last_state_payload, 0, sizeof(last_state_payload));
    if (clear_storage) xiaotai_kv_host_clear();
}

int harness_start(const xiaotai_ai_vision_config_t *config, bool clear_storage)
{
    reset_records(clear_storage);
    xiaotai_ai_vision_reset();
    if (config && xiaotai_ai_vision_configure(config) != XIAOTAI_OK) {
        fprintf(stderr, "configure refused before start\n");
        return 1;
    }
    return start(s_with_vision, 2);
}

int harness_start_without_vision(void)
{
    reset_records(true);
    xiaotai_ai_vision_reset();
    return start(s_without_vision, 1);
}

void harness_stop(void)
{
    xiaotai_framework_reset();
    xiaotai_ai_vision_reset();
}

/* ------------------------------------------------------------- owner calls */

static xiaotai_signal_t s_sync;
static bool s_sync_ready;

static void give(void *ctx)
{
    (void)ctx;
    xiaotai_signal_give(&s_sync);
}

void drain(void)
{
    if (!s_sync_ready) {
        (void)xiaotai_signal_init(&s_sync);
        s_sync_ready = true;
    }
    /* Several rounds: what a round queues (an event the module emits) runs
     * before the next round ends. */
    for (int i = 0; i < 4; ++i) {
        if (xiaotai_run(give, NULL) != XIAOTAI_OK || !xiaotai_signal_take(&s_sync, 3000)) {
            fprintf(stderr, "owner task did not answer\n");
            abort();
        }
    }
}

void on_owner(void (*fn)(void *ctx), void *ctx)
{
    if (xiaotai_run(fn, ctx) != XIAOTAI_OK) {
        fprintf(stderr, "owner queue refused a job\n");
        abort();
    }
    drain();
}

typedef struct {
    uint32_t id, generation;
    xiaotai_err_t result;
    bool flag;
    int32_t value;
    const char *name;
    const char *activity, *slot, *scene;
    const void **out;
    size_t capacity, count;
    xiaotai_video_event_t uplink;
    xiaotai_ai_vision_status_t status;
} owner_call_t;

static void do_ai_begin(void *ctx)
{
    owner_call_t *c = ctx;
    const xiaotai_activity_t ai = {
        .name = "ai", .resources = XIAOTAI_RESOURCE_AUDIO_IN | XIAOTAI_RESOURCE_AUDIO_OUT | XIAOTAI_RESOURCE_LINK,
        .revoked = on_ai_revoked, .generation = c->generation,
    };
    if (xiaotai_activity_begin(&ai, &c->id) != XIAOTAI_OK) c->id = 0;
}

uint32_t owner_ai_begin(void) { return owner_ai_begin_at(0); }

uint32_t owner_ai_begin_at(uint32_t generation)
{
    owner_call_t c = {.generation = generation};
    on_owner(do_ai_begin, &c);
    return c.id;
}

static void do_generation(void *ctx)
{
    owner_call_t *c = ctx;
    xiaotai_activity_set_generation(c->id, c->generation);
}

void owner_ai_generation(uint32_t id, uint32_t generation)
{
    owner_call_t c = {.id = id, .generation = generation};
    on_owner(do_generation, &c);
}

static void do_end(void *ctx) { xiaotai_activity_end(((owner_call_t *)ctx)->id); }

void owner_ai_end(uint32_t id)
{
    owner_call_t c = {.id = id};
    on_owner(do_end, &c);
}

static void do_call_begin(void *ctx)
{
    static const xiaotai_activity_t call = {
        .name = "call", .resources = XIAOTAI_RESOURCE_AUDIO_IN | XIAOTAI_RESOURCE_AUDIO_OUT | XIAOTAI_RESOURCE_LINK,
    };
    owner_call_t *c = ctx;
    if (xiaotai_activity_begin(&call, &c->id) != XIAOTAI_OK) c->id = 0;
}

uint32_t owner_call_begin(void)
{
    owner_call_t c = {0};
    on_owner(do_call_begin, &c);
    return c.id;
}

void owner_call_end(uint32_t id) { owner_ai_end(id); }

static void do_current(void *ctx) { ((owner_call_t *)ctx)->id = xiaotai_activity_current(NULL); }

uint32_t owner_current_activity(void)
{
    owner_call_t c = {0};
    on_owner(do_current, &c);
    return c.id;
}

static void do_activity_generation(void *ctx)
{
    owner_call_t *c = ctx;
    c->generation = xiaotai_activity_generation(c->id);
}

uint32_t owner_activity_generation(uint32_t id)
{
    owner_call_t c = {.id = id};
    on_owner(do_activity_generation, &c);
    return c.generation;
}

static void do_uplink(void *ctx)
{
    owner_call_t *c = ctx;
    const xiaotai_args_t args = {.value = (int32_t)c->uplink.phase};
    c->result = xiaotai_event_emit(XIAOTAI_EVENT_VIDEO_UPLINK, &args, &c->uplink, sizeof(c->uplink));
}

void owner_uplink(uint32_t generation, xiaotai_video_phase_t phase, xiaotai_err_t error)
{
    owner_call_t c = {.uplink = {.generation = generation, .phase = phase, .error = error}};
    on_owner(do_uplink, &c);
}

static void do_set_enabled(void *ctx)
{
    owner_call_t *c = ctx;
    c->result = xiaotai_ai_vision_set_enabled(c->flag);
}

xiaotai_err_t owner_set_enabled(bool enabled)
{
    owner_call_t c = {.flag = enabled};
    on_owner(do_set_enabled, &c);
    return c.result;
}

static void do_status(void *ctx) { xiaotai_ai_vision_status(&((owner_call_t *)ctx)->status); }

xiaotai_ai_vision_status_t owner_status(void)
{
    owner_call_t c = {0};
    on_owner(do_status, &c);
    return c.status;
}

static void do_contributions(void *ctx)
{
    owner_call_t *c = ctx;
    c->count = xiaotai_activity_contributions(c->activity, c->slot, c->scene, c->out, c->capacity);
}

size_t owner_contributions(const char *activity, const char *slot, const char *scene, const void **out,
                           size_t capacity)
{
    owner_call_t c = {.activity = activity, .slot = slot, .scene = scene, .out = out, .capacity = capacity};
    on_owner(do_contributions, &c);
    return c.count;
}

static void do_setting_set(void *ctx)
{
    owner_call_t *c = ctx;
    c->result = xiaotai_setting_set_int(c->name, c->value);
}

xiaotai_err_t owner_setting_set(const char *name, int32_t value)
{
    owner_call_t c = {.name = name, .value = value};
    on_owner(do_setting_set, &c);
    return c.result;
}

xiaotai_err_t post_vision_intent(int32_t value)
{
    const xiaotai_args_t args = {.value = value};
    const xiaotai_err_t err = xiaotai_intent_post(XIAOTAI_INTENT_VISION_ENABLE, &args);
    drain();
    return err;
}

static char s_diag[512];

static void collect(void *ctx, const char *text)
{
    (void)ctx;
    strncat(s_diag, text, sizeof(s_diag) - strlen(s_diag) - 1);
}

static void do_diag(void *ctx)
{
    (void)ctx;
    s_diag[0] = '\0';
    const xiaotai_reply_t reply = {.write = collect};
    xiaotai_diag_report("ai_vision", &reply);
}

const char *owner_diag(void)
{
    on_owner(do_diag, NULL);
    return s_diag;
}

static uint32_t s_quiesce_ticket;

static void do_quiesce(void *ctx)
{
    owner_call_t *c = ctx;
    if (c->flag) {
        c->result = xiaotai_quiesce_begin("test", &s_quiesce_ticket);
    } else {
        xiaotai_quiesce_end(s_quiesce_ticket);
        s_quiesce_ticket = 0;
    }
}

void owner_quiesce(bool quiesced)
{
    owner_call_t c = {.flag = quiesced};
    on_owner(do_quiesce, &c);
}

/* ------------------------------------------------------------- event log */

#define LOG_READ_MAX 64U

static size_t read_log(xiaotai_eventlog_entry_t *entries)
{
    const uint32_t latest = xiaotai_eventlog_latest();
    return xiaotai_eventlog_read(latest > LOG_READ_MAX ? latest - LOG_READ_MAX : 0, entries, LOG_READ_MAX);
}

static bool ours(const xiaotai_eventlog_entry_t *entry, const char *label)
{
    return strcmp(entry->source, "ai_vision") == 0 && strcmp(entry->label, label) == 0;
}

int eventlog_count(const char *label)
{
    xiaotai_eventlog_entry_t entries[LOG_READ_MAX];
    const size_t count = read_log(entries);
    int found = 0;
    for (size_t i = 0; i < count; ++i) found += ours(&entries[i], label) ? 1 : 0;
    return found;
}

bool eventlog_last(const char *label, uint32_t *generation, int32_t *value, uint8_t *level)
{
    xiaotai_eventlog_entry_t entries[LOG_READ_MAX];
    const size_t count = read_log(entries);
    for (size_t i = count; i-- > 0;) {
        if (!ours(&entries[i], label)) continue;
        *generation = entries[i].generation;
        *value = entries[i].value;
        *level = entries[i].level;
        return true;
    }
    return false;
}
