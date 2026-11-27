// SPDX-License-Identifier: MIT
#include "video_call_internal.h"

#include <inttypes.h>

#include "xiaotai/at_command.h"

static void write_status(const xiaotai_reply_t *reply, const char *prefix)
{
    xiaotai_video_call_status_t s;
    xiaotai_video_call_status(&s);
    xiaotai_at_printf(reply, "%svideo=%u camera=%u available=%u gen=%" PRIu32 " sending=%u receiving=%u remote=%u "
                       "unsupported=%u error=%d sessions=%" PRIu32 " subscribe_retries=%" PRIu32 " stale_calls=%" PRIu32,
                       prefix, (unsigned)s.active, (unsigned)s.camera_enabled, (unsigned)s.available, s.link,
                       (unsigned)s.sending, (unsigned)s.receiving, (unsigned)s.remote, (unsigned)s.unsupported,
                       s.error, s.sessions, s.subscribe_retries, s.stale_calls);
}

static xiaotai_err_t show(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    write_status(reply, "+CALL-VIDEO: ");
    return XIAOTAI_AT_PASS;
}

static void diagnose(const xiaotai_reply_t *reply) { write_status(reply, ""); }

static xiaotai_err_t dial(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    uint32_t index;
    if (!xiaotai_at_parse_u32(argv[0], 0, UINT8_MAX, &index)) {
        return xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_ARG, "INDEX");
    }
    xiaotai_video_call_status_t status;
    xiaotai_video_call_status(&status);
    if (!status.available) return XIAOTAI_ERR_NOT_SUPPORTED;
    for (size_t i = 0; video_call_hosts[i]; ++i) {
        const video_call_host_t *host = video_call_hosts[i];
        if (!host->dial_contact) continue;
        xiaotai_err_t err = host->dial_contact(index);
        if (err != XIAOTAI_ERR_NOT_FOUND) return err;
    }
    return XIAOTAI_ERR_NOT_FOUND;
}

static xiaotai_err_t camera(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    bool enabled;
    if (!xiaotai_at_parse_flag(argv[0], &enabled)) return xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_ARG, "CAMERA");
    return xiaotai_video_call_set_camera(0, enabled);
}

xiaotai_err_t video_call_at_init(void)
{
    static const xiaotai_at_verb_t verbs[] = {
        {.run = show},
        {.word = "VIDEO", .min_args = 1, .max_args = 1, .usage = "<n>", .run = dial},
        {.word = "CAMERA", .min_args = 1, .max_args = 1, .usage = "0|1", .run = camera},
    };
    const xiaotai_err_t err = xiaotai_at_verbs_register("AT+CALL", verbs, sizeof(verbs) / sizeof(verbs[0]));
    return err == XIAOTAI_OK ? xiaotai_diag_register("video_call", diagnose) : err;
}
