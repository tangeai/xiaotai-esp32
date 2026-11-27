// SPDX-License-Identifier: MIT
/* AI vision module: still pictures from the camera on the AI conversation's
 * link while the conversation runs.
 *
 * The module is a session extension of the activity "ai". The activity's
 * generation is the link of a conversation the service accepted: the AI talk
 * module sets it when the handshake passes and clears it the moment the
 * conversation starts playing out its tail. Pictures therefore flow exactly
 * while that generation is non-zero, the user's switch is on and the device
 * can produce them; the video engine captures, encodes and paces, this module
 * decides only when.
 *
 * Everything runs on the framework owner task (attachment callbacks, the video
 * engine's events, the setting's change, the intent, the [owner] functions), so
 * the state needs no lock. The state is static: the module allocates nothing.
 * The engine takes the encoder's output buffer when the uplink opens and frees
 * it when it closes, so nothing is held while no conversation runs. */
#include <stdio.h>
#include <string.h>
#include "xiaotai/eventlog.h"
#include "xiaotai/framework.h"
#include "xiaotai/link.h"
#include "xiaotai/modules/ai_talk.h"
#include "xiaotai/modules/ai_vision.h"
#include "xiaotai/port/log.h"
#include "xiaotai/video.h"
#include "xiaotai/video_link.h"
#include "xiaotai/video_profiles.h"

#define VISION_SOURCE "ai_vision"
#define VISION_SETTING "ai_vision.enabled"
#define VISION_SETTING_KEY "ai_vision"

static const char *TAG = "ai_vision";

/* One fresh 320 x 240 JPEG a second, starting at quality 45; a stale picture
 * is replaced at once instead of leaving a one-second hole; a failed uplink is
 * not retried within the conversation (retry_ms 0). */
static const xiaotai_video_profile_t STILL = XIAOTAI_VIDEO_PROFILE_JPEG_STILL;

/* The video part of the conversation's link: JPEG on video stream 0 (every
 * JPEG is a key frame, which the engine marks), a peer's subscription to any
 * other stream refused, nothing received. The service takes the pictures
 * without subscribing, so this module's permission opens the send gate once
 * the session is accepted. */
static const xiaotai_link_media_t LINK_VIDEO = {
    .send = true,
    .send_stream = 0,
    .peer_streams = XIAOTAI_LINK_PEER_STREAM_REFUSE,
    .send_format = {.codec = XIAOTAI_LINK_CODEC_JPEG, .rate_hz = 0, .channels = 0},
    .receive = false,
};

static const xiaotai_contribution_t CONTRIBUTIONS[] = {
    {.slot = XIAOTAI_LINK_SLOT_VIDEO, .scene = XIAOTAI_AI_TALK_SCENE, .data = &LINK_VIDEO},
    {.slot = NULL, .scene = NULL, .data = NULL},
};

typedef struct {
    xiaotai_ai_vision_config_t config;
    bool initialized;               /* init ran: configure is refused from then on */
    bool available;                 /* a JPEG uplink of the still profile can be opened */
    xiaotai_err_t unavailable;      /* why not available (diagnostics); XIAOTAI_OK when available */
    bool enabled;                   /* the user's switch */
    xiaotai_ai_vision_state_t state;
    xiaotai_err_t error;            /* why FAILED */
    uint32_t activity;              /* id of the activity "ai" that runs; 0: none */
    uint32_t generation;            /* that activity's generation as last heard; 0: not usable */
    uint32_t link;                  /* generation of the open video session; 0: none */
    uint32_t failed;                /* generation whose pictures failed: never reopened */
    xiaotai_err_t failed_error;
    uint32_t counted;               /* generation last counted in sessions */
    uint32_t sessions, failures;
    uint32_t ignored;               /* uplink events and callbacks for something not this module's */
    uint32_t events_lost;           /* state events the framework's queue did not take */
} vision_t;

static vision_t s_vision = {.enabled = true};

/* ------------------------------------------------------------------- state */

