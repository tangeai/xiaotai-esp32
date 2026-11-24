// SPDX-License-Identifier: MIT
/* Device tools of the AI conversation: what a user asks the device to do to
 * itself while talking to it ("louder", "mute", "dim the screen", "what time
 * is it"). Each tool is one action name the service calls
 * (xiaotai/modules/ai_talk.h, tools):
 *
 *   set_volume         mode set|up|down|mute|unmute, value
 *   set_brightness     mode set|up|down, value
 *   get_device_status  no arguments: time, date, weekday, volume, brightness
 *
 * Which ones exist follows what the board and the build have, never a product
 * name: a tool is registered only when the setting it changes is (the settings
 * module registers audio.volume with a speaker and screen.brightness with a
 * dimmable display), and the module answers a request for an unregistered
 * name with "unsupported" on its own.
 *
 * Every answer is a result object {"ok", "status", "message", ...}: `status`
 * is one of the fixed words below, which the service's tool description
 * lists; `message` is a sentence the model may say; the numbers are what the
 * device holds after the call. Handlers run on the owner task, read and
 * write settings only, and neither block nor allocate. */
#include "ai_device_tools.h"

#include <stdio.h>
#include <string.h>
#include "xiaotai/framework.h"
#include "xiaotai/json.h"
#include "xiaotai/modules/ai_talk.h"
#include "xiaotai/modules/settings.h"
#include "xiaotai/port/clock.h"

#define REPLY_BYTES 288U
#define BRIGHTNESS_STEP 20

static const char ok[] = "ok", at_max[] = "at_max", at_min[] = "at_min", invalid[] = "invalid_params",
                  busy[] = "busy", failed[] = "failed";

typedef struct {
    xiaotai_json_writer_t writer;
    char text[REPLY_BYTES];
} reply_t;

static void begin(reply_t *reply, const char *status, const char *message)
{
    xiaotai_json_writer_init(&reply->writer, reply->text, sizeof(reply->text));
    xiaotai_json_object_begin(&reply->writer, NULL);
    xiaotai_json_put_bool(&reply->writer, "ok", status == ok || status == at_max || status == at_min);
    xiaotai_json_put_string(&reply->writer, "status", status);
    xiaotai_json_put_string(&reply->writer, "message", message);
}

static void end(const xiaotai_ai_talk_tool_call_t *call, reply_t *reply)
{
    xiaotai_json_object_end(&reply->writer);
    if (xiaotai_json_writer_end(&reply->writer, NULL) == XIAOTAI_OK)
        (void)xiaotai_ai_talk_tool_reply(call->session, call->request, reply->text);
}

/* A write the registry refused: maintenance, or the hardware said no. */
static const char *refusal(xiaotai_err_t error) { return error == XIAOTAI_ERR_BUSY ? busy : failed; }

static const char *refusal_text(xiaotai_err_t error)
{
    return error == XIAOTAI_ERR_BUSY ? "设备正在维护，暂时不能修改设置。" : "设备没有接受这次修改。";
}

typedef enum { MODE_SET, MODE_UP, MODE_DOWN, MODE_MUTE, MODE_UNMUTE } level_mode_t;

/* mode and value of a level tool; false: answer invalid_params. `value` is
 * the level for set (0..100) and the amount for up and down (1..100, optional). */
static bool level_arguments(const xiaotai_ai_talk_tool_call_t *call, bool mutes, level_mode_t *mode,
                            bool *has_value, int32_t *value)
{
    char word[12];
    const xiaotai_err_t named = xiaotai_ai_talk_tool_arg_word(call, "mode", word, sizeof(word));
    const xiaotai_err_t numbered = xiaotai_ai_talk_tool_arg_int(call, "value", value);
    if ((named != XIAOTAI_OK && named != XIAOTAI_ERR_NOT_FOUND) ||
        (numbered != XIAOTAI_OK && numbered != XIAOTAI_ERR_NOT_FOUND)) return false;
    *has_value = numbered == XIAOTAI_OK;
    /* A bare number is the level the user named. */
    if (named == XIAOTAI_ERR_NOT_FOUND || !strcmp(word, "set")) *mode = MODE_SET;
    else if (!strcmp(word, "up")) *mode = MODE_UP;
    else if (!strcmp(word, "down")) *mode = MODE_DOWN;
    else if (mutes && !strcmp(word, "mute")) *mode = MODE_MUTE;
    else if (mutes && !strcmp(word, "unmute")) *mode = MODE_UNMUTE;
    else return false;
    if (*mode == MODE_SET) return *has_value && *value >= 0 && *value <= 100;
    return !*has_value || *mode > MODE_DOWN || (*value >= 1 && *value <= 100);
}

