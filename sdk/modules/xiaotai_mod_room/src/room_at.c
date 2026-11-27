// SPDX-License-Identifier: MIT
#include "room_internal.h"

#include <inttypes.h>
#include <string.h>
#include "xiaotai/at_command.h"
#include "xiaotai/eventlog.h"

static void show(const xiaotai_room_status_t *s, const xiaotai_reply_t *out, const char *prefix)
{
    const char *code = room_digits(s->room_code, XIAOTAI_ROOM_CODE_DIGITS, false) ? s->room_code : "-";
    xiaotai_at_printf(out, "%sstate=%u code=%s assigned=%u known=%u open=%u busy=%u members=%u online=%u "
                     "transmitting=%u gen=%" PRIu32 " revision=%" PRIu32 " error=%d", prefix, (unsigned)s->phase,
                     code, (unsigned)s->assigned, (unsigned)s->assignment_known, (unsigned)s->attending,
                     (unsigned)s->busy, (unsigned)s->listed_count, (unsigned)s->member_count, (unsigned)s->transmitting,
                     s->generation, s->revision, (int)s->error);
}

static xiaotai_err_t at_status(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)argc;
    (void)argv;
    xiaotai_room_status_t s;
    xiaotai_room_status(&s);
    show(&s, out, "+ROOM: ");
    return XIAOTAI_OK;
}

static void diagnose(const xiaotai_reply_t *out)
{
    xiaotai_room_status_t s;
    xiaotai_room_stats_t n;
    xiaotai_room_status(&s);
    xiaotai_room_stats(&n);
    show(&s, out, "");
    xiaotai_at_printf(out, "+ROOM-SESSION: wait=%u assignment_version=%" PRIu32 " attempts=%u business_code=%" PRId32
                     " settled=%u members_ready=%u members_truncated=%u members_revision=%" PRIu32 " speaking=%u",
                     (unsigned)s.wait, s.assignment_version, (unsigned)s.attempts, s.code, (unsigned)xiaotai_room_settled(),
                     (unsigned)s.members_ready, (unsigned)s.members_truncated, s.members_revision, (unsigned)s.speaking_count);
    xiaotai_at_printf(out, "+ROOM-REQUESTS: syncs=%" PRIu32 " sync_failures=%" PRIu32 " assignments_stale=%" PRIu32
                     " tokens=%" PRIu32 " token_failures=%" PRIu32 " connects=%" PRIu32 " connect_failures=%" PRIu32
                     " joins=%" PRIu32 " join_failures=%" PRIu32 " heartbeats=%" PRIu32 " heartbeat_failures=%" PRIu32
                     " lease_expired=%" PRIu32 " releases=%" PRIu32 " release_failures=%" PRIu32 " requests_lost=%" PRIu32,
                     n.syncs, n.sync_failures, n.assignments_stale, n.tokens, n.token_failures, n.connects, n.connect_failures,
                     n.joins, n.join_failures, n.heartbeats, n.heartbeat_failures, n.lease_expired, n.releases,
                     n.release_failures, n.requests_lost);
    xiaotai_at_printf(out, "+ROOM-STATS: snapshots=%" PRIu32 " snapshots_rejected=%" PRIu32 " members_dropped=%" PRIu32
                     " signals_sent=%" PRIu32 " signals_failed=%" PRIu32 " commands_dropped=%" PRIu32
                     " preempted=%" PRIu32 " activity_busy=%" PRIu32 " talk_presses=%" PRIu32 " talk_refused=%" PRIu32
                     " talk_keepalive_releases=%" PRIu32 " stale=%" PRIu32,
                     n.snapshots, n.snapshots_rejected, n.members_dropped, n.signals_sent, n.signals_failed, n.commands_dropped,
                     n.preempted, n.activity_busy, n.talk_presses, n.talk_refused, n.talk_keepalive_releases, n.stale);
    const room_t *room = room_context();
    if (room && room->token_shape.valid) {
        xiaotai_at_printf(out, "+ROOM-TOKEN: peer_type=%u peer_bytes=%" PRIu32 " token_type=%u token_bytes=%" PRIu32,
                         (unsigned)room->token_shape.peer_type, room->token_shape.peer_bytes,
                         (unsigned)room->token_shape.token_type, room->token_shape.token_bytes);
    }
}

#define ACTION(name, action) \
    static xiaotai_err_t at_##name(int argc, const char *const *argv, const xiaotai_reply_t *out) \
    { \
        (void)argc; (void)argv; (void)out; \
        return action; \
    }
ACTION(open, xiaotai_room_attend(true))
ACTION(close, xiaotai_room_attend(false))
ACTION(refresh, xiaotai_room_refresh())
ACTION(leave, xiaotai_room_leave())
ACTION(connect, xiaotai_room_connect())
ACTION(disconnect, xiaotai_room_disconnect())
#undef ACTION

static xiaotai_err_t at_create(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)out;
    if (argc == 1 && !xiaotai_at_is_digits(argv[0], XIAOTAI_ROOM_PASSWORD_DIGITS)) return XIAOTAI_ERR_INVALID_ARG;
    return xiaotai_room_create(argc == 1 ? argv[0] : NULL);
}

static xiaotai_err_t at_join(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)out;
    if (!xiaotai_at_is_digits(argv[0], XIAOTAI_ROOM_CODE_DIGITS) ||
        (argc == 2 && !xiaotai_at_is_digits(argv[1], XIAOTAI_ROOM_PASSWORD_DIGITS))) return XIAOTAI_ERR_INVALID_ARG;
    return xiaotai_room_join(argv[0], argc == 2 ? argv[1] : NULL);
}

