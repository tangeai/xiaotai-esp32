// SPDX-License-Identifier: MIT
#include "companion_internal.h"

#include <string.h>

#include "xiaotai/hal.h"
#include "xiaotai/media.h"
#include "xiaotai/modules/binding.h"
#include "xiaotai/modules/provisioning.h"
#include "xiaotai/modules/settings.h"
#include "xiaotai/platform.h"
#include "xiaotai/port/log.h"
#include "xiaotai/port/os.h"

static struct {
    companion_feedback_phase_t phase;
    uint32_t generation, announce_timer, automatic_revision;
    int64_t announce_due_us;
    const char *session_cue;
    char binding_code[XIAOTAI_BINDING_CODE_MAX + 1U];
    bool online, newly_bound, verifying, blocked, guidance;
    bool system_started, startup_done, startup_error;
    bool connected_hold, automatic, await_announcement, attempted, rejected;
} s;

static const xiaotai_feedback_effect_t *effect(const char *name)
{
    const xiaotai_feedback_table_t *table = companion.style->feedback;
    for (size_t i = 0; table && i < table->effect_count; ++i)
        if (!strcmp(table->effects[i].name, name)) return &table->effects[i];
    return NULL;
}

static xiaotai_err_t fire(const char *name, const xiaotai_feedback_options_t *options)
{
    const xiaotai_feedback_effect_t *row = effect(name);
    if (!row || (row->sound.group == XIAOTAI_APP_COMPANION_GROUP_GUIDANCE && !s.guidance))
        return XIAOTAI_OK;
    return xiaotai_feedback_fire(name, options);
}

static void state(const char *name, bool active)
{
    const xiaotai_feedback_table_t *table = companion.style->feedback;
    for (size_t i = 0; table && i < table->state_count; ++i) {
        if (!strcmp(table->states[i].name, name)) {
            (void)xiaotai_feedback_set(name, active);
            return;
        }
    }
}

static void update_states(void)
{
    const bool binding = companion_on_page(XIAOTAI_APP_COMPANION_PAGE_BINDING);
    const bool call_visible = !companion.has_display || companion_on_page(XIAOTAI_APP_COMPANION_PAGE_CALL);
    const bool own_session = companion.status.foreground && !companion.status.setup && !xiaotai_quiesced();
    state(XIAOTAI_APP_COMPANION_STATE_INCOMING, own_session && call_visible && s.phase == COMPANION_FEEDBACK_INCOMING);
    state(XIAOTAI_APP_COMPANION_STATE_OUTGOING, own_session && s.phase == COMPANION_FEEDBACK_DIALING);
    state(XIAOTAI_APP_COMPANION_STATE_CALL, own_session && s.phase == COMPANION_FEEDBACK_CALL);
    state(XIAOTAI_APP_COMPANION_STATE_TALK, own_session && s.phase == COMPANION_FEEDBACK_TALK);
    state(XIAOTAI_APP_COMPANION_STATE_BINDING, binding);
    state(XIAOTAI_APP_COMPANION_STATE_SETUP, companion_on_page(XIAOTAI_APP_COMPANION_PAGE_SETUP));
    state(XIAOTAI_APP_COMPANION_STATE_OFFLINE, !s.online && !binding && !companion.status.setup && !s.startup_error);
    state(XIAOTAI_APP_COMPANION_STATE_ONLINE, s.online);
    state(XIAOTAI_APP_COMPANION_STATE_ERROR, s.startup_error || (own_session && s.phase == COMPANION_FEEDBACK_FAILED));
}

static void cancel_session_cues(void)
{
    const char *cue = s.session_cue;
    s.session_cue = NULL;
    /* This reducer runs on the owner; cancelling does not enqueue work. */
    if (cue) (void)xiaotai_feedback_cancel(cue, false);
}

static bool session_cues_ready(void)
{
    return s.online && !xiaotai_restart_pending() && !xiaotai_quiesced() &&
           !companion_on_page(XIAOTAI_APP_COMPANION_PAGE_BINDING);
}