/* ------------------------------------------------------------------ volume */

/* The level the settings page would show: a multiple of the product's step
 * (what the preference store keeps), at least one step for a level above 0. */
static int32_t volume_level(int32_t percent)
{
    const int32_t step = xiaotai_settings_volume_step_percent() ? xiaotai_settings_volume_step_percent() : 1;
    if (percent <= 0) return 0;
    int32_t level = (percent + step / 2) / step * step;
    if (level < step) level = step;
    return level > 100 ? 100 : level;
}

static void volume_reply(const xiaotai_ai_talk_tool_call_t *call, const char *status, const char *message)
{
    xiaotai_settings_status_t now;
    xiaotai_settings_status(&now);
    reply_t reply;
    begin(&reply, status, message);
    xiaotai_json_put_int(&reply.writer, "volume", now.volume);
    xiaotai_json_put_bool(&reply.writer, "muted", now.speaker_muted);
    end(call, &reply);
}

static void volume_tool(const xiaotai_ai_talk_tool_call_t *call, void *unused)
{
    (void)unused;
    if (!call->device_action) return;
    level_mode_t mode = MODE_SET;
    bool has_value = false;
    int32_t value = 0;
    if (!level_arguments(call, true, &mode, &has_value, &value)) {
        volume_reply(call, invalid, "参数无效：mode 取 set、up、down、mute、unmute；set 要带 0 到 100 的 value。");
        return;
    }
    xiaotai_settings_status_t before;
    xiaotai_settings_status(&before);
    xiaotai_err_t err = XIAOTAI_OK;
    const char *status = ok;
    if (mode == MODE_MUTE || mode == MODE_UNMUTE) {
        err = xiaotai_setting_set_int(XIAOTAI_SETTING_AUDIO_SPEAKER_MUTED, mode == MODE_MUTE);
    } else {
        const int32_t step = xiaotai_settings_volume_step_percent() ? xiaotai_settings_volume_step_percent() : 1;
        const int32_t amount = has_value ? value : step;
        int32_t target = volume_level(mode == MODE_SET ? value : mode == MODE_UP ? before.volume + amount :
                                                                                  before.volume - amount);
        /* "A little louder" moves at least one step. */
        if (mode == MODE_UP && target <= before.volume) target = before.volume + step > 100 ? 100 : before.volume + step;
        if (mode == MODE_DOWN && target >= before.volume) target = before.volume > step ? before.volume - step : 0;
        if (mode == MODE_UP && before.volume >= 100) status = at_max;
        if (mode == MODE_DOWN && before.volume == 0) status = at_min;
        if (target != before.volume) err = xiaotai_setting_set_int(XIAOTAI_SETTING_AUDIO_VOLUME, target);
        /* Asking for an audible level ends a mute. */
        if (err == XIAOTAI_OK && before.speaker_muted && mode != MODE_DOWN && target > 0)
            err = xiaotai_setting_set_int(XIAOTAI_SETTING_AUDIO_SPEAKER_MUTED, 0);
    }
    if (err != XIAOTAI_OK) volume_reply(call, refusal(err), refusal_text(err));
    else volume_reply(call, status, status == at_max ? "已经是最大音量。" : status == at_min ? "已经是最小音量。" :
                                    "音量已设置。");
}

/* -------------------------------------------------------------- brightness */

static void brightness_reply(const xiaotai_ai_talk_tool_call_t *call, const char *status, const char *message)
{
    xiaotai_settings_status_t now;
    xiaotai_settings_status(&now);
    reply_t reply;
    begin(&reply, status, message);
    xiaotai_json_put_int(&reply.writer, "brightness", now.screen_brightness);
    end(call, &reply);
}

