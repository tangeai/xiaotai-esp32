// SPDX-License-Identifier: MIT
#include <inttypes.h>
#include <string.h>
#include "internal.h"
#include "xiaotai/port/log.h"
#include "xiaotai/port/os.h"
#include "xiaotai/port/sys.h"

bool at_id_valid(const char *name)
{
    if (!name || !*name) return false;
    bool expect_segment = true;
    for (size_t i = 0; name[i]; ++i) {
        const char c = name[i];
        if (i == XIAOTAI_AT_ID_MAX) return false;
        if (c == '.') {
            if (expect_segment) return false;
            expect_segment = true;
        } else {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
            expect_segment = false;
        }
    }
    return !expect_segment;
}

bool at_name_allowed(const char *name, const char *const *allow)
{
    if (!allow) return true;
    for (; *allow; ++allow) if (!strcmp(name, *allow)) return true;
    return false;
}

typedef struct {
    const xiaotai_reply_t *out;
    char prefix[XIAOTAI_AT_ID_MAX + 4];
    bool wrote;
} status_output_t;

static void status_write(void *ctx, const char *text)
{
    status_output_t *status = ctx;
    while (text && *text) {
        const char *end = strchr(text, '\n');
        const size_t len = end ? (size_t)(end - text) : strlen(text);
        if (len) {
            const int shown = (int)(len > XIAOTAI_AT_OUT_LINE_MAX + 1 ? XIAOTAI_AT_OUT_LINE_MAX + 1 : len);
            xiaotai_at_printf(status->out, "%s%.*s", *text == '+' ? "" : status->prefix, shown, text);
            status->wrote = true;
        }
        text = end ? end + 1 : NULL;
    }
}

static void status_section(const char *name, xiaotai_diag_fn_t report, void *ctx)
{
    status_output_t *status = ctx;
    if (!at_id_valid(name)) return;
    char *prefix = status->prefix;
    *prefix++ = '+';
    for (; *name; ++name) {
        const char c = *name;
        *prefix++ = c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c == '.' ? '-' : c;
    }
    memcpy(prefix, ": ", sizeof(": "));
    const xiaotai_reply_t out = {.write = status_write, .ctx = status};
    report(&out);
}

static xiaotai_err_t status_command(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    if (argc && !at_id_valid(argv[0]))
        return xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_ARG, "SECTION_NAME");
    status_output_t status = {.out = reply};
    xiaotai_diag_visit_sections(argc ? argv[0] : NULL, status_section, &status);
    return argc && !status.wrote ? xiaotai_at_fail(reply, XIAOTAI_ERR_NOT_FOUND, "SECTION") : XIAOTAI_OK;
}

static void state_report(const xiaotai_reply_t *reply)
{
    const char *activity = NULL;
    const uint32_t id = xiaotai_activity_current(&activity);
    const char *app = xiaotai_app_foreground();
    xiaotai_at_printf(reply, "+STATE: started=1 app=%s activity=%s id=%" PRIu32 " uptime_ms=%" PRId64,
                      app ? app : "-", id && activity ? activity : "none", id, xiaotai_time_us() / 1000);
}

static xiaotai_err_t state(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    state_report(reply);
    return XIAOTAI_OK;
}

const xiaotai_setting_t *at_setting_find(const char *name)
{
    for (size_t i = 0; i < xiaotai_setting_count(); ++i) {
        const xiaotai_setting_t *setting = xiaotai_setting_at(i);
        if (!strcmp(name, setting->name)) return setting;
    }
    return NULL;
}

static xiaotai_err_t setting_line(const xiaotai_setting_t *setting, const xiaotai_reply_t *reply)
{
    if (setting->kind == XIAOTAI_SETTING_TEXT) {
        char value[XIAOTAI_SETTING_TEXT_MAX];
        const xiaotai_err_t err = xiaotai_setting_get_text(setting->name, value, sizeof(value));
        if (err != XIAOTAI_OK) return err;
        char hex[XIAOTAI_SETTING_TEXT_MAX * 2 + 1];
        (void)xiaotai_at_text_encode(value, hex, sizeof(hex));
        xiaotai_at_printf(reply, "+SET: name=%s kind=text value_hex=%s min=%" PRId32 " max=%" PRId32,
                          setting->name, hex, setting->min, setting->max);
    } else {
        int32_t value;
        const xiaotai_err_t err = xiaotai_setting_get_int(setting->name, &value);
        if (err != XIAOTAI_OK) return err;
        xiaotai_at_printf(reply, "+SET: name=%s kind=%s value=%" PRId32 " min=%" PRId32 " max=%" PRId32,
                          setting->name, setting->kind == XIAOTAI_SETTING_BOOL ? "bool" : "int", value,
                          setting->min, setting->max);
    }
    return XIAOTAI_OK;
}

static xiaotai_err_t setting_read(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    if (argc) {
        if (!at_id_valid(argv[0])) return xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_ARG, "SETTING");
        const xiaotai_setting_t *setting = at_setting_find(argv[0]);
        return setting ? setting_line(setting, reply) : xiaotai_at_fail(reply, XIAOTAI_ERR_NOT_FOUND, "SETTING");
    }
    for (size_t i = 0; i < xiaotai_setting_count(); ++i) {
        const xiaotai_err_t err = setting_line(xiaotai_setting_at(i), reply);
        if (err != XIAOTAI_OK) return err;
    }
    return XIAOTAI_OK;
}

