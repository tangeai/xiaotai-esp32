// SPDX-License-Identifier: MIT
// The conversation's device tools (ai_device_tools.c) against fakes of the
// settings registry, the settings module, the clock port and the tool
// registry: which tools a board gets, every mode, what is refused and what
// the answer says.
#include "ai_device_tools.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xiaotai/framework.h"
#include "xiaotai/json.h"
#include "xiaotai/modules/ai_talk.h"
#include "xiaotai/modules/settings.h"
#include "xiaotai/port/clock.h"

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\nreply: %s\n", __FILE__, __LINE__, #condition, reply_text); abort(); \
} } while (0)

/* ------------------------------------------------------------------- fakes */

static struct {
    const char *name;
    xiaotai_ai_talk_tool_fn_t handler;
} tools[8];
static size_t tool_count;
static char reply_text[XIAOTAI_AI_TALK_TOOL_REPLY_MAX];
static unsigned replies, writes;
static xiaotai_settings_status_t device;
static uint8_t step = 10;
static xiaotai_err_t write_error;
static bool synced;
static xiaotai_clock_fields_t local = {.year = 2026, .month = 11, .day = 24, .hour = 7, .minute = 5, .weekday = 2};

xiaotai_err_t xiaotai_ai_talk_tool_register(const char *name, xiaotai_ai_talk_tool_fn_t handler, void *ctx)
{
    CHECK(tool_count < sizeof(tools) / sizeof(tools[0]) && !ctx);
    tools[tool_count].name = name;
    tools[tool_count++].handler = handler;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_ai_talk_tool_reply(uint32_t session, uint32_t request, const char *object)
{
    CHECK(session == 7 && request == 9 && strlen(object) < sizeof(reply_text));
    strcpy(reply_text, object);
    ++replies;
    return XIAOTAI_OK;
}

void xiaotai_settings_status(xiaotai_settings_status_t *status) { *status = device; }
uint8_t xiaotai_settings_volume_step_percent(void) { return step; }

xiaotai_err_t xiaotai_setting_set_int(const char *name, int32_t value)
{
    ++writes;
    if (write_error != XIAOTAI_OK) return write_error;
    if (!strcmp(name, XIAOTAI_SETTING_AUDIO_VOLUME)) {
        CHECK(device.has_speaker && value >= 0 && value <= 100);
        device.volume = (uint8_t)value;
    } else if (!strcmp(name, XIAOTAI_SETTING_AUDIO_SPEAKER_MUTED)) {
        CHECK(device.has_speaker && (value == 0 || value == 1));
        device.speaker_muted = value != 0;
    } else if (!strcmp(name, XIAOTAI_SETTING_SCREEN_BRIGHTNESS)) {
        CHECK(device.has_brightness && value >= XIAOTAI_SETTINGS_BRIGHTNESS_MIN && value <= 100);
        device.screen_brightness = (uint8_t)value;
    } else {
        CHECK(!"an unexpected setting was written");
    }
    return XIAOTAI_OK;
}

bool __wrap_xiaotai_clock_synced(void);
int64_t __wrap_xiaotai_clock_unix_ms(void);
xiaotai_err_t __wrap_xiaotai_clock_local(int64_t unix_ms, xiaotai_clock_fields_t *fields);
bool __wrap_xiaotai_clock_synced(void) { return synced; }
int64_t __wrap_xiaotai_clock_unix_ms(void) { return 1795000000000LL; }
xiaotai_err_t __wrap_xiaotai_clock_local(int64_t unix_ms, xiaotai_clock_fields_t *fields)
{
    CHECK(unix_ms == 1795000000000LL);
    *fields = local;
    return XIAOTAI_OK;
}

/* ----------------------------------------------------------------- helpers */

static bool has_tool(const char *name)
{
    for (size_t i = 0; i < tool_count; ++i) {
        if (!strcmp(tools[i].name, name)) return true;
    }
    return false;
}

static void boot(bool speaker, bool brightness)
{
    tool_count = 0;
    device = (xiaotai_settings_status_t){.has_speaker = speaker, .has_brightness = brightness, .has_screen = brightness,
        .volume = speaker ? 60 : 0, .screen_brightness = brightness ? 100 : 0};
    step = 10;
    write_error = XIAOTAI_OK;
    synced = true;
    CHECK(companion_ai_device_tools_init() == XIAOTAI_OK);
}

static void invoke(const char *name, const char *json)
{
    for (size_t i = 0; i < tool_count; ++i) {
        if (strcmp(tools[i].name, name)) continue;
        const xiaotai_ai_talk_tool_call_t call = {.session = 7, .request = 9, .name = name, .arguments = json,
            .size = strlen(json), .device_action = true, .has_arguments = strcmp(json, "{}") != 0};
        reply_text[0] = 0;
        const unsigned before = replies;
        tools[i].handler(&call, NULL);
        CHECK(replies == before + 1U);
        return;
    }
    CHECK(!"the tool is not registered");
}

static xiaotai_json_token_t tokens[48];
static xiaotai_json_doc_t doc;

static int field(const char *key)
{
    size_t count = 0;
    CHECK(xiaotai_json_parse(reply_text, strlen(reply_text), tokens, 48, &count, &doc) == XIAOTAI_OK);
    return xiaotai_json_get(&doc, 0, key);
}

static int64_t number(const char *key)
{
    int64_t value = -1;
    CHECK(xiaotai_json_int(&doc, field(key), &value) == XIAOTAI_OK);
    return value;
}

static bool flag(const char *key)
{
    bool value = false;
    CHECK(xiaotai_json_bool(&doc, field(key), &value) == XIAOTAI_OK);
    return value;
}

static bool text_is(const char *key, const char *expected) { return xiaotai_json_equals(&doc, field(key), expected); }

/* ok and status of the last answer; every answer carries a message. */
static bool answered(bool ok, const char *status)
{
    char message[160];
    CHECK(xiaotai_json_string(&doc, field("message"), message, sizeof(message), NULL) == XIAOTAI_OK && message[0]);
    return flag("ok") == ok && text_is("status", status);
}

/* ------------------------------------------------------------------- cases */

static void registration(void)
{
    boot(false, false);
    CHECK(tool_count == 1 && has_tool("get_device_status"));
    boot(true, false);
    CHECK(tool_count == 2 && has_tool("set_volume") && !has_tool("set_brightness"));
    boot(false, true);
    CHECK(tool_count == 2 && has_tool("set_brightness") && !has_tool("set_volume"));
    boot(true, true);
    CHECK(tool_count == 3);
}

static void volume(void)
{
    boot(true, false);
    invoke("set_volume", "{\"mode\":\"set\",\"value\":30}");
    CHECK(answered(true, "ok") && number("volume") == 30 && !flag("muted") && device.volume == 30);
    /* The level the settings page keeps: a multiple of the step, never 0 for an audible request. */
    invoke("set_volume", "{\"mode\":\"set\",\"value\":55}");
    CHECK(answered(true, "ok") && number("volume") == 60);
    invoke("set_volume", "{\"mode\":\"set\",\"value\":3}");
    CHECK(answered(true, "ok") && number("volume") == 10);
    invoke("set_volume", "{\"mode\":\"set\",\"value\":0}");
    CHECK(answered(true, "ok") && number("volume") == 0);
    invoke("set_volume", "{\"value\":\"50%\"}");
    CHECK(answered(true, "ok") && number("volume") == 50);
    invoke("set_volume", "{\"mode\":\"UP\"}");
    CHECK(answered(true, "ok") && number("volume") == 60);
    invoke("set_volume", "{\"mode\":\"up\",\"value\":25}");
    CHECK(answered(true, "ok") && number("volume") == 90);
    invoke("set_volume", "{\"mode\":\"up\",\"value\":2}");
    CHECK(answered(true, "ok") && number("volume") == 100);
    writes = 0;
    invoke("set_volume", "{\"mode\":\"up\"}");
    CHECK(answered(true, "at_max") && number("volume") == 100 && writes == 0);
    invoke("set_volume", "{\"mode\":\"down\",\"value\":40}");
    CHECK(answered(true, "ok") && number("volume") == 60);
    invoke("set_volume", "{\"mode\":\"down\"}");
    CHECK(answered(true, "ok") && number("volume") == 50);
    invoke("set_volume", "{\"mode\":\"down\",\"value\":100}");
    CHECK(answered(true, "ok") && number("volume") == 0);
    writes = 0;
    invoke("set_volume", "{\"mode\":\"down\"}");
    CHECK(answered(true, "at_min") && number("volume") == 0 && writes == 0);

    invoke("set_volume", "{\"mode\":\"set\",\"value\":40}");
    invoke("set_volume", "{\"mode\":\"mute\"}");
    CHECK(answered(true, "ok") && flag("muted") && number("volume") == 40);
    /* Quieter stays muted; an audible level or "louder" ends the mute. */
    invoke("set_volume", "{\"mode\":\"down\"}");
    CHECK(answered(true, "ok") && flag("muted") && number("volume") == 30);
    invoke("set_volume", "{\"mode\":\"up\"}");
    CHECK(answered(true, "ok") && !flag("muted") && number("volume") == 40);
    invoke("set_volume", "{\"mode\":\"mute\"}");
    invoke("set_volume", "{\"mode\":\"set\",\"value\":40}");
    CHECK(answered(true, "ok") && !flag("muted") && number("volume") == 40);
    invoke("set_volume", "{\"mode\":\"mute\"}");
    invoke("set_volume", "{\"mode\":\"unmute\"}");
    CHECK(answered(true, "ok") && !flag("muted") && number("volume") == 40);

    step = 5;
    invoke("set_volume", "{\"mode\":\"set\",\"value\":43}");
    CHECK(answered(true, "ok") && number("volume") == 45);
    step = 10;
}

static void volume_refusals(void)
{
    boot(true, false);
    static const char *const bad[] = {
        "{}", "{\"mode\":\"set\"}", "{\"mode\":\"louder\"}", "{\"mode\":\"set\",\"value\":101}",
        "{\"mode\":\"set\",\"value\":-1}", "{\"mode\":\"set\",\"value\":\"loud\"}", "{\"mode\":\"up\",\"value\":0}",
        "{\"mode\":\"down\",\"value\":500}", "{\"mode\":[\"up\"]}", "{\"mode\":\"set\",\"value\":{\"n\":3}}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        writes = 0;
        invoke("set_volume", bad[i]);
        CHECK(answered(false, "invalid_params") && number("volume") == 60 && writes == 0);
    }
    write_error = XIAOTAI_ERR_BUSY;
    invoke("set_volume", "{\"mode\":\"set\",\"value\":20}");
    CHECK(answered(false, "busy") && number("volume") == 60);
    invoke("set_volume", "{\"mode\":\"mute\"}");
    CHECK(answered(false, "busy") && !flag("muted"));
    write_error = XIAOTAI_ERR_NOT_SUPPORTED;
    invoke("set_volume", "{\"mode\":\"up\"}");
    CHECK(answered(false, "failed") && number("volume") == 60);
    /* A notification of the same name does nothing and answers nothing. */
    const xiaotai_ai_talk_tool_call_t notice = {.session = 7, .name = "set_volume",
        .arguments = "{\"mode\":\"mute\"}", .size = 15};
    const unsigned before = replies;
    write_error = XIAOTAI_OK;
    writes = 0;
    for (size_t i = 0; i < tool_count; ++i) tools[i].handler(&notice, NULL);
    CHECK(replies == before && writes == 0);
}

static void brightness(void)
{
    boot(true, true);
    invoke("set_brightness", "{\"mode\":\"set\",\"value\":30}");
    CHECK(answered(true, "ok") && number("brightness") == 30);
    invoke("set_brightness", "{\"mode\":\"up\"}");
    CHECK(answered(true, "ok") && number("brightness") == 50);
    invoke("set_brightness", "{\"mode\":\"up\",\"value\":70}");
    CHECK(answered(true, "ok") && number("brightness") == 100);
    writes = 0;
    invoke("set_brightness", "{\"mode\":\"up\"}");
    CHECK(answered(true, "at_max") && number("brightness") == 100 && writes == 0);
    invoke("set_brightness", "{\"mode\":\"down\",\"value\":95}");
    CHECK(answered(true, "ok") && number("brightness") == XIAOTAI_SETTINGS_BRIGHTNESS_MIN);
    writes = 0;
    invoke("set_brightness", "{\"mode\":\"down\"}");
    CHECK(answered(true, "at_min") && number("brightness") == XIAOTAI_SETTINGS_BRIGHTNESS_MIN && writes == 0);
    /* "As dark as it goes" is the floor, not an error. */
    invoke("set_brightness", "{\"value\":80}");
    invoke("set_brightness", "{\"mode\":\"set\",\"value\":0}");
    CHECK(answered(true, "ok") && number("brightness") == XIAOTAI_SETTINGS_BRIGHTNESS_MIN);
    static const char *const bad[] = {"{}", "{\"mode\":\"mute\"}", "{\"mode\":\"set\",\"value\":120}",
                                      "{\"mode\":\"down\",\"value\":0}", "{\"mode\":\"dim\"}"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        invoke("set_brightness", bad[i]);
        CHECK(answered(false, "invalid_params") && number("brightness") == XIAOTAI_SETTINGS_BRIGHTNESS_MIN);
    }
    write_error = XIAOTAI_ERR_BUSY;
    invoke("set_brightness", "{\"mode\":\"set\",\"value\":60}");
    CHECK(answered(false, "busy") && number("brightness") == XIAOTAI_SETTINGS_BRIGHTNESS_MIN);
}

static void status(void)
{
    boot(true, true);
    invoke("get_device_status", "{}");
    CHECK(answered(true, "ok") && flag("time_synced") && text_is("time", "07:05") && text_is("date", "2026-11-24"));
    CHECK(text_is("weekday", "tuesday") && number("volume") == 60 && !flag("muted") && number("brightness") == 100);
    /* An unset clock names no time; a board without the part names no level. */
    boot(false, false);
    synced = false;
    invoke("get_device_status", "{\"anything\":1}");
    CHECK(answered(true, "ok") && !flag("time_synced"));
    CHECK(field("time") < 0 && field("date") < 0 && field("weekday") < 0 && field("volume") < 0 &&
          field("muted") < 0 && field("brightness") < 0);
}

int main(void)
{
    registration();
    volume();
    volume_refusals();
    brightness();
    status();
    puts("PASS: device tools follow the board, set what was asked and say what the device now holds");
    return 0;
}
