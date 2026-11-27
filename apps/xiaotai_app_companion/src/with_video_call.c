// SPDX-License-Identifier: MIT
#include "companion_call_ui.h"

#include <string.h>

#include "xiaotai/modules/video_call.h"

static companion_call_camera_t read(uint32_t link)
{
    xiaotai_video_call_status_t status;
    xiaotai_video_call_status(&status);
    if (!link || link != status.link) return (companion_call_camera_t){0};
    return (companion_call_camera_t){.active = status.active, .camera = status.camera,
                                     .enabled = status.camera_enabled, .remote = status.remote};
}

static const companion_call_video_ops_t ops = {.read = read, .set = xiaotai_video_call_set_camera};

static void event(const xiaotai_event_t *message, void *unused)
{
    (void)unused;
    if (!message->data || message->size != sizeof(xiaotai_video_call_status_t)) return;
    const xiaotai_video_call_status_t *reported = message->data;
    xiaotai_video_call_status_t current;
    xiaotai_video_call_status(&current);
    if (reported->link != current.link) return;
    companion_video_format_t format = COMPANION_VIDEO_UNCHANGED;
    if (!strcmp(message->name, XIAOTAI_EVENT_VIDEO_CALL_UNSUPPORTED)) {
        const int32_t value = message->args.value;
        if (!current.active || !current.link || (value != 0 && value != 1) ||
            reported->unsupported != (value == 1) || current.unsupported != reported->unsupported) return;
        format = value == 1 ? COMPANION_VIDEO_UNSUPPORTED : COMPANION_VIDEO_RECOVERED;
    }
    companion_call_ui_video_event(current.link, format);
}

static xiaotai_err_t init(void)
{
    static const char *const events[] = {
        XIAOTAI_EVENT_VIDEO_CALL_STATE, XIAOTAI_EVENT_VIDEO_CALL_REMOTE, XIAOTAI_EVENT_VIDEO_CALL_UNSUPPORTED,
    };
    companion_call_ui_set_video(&ops);
    xiaotai_err_t err = XIAOTAI_OK;
    for (size_t i = 0; err == XIAOTAI_OK && companion.has_display && i < sizeof(events) / sizeof(events[0]); ++i)
        err = xiaotai_event_subscribe(events[i], event, NULL);
    return err;
}

static bool contact_video_available(const char *kind)
{
    /* The scenes of the call modules, by the kind of contact each one dials. */
    return xiaotai_video_call_offers(!strcmp(kind, "device") ? "call" : !strcmp(kind, "wechat") ? "voip" : NULL);
}

static void reset(void) { companion_call_ui_set_video(NULL); }

const xiaotai_app_companion_part_t xiaotai_app_companion_with_video_call = {
    .name = "video_call", .init = init, .reset = reset, .contact_video_available = contact_video_available,
};
