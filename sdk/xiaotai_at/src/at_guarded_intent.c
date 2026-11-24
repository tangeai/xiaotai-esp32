// SPDX-License-Identifier: MIT
#include <limits.h>
#include "internal.h"

#if XIAOTAI_AT_INTENT_COMMAND
static xiaotai_err_t xiaotai_at_guarded_intent_post(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    xiaotai_args_t args = {0};
    if (!at_id_valid(argv[0])) return xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_ARG, "INTENT");
    if (!xiaotai_intent_exists(argv[0])) return XIAOTAI_ERR_NOT_FOUND;
    if (!at_intent_allowed(argv[0])) return XIAOTAI_ERR_NOT_ALLOWED;
    if (argc >= 2 && !xiaotai_at_parse_i32(argv[1], INT32_MIN, INT32_MAX, &args.value))
        return xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_ARG, "VALUE");
    if (argc == 3 && xiaotai_at_text_decode(argv[2], args.text, sizeof(args.text)) != XIAOTAI_OK)
        return xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_ARG, "TEXT");
    return xiaotai_intent_post_external(argv[0], &args);
}

xiaotai_err_t xiaotai_at_guarded_intent_register(void)
{
    static const xiaotai_at_verb_t commands[] = {{.min_args = 1, .max_args = 3,
        .run = xiaotai_at_guarded_intent_post, .usage = "<intent> [<value> [<text-hex>]]"}};
    return xiaotai_at_verbs_register("AT+INTENT", commands, 1);
}
#endif
