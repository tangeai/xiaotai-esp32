// SPDX-License-Identifier: MIT
#include "companion_internal.h"

#include <string.h>

#include "xiaotai/media.h"
#include "xiaotai/modules/ai_talk.h"
#include "xiaotai/modules/wake.h"
#include "xiaotai/platform.h"
#include "xiaotai/port/os.h"

#define ACK_US INT64_C(1000000)
#define VOICE_NOTICE_MS 3200U

static struct {
    int64_t expires;
    uint32_t seen, pending, activity;
    bool male;
} s_ack;

static void cancel_admission_ack(void)
{
    if (!s_ack.activity) return;
    s_ack.activity = 0;
    companion_feedback_cancel_wake_ack(s_ack.male);
}

static void activity_ended(uint32_t id, uint32_t generation, void *ctx)
{
    (void)generation;
    (void)ctx;
    if (s_ack.activity == id) cancel_admission_ack();
}

static void activity_activated(uint32_t id, uint32_t generation, void *ctx)
{
    if (generation) activity_ended(id, generation, ctx);
}

static void acknowledge(const xiaotai_ai_talk_status_t *talk)
{
    if (!s_ack.pending) return;
    const int64_t remaining = s_ack.expires - xiaotai_time_us();
    if (remaining < 1000 || talk->session != s_ack.pending || talk->phase != XIAOTAI_AI_TALK_CONNECTING ||
        companion.status.setup || xiaotai_quiesced()) {
        s_ack.pending = 0;
        return;
    }
    if (companion.config.wake_ack_on_admission) {
        const char *name = NULL;
        const uint32_t id = xiaotai_activity_current(&name);
        s_ack.pending = 0;
        if (id && name && !strcmp(name, XIAOTAI_AI_TALK_ACTIVITY)) {
            /* No media generation exists yet. The AI owner cancels this
             * ordinary clip synchronously if the activity ends or activates. */
            s_ack.activity = id;
            companion_feedback_wake_ack_at_admission((uint32_t)(remaining / 1000), s_ack.male);
        }
        return;
    }
    if (!talk->link) return;
    xiaotai_media_status_t media;
    xiaotai_media_status(&media);
    if (media.generation != talk->link) return;
    s_ack.pending = 0;
    companion_feedback_wake_ack(talk->link, (uint32_t)(remaining / 1000), s_ack.male);
}

static void changed(const xiaotai_event_t *event, void *unused)
{
    (void)unused;
    if (!s_ack.pending) return;
    if (!strcmp(event->name, XIAOTAI_EVENT_QUIESCE) && event->args.value) {
        s_ack.pending = 0;
        return;
    }
    xiaotai_ai_talk_status_t talk;
    xiaotai_ai_talk_status(&talk);
    if (!strcmp(event->name, XIAOTAI_EVENT_AI_TALK_MEDIA)) {
        if (!event->data || event->size != sizeof(talk)) return;
        const xiaotai_ai_talk_status_t *opened = event->data;
        if (opened->session != talk.session || opened->link != talk.link) return;
    }
    acknowledge(&talk);
}

static void focus_home(void)
{
    if (!companion_on_page(XIAOTAI_APP_COMPANION_PAGE_HOME)) (void)companion_reset_home(0);
}

static void detected(const xiaotai_event_t *event, void *unused)
{
    (void)unused;
    if (!event->data || event->size != sizeof(xiaotai_local_wake_detection_t) ||
        !companion.status.foreground || companion.status.setup || xiaotai_quiesced()) return;
    const xiaotai_local_wake_detection_t *detection = event->data;
    xiaotai_ai_talk_status_t talk;
    xiaotai_ai_talk_status(&talk);
    switch (detection->outcome) {
        case XIAOTAI_LOCAL_WAKE_STARTED: {
            if (!detection->session || talk.session != detection->session ||
                talk.phase == XIAOTAI_AI_TALK_IDLE || talk.phase == XIAOTAI_AI_TALK_ENDING ||
                s_ack.seen == talk.session) return;
            cancel_admission_ack();
            s_ack.seen = s_ack.pending = talk.session;
            s_ack.expires = xiaotai_time_us() + ACK_US;
            int32_t voice = 0;
            (void)xiaotai_setting_get_int(XIAOTAI_SETTING_APP_COMPANION_ACK_VOICE, &voice);
            s_ack.male = voice == 1;
            focus_home();
            companion_home_ack();
            acknowledge(&talk);
            break;
        }
        case XIAOTAI_LOCAL_WAKE_BUSY:
            if (talk.phase >= XIAOTAI_AI_TALK_LISTENING && talk.phase <= XIAOTAI_AI_TALK_SPEAKING) {
                focus_home();
                companion_home_session_notice("我在听，请直接说。", VOICE_NOTICE_MS);
            }
            break;
        case XIAOTAI_LOCAL_WAKE_REFUSED:
            if (detection->error == XIAOTAI_ERR_PLATFORM_OFFLINE && !companion_home_network_up()) return;
            focus_home();
            companion_home_notice("现在暂时不能开始对话", VOICE_NOTICE_MS);
            companion_user_activity();
            break;
        case XIAOTAI_LOCAL_WAKE_NO_PREROLL:
        case XIAOTAI_LOCAL_WAKE_ANNOUNCED:
            break;
    }
}

static void foreground(bool visible)
{
    if (!visible) {
        s_ack.pending = 0;
        cancel_admission_ack();
    }
}

static void reset(void) { memset(&s_ack, 0, sizeof(s_ack)); }

static xiaotai_err_t init(void)
{
    if (companion.config.wake_ack_on_admission) {
        static const xiaotai_attachment_t attachment = {
            .end = activity_ended, .session = activity_activated,
        };
        const xiaotai_err_t err = xiaotai_activity_attach(XIAOTAI_AI_TALK_ACTIVITY, &attachment);
        if (err != XIAOTAI_OK) return err;
    }
    xiaotai_err_t err = xiaotai_event_subscribe(XIAOTAI_EVENT_WAKE_DETECTED, detected, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_event_subscribe(XIAOTAI_EVENT_AI_TALK_MEDIA, changed, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_event_subscribe(XIAOTAI_EVENT_AI_TALK_STATE, changed, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_event_subscribe(XIAOTAI_EVENT_QUIESCE, changed, NULL);
    return err;
}

const xiaotai_app_companion_part_t xiaotai_app_companion_with_wake = {
    .name = "wake", .init = init, .foreground = foreground, .reset = reset,
};