static void session_cue(const char *cue)
{
    if (cue && effect(cue) && fire(cue, NULL) == XIAOTAI_OK) s.session_cue = cue;
}

void companion_feedback_room_attempt(bool started, bool failed, uint32_t activity)
{
    if (s.phase != COMPANION_FEEDBACK_IDLE && s.phase != COMPANION_FEEDBACK_FAILED) return;
    /* Room has its own activity ID, not a call or AI media generation. */
    if (xiaotai_activity_current(NULL) != (started ? activity : 0U)) return;
    cancel_session_cues();
    if (!started && session_cues_ready())
        session_cue(failed ? XIAOTAI_APP_COMPANION_EFFECT_FAILED : XIAOTAI_APP_COMPANION_EFFECT_ENDED);
}

/* DIALING includes caller-side connecting before the peer answers;
 * CONNECTING is the silent continuation of an accepted incoming call. */
void companion_feedback_session(companion_feedback_phase_t phase, uint32_t generation)
{
    const companion_feedback_phase_t before = s.phase;
    const bool terminal = phase == COMPANION_FEEDBACK_IDLE || phase == COMPANION_FEEDBACK_FAILED;
    if (terminal && generation && s.generation && generation != s.generation) return;
    if (phase == before && (phase == COMPANION_FEEDBACK_IDLE || generation == s.generation)) return;

    const bool promoted = generation && !s.generation && phase == before;
    s.phase = phase;
    s.generation = phase == COMPANION_FEEDBACK_IDLE ? 0 : generation;
    update_states();
    if (promoted) return;
    cancel_session_cues();
    if (!session_cues_ready()) return;

    const char *cue = NULL;
    if (phase == COMPANION_FEEDBACK_INCOMING) cue = XIAOTAI_APP_COMPANION_EFFECT_INCOMING;
    else if (phase == COMPANION_FEEDBACK_DIALING) cue = XIAOTAI_APP_COMPANION_EFFECT_DIALING;
    else if (phase == COMPANION_FEEDBACK_FAILED) cue = XIAOTAI_APP_COMPANION_EFFECT_FAILED;
    else if (phase == COMPANION_FEEDBACK_IDLE && before != COMPANION_FEEDBACK_IDLE &&
             before != COMPANION_FEEDBACK_FAILED) cue = XIAOTAI_APP_COMPANION_EFFECT_ENDED;
    session_cue(cue);
}

static const char *wake_ack_effect(bool male)
{
    return male ? XIAOTAI_APP_COMPANION_EFFECT_ACK_MALE : XIAOTAI_APP_COMPANION_EFFECT_ACK_FEMALE;
}

static void wake_ack(uint32_t generation, uint32_t valid_ms, bool male)
{
    if (!valid_ms) return;
    const xiaotai_feedback_options_t options = {.generation = generation, .valid_ms = valid_ms};
    (void)fire(wake_ack_effect(male), &options);
}

void companion_feedback_wake_ack(uint32_t generation, uint32_t valid_ms, bool male)
{
    if (generation) wake_ack(generation, valid_ms, male);
}

void companion_feedback_wake_ack_at_admission(uint32_t valid_ms, bool male)
{
    wake_ack(0, valid_ms, male);
}

void companion_feedback_cancel_wake_ack(bool male)
{
    (void)xiaotai_feedback_cancel(wake_ack_effect(male), true);
}

static void release_connected(xiaotai_err_t result, void *ctx)
{
    (void)result;
    (void)ctx;
    if (!s.connected_hold) return;
    s.connected_hold = false;
    xiaotai_restart_release();
}

static void wifi_connected(void)
{
    const xiaotai_feedback_effect_t *row = effect(XIAOTAI_APP_COMPANION_EFFECT_CONNECTED);
    if (s.connected_hold || !row || (row->sound.group == XIAOTAI_APP_COMPANION_GROUP_GUIDANCE && !s.guidance)) return;
    const xiaotai_feedback_options_t options = {.done = release_connected};
    xiaotai_restart_hold();
    s.connected_hold = true;
    if (xiaotai_feedback_fire(XIAOTAI_APP_COMPANION_EFFECT_CONNECTED, &options) != XIAOTAI_OK)
        release_connected(XIAOTAI_ERR_INVALID_STATE, NULL);
}

