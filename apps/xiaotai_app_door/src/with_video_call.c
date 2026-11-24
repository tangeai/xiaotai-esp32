// SPDX-License-Identifier: MIT
#include "door_internal.h"
#include "xiaotai/modules/video_call.h"

#include <string.h>

static const xiaotai_video_call_status_t *status_for_call(const xiaotai_event_t *event)
{
    if (strcmp(event->name, XIAOTAI_EVENT_VIDEO_CALL_STATE) &&
        strcmp(event->name, XIAOTAI_EVENT_VIDEO_CALL_REMOTE) &&
        strcmp(event->name, XIAOTAI_EVENT_VIDEO_CALL_UNSUPPORTED)) return NULL;
    const xiaotai_video_call_status_t *video = event->size == sizeof(*video) ? event->data : NULL;
    if (!video || !video->link) return NULL;
    xiaotai_call_status_t current;
    xiaotai_call_status(&current);
    return (door_call_matches(&current) && current.link == video->link) || door_station_video_link(video->link) ?
           video : NULL;
}

static bool claim(const xiaotai_event_t *event) { return status_for_call(event) != NULL; }

static void on_video(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    const xiaotai_video_call_status_t *video = status_for_call(event);
    if (!video) return;
    if (!strcmp(event->name, XIAOTAI_EVENT_VIDEO_CALL_UNSUPPORTED)) {
        xiaotai_video_call_status_t current;
        xiaotai_video_call_status(&current);
        const int32_t value = event->args.value;
        if (!current.active || current.link != video->link || (value != 0 && value != 1) ||
            video->unsupported != (value == 1) || video->unsupported != current.unsupported) return;
        if (value == 1) door_format_show(video->link, &door.call_format_noted, DOOR_CALL_FORMAT_TOAST);
        else door_format_clear(video->link, &door.call_format_noted, DOOR_CALL_FORMAT_TOAST);
    } else if (!strcmp(event->name, XIAOTAI_EVENT_VIDEO_CALL_REMOTE)) {
        door.call_picture_generation = video->link;
        door.call_picture = event->args.value == 1;
        door_render();
    }
}

static xiaotai_err_t init(void)
{
    xiaotai_err_t err = xiaotai_event_subscribe(XIAOTAI_EVENT_VIDEO_CALL_STATE, on_video, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_event_subscribe(XIAOTAI_EVENT_VIDEO_CALL_REMOTE, on_video, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_event_subscribe(XIAOTAI_EVENT_VIDEO_CALL_UNSUPPORTED, on_video, NULL);
    return err;
}

const door_feature_t door_with_video_call = {.init = init, .claim = claim};