static void publish(xiaotai_ai_vision_state_t state, xiaotai_err_t error)
{
    s_vision.error = state == XIAOTAI_AI_VISION_FAILED ? error : XIAOTAI_OK;
    if (state == s_vision.state) return;
    s_vision.state = state;
    xiaotai_ai_vision_status_t status;
    xiaotai_ai_vision_status(&status);
    const xiaotai_args_t args = {.value = (int32_t)state};
    if (xiaotai_event_emit(XIAOTAI_EVENT_AI_VISION_STATE, &args, &status, sizeof(status)) != XIAOTAI_OK) {
        ++s_vision.events_lost;
    }
}

/* The pictures of this conversation failed: reported once, never retried
 * within it (the conversation itself goes on with audio alone). */
static void fail(uint32_t generation, xiaotai_err_t error)
{
    if (error == XIAOTAI_OK) error = XIAOTAI_FAIL;
    s_vision.failed = generation;
    s_vision.failed_error = error;
    ++s_vision.failures;
    xiaotai_eventlog_error(VISION_SOURCE, "failed", generation, error);
    XIAOTAI_LOGW(TAG, "pictures failed gen=%lu err=%d; the conversation goes on without them",
                 (unsigned long)generation, (int)error);
    publish(XIAOTAI_AI_VISION_FAILED, error);
}

/* Permission first, so the engine's first readiness check already passes; the
 * transport's link object carries the frames (xiaotai/video_link.h). */
static void open_session(uint32_t generation)
{
    if (generation == s_vision.failed) {
        publish(XIAOTAI_AI_VISION_FAILED, s_vision.failed_error);
        return;
    }
    if (s_vision.counted != generation) {
        s_vision.counted = generation;
        ++s_vision.sessions;
    }
    xiaotai_err_t err = xiaotai_link_permit(generation, XIAOTAI_LINK_VIDEO, true);
    if (err == XIAOTAI_OK) {
        const xiaotai_video_session_t session = {
            .generation = generation,
            .uplink = &STILL,
            .downlink = NULL,
            .link = xiaotai_video_transport_link(),
            .camera = 0,
            .display = 0,
            .placement = NULL,
        };
        err = xiaotai_video_open(&session);
        if (err != XIAOTAI_OK) (void)xiaotai_link_permit(generation, XIAOTAI_LINK_VIDEO, false);
    }
    if (err != XIAOTAI_OK) {
        fail(generation, err);
        return;
    }
    s_vision.link = generation;
    xiaotai_eventlog_note(VISION_SOURCE, "opened", generation, 0);
    publish(XIAOTAI_AI_VISION_STARTING, XIAOTAI_OK);
}

/* withdraw: the link outlives the pictures (the tail plays, the switch went
 * off), so the permission is taken back as well. When the activity ends, the
 * host releases the link itself, which clears the permission. */
static void close_session(bool withdraw)
{
    const uint32_t link = s_vision.link;
    if (link == 0U) return;
    s_vision.link = 0;
    xiaotai_video_close(link);
    if (withdraw) (void)xiaotai_link_permit(link, XIAOTAI_LINK_VIDEO, false);
    xiaotai_eventlog_note(VISION_SOURCE, "closed", link, 0);
}

/* Brings the video session in line with the activity's generation and the
 * switch: the one place that opens and closes. */
static void apply(bool withdraw)
{
    if (!s_vision.available) return;
    const uint32_t wanted = s_vision.enabled ? s_vision.generation : 0U;
    if (wanted != 0U && s_vision.link == wanted) return;
    close_session(withdraw);
    if (wanted == 0U) {
        publish(XIAOTAI_AI_VISION_OFF, XIAOTAI_OK);
        return;
    }
    open_session(wanted);
}

static void switch_to(bool enabled)
{
    if (s_vision.enabled == enabled) return;
    s_vision.enabled = enabled;
    xiaotai_eventlog_note(VISION_SOURCE, "switch", 0, enabled ? 1 : 0);
    /* The generation the framework holds now, in case the switch is turned on
     * between two attachment callbacks. */
    if (s_vision.activity != 0U) s_vision.generation = xiaotai_activity_generation(s_vision.activity);
    apply(true);
}

/* -------------------------------------------------------------- callbacks */

static void on_begin(uint32_t id, uint32_t generation, void *ctx)
{
    (void)ctx;
    s_vision.activity = id;
    s_vision.generation = generation;
    s_vision.failed = 0;
    apply(true);
}

