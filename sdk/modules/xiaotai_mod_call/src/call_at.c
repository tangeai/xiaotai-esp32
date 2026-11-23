// SPDX-License-Identifier: MIT
#include "call_internal.h"

#include "xiaotai/at_command.h"
#include "xiaotai/framework.h"

static const char *state_name(const xiaotai_call_status_t *s)
{
    switch (s->state) {
    case XIAOTAI_CALL_OUTGOING: return "outgoing";
    case XIAOTAI_CALL_INCOMING: return "incoming";
    case XIAOTAI_CALL_CONNECTING: return "connecting";
    case XIAOTAI_CALL_ACTIVE: return "active";
    case XIAOTAI_CALL_IDLE:
        switch (s->end) {
        case XIAOTAI_CALL_END_NONE: return "idle";
        case XIAOTAI_CALL_END_BUSY:
        case XIAOTAI_CALL_END_FAILED:
        case XIAOTAI_CALL_END_NETWORK:
        case XIAOTAI_CALL_END_NO_MEMORY:
        case XIAOTAI_CALL_END_AUDIO: return "error";
        default: return "ended";
        }
    }
    return "error";
}

static void write_status(const xiaotai_reply_t *out, const xiaotai_call_status_t *s)
{
    const char *peer = xiaotai_at_is_id(s->peer_id, XIAOTAI_CALL_PEER_ID_MAX - 1U) ? s->peer_id : "-";
    xiaotai_at_printf(out, "+CALL: kind=device state=%s gen=%lu muted=%u end=%u peer=%s error=%d",
                      state_name(s), (unsigned long)s->session, (unsigned)s->muted,
                      (unsigned)s->end, peer, (int)s->error);
}

static xiaotai_err_t status(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)argc;
    (void)argv;
    xiaotai_call_status_t s;
    xiaotai_call_status(&s);
    write_status(out, &s);
    return XIAOTAI_AT_PASS;
}

static void diagnose(const xiaotai_reply_t *out)
{
    xiaotai_call_status_t s;
    call_diagnostic_counters_t d;
    xiaotai_call_status(&s);
    call_diagnostic_counters(&d);
    write_status(out, &s);
    xiaotai_at_printf(out, "+CALL-DIAG: link=%lu flags=%lu releasing=%u dialled=%lu announced=%lu answered=%lu "
                      "refused_busy=%lu refused_media=%lu stale_calls=%lu releases_pending=%u "
                      "releases_unconfirmed=%lu profile_fallbacks=%lu profile_parts_ignored=%lu "
                      "video_parts_ignored=%lu listen_failures=%lu",
                      (unsigned long)s.link, (unsigned long)s.flags, (unsigned)s.releasing,
                      (unsigned long)s.dialled, (unsigned long)s.announced, (unsigned long)s.answered,
                      (unsigned long)s.refused_busy, (unsigned long)s.refused_media, (unsigned long)s.stale_calls,
                      (unsigned)s.releases_pending, (unsigned long)s.releases_unconfirmed,
                      (unsigned long)d.profile_fallbacks, (unsigned long)d.profile_parts_ignored,
                      (unsigned long)d.video_parts_ignored, (unsigned long)d.listen_failures);
}

static xiaotai_err_t dial_device(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)argc;
    (void)out;
    if (!xiaotai_at_is_id(argv[0], XIAOTAI_CALL_PEER_ID_MAX - 1U)) return XIAOTAI_ERR_INVALID_ARG;
    return call_dial_device_at(argv[0]);
}

static xiaotai_err_t dial_index(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)argc;
    (void)out;
    uint32_t index;
    if (!xiaotai_at_parse_u32(argv[0], 0, UINT8_MAX, &index)) return XIAOTAI_ERR_INVALID_ARG;
    bool owned;
    const xiaotai_err_t err = call_index_owner((uint8_t)index, &owned);
    if (err == XIAOTAI_ERR_NOT_FOUND) return XIAOTAI_ERR_INVALID_ARG;
    if (err != XIAOTAI_OK) return err;
    return owned ? xiaotai_call_dial_contact(index, 0, NULL) : XIAOTAI_AT_PASS;
}

static uint32_t current_call(void)
{
    xiaotai_call_status_t s;
    xiaotai_call_status(&s);
    return s.session;
}

static xiaotai_err_t accept(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)argc;
    (void)argv;
    (void)out;
    const uint32_t session = current_call();
    return session ? xiaotai_call_accept(session) : XIAOTAI_AT_PASS;
}

static xiaotai_err_t reject(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)argc;
    (void)argv;
    (void)out;
    const uint32_t session = current_call();
    return session ? xiaotai_call_reject(session) : XIAOTAI_AT_PASS;
}

static xiaotai_err_t hangup(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)argc;
    (void)argv;
    (void)out;
    xiaotai_call_status_t status;
    xiaotai_call_status(&status);
    if (!status.session) return XIAOTAI_AT_PASS;
    if (status.state == XIAOTAI_CALL_INCOMING && call_config()->at_reject_incoming_hangup)
        return XIAOTAI_ERR_INVALID_STATE;
    return xiaotai_call_hangup(status.session);
}

static xiaotai_err_t mute(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)argc;
    (void)out;
    bool muted;
    if (!xiaotai_at_parse_flag(argv[0], &muted)) return XIAOTAI_ERR_INVALID_ARG;
    const uint32_t session = current_call();
    return session ? xiaotai_call_set_muted(session, muted) : XIAOTAI_AT_PASS;
}

xiaotai_err_t call_at_register(void)
{
    static const xiaotai_at_verb_t verbs[] = {
        {.run = status},
        {.word = "DEVICE", .min_args = 1, .max_args = 1, .run = dial_device, .usage = "<device-id>"},
        {.word = "INDEX", .min_args = 1, .max_args = 1, .run = dial_index, .usage = "<n>"},
        {.word = "ACCEPT", .run = accept}, {.word = "REJECT", .run = reject}, {.word = "HANGUP", .run = hangup},
        {.word = "MUTE", .min_args = 1, .max_args = 1, .run = mute, .usage = "0|1"},
    };
    xiaotai_err_t err = xiaotai_at_verbs_register("AT+CALL", verbs, sizeof(verbs) / sizeof(verbs[0]));
    if (err == XIAOTAI_OK) err = xiaotai_diag_register("call", diagnose);
    return err;
}