static void brightness_tool(const xiaotai_ai_talk_tool_call_t *call, void *unused)
{
    (void)unused;
    if (!call->device_action) return;
    level_mode_t mode = MODE_SET;
    bool has_value = false;
    int32_t value = 0;
    if (!level_arguments(call, false, &mode, &has_value, &value)) {
        brightness_reply(call, invalid, "参数无效：mode 取 set、up、down；set 要带 1 到 100 的 value。");
        return;
    }
    xiaotai_settings_status_t before;
    xiaotai_settings_status(&before);
    const int32_t amount = has_value ? value : BRIGHTNESS_STEP;
    int32_t target = mode == MODE_SET ? value : mode == MODE_UP ? before.screen_brightness + amount :
                                                                  before.screen_brightness - amount;
    /* The darkest level still shows the screen: "as dark as it goes". */
    if (target < XIAOTAI_SETTINGS_BRIGHTNESS_MIN) target = XIAOTAI_SETTINGS_BRIGHTNESS_MIN;
    if (target > 100) target = 100;
    const char *status = mode == MODE_UP && before.screen_brightness >= 100 ? at_max :
                         mode == MODE_DOWN && before.screen_brightness <= XIAOTAI_SETTINGS_BRIGHTNESS_MIN ? at_min : ok;
    const xiaotai_err_t err = target == before.screen_brightness ? XIAOTAI_OK :
        xiaotai_setting_set_int(XIAOTAI_SETTING_SCREEN_BRIGHTNESS, target);
    if (err != XIAOTAI_OK) brightness_reply(call, refusal(err), refusal_text(err));
    else brightness_reply(call, status, status == at_max ? "屏幕已经最亮。" : status == at_min ? "屏幕已经最暗。" :
                                        "屏幕亮度已设置。");
}

/* ------------------------------------------------------------------ status */

static void status_tool(const xiaotai_ai_talk_tool_call_t *call, void *unused)
{
    (void)unused;
    if (!call->device_action) return;
    static const char *const weekdays[7] = {"sunday", "monday", "tuesday", "wednesday", "thursday", "friday",
                                            "saturday"};
    xiaotai_settings_status_t now;
    xiaotai_settings_status(&now);
    xiaotai_clock_fields_t local;
    const bool synced = xiaotai_clock_synced() &&
                        xiaotai_clock_local(xiaotai_clock_unix_ms(), &local) == XIAOTAI_OK && local.weekday < 7U;
    reply_t reply;
    begin(&reply, ok, synced ? "设备状态已读取。" : "设备状态已读取；时间还没有同步，不要报时间。");
    xiaotai_json_put_bool(&reply.writer, "time_synced", synced);
    if (synced) {
        char text[16];
        (void)snprintf(text, sizeof(text), "%02u:%02u", (unsigned)local.hour, (unsigned)local.minute);
        xiaotai_json_put_string(&reply.writer, "time", text);
        (void)snprintf(text, sizeof(text), "%04d-%02u-%02u", (int)local.year, (unsigned)local.month,
                       (unsigned)local.day);
        xiaotai_json_put_string(&reply.writer, "date", text);
        xiaotai_json_put_string(&reply.writer, "weekday", weekdays[local.weekday]);
    }
    if (now.has_speaker) {
        xiaotai_json_put_int(&reply.writer, "volume", now.volume);
        xiaotai_json_put_bool(&reply.writer, "muted", now.speaker_muted);
    }
    if (now.has_brightness) xiaotai_json_put_int(&reply.writer, "brightness", now.screen_brightness);
    end(call, &reply);
}

/* --------------------------------------------------------------- lifecycle */

xiaotai_err_t companion_ai_device_tools_init(void)
{
    xiaotai_settings_status_t settings;
    xiaotai_settings_status(&settings);
    xiaotai_err_t err = xiaotai_ai_talk_tool_register("get_device_status", status_tool, NULL);
    if (err == XIAOTAI_OK && settings.has_speaker)
        err = xiaotai_ai_talk_tool_register("set_volume", volume_tool, NULL);
    if (err == XIAOTAI_OK && settings.has_brightness)
        err = xiaotai_ai_talk_tool_register("set_brightness", brightness_tool, NULL);
    return err;
}
