// SPDX-License-Identifier: MIT
#include <limits.h>
#include "internal.h"

#if XIAOTAI_AT_SETTING_WRITE
static xiaotai_err_t xiaotai_at_guarded_set_write(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    if (!at_id_valid(argv[0])) return xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_ARG, "SETTING");
    const xiaotai_setting_t *setting = at_setting_find(argv[0]);
    if (!setting) return xiaotai_at_fail(reply, XIAOTAI_ERR_NOT_FOUND, "SETTING");
    if (!at_setting_write_allowed(setting->name)) return XIAOTAI_ERR_NOT_ALLOWED;
    xiaotai_err_t err = XIAOTAI_ERR_INVALID_ARG;
    if (setting->kind == XIAOTAI_SETTING_TEXT) {
        char text[XIAOTAI_SETTING_TEXT_MAX];
        if (xiaotai_at_text_decode(argv[1], text, sizeof(text)) == XIAOTAI_OK)
            err = xiaotai_setting_set_text(setting->name, text);
    } else {
        int32_t value;
        if (xiaotai_at_parse_i32(argv[1], INT32_MIN, INT32_MAX, &value))
            err = xiaotai_setting_set_int(setting->name, value);
    }
    return err == XIAOTAI_ERR_INVALID_ARG || err == XIAOTAI_ERR_INVALID_SIZE
        ? xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_ARG, "VALUE") : err;
}

xiaotai_err_t xiaotai_at_guarded_set_register(void)
{
    static const xiaotai_at_verb_t verbs[] = {{.min_args = 2, .max_args = 2,
        .run = xiaotai_at_guarded_set_write, .usage = "<name> <value>"}};
    return xiaotai_at_verbs_register("AT+SET", verbs, 1);
}
#endif
