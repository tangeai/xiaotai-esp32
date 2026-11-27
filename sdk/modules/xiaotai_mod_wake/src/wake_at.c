// SPDX-License-Identifier: MIT
#include "wake_internal.h"

#include <inttypes.h>
#include <string.h>
#include "xiaotai/at_command.h"

#define PHRASE_BYTES (XIAOTAI_COMMAND_LINE_MAX / 2U + 1U)

static bool prefix_matches(const char *text, const char *const *phrases)
{
    while (*text && strchr(" \t\r\n\v\f", *text)) ++text;
    for (size_t i = 0; phrases[i]; ++i) {
        if (phrases[i][0] && strncmp(text, phrases[i], strlen(phrases[i])) == 0) return true;
    }
    return false;
}

static const char *const *model_phrases(void)
{
    const xiaotai_wake_model_t *model = xiaotai_wake_model();
    return model && model->phrases && model->phrases[0] ? model->phrases : NULL;
}

static xiaotai_err_t at_test(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    const char *const *phrases = model_phrases();
    if (!phrases) return XIAOTAI_ERR_NOT_SUPPORTED;
    char text[PHRASE_BYTES];
    const xiaotai_err_t error = xiaotai_at_text_decode(argv[0], text, sizeof(text));
    if (error != XIAOTAI_OK) return error;
    if (!prefix_matches(text, phrases)) return XIAOTAI_ERR_NOT_FOUND;
    xiaotai_wake_status_t engine;
    xiaotai_wake_status(&engine);
    const xiaotai_wake_detection_t injection = {.threshold_milli = engine.threshold_milli};
    const xiaotai_local_wake_outcome_t outcome = local_wake_inject(&injection);
    xiaotai_at_printf(reply, "+WAKE-TEST: intent=wake outcome=%s", local_wake_outcome_name(outcome));
    return XIAOTAI_OK;
}

static xiaotai_err_t at_selftest(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    const char *const *phrases = model_phrases();
    if (!phrases) return XIAOTAI_ERR_NOT_SUPPORTED;
    static const char negative[] = "\xe4\xbb\x8a\xe5\xa4\xa9\xe5\xa4\xa9\xe6\xb0\x94"
                                   "\xe6\x80\x8e\xe4\xb9\x88\xe6\xa0\xb7";
    static const char continuation[] = "\xef\xbc\x8c\xe4\xbb\x8a\xe5\xa4\xa9\xe5\xa4\xa9"
                                       "\xe6\xb0\x94\xe6\x80\x8e\xe4\xb9\x88\xe6\xa0\xb7";
    unsigned passed = !prefix_matches(negative, phrases), total = 1;
    char text[PHRASE_BYTES] = {' '};
    for (size_t i = 0; phrases[i]; ++i) {
        total += 2U;
        if (prefix_matches(phrases[i], phrases)) ++passed;
        const size_t length = strlen(phrases[i]);
        if (length > sizeof(text) - 1U - sizeof(continuation)) continue;
        memcpy(text + 1U, phrases[i], length);
        memcpy(text + 1U + length, continuation, sizeof(continuation));
        if (prefix_matches(text, phrases)) ++passed;
    }
    xiaotai_at_printf(reply, "+WAKE-SELFTEST: passed=%u total=%u", passed, total);
    return passed == total ? XIAOTAI_OK : XIAOTAI_FAIL;
}

static void diagnose(const xiaotai_reply_t *reply)
{
    static const char *const gates[] = {"listening", "disabled", "activity", "quiesced", "unavailable"};
    xiaotai_local_wake_status_t state;
    xiaotai_local_wake_status(&state);
    const char *gate = (unsigned)state.gate < sizeof(gates) / sizeof(gates[0]) ? gates[state.gate] : "unknown";
    const xiaotai_local_wake_detection_t *last = &state.last;
    xiaotai_at_printf(reply, "+WAKE-MODULE: enabled=%d gate=%s gate_error=%d detections=%" PRIu32
        " started=%" PRIu32 " announced=%" PRIu32 " busy=%" PRIu32 " no_preroll=%" PRIu32
        " refused=%" PRIu32 " injected=%" PRIu32 " last=%s sequence=%" PRIu32
        " score=%u threshold=%u error=%d session=%" PRIu32,
        (int)state.enabled, gate, (int)local_wake_gate_error(), state.detections, state.started, state.announced,
        state.busy, state.no_preroll, state.refused, state.injected,
        state.detections || state.injected ? local_wake_outcome_name(last->outcome) : "none",
        last->sequence, (unsigned)last->score_milli, (unsigned)last->threshold_milli, (int)last->error, last->session);
}

xiaotai_err_t local_wake_at_register(void)
{
    static const xiaotai_at_verb_t verbs[] = {
        {.word = "TEST", .min_args = 1, .max_args = 1, .usage = "<phrase-hex>", .run = at_test},
        {.word = "SELFTEST", .run = at_selftest},
    };
    const xiaotai_err_t error = xiaotai_at_verbs_register("AT+WAKE", verbs, sizeof(verbs) / sizeof(verbs[0]));
    return error == XIAOTAI_OK ? xiaotai_diag_register("wake", diagnose) : error;
}