static void cancel_announcement_timer(void)
{
    if (!s.announce_timer) return;
    xiaotai_timer_cancel(s.announce_timer);
    s.announce_timer = 0;
}

static void reset_announcement(void)
{
    cancel_announcement_timer();
    s.binding_code[0] = '\0';
    s.announce_due_us = 0;
    s.automatic_revision = 0;
    s.automatic = s.await_announcement = s.attempted = s.rejected = false;
}

static bool can_announce(void)
{
    return companion.config.announce_on_code && s.binding_code[0] &&
           (!companion.config.announce_follows_guidance || s.guidance) &&
           !xiaotai_quiesced() && companion_on_page(XIAOTAI_APP_COMPANION_PAGE_BINDING);
}

static int64_t after_ms(uint32_t ms)
{
    const int64_t now = xiaotai_time_us();
    const int64_t delay_us = (int64_t)ms * 1000;
    return now > INT64_MAX - delay_us ? INT64_MAX : now + delay_us;
}

static void announcement_tick(void *ctx);

static void schedule_announcement(uint32_t delay_ms)
{
    if (s.announce_timer) return;
    if (xiaotai_timer_start(delay_ms, announcement_tick, NULL, &s.announce_timer) != XIAOTAI_OK)
        XIAOTAI_LOGW("companion", "binding announcement timer unavailable");
}

static void maybe_announce(void)
{
    if (!can_announce()) {
        cancel_announcement_timer();
        return;
    }
    const uint32_t interval = (uint32_t)companion.config.announce_repeat_s * 1000U;
    if (s.rejected || s.await_announcement || (s.attempted && !interval)) return;
    const int64_t left_us = s.announce_due_us - xiaotai_time_us();
    if (left_us > 0) {
        schedule_announcement((uint32_t)((left_us + 999) / 1000));
        return;
    }
    if (xiaotai_feedback_sound_pending()) {
        schedule_announcement(XIAOTAI_APP_COMPANION_ANNOUNCE_WAIT_MS);
        return;
    }
    const xiaotai_err_t result = xiaotai_binding_announce();
    s.attempted = true;
    if (result == XIAOTAI_OK) {
        xiaotai_binding_status_t binding;
        xiaotai_binding_status(&binding);
        s.automatic = true;
        s.automatic_revision = binding.revision;
        if (interval && companion.config.announce_repeat_from_end) {
            s.await_announcement = true;
            return;
        }
    } else if (result != XIAOTAI_ERR_BUSY) {
        /* No other trigger may try this code after a permanent refusal. */
        s.rejected = true;
        return;
    }
    if (interval) {
        s.announce_due_us = after_ms(interval);
        schedule_announcement(interval);
    }
}

static void announcement_tick(void *ctx)
{
    (void)ctx;
    s.announce_timer = 0;
    xiaotai_binding_status_t binding;
    xiaotai_binding_status(&binding);
    if (binding.phase != XIAOTAI_BINDING_WAITING || strcmp(binding.verification, s.binding_code)) {
        reset_announcement();
        return;
    }
    maybe_announce();
}

void companion_feedback_visible(void)
{
    if (!companion.initialized || !companion.style) return;
    update_states();
    if (companion.status.setup || !companion.status.foreground) cancel_session_cues();
    if (!s.announce_timer) maybe_announce();
    else if (!can_announce()) cancel_announcement_timer();
}

