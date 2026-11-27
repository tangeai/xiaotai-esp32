// SPDX-License-Identifier: MIT
#include "stream_internal.h"

#include <stdio.h>

#include "xiaotai/framework.h"

static const char *phase_name(xiaotai_stream_phase_t phase)
{
    static const char *const names[] = {"closed", "listening", "starting", "active"};
    return names[phase];
}

static const char *end_name(xiaotai_stream_end_t end)
{
    static const char *const names[] = {"peer", "local", "revoked", "busy", "closed", "link", "media", "binding"};
    return names[end];
}

static void closed_names(uint32_t reasons, char *out, size_t capacity)
{
    static const struct { uint32_t bit; const char *name; } closed[] = {
        {XIAOTAI_STREAM_CLOSED_DISABLED, "disabled"}, {XIAOTAI_STREAM_CLOSED_HELD, "held"},
        {XIAOTAI_STREAM_CLOSED_QUIESCED, "quiesced"}, {XIAOTAI_STREAM_CLOSED_OFFLINE, "offline"},
        {XIAOTAI_STREAM_CLOSED_PROFILE, "profile"}, {XIAOTAI_STREAM_CLOSED_MEMORY, "memory"},
        {XIAOTAI_STREAM_CLOSED_PLATFORM, "platform"}, {XIAOTAI_STREAM_CLOSED_CONTACTS, "contacts"},
    };
    size_t n = 0;
    for (size_t i = 0; i < sizeof(closed) / sizeof(closed[0]); ++i) {
        if (!(reasons & closed[i].bit)) continue;
        const int added = snprintf(out + n, capacity - n, "%s%s", n ? "|" : "", closed[i].name);
        if (added < 0 || (size_t)added >= capacity - n) return;
        n += (size_t)added;
    }
    if (!n) (void)snprintf(out, capacity, "-");
}

static void diagnose(const xiaotai_reply_t *out)
{
    xiaotai_stream_status_t status;
    xiaotai_stream_stats_t stats;
    xiaotai_stream_status(&status);
    xiaotai_stream_stats(&stats);
    char reasons[64];
    closed_names(status.closed, reasons, sizeof(reasons));
    char line[456];
    (void)snprintf(line, sizeof(line),
                   "phase=%s closed=%s gen=%lu peer_audio=%u peer_video=%u video_declared=%u holds=%u last_end=%s error=%d",
                   phase_name(status.phase), reasons, (unsigned long)status.generation, (unsigned)status.peer_audio,
                   (unsigned)status.peer_video, (unsigned)status.video_declared, (unsigned)status.holds,
                   end_name(status.last_end), status.last_error);
    out->write(out->ctx, line);
    (void)snprintf(line, sizeof(line),
                   "+STREAM-STATS: admitted=%lu started=%lu ended_peer=%lu ended_local=%lu ended_revoked=%lu "
                   "ended_link=%lu ended_media=%lu ended_binding=%lu refused_busy=%lu refused_closed=%lu "
                   "preempted_others=%lu stale_stops=%lu listen_failures=%lu profile_overflows=%lu extensions_ignored=%lu",
                   (unsigned long)stats.admitted, (unsigned long)stats.started, (unsigned long)stats.ended_peer,
                   (unsigned long)stats.ended_local, (unsigned long)stats.ended_revoked,
                   (unsigned long)stats.ended_link, (unsigned long)stats.ended_media,
                   (unsigned long)stats.ended_binding, (unsigned long)stats.refused_busy,
                   (unsigned long)stats.refused_closed, (unsigned long)stats.preempted_others,
                   (unsigned long)stats.stale_stops, (unsigned long)stats.listen_failures,
                   (unsigned long)stats.profile_overflows, (unsigned long)stats.extensions_ignored);
    out->write(out->ctx, line);
}

static void setting_changed(const char *name, void *ctx)
{
    (void)name;
    (void)ctx;
    stream_admission_update();
}

static void stop_intent(const xiaotai_args_t *args, void *ctx)
{
    (void)args;
    (void)ctx;
    stream_stop_current();
}

xiaotai_err_t stream_controls_register(void)
{
    const xiaotai_setting_t setting = {
        .name = XIAOTAI_SETTING_STREAM_ENABLED, .key = "stream_on", .kind = XIAOTAI_SETTING_BOOL,
        .min = 0, .max = 1, .default_value = 1, .changed = setting_changed,
    };
    xiaotai_err_t err = xiaotai_setting_register(&setting);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle(XIAOTAI_INTENT_STREAM_STOP, stop_intent, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_diag_register("stream", diagnose);
    return err;
}