static void port_report(const xiaotai_reply_t *reply) { xiaotai_port_report(reply->write, reply->ctx); }

/* AT+LOGLEVEL: XiaoTai's own lines (app), one module's lines, and the
 * platform's and third-party components' lines (sys); components with logs of
 * their own (the RTC stack) add a line each and a verb to change it. */
static xiaotai_err_t log_level_show(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    xiaotai_at_printf(reply, "+LOGLEVEL: target=app level=%u max=%u", (unsigned)xiaotai_log_level(),
                      (unsigned)xiaotai_log_level_max());
    xiaotai_at_printf(reply, "+LOGLEVEL: target=sys level=%u max=%u", (unsigned)xiaotai_log_system_level(),
                      (unsigned)xiaotai_log_system_level_max());
    const char *module;
    uint8_t level;
    for (size_t i = 0; xiaotai_log_module_at(i, &module, &level); ++i) {
        xiaotai_at_printf(reply, "+LOGLEVEL: target=module name=%s level=%u", module, (unsigned)level);
    }
    uint8_t app, system;
    if (xiaotai_log_saved(&app, &system)) {
        xiaotai_at_printf(reply, "+LOGLEVEL: target=saved app=%u sys=%u", (unsigned)app, (unsigned)system);
    }
    return XIAOTAI_AT_PASS;
}

static xiaotai_err_t log_level_app(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    uint32_t level;
    if (!xiaotai_at_parse_u32(argv[0], 0, XIAOTAI_LOG_LEVEL_DEBUG, &level))
        return xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_ARG, "LEVEL");
    return xiaotai_log_set_level((uint8_t)level);
}

static xiaotai_err_t log_level_sys(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    uint32_t level;
    if (!xiaotai_at_parse_u32(argv[0], 0, xiaotai_log_system_level_max(), &level))
        return xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_ARG, "LEVEL");
    return xiaotai_log_set_system_level((uint8_t)level);
}

/* "-" gives the module back to the app threshold. */
static xiaotai_err_t log_level_module(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    uint32_t level = XIAOTAI_LOG_LEVEL_INHERIT;
    if (strcmp(argv[1], "-") != 0 && !xiaotai_at_parse_u32(argv[1], 0, XIAOTAI_LOG_LEVEL_DEBUG, &level))
        return xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_ARG, "LEVEL");
    const xiaotai_err_t err = xiaotai_log_set_module_level(argv[0], (uint8_t)level);
    if (err == XIAOTAI_ERR_INVALID_ARG) return xiaotai_at_fail(reply, err, "MODULE");
    return err == XIAOTAI_ERR_NO_MEM ? xiaotai_at_fail(reply, err, "MODULES_FULL") : err;
}

static xiaotai_err_t log_level_save(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    (void)reply;
    return xiaotai_log_save();
}

static xiaotai_err_t log_level_forget(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    (void)reply;
    return xiaotai_log_forget();
}

xiaotai_err_t xiaotai_engine_at_init(void)
{
    static const xiaotai_at_verb_t status_verb[] = {{.max_args = 1, .run = status_command, .usage = "[<section>]"}};
    static const xiaotai_at_verb_t state_verb[] = {{.run = state}};
    static const xiaotai_at_verb_t setting_read_verb[] = {{.max_args = 1, .run = setting_read, .usage = "[<name>]"}};
    static const xiaotai_at_verb_t log_level_verbs[] = {
        {.run = log_level_show},
        {.word = "APP", .min_args = 1, .max_args = 1, .run = log_level_app, .usage = "<0..4>"},
        {.word = "SYS", .min_args = 1, .max_args = 1, .run = log_level_sys, .usage = "<0..3>"},
        {.word = "MODULE", .min_args = 2, .max_args = 2, .run = log_level_module, .usage = "<module> <0..4|->"},
        {.word = "SAVE", .run = log_level_save},
        {.word = "FORGET", .run = log_level_forget},
    };
    xiaotai_err_t err = xiaotai_at_open();
    if (err == XIAOTAI_OK) err = xiaotai_at_verbs_register("AT+STATUS", status_verb, 1);
    if (err == XIAOTAI_OK) err = xiaotai_at_verbs_register("AT+STATE", state_verb, 1);
    if (err == XIAOTAI_OK) err = xiaotai_at_verbs_register("AT+LOGLEVEL", log_level_verbs,
                                                             sizeof(log_level_verbs) / sizeof(log_level_verbs[0]));
    if (err == XIAOTAI_OK) err = xiaotai_at_verbs_register("AT+SET", setting_read_verb, 1);
#if XIAOTAI_AT_SETTING_WRITE
    if (err == XIAOTAI_OK) err = xiaotai_at_guarded_set_register();
#endif
#if XIAOTAI_AT_INTENT_COMMAND
    if (err == XIAOTAI_OK) err = xiaotai_at_guarded_intent_register();
#endif
    if (err == XIAOTAI_OK) err = xiaotai_diag_register("state", state_report);
    if (err == XIAOTAI_OK) err = xiaotai_diag_register("mem", at_mem_report);
    if (err == XIAOTAI_OK) err = xiaotai_diag_register("board", at_board_report);
    if (err == XIAOTAI_OK) err = xiaotai_diag_register("port", port_report);
    if (err == XIAOTAI_OK && !at_command_tables_omitted()) err = at_log_attach();
    if (err == XIAOTAI_OK) err = at_console_ensure_off_owner_reader();
    return err;
}