static void binding_event(const xiaotai_event_t *event)
{
    if (!strcmp(event->name, XIAOTAI_EVENT_BINDING_CHANGED) && event->args.value != XIAOTAI_BINDING_WAITING) {
        reset_announcement();
        return;
    }
    xiaotai_binding_status_t binding;
    xiaotai_binding_status(&binding);
    if (binding.phase != XIAOTAI_BINDING_WAITING || !binding.verification[0]) {
        reset_announcement();
        return;
    }
    if (!strcmp(event->name, XIAOTAI_EVENT_BINDING_CODE)) {
        if (strcmp(event->args.text, binding.verification)) return;
        if (strcmp(binding.verification, s.binding_code)) {
            reset_announcement();
            strcpy(s.binding_code, binding.verification);
        }
    } else if (!strcmp(event->name, XIAOTAI_EVENT_BINDING_ANNOUNCED) && s.automatic &&
               (!binding.announcing || binding.revision != s.automatic_revision)) {
        s.automatic = false;
        if (s.await_announcement) {
            s.await_announcement = false;
            s.announce_due_us = after_ms((uint32_t)companion.config.announce_repeat_s * 1000U);
        }
    }
    companion_feedback_visible();
}

static void apply_guidance(bool enabled)
{
    s.guidance = enabled;
    (void)xiaotai_feedback_group_enable(XIAOTAI_APP_COMPANION_GROUP_GUIDANCE, enabled);
    if (!enabled && companion.config.announce_follows_guidance) {
        cancel_announcement_timer();
        if (s.automatic) {
            xiaotai_binding_status_t binding;
            xiaotai_binding_status(&binding);
            /* A manual replay may have started while the previous end event
             * waits in the owner queue. Do not stop that newer request. */
            if (binding.phase == XIAOTAI_BINDING_WAITING && binding.announcing &&
                binding.revision == s.automatic_revision &&
                !strcmp(binding.verification, s.binding_code)) xiaotai_binding_announce_stop();
        }
    }
    companion_feedback_visible();
}

bool companion_feedback_audio_error(void) { return s.startup_error; }

static void settle_startup(void)
{
    if (!s.system_started || s.startup_done) return;
    if (xiaotai_hal_audio_output(0)) {
        xiaotai_media_status_t media;
        xiaotai_media_status(&media);
        if (!media.startup_complete) return;
        s.startup_error = media.startup_error != XIAOTAI_OK;
    }
    s.startup_done = true;
    if (!s.startup_error && fire(XIAOTAI_APP_COMPANION_EFFECT_STARTUP, NULL) != XIAOTAI_OK)
        s.startup_error = true;
    if (s.startup_error) companion_setup_refresh();
    companion_feedback_visible();
}

static void started(void)
{
    if (s.system_started) return;
    int32_t guidance = 1;
    (void)xiaotai_setting_get_int(XIAOTAI_SETTING_GUIDANCE_ENABLED, &guidance);
    apply_guidance(guidance != 0);
    s.system_started = true;
    settle_startup();
}