static xiaotai_err_t talk(bool pressed)
{
    if (!pressed) return xiaotai_room_talk(0, false);
    xiaotai_room_status_t s;
    xiaotai_room_status(&s);
    return xiaotai_room_talk(s.generation, true);
}

static xiaotai_err_t at_ptt(int argc, const char *const *argv, const xiaotai_reply_t *out)
{
    (void)argc;
    (void)out;
    bool pressed;
    if (!xiaotai_at_parse_flag(argv[0], &pressed)) return XIAOTAI_ERR_INVALID_ARG;
    return talk(pressed) == XIAOTAI_OK ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_STATE;
}

typedef enum {
    INTENT_ATTEND, INTENT_CREATE, INTENT_JOIN, INTENT_LEAVE,
    INTENT_REFRESH, INTENT_CONNECT, INTENT_DISCONNECT, INTENT_TALK,
} intent_kind_t;

typedef struct { const char *name; intent_kind_t kind; } intent_t;

static xiaotai_err_t join_intent(const char *text, size_t length)
{
    const size_t code_size = XIAOTAI_ROOM_CODE_DIGITS;
    if (length != code_size &&
        (length != code_size + 1U + XIAOTAI_ROOM_PASSWORD_DIGITS || text[code_size] != ' '))
        return XIAOTAI_ERR_INVALID_ARG;
    char code[XIAOTAI_ROOM_CODE_DIGITS + 1U];
    memcpy(code, text, code_size);
    code[code_size] = '\0';
    return xiaotai_room_join(code, length == code_size ? NULL : text + code_size + 1U);
}

static void intent(const xiaotai_args_t *args, void *ctx)
{
    const intent_t *entry = ctx;
    const char *end = memchr(args->text, '\0', sizeof(args->text));
    xiaotai_err_t err = XIAOTAI_ERR_INVALID_ARG;
    switch (entry->kind) {
    case INTENT_ATTEND:
        if (args->value == 0 || args->value == 1) err = xiaotai_room_attend(args->value != 0);
        break;
    case INTENT_CREATE:
        if (end) err = xiaotai_room_create(args->text);
        if (err == XIAOTAI_ERR_INVALID_ARG) room_result(XIAOTAI_ROOM_ACTION_CREATE, err, 0);
        break;
    case INTENT_JOIN:
        if (end) err = join_intent(args->text, (size_t)(end - args->text));
        if (err == XIAOTAI_ERR_INVALID_ARG) room_result(XIAOTAI_ROOM_ACTION_JOIN, err, 0);
        break;
    case INTENT_LEAVE: err = xiaotai_room_leave(); break;
    case INTENT_REFRESH: err = xiaotai_room_refresh(); break;
    case INTENT_CONNECT: err = xiaotai_room_connect(); break;
    case INTENT_DISCONNECT: err = xiaotai_room_disconnect(); break;
    case INTENT_TALK:
        if (args->value == 0 || args->value == 1) err = talk(args->value != 0);
        break;
    }
    if (err != XIAOTAI_OK) xiaotai_eventlog_error(ROOM_LOG_SOURCE, "intent.refused", 0, err);
}

xiaotai_err_t room_controls_init(void)
{
    static const xiaotai_at_verb_t verbs[] = {
        {.run = at_status},
        {.word = "OPEN", .run = at_open}, {.word = "CLOSE", .run = at_close},
        {.word = "REFRESH", .run = at_refresh}, {.word = "LEAVE", .run = at_leave},
        {.word = "CREATE", .max_args = 1, .run = at_create, .usage = "[<4-digit-password>]"},
        {.word = "JOIN", .min_args = 1, .max_args = 2, .run = at_join, .usage = "<6-digit-code> [<4-digit-password>]"},
        {.word = "CONNECT", .run = at_connect}, {.word = "DISCONNECT", .run = at_disconnect},
        {.word = "PTT", .min_args = 1, .max_args = 1, .run = at_ptt, .usage = "0|1", .console_arg = "0"},
    };
    static intent_t intents[] = {
        {XIAOTAI_INTENT_ROOM_ATTEND, INTENT_ATTEND}, {XIAOTAI_INTENT_ROOM_CREATE, INTENT_CREATE},
        {XIAOTAI_INTENT_ROOM_JOIN, INTENT_JOIN}, {XIAOTAI_INTENT_ROOM_LEAVE, INTENT_LEAVE},
        {XIAOTAI_INTENT_ROOM_REFRESH, INTENT_REFRESH}, {XIAOTAI_INTENT_ROOM_CONNECT, INTENT_CONNECT},
        {XIAOTAI_INTENT_ROOM_DISCONNECT, INTENT_DISCONNECT}, {XIAOTAI_INTENT_ROOM_TALK, INTENT_TALK},
    };
    xiaotai_err_t err = xiaotai_at_verbs_register("AT+ROOM", verbs, sizeof(verbs) / sizeof(*verbs));
    if (err == XIAOTAI_OK) err = xiaotai_diag_register(ROOM_LOG_SOURCE, diagnose);
    for (size_t i = 0; err == XIAOTAI_OK && i < sizeof(intents) / sizeof(*intents); ++i)
        err = xiaotai_intent_handle(intents[i].name, intent, intents + i);
    return err;
}