static void on_session(uint32_t id, uint32_t generation, void *ctx)
{
    (void)ctx;
    if (id == 0U || id != s_vision.activity) {
        ++s_vision.ignored;
        return;
    }
    s_vision.generation = generation;
    apply(true);
}

/* Runs before the host hears of a revocation, so the camera is let go before
 * the next activity starts. */
static void on_end(uint32_t id, uint32_t generation, void *ctx)
{
    (void)generation;
    (void)ctx;
    if (id == 0U || id != s_vision.activity) {
        ++s_vision.ignored;
        return;
    }
    s_vision.activity = 0;
    s_vision.generation = 0;
    apply(false);
    s_vision.failed = 0;
}

static void on_uplink(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    const xiaotai_video_event_t *uplink = event->data;
    if (!uplink || event->size != sizeof(*uplink) || uplink->generation == 0U ||
        uplink->generation != s_vision.link) {
        ++s_vision.ignored;
        return;
    }
    if (uplink->generation == s_vision.failed) return;   /* stays failed for this conversation */
    switch (uplink->phase) {
    case XIAOTAI_VIDEO_STARTING:
        publish(XIAOTAI_AI_VISION_STARTING, XIAOTAI_OK);
        break;
    case XIAOTAI_VIDEO_RUNNING:
        publish(XIAOTAI_AI_VISION_RUNNING, XIAOTAI_OK);
        break;
    case XIAOTAI_VIDEO_ERROR:
        /* The session stays open until the conversation lets go of it; the
         * engine holds nothing for a failed uplink and does not retry it. */
        fail(uplink->generation, uplink->error);
        break;
    case XIAOTAI_VIDEO_OFF:
    case XIAOTAI_VIDEO_STOPPING:
    default:
        break;
    }
}

static void on_setting(const char *name, void *ctx)
{
    (void)name;
    (void)ctx;
    int32_t value = 1;
    if (xiaotai_setting_get_int(VISION_SETTING, &value) == XIAOTAI_OK) switch_to(value != 0);
}

static void on_intent(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    const int32_t value = args ? args->value : 0;
    if (value != 0 && value != 1 && value != -1) {
        ++s_vision.ignored;
        return;
    }
    (void)xiaotai_ai_vision_set_enabled(value == -1 ? !s_vision.enabled : value == 1);
}

static void on_diag(const xiaotai_reply_t *out)
{
    char line[224];
    snprintf(line, sizeof(line),
             "enabled=%d state=%d link=%lu error=%d sessions=%lu failures=%lu user_switch=%d unavailable=%d "
             "ignored=%lu events_lost=%lu",
             s_vision.enabled ? 1 : 0, (int)s_vision.state, (unsigned long)s_vision.link, (int)s_vision.error,
             (unsigned long)s_vision.sessions, (unsigned long)s_vision.failures,
             s_vision.config.user_switch ? 1 : 0, (int)s_vision.unavailable, (unsigned long)s_vision.ignored,
             (unsigned long)s_vision.events_lost);
    out->write(out->ctx, line);
}

/* ------------------------------------------------------------- public API */

xiaotai_err_t xiaotai_ai_vision_configure(const xiaotai_ai_vision_config_t *config)
{
    if (s_vision.initialized) return XIAOTAI_ERR_INVALID_STATE;
    if (config) {
        s_vision.config = *config;
    } else {
        memset(&s_vision.config, 0, sizeof(s_vision.config));
    }
    return XIAOTAI_OK;
}

void xiaotai_ai_vision_status(xiaotai_ai_vision_status_t *status)
{
    if (!status) return;
    memset(status, 0, sizeof(*status));
    status->enabled = s_vision.enabled;
    status->state = s_vision.state;
    status->link = s_vision.link;
    status->error = s_vision.error;
    status->sessions = s_vision.sessions;
    status->failures = s_vision.failures;
}