static void reduce(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    const char *name = event->name;
    if (!strcmp(name, XIAOTAI_EVENT_STARTED)) started();
    else if (!strcmp(name, XIAOTAI_EVENT_MEDIA_STARTUP)) settle_startup();
    else if (!strcmp(name, XIAOTAI_EVENT_SETTINGS_CHANGED)) {
        if (!strcmp(event->args.text, XIAOTAI_SETTING_GUIDANCE_ENABLED)) apply_guidance(event->args.value != 0);
    } else if (!strcmp(name, XIAOTAI_EVENT_PROVISIONING_OPENED)) {
        (void)fire(XIAOTAI_APP_COMPANION_EFFECT_SETUP, NULL);
    } else if (!strcmp(name, XIAOTAI_EVENT_PROVISIONING_CHANGED)) {
        const bool verifying = event->args.value == XIAOTAI_PROVISIONING_VERIFYING;
        const xiaotai_provisioning_status_t *status =
            event->size == sizeof(xiaotai_provisioning_status_t) ? event->data : NULL;
        if (verifying && !s.verifying && !xiaotai_restart_pending() && !(status && status->restart_pending))
            (void)fire(XIAOTAI_APP_COMPANION_EFFECT_CONNECTING, NULL);
        s.verifying = verifying;
    } else if (!strcmp(name, XIAOTAI_EVENT_PROVISIONING_DONE)) {
        wifi_connected();
    } else if (!strcmp(name, XIAOTAI_EVENT_PROVISIONING_FAILED)) {
        (void)fire(XIAOTAI_APP_COMPANION_EFFECT_SETUP_FAILED, NULL);
    } else if (!strcmp(name, XIAOTAI_EVENT_BINDING_BOUND)) {
        if (event->args.value == 1) s.newly_bound = true;
        binding_event(event);
    } else if (!strcmp(name, XIAOTAI_EVENT_PLATFORM_ONLINE)) {
        s.online = true;
        (void)fire(s.newly_bound ? XIAOTAI_APP_COMPANION_EFFECT_BOUND : XIAOTAI_APP_COMPANION_EFFECT_ONLINE, NULL);
        s.newly_bound = false;
        update_states();
    } else if (!strcmp(name, XIAOTAI_EVENT_PLATFORM_OFFLINE)) {
        s.online = false;
        cancel_session_cues();
        (void)fire(XIAOTAI_APP_COMPANION_EFFECT_OFFLINE, NULL);
        update_states();
    } else if (!strcmp(name, XIAOTAI_EVENT_NET_STATE)) {
        const bool blocked = event->args.value == XIAOTAI_NET_BLOCKED;
        if (blocked && !s.blocked) {
            cancel_session_cues();
            (void)fire(XIAOTAI_APP_COMPANION_EFFECT_FAILED, NULL);
        }
        s.blocked = blocked;
    } else if (!strcmp(name, XIAOTAI_EVENT_PLATFORM_PAUSED)) {
        cancel_session_cues();
        (void)fire(XIAOTAI_APP_COMPANION_EFFECT_FAILED, NULL);
    } else if (!strcmp(name, XIAOTAI_EVENT_QUIESCE)) {
        companion_feedback_visible();
    } else {
        binding_event(event);
    }
}

xiaotai_err_t companion_feedback_init(void)
{
    static const xiaotai_subscriber_descriptor_t events[] = {
        {XIAOTAI_EVENT_STARTED, reduce, NULL},
        {XIAOTAI_EVENT_MEDIA_STARTUP, reduce, NULL},
        {XIAOTAI_EVENT_SETTINGS_CHANGED, reduce, NULL},
        {XIAOTAI_EVENT_PROVISIONING_OPENED, reduce, NULL},
        {XIAOTAI_EVENT_PROVISIONING_CHANGED, reduce, NULL},
        {XIAOTAI_EVENT_PROVISIONING_DONE, reduce, NULL},
        {XIAOTAI_EVENT_PROVISIONING_FAILED, reduce, NULL},
        {XIAOTAI_EVENT_BINDING_BOUND, reduce, NULL},
        {XIAOTAI_EVENT_PLATFORM_ONLINE, reduce, NULL},
        {XIAOTAI_EVENT_PLATFORM_OFFLINE, reduce, NULL},
        {XIAOTAI_EVENT_NET_STATE, reduce, NULL},
        {XIAOTAI_EVENT_PLATFORM_PAUSED, reduce, NULL},
        {XIAOTAI_EVENT_QUIESCE, reduce, NULL},
        {XIAOTAI_EVENT_BINDING_CHANGED, reduce, NULL},
        {XIAOTAI_EVENT_BINDING_CODE, reduce, NULL},
        {XIAOTAI_EVENT_BINDING_ANNOUNCED, reduce, NULL},
    };
    s.guidance = true;
    for (unsigned i = 0; i < sizeof(events) / sizeof(events[0]); ++i) {
        const xiaotai_err_t err = xiaotai_event_subscribe_static(&events[i]);
        if (err != XIAOTAI_OK) return err;
    }
    return XIAOTAI_OK;
}

void companion_feedback_reset(void)
{
    cancel_announcement_timer();
    release_connected(XIAOTAI_ERR_INVALID_STATE, NULL);
    memset(&s, 0, sizeof(s));
}
