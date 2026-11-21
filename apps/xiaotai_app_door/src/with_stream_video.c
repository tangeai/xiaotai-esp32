// SPDX-License-Identifier: MIT
#include "door_internal.h"
#include "xiaotai/modules/stream_video.h"

#include <string.h>

static uint32_t generation(const xiaotai_event_t *event)
{
    if (!strcmp(event->name, XIAOTAI_EVENT_STREAM_VIDEO_UNSUPPORTED)) return (uint32_t)event->args.value;
    if (!strcmp(event->name, XIAOTAI_EVENT_STREAM_VIDEO_REMOTE) && event->data && event->size == sizeof(uint32_t)) {
        uint32_t id;
        memcpy(&id, event->data, sizeof(id));
        return id;
    }
    return 0;
}

static bool claim(const xiaotai_event_t *event)
{
    return door.stream_generation && generation(event) == door.stream_generation;
}

static void on_video(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    if (!strcmp(event->name, XIAOTAI_EVENT_STREAM_VIDEO_REMOTE)) {
        if (!claim(event)) return;
        door.stream_picture = event->args.value == 1;
        door_render();
        return;
    }
    const uint32_t link = (uint32_t)event->args.value;
    if (!link || door.stream_generation != link) return;
    xiaotai_stream_video_status_t current;
    xiaotai_stream_video_status(&current);
    const bool unsupported = !strcmp(event->name, XIAOTAI_EVENT_STREAM_VIDEO_UNSUPPORTED);
    if (current.generation != link || current.remote_unsupported != unsupported) return;
    if (unsupported) door_format_show(link, &door.stream_format_noted, DOOR_STREAM_FORMAT_TOAST);
    else door_format_clear(link, &door.stream_format_noted, DOOR_STREAM_FORMAT_TOAST);
}

static xiaotai_err_t init(void)
{
    xiaotai_err_t err = xiaotai_event_subscribe(XIAOTAI_EVENT_STREAM_VIDEO_REMOTE, on_video, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_event_subscribe(XIAOTAI_EVENT_STREAM_VIDEO_UNSUPPORTED, on_video, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_event_subscribe(XIAOTAI_EVENT_STREAM_VIDEO_FORMAT_RECOVERED, on_video, NULL);
    return err;
}

const door_feature_t door_with_stream_video = {.init = init, .claim = claim};