xiaotai_err_t xiaotai_ai_vision_set_enabled(bool enabled)
{
    if (!s_vision.initialized || !xiaotai_on_owner()) return XIAOTAI_ERR_INVALID_STATE;
    if (s_vision.config.user_switch) {
        /* The setting's change handler applies it; an unchanged value calls
         * nothing. Refused while the system is quiesced (nothing is written
         * then), and the switch keeps its value. */
        const xiaotai_err_t err = xiaotai_setting_set_int(VISION_SETTING, enabled ? 1 : 0);
        if (err != XIAOTAI_OK) return err;
    } else {
        switch_to(enabled);
    }
    return s_vision.available ? XIAOTAI_OK : XIAOTAI_ERR_NOT_SUPPORTED;
}

/* ------------------------------------------------------------- lifecycle */

/* XIAOTAI_OK when a still-picture uplink can be opened: the engine has a
 * camera with a JPEG encoder behind it, the camera can produce the picture
 * size, and the engine took the profile. */
static xiaotai_err_t declare_uplink(void)
{
    xiaotai_video_capabilities_t capabilities;
    memset(&capabilities, 0, sizeof(capabilities));
    xiaotai_video_capabilities(&capabilities);
    if ((capabilities.uplink_codecs & XIAOTAI_VIDEO_CODEC_BIT(XIAOTAI_VIDEO_CODEC_JPEG)) == 0U) {
        return XIAOTAI_ERR_NOT_SUPPORTED;
    }
    uint16_t width = 0;
    uint16_t height = 0;
    const xiaotai_err_t err = xiaotai_video_resolve(&STILL, 0, &width, &height);
    if (err != XIAOTAI_OK) return err;
    return xiaotai_video_require_uplink(&STILL);
}

xiaotai_err_t xiaotai_module_ai_vision_init(void)
{
    const xiaotai_ai_vision_config_t config = s_vision.config;
    memset(&s_vision, 0, sizeof(s_vision));
    s_vision.config = config;
    s_vision.initialized = true;
    s_vision.enabled = true;

    s_vision.unavailable = declare_uplink();
    s_vision.available = s_vision.unavailable == XIAOTAI_OK;
    s_vision.state = s_vision.available ? XIAOTAI_AI_VISION_OFF : XIAOTAI_AI_VISION_UNAVAILABLE;
    if (!s_vision.available) {
        const xiaotai_eventlog_event_t event = {
            .source = VISION_SOURCE, .label = "unavailable", .generation = 0, .value = s_vision.unavailable,
            .level = (uint8_t)XIAOTAI_EVENTLOG_INFO, .kind = (uint8_t)XIAOTAI_EVENTLOG_ERR,
        };
        xiaotai_eventlog_emit(&event);
        XIAOTAI_LOGI(TAG, "unavailable: %d", (int)s_vision.unavailable);
    }

    /* Attached also when unavailable (without the link part), so the switch
     * and the state behave the same on every device. */
    const xiaotai_attachment_t attachment = {
        .begin = on_begin,
        .end = on_end,
        .session = on_session,
        .ctx = NULL,
        .contributions = s_vision.available ? CONTRIBUTIONS : NULL,
    };
    xiaotai_err_t err = xiaotai_activity_attach(XIAOTAI_AI_TALK_ACTIVITY, &attachment);
    if (err == XIAOTAI_OK && s_vision.available) {
        err = xiaotai_event_subscribe(XIAOTAI_EVENT_VIDEO_UPLINK, on_uplink, NULL);
    }
    if (err == XIAOTAI_OK && s_vision.config.user_switch) {
        const xiaotai_setting_t setting = {
            .name = VISION_SETTING,
            .key = VISION_SETTING_KEY,
            .kind = XIAOTAI_SETTING_BOOL,
            .min = 0,
            .max = 1,
            .default_value = 1,
            .default_text = NULL,
            .changed = on_setting,
            .ctx = NULL,
        };
        err = xiaotai_setting_register(&setting);
        int32_t stored = 1;
        if (err == XIAOTAI_OK && xiaotai_setting_get_int(VISION_SETTING, &stored) == XIAOTAI_OK) {
            s_vision.enabled = stored != 0;
        }
    }
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle(XIAOTAI_INTENT_VISION_ENABLE, on_intent, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_diag_register(VISION_SOURCE, on_diag);
    return err;
}

void xiaotai_ai_vision_reset(void)
{
    memset(&s_vision, 0, sizeof(s_vision));
    s_vision.enabled = true;
}
