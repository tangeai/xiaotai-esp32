// SPDX-License-Identifier: MIT
#include "wechat_internal.h"

#include <inttypes.h>
#include "xiaotai/at_command.h"
#include "xiaotai/modules/contacts.h"

static const char *state_name(const xiaotai_wechat_status_t *status)
{
    static const char *const names[] = {"idle", "outgoing", "incoming", "connecting", "active"};
    if (status->state != XIAOTAI_WECHAT_IDLE)
        return (unsigned)status->state < sizeof(names) / sizeof(*names) ? names[status->state] : "error";
    switch (status->end) {
    case XIAOTAI_WECHAT_END_NONE: return "idle";
    case XIAOTAI_WECHAT_END_NO_ANSWER:
    case XIAOTAI_WECHAT_END_MISSED:
    case XIAOTAI_WECHAT_END_CONTACT:
    case XIAOTAI_WECHAT_END_FAILED:
    case XIAOTAI_WECHAT_END_NETWORK:
    case XIAOTAI_WECHAT_END_NO_MEMORY:
    case XIAOTAI_WECHAT_END_AUDIO:
        return "error";
    case XIAOTAI_WECHAT_END_PEER_HANGUP:
        if (status->peer_reason == 6 || status->peer_reason == 8) return "error";
        break;
    default: break;
    }
    return "ended";
}

static void show(const xiaotai_wechat_status_t *status, const xiaotai_reply_t *out)
{
    xiaotai_at_printf(out, "+CALL: kind=wechat state=%s gen=%" PRIu32 " muted=%u end=%u peer=- error=%d",
                     state_name(status), status->session, (unsigned)status->muted,
                     (unsigned)status->end, (int)status->error);
}

static xiaotai_err_t at_show(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)argc;
    (void)argv;
    xiaotai_wechat_status_t status;
    xiaotai_wechat_status(&status);
    show(&status, out);
    return XIAOTAI_OK;
}

static xiaotai_err_t at_dial(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)argc;
    (void)out;
    return xiaotai_wechat_dial(argv[0], 0, NULL);
}

static xiaotai_err_t at_index(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)argc;
    (void)out;
    uint32_t index;
    if (!xiaotai_at_parse_u32(argv[0], 0, UINT8_MAX, &index)) return XIAOTAI_ERR_INVALID_ARG;
    xiaotai_contact_t contact;
    const xiaotai_err_t err = xiaotai_contacts_at((uint8_t)index, XIAOTAI_CONTACTS_STALE_OK, &contact);
    if (err != XIAOTAI_OK) return err == XIAOTAI_ERR_NOT_FOUND ? XIAOTAI_ERR_INVALID_ARG : err;
    if (contact.kind != wx_contact_kind()) return XIAOTAI_AT_PASS;
    return xiaotai_wechat_dial_contact(index, 0, NULL);
}

#define CONTROL(name) \
    static xiaotai_err_t at_##name(int argc, const char *const *argv, const xiaotai_reply_t *out) \
    { \
        (void)argc; (void)argv; (void)out; \
        xiaotai_wechat_status_t status; \
        xiaotai_wechat_status(&status); \
        return status.session ? xiaotai_wechat_##name(status.session) : XIAOTAI_AT_PASS; \
    }
CONTROL(accept)
CONTROL(reject)
#undef CONTROL

static xiaotai_err_t at_hangup(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)argc;
    (void)argv;
    (void)out;
    xiaotai_wechat_status_t status;
    xiaotai_wechat_status(&status);
    if (!status.session) return XIAOTAI_AT_PASS;
    if (status.state == XIAOTAI_WECHAT_INCOMING && wx_config()->at_reject_incoming_hangup)
        return XIAOTAI_ERR_INVALID_STATE;
    return xiaotai_wechat_hangup(status.session);
}

static xiaotai_err_t at_mute(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)argc;
    (void)out;
    bool muted;
    if (!xiaotai_at_parse_flag(argv[0], &muted)) return XIAOTAI_ERR_INVALID_ARG;
    xiaotai_wechat_status_t status;
    xiaotai_wechat_status(&status);
    return status.session ? xiaotai_wechat_set_muted(status.session, muted) : XIAOTAI_AT_PASS;
}

static void diagnose(const xiaotai_reply_t *out)
{
    xiaotai_wechat_status_t status;
    xiaotai_wechat_status(&status);
    uint32_t extra, lost, invalid;
    wx_diagnostics(&extra, &lost, &invalid);
    show(&status, out);
    xiaotai_at_printf(out, "+WECHAT: dialled=%" PRIu32 " announced=%" PRIu32 " answered=%" PRIu32
                     " refused=%" PRIu32 " refusals_failed=%" PRIu32 " reject_invalid=%" PRIu32
                     " early_rooms=%" PRIu32 " stale_calls=%" PRIu32 " video_extra=%" PRIu32
                     " video_fallbacks=%" PRIu32 " events_lost=%" PRIu32,
                     status.dialled, status.announced, status.answered, status.refused, status.refusals_failed,
                     invalid, status.early_rooms, status.stale_calls, extra, wx_profile_video_fallbacks(), lost);
}

xiaotai_err_t wx_at_init(void)
{
    static const xiaotai_at_verb_t verbs[] = {
        {.run = at_show},
        {.word = "WX", .min_args = 1, .max_args = 1, .run = at_dial, .usage = "<contact-id>"},
        {.word = "INDEX", .min_args = 1, .max_args = 1, .run = at_index, .usage = "<n>"},
        {.word = "ACCEPT", .run = at_accept},
        {.word = "REJECT", .run = at_reject},
        {.word = "HANGUP", .run = at_hangup},
        {.word = "MUTE", .min_args = 1, .max_args = 1, .run = at_mute, .usage = "0|1"},
    };
    const xiaotai_err_t err = xiaotai_at_verbs_register("AT+CALL", verbs, sizeof(verbs) / sizeof(*verbs));
    return err == XIAOTAI_OK ? xiaotai_diag_register("wechat", diagnose) : err;
}
