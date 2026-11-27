// SPDX-License-Identifier: MIT
#include "companion_internal.h"

#include <string.h>

#include "xiaotai/modules/stream.h"
#include "xiaotai/modules/stream_video.h"

static uint32_t warned_generation;
static const char video_format_notice[] = "companion.stream_format";

static bool stream_page(const char *id)
{
    return id && (!strcmp(id, XIAOTAI_APP_COMPANION_PAGE_HOME) || !strcmp(id, XIAOTAI_APP_COMPANION_PAGE_STREAM));
}

static const char *camera_text(xiaotai_video_phase_t phase)
{
    switch (phase) {
    case XIAOTAI_VIDEO_STARTING: return "摄像头启动中";
    case XIAOTAI_VIDEO_RUNNING: return "视频查看中";
    case XIAOTAI_VIDEO_ERROR: return "摄像头异常";
    default: return NULL;
    }
}

static void decorate(xiaotai_view_t *view)
{
    if (!stream_page(view->id)) return;
    xiaotai_stream_status_t stream;
    xiaotai_stream_video_status_t video;
    xiaotai_stream_status(&stream);
    if (!stream.generation) return;
    xiaotai_stream_video_status(&video);
    if (video.generation != stream.generation) return;
    if (video.remote_ready) view->flags |= XIAOTAI_VIEW_VIDEO;
    if (!companion.config.stream_camera_status || stream.phase != XIAOTAI_STREAM_ACTIVE) return;
    /* Preserve the home policy and the pending-stop/maintenance messages. */
    const char **label = !strcmp(view->id, XIAOTAI_APP_COMPANION_PAGE_HOME) ? &view->status : &view->text;
    const char *phase = camera_text(video.uplink);
    if (phase && *label && !strcmp(*label, "远程查看中")) *label = phase;
}

static void uplink(const xiaotai_event_t *event, void *unused)
{
    (void)unused;
    const xiaotai_video_event_t *update = event->data;
    if (!update || event->size != sizeof(*update) || !companion.status.foreground ||
        !stream_page(companion.status.page)) return;
    xiaotai_stream_status_t stream;
    xiaotai_stream_video_status_t video;
    xiaotai_stream_status(&stream);
    xiaotai_stream_video_status(&video);
    if (stream.phase == XIAOTAI_STREAM_ACTIVE && update->generation &&
        stream.generation == update->generation && video.generation == update->generation &&
        video.uplink == update->phase) companion_render();
}

static void remote(const xiaotai_event_t *event, void *unused)
{
    (void)unused;
    if (!event->data || event->size != sizeof(uint32_t) || !companion.status.foreground ||
        !stream_page(companion.status.page)) return;
    const uint32_t generation = *(const uint32_t *)event->data;
    xiaotai_stream_status_t stream;
    xiaotai_stream_status(&stream);
    if (generation && (stream.generation == generation || (!stream.generation && !event->args.value))) companion_render();
}

static void format_feedback(const xiaotai_event_t *event, void *unused)
{
    (void)unused;
    const uint32_t generation = (uint32_t)event->args.value;
    if (!generation) return;
    xiaotai_stream_status_t stream;
    xiaotai_stream_video_status_t video;
    xiaotai_stream_status(&stream);
    xiaotai_stream_video_status(&video);
    const bool unsupported = !strcmp(event->name, XIAOTAI_EVENT_STREAM_VIDEO_UNSUPPORTED);
    if (stream.generation != generation || video.generation != generation ||
        video.remote_unsupported != unsupported) return;
    if (!unsupported) {
        if (warned_generation == generation) {
            warned_generation = 0;
            xiaotai_view_dismiss(XIAOTAI_LAYER_TOAST, video_format_notice);
        }
        return;
    }
    if (warned_generation == generation || !companion.status.foreground || !stream_page(companion.status.page)) return;
    warned_generation = generation;
    xiaotai_view_show(&(const xiaotai_view_t){
        .id = video_format_notice, .layer = XIAOTAI_LAYER_TOAST, .kind = XIAOTAI_VIEW_NOTICE,
        .text = "视频格式不支持", .duration_ms = companion_toast_ms(),
    });
}

static xiaotai_err_t init(void)
{
    if (!companion.has_display) return XIAOTAI_OK;
    xiaotai_err_t err = xiaotai_event_subscribe(XIAOTAI_EVENT_STREAM_VIDEO_REMOTE, remote, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_event_subscribe(XIAOTAI_EVENT_STREAM_VIDEO_UNSUPPORTED, format_feedback, NULL);
    if (err == XIAOTAI_OK)
        err = xiaotai_event_subscribe(XIAOTAI_EVENT_STREAM_VIDEO_FORMAT_RECOVERED, format_feedback, NULL);
    if (err == XIAOTAI_OK && companion.config.stream_camera_status)
        err = xiaotai_event_subscribe(XIAOTAI_EVENT_STREAM_VIDEO_UPLINK, uplink, NULL);
    return err;
}

static void reset(void) { warned_generation = 0; }

const xiaotai_app_companion_part_t xiaotai_app_companion_with_stream_video = {
    .name = "stream_video", .init = init, .reset = reset, .decorate_view = decorate,
};
