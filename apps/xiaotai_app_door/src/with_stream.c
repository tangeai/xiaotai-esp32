// SPDX-License-Identifier: MIT
#include "door_internal.h"
#include "xiaotai/modules/stream.h"
#include "xiaotai/port/os.h"
#include "xiaotai/ui.h"

#include <string.h>

#define STOP_PENDING_MS 1500U

static void clear_stop_pending(void)
{
    xiaotai_timer_cancel(door.stream_stop_timer);
    door.stream_stop_timer = 0;
    door.stream_stop_until_us = 0;
}

static void stop_pending_expired(void *unused)
{
    (void)unused;
    door.stream_stop_timer = 0;
    door.stream_stop_until_us = 0;
    door_render();
}

static bool end_matches(const xiaotai_event_t *event)
{
    const xiaotai_stream_ended_t *ended = event->data;
    return ended && event->size == sizeof(*ended) && ended->started && door.stream_generation &&
           ended->generation == door.stream_generation;
}

static bool claim(const xiaotai_event_t *event)
{
    /* A web stream is shown behind the door home screen where the door is the
     * product's home, or already in front. Elsewhere the application that is
     * the home keeps presenting streams (the door is one page of its pager). */
    if (!strcmp(event->name, XIAOTAI_EVENT_STREAM_STARTED)) {
        const char *home = xiaotai_app_home();
        return event->args.value != 0 && (door.foreground || !home || !strcmp(home, "door"));
    }
    if (!strcmp(event->name, XIAOTAI_EVENT_STREAM_ENDED)) return end_matches(event);
    // Idle admission changes and refused viewers have no presentation.
    return !strcmp(event->name, XIAOTAI_EVENT_STREAM_STATE) && door.stream_generation;
}

static void stream_event(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    if (!strcmp(event->name, XIAOTAI_EVENT_STREAM_STARTED) && event->args.value) {
        const uint32_t generation = (uint32_t)event->args.value;
        if (generation != door.stream_generation) {
            clear_stop_pending();
            door_format_clear(door.stream_generation, &door.stream_format_noted, DOOR_STREAM_FORMAT_TOAST);
            door.stream_picture = false;
            door.stream_format_noted = 0;
            door_overlay_close();
        }
        door.stream_generation = generation;
        door_page(DOOR_HOME);
    } else if (!strcmp(event->name, XIAOTAI_EVENT_STREAM_ENDED) && end_matches(event)) {
        clear_stop_pending();
        door_format_clear(door.stream_generation, &door.stream_format_noted, DOOR_STREAM_FORMAT_TOAST);
        door.stream_generation = door.stream_format_noted = 0;
        door.stream_picture = false;
        if (!door.session) door_finish_presentation();
        else door_render();
    }
}

static void stop_stream(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    const uint32_t generation = (uint32_t)args->value;
    if (!door.foreground || door.page != DOOR_HOME || door.overlay || door.setup ||
        !generation || generation != door.stream_generation || door.stream_stop_until_us > xiaotai_time_us()) return;
    xiaotai_ui_info_t display;
    if (xiaotai_ui_inspect(0, &display) == XIAOTAI_OK && display.overlay[0]) return;
    xiaotai_stream_status_t stream;
    xiaotai_stream_status(&stream);
    if (stream.phase != XIAOTAI_STREAM_ACTIVE || stream.generation != generation) return;
    const xiaotai_err_t err = xiaotai_stream_stop(generation);
    if (err != XIAOTAI_OK) {
        xiaotai_view_toast("操作未受理，请重试", 0);
        return;
    }
    clear_stop_pending();
    door.stream_stop_until_us = xiaotai_time_us() + STOP_PENDING_MS * INT64_C(1000);
    /* A full timer table must not leave an inert button with no repaint. */
    (void)xiaotai_timer_start(STOP_PENDING_MS, stop_pending_expired, NULL, &door.stream_stop_timer);
    door_render();
}

static xiaotai_err_t init(void)
{
    xiaotai_err_t err = xiaotai_intent_handle(XIAOTAI_INTENT_APP_DOOR_STREAM_STOP, stop_stream, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_event_subscribe(XIAOTAI_EVENT_STREAM_STARTED, stream_event, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_event_subscribe(XIAOTAI_EVENT_STREAM_ENDED, stream_event, NULL);
    return err;
}

const door_feature_t door_with_stream = {.init = init, .claim = claim};
