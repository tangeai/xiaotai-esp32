// SPDX-License-Identifier: MIT
/* The three modes (indoor station, doorbell, access machine): the choice a device shows
 * before it was configured, the settings page, the questions before a mode or
 * a counterpart changes, and the indoor station's list of recent door calls. */
#include "door_internal.h"

#include "xiaotai/modules/settings.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#include "xiaotai/ui.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* ASK_INDOOR + mode. */
typedef enum { ASK_NONE, ASK_REMOVE, ASK_INDOOR, ASK_BELL, ASK_ACCESS, ASK_BELL_INDOOR } question_t;

static struct {
    question_t question;
    uint32_t token;
    char id[XIAOTAI_SETTING_TEXT_MAX];
} ask;

#define LOG_MAX 12U
typedef struct {
    int64_t at_us;
    uint8_t kind;            /* door_log_kind_t */
    char label[XIAOTAI_SETTING_TEXT_MAX];
} entry_t;

typedef struct {
    entry_t entries[LOG_MAX];
    uint8_t count;           /* entries[0] is the newest */
} history_t;
static history_t *history;     /* from init on */

void door_mode_reset(void)
{
    memset(&ask, 0, sizeof(ask));
    xiaotai_mem_free(history);
    history = NULL;
}

/* ------------------------------------------------------------------- choice */

/* A device that was used before counts as configured: only a fresh one asks. */
bool door_mode_known(void) { return door_pages_on() || door.chosen || door.sender || door.peer_count > 0; }

static xiaotai_err_t apply_mode(xiaotai_app_door_mode_t mode)
{
    xiaotai_err_t err = xiaotai_setting_set_int(XIAOTAI_SETTING_APP_DOOR_SENDER, mode);
    if (err == XIAOTAI_OK) err = xiaotai_setting_set_int(XIAOTAI_SETTING_APP_DOOR_CHOSEN, 1);
    if (err != XIAOTAI_OK) xiaotai_view_toast(err == XIAOTAI_ERR_BUSY ? "维护中，设置暂不可修改" : "设置没有保存，请重试", 0);
    return err;
}

void door_mode_render(void)
{
    /* Two systems, four places: a doorbell (its indoor station here, the bell at the door, usually a device
     * without a screen) and an access system (the indoor station, the access machine at the gate). */
    const bool camera = door_has_camera();
    xiaotai_view_action_t actions[6] = {
        {.label = "可视门铃", .intent = XIAOTAI_INTENT_APP_DOOR_MODE, .value = XIAOTAI_APP_DOOR_BELL_INDOOR + 1,
         .icon = XIAOTAI_ICON_VIDEO, .role = XIAOTAI_ROLE_PRIMARY},
        {.label = "门铃室外机", .intent = XIAOTAI_INTENT_APP_DOOR_MODE, .value = XIAOTAI_APP_DOOR_BELL + 1,
         .icon = XIAOTAI_ICON_DOOR},
        {.label = "门禁室内机", .intent = XIAOTAI_INTENT_APP_DOOR_MODE, .value = XIAOTAI_APP_DOOR_INDOOR + 1,
         .icon = XIAOTAI_ICON_VIDEO},
        {.label = "门禁机", .intent = XIAOTAI_INTENT_APP_DOOR_MODE, .value = XIAOTAI_APP_DOOR_ACCESS + 1,
         .icon = XIAOTAI_ICON_GROUP},
        {.intent = XIAOTAI_INTENT_APP_DOOR_HOME, .trigger = XIAOTAI_TRIGGER_SWIPE_LEFT, .value = 1,
         .icon = door_pager_icon(1)},
        {.intent = XIAOTAI_INTENT_APP_DOOR_HOME, .trigger = XIAOTAI_TRIGGER_SWIPE_RIGHT, .value = -1,
         .icon = door_pager_icon(-1)},
    };
    const xiaotai_view_item_t rows[] = {
        {.label = "可视门铃", .detail = "放在家里，门口按铃时响铃接听", .icon = XIAOTAI_ICON_VIDEO,
         .intent = XIAOTAI_INTENT_APP_DOOR_MODE, .value = XIAOTAI_APP_DOOR_BELL_INDOOR + 1},
        {.label = "门铃室外机", .detail = camera ? "装在一户的门口，访客按铃" : "装在一户的门口，只有声音",
         .icon = XIAOTAI_ICON_DOOR, .intent = XIAOTAI_INTENT_APP_DOOR_MODE, .value = XIAOTAI_APP_DOOR_BELL + 1},
        {.label = "门禁室内机", .detail = "放在家里或前台，看门口、开门", .icon = XIAOTAI_ICON_VIDEO,
         .intent = XIAOTAI_INTENT_APP_DOOR_MODE, .value = XIAOTAI_APP_DOOR_INDOOR + 1},
        {.label = "门禁机", .detail = camera ? "装在单元或公司大门，拨房号" : "装在大门，拨房号，只有声音", .icon = XIAOTAI_ICON_GROUP,
         .intent = XIAOTAI_INTENT_APP_DOOR_MODE, .value = XIAOTAI_APP_DOOR_ACCESS + 1},
    };
    const xiaotai_view_item_t items[] = {
        {.label = "可视门铃", .detail = "放在家里。门口按铃时响铃、接听，也能听门口"},
        {.label = "门铃室外机", .detail = camera ? "装在一户的门口。访客按铃，家里可以查看" :
                                                  "装在一户的门口。访客按铃，只有声音"},
        {.label = "门禁室内机", .detail = "放在家里或前台。查看门口，接听访客，开门"},
        {.label = "门禁机", .detail = camera ? "装在单元或公司大门。访客拨房号找住户" :
                                              "装在单元或公司大门。访客拨房号，只有声音"},
    };
    const xiaotai_app_door_home_view_t custom = {.state = XIAOTAI_APP_DOOR_NO_STATION, .station = true};
    xiaotai_view_t view = {
        .kind = XIAOTAI_VIEW_CUSTOM, .id = XIAOTAI_APP_DOOR_PAGE_MODE, .custom = XIAOTAI_APP_DOOR_TEMPLATE_MODE,
        .title = "这台设备放在哪里？", .subtitle = "以后可以在门禁设置里修改",
        /* Rows on a small display have one line for it. */
        .items = door_own_template() ? items : rows, .item_count = 4, .custom_data = &custom, .custom_size = sizeof(custom), .actions = actions, .action_count = 6,
        .flags = XIAOTAI_VIEW_MAY_SLEEP,
    };
    if (!door_home_template()) {
        /* The generic layout: a row for each place, and paging by swiping. */
        view.kind = XIAOTAI_VIEW_LIST;
        view.custom = NULL;
        view.subtitle = NULL;
        view.items = rows;
        actions[4].icon = actions[5].icon = NULL;
        view.actions = actions + 4;
        view.action_count = 2;
    }
    door_register_view(&view);
}

/* ----------------------------------------------------------------- settings */

static xiaotai_app_door_mode_t current_mode(void) { return door_mode(); }

static const char *mode_name(xiaotai_app_door_mode_t mode)
{
    return mode == XIAOTAI_APP_DOOR_ACCESS ? "门禁机" : mode == XIAOTAI_APP_DOOR_BELL ? "门铃室外机" :
           mode == XIAOTAI_APP_DOOR_BELL_INDOOR ? "可视门铃" : "门禁室内机";
}

static const char *count_text(char *out, size_t size)
{
    if (!door.peer_count) return "还没有添加";
    (void)snprintf(out, size, "已添加 %u 台", (unsigned)door.peer_count);
    return out;
}

void door_settings_render(void)
{
    xiaotai_view_item_t items[4 + XIAOTAI_APP_DOOR_PEERS_MAX];
    char counted[32], labels[XIAOTAI_APP_DOOR_PEERS_MAX][XIAOTAI_SETTING_TEXT_MAX + 16];
    size_t count = 0;
    if (!door_pages_on())       /* with a page for each mode, paging chooses it */
        items[count++] = (xiaotai_view_item_t){.label = "工作模式", .detail = mode_name(current_mode()),
            .intent = XIAOTAI_INTENT_APP_DOOR_MODE, .icon = XIAOTAI_ICON_DOOR};
    items[count++] = (xiaotai_view_item_t){.label = door.access ? "管理处" : door.sender ? "室内机" :
                                                    door.bell_indoor ? "门铃" : "室外机",
        .detail = count_text(counted, sizeof(counted)), .intent = XIAOTAI_INTENT_APP_DOOR_DEVICES,
        .icon = XIAOTAI_ICON_CONTACTS};
    if (!door.sender && !door.bell_indoor) {
        items[count++] = (xiaotai_view_item_t){.label = "来访记录", .detail = "最近的门口呼叫",
            .intent = XIAOTAI_INTENT_APP_DOOR_LOG, .icon = XIAOTAI_ICON_CLOCK};
    }
    for (uint8_t i = 0; i < door.peer_count; ++i) {
        char name[XIAOTAI_SETTING_TEXT_MAX];
        door_peer_label(door.peers[i], name, sizeof(name));
        (void)snprintf(labels[i], sizeof(labels[i]), "移除 %s", name);
        items[count++] = (xiaotai_view_item_t){.label = labels[i],
            .detail = door.sender ? "移除后不能再查看这里" : "移除后不再接听它的呼叫",
            .intent = XIAOTAI_INTENT_APP_DOOR_REMOVE, .value = i, .icon = XIAOTAI_ICON_STOP};
    }
    if (door.sender && xiaotai_intent_exists(XIAOTAI_INTENT_SYSTEM_MENU)) {
        items[count++] = (xiaotai_view_item_t){.label = "系统菜单", .detail = "网络、音量和其他设置",
            .intent = XIAOTAI_INTENT_SYSTEM_MENU, .icon = XIAOTAI_ICON_MENU};
    }
    const xiaotai_view_action_t back = {
        .label = "返回", .role = XIAOTAI_ROLE_BACK, .intent = XIAOTAI_INTENT_APP_DOOR_BACK,
    };
    const xiaotai_view_t view = {
        .kind = XIAOTAI_VIEW_CUSTOM, .id = XIAOTAI_APP_DOOR_PAGE_SETTINGS, .custom = XIAOTAI_APP_DOOR_TEMPLATE_DEVICES,
        .title = "门禁设置", .actions = &back, .action_count = 1, .items = items, .item_count = count,
    };
    door_show_list(&view);
}

static void settings_intent(const xiaotai_args_t *args, void *ctx)
{
    (void)args;
    (void)ctx;
    if (!door.foreground || door.page != DOOR_HOME || door.overlay || door.setup || door_busy()) return;
    door_page(DOOR_SETTINGS);
}

/* ---------------------------------------------------------------- questions */

static void ask_show(question_t question, const char *title, const char *subtitle, const char *text, const char *yes)
{
    ask.question = question;
    do { ask.token = xiaotai_generation_next(); } while (ask.token <= 1U);
    door.overlay = DOOR_DIALOG_ASK;
    const xiaotai_view_action_t actions[] = {
        {.label = "取消", .intent = XIAOTAI_INTENT_APP_DOOR_ASK, .role = XIAOTAI_ROLE_BACK},
        {.label = yes, .intent = XIAOTAI_INTENT_APP_DOOR_ASK, .role = XIAOTAI_ROLE_PRIMARY,
         .style = question == ASK_REMOVE ? XIAOTAI_STYLE_DESTRUCTIVE : XIAOTAI_STYLE_POSITIVE,
         .value = (int32_t)ask.token},
    };
    const xiaotai_view_t view = {
        .layer = XIAOTAI_LAYER_OVERLAY, .kind = XIAOTAI_VIEW_CUSTOM, .custom = XIAOTAI_APP_DOOR_TEMPLATE_DIALOG,
        .id = XIAOTAI_APP_DOOR_OVERLAY_ASK, .title = title, .subtitle = subtitle, .text = text,
        .actions = actions, .action_count = 2,
    };
    xiaotai_ui_activity();
    xiaotai_view_show(&view);
}

static void mode_intent(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    if (!door.foreground || door.overlay || door.setup || args->value < 0 || args->value > XIAOTAI_APP_DOOR_BELL_INDOOR + 1)
        return;
    if (door_pages_on()) return;        /* every mode is a page: paging chooses */
    if (door_busy()) {
        xiaotai_view_toast("通话或查看中不能切换模式", 0);
        return;
    }
    if (args->value == 0) {
        if (door.page == DOOR_SETTINGS || door.page == DOOR_HOME) door_page(DOOR_MODE);
        return;
    }
    if (door.page != DOOR_MODE && door.page != DOOR_SETTINGS && (door.page != DOOR_HOME || door_mode_known())) return;
    const xiaotai_app_door_mode_t mode = (xiaotai_app_door_mode_t)(args->value - 1);
    if (!door_mode_known()) {
        if (apply_mode(mode) != XIAOTAI_OK) return;
        door_page(DOOR_HOME);
        xiaotai_view_toast(mode == XIAOTAI_APP_DOOR_BELL_INDOOR ? "接下来请添加门铃" :
                           mode != XIAOTAI_APP_DOOR_INDOOR ? "长按左上角标题可以进门禁设置" : "接下来请添加室外机", 0);
        return;
    }
    if (mode == current_mode()) {
        door_page(DOOR_HOME);
        return;
    }
    if (mode == XIAOTAI_APP_DOOR_ACCESS) {
        ask_show(ASK_ACCESS, "改为门禁机？", "装在单元或公司大门，访客拨房号",
                 door_has_camera() ? "已添加的设备会成为管理处，可以随时查看本机的画面。" :
                                     "已添加的设备会成为管理处，可以随时听本机的声音。", "改为门禁机");
    } else if (mode == XIAOTAI_APP_DOOR_BELL) {
        ask_show(ASK_BELL, "改为门铃室外机？", "装在一户的门口，给访客按铃",
                 door_has_camera() ? "已添加的设备会成为室内机，可以随时查看本机的画面。" :
                                     "已添加的设备会成为室内机，可以随时听本机的声音。", "改为门铃室外机");
    } else if (mode == XIAOTAI_APP_DOOR_BELL_INDOOR) {
        ask_show(ASK_BELL_INDOOR, "改为可视门铃？", "放在家里，门口按铃时响铃接听",
                 "已添加的设备会成为门铃，不能再查看本机。", "改为可视门铃");
    } else {
        ask_show(ASK_INDOOR, "改为门禁室内机？", "放在室内，查看和接听门口、开门",
                 "已添加的设备会成为室外机，不能再查看本机。", "改为门禁室内机");
    }
}

static void remove_intent(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    if (!door.foreground || door.page != DOOR_SETTINGS || door.overlay || door.setup || door_busy()) return;
    if (args->value < 0 || args->value >= door.peer_count) return;
    memcpy(ask.id, door.peers[args->value], sizeof(ask.id));
    char name[XIAOTAI_SETTING_TEXT_MAX];
    door_peer_label(ask.id, name, sizeof(name));
    ask_show(ASK_REMOVE, "移除这台设备？", name,
             door.sender ? "移除后，它不能再查看这里，访客呼叫时也不再响铃。" : "移除后，它的呼叫不再由门禁显示。", "移除");
}

static void ask_intent(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    if (!door.foreground || door.overlay != DOOR_DIALOG_ASK) return;
    const uint32_t token = (uint32_t)args->value;
    const question_t question = ask.question;
    if (token && token != ask.token) return;
    door_overlay_close();
    ask.question = ASK_NONE;
    if (!token) return;
    if (door_busy()) {
        xiaotai_view_toast("通话或查看中不能修改", 0);
        return;
    }
    if (question == ASK_REMOVE) {
        const xiaotai_err_t err = door_peer_remove(ask.id);
        if (err != XIAOTAI_OK) xiaotai_view_toast(err == XIAOTAI_ERR_BUSY ? "维护中，设置暂不可修改" : "没有移除，请重试", 0);
        door_render();
        return;
    }
    const xiaotai_app_door_mode_t mode = (xiaotai_app_door_mode_t)(question - ASK_INDOOR);
    if (apply_mode(mode) != XIAOTAI_OK) return;
    door_page(DOOR_HOME);
    xiaotai_view_toast(mode == XIAOTAI_APP_DOOR_ACCESS ? "已改为门禁机，长按标题可以进设置" :
                       mode == XIAOTAI_APP_DOOR_BELL ? "已改为门铃室外机，长按标题可以进设置" :
                       mode == XIAOTAI_APP_DOOR_BELL_INDOOR ? "已改为可视门铃" : "已改为门禁室内机", 0);
}

/* ---------------------------------------------------- recent calls (memory) */

void door_log_add(door_log_kind_t kind, const char *id)
{
    if (door.sender || door.bell_indoor) return;
    if (history->count < LOG_MAX) ++history->count;
    memmove(&history->entries[1], &history->entries[0], (history->count - 1U) * sizeof(history->entries[0]));
    entry_t *entry = &history->entries[0];
    entry->at_us = xiaotai_time_us();
    entry->kind = (uint8_t)kind;
    door_peer_label(id, entry->label, sizeof(entry->label));
    if (kind == DOOR_LOG_MISSED) ++door.missed;
}

static void ago(int64_t at_us, char *out, size_t size)
{
    const int64_t minutes = (xiaotai_time_us() - at_us) / 60000000LL;
    if (minutes < 1) (void)snprintf(out, size, "刚刚");
    else if (minutes < 60) (void)snprintf(out, size, "%d 分钟前", (int)minutes);
    else if (minutes < 24 * 60) (void)snprintf(out, size, "%d 小时前", (int)(minutes / 60));
    else (void)snprintf(out, size, "%d 天前", (int)(minutes / (24 * 60)));
}

void door_log_render(void)
{
    static const char *const words[] = {
        [DOOR_LOG_MISSED] = "未接", [DOOR_LOG_ANSWERED] = "已接听", [DOOR_LOG_REJECTED] = "已拒绝",
        [DOOR_LOG_OPENED] = "已开门",
    };
    xiaotai_view_item_t items[LOG_MAX];
    char details[LOG_MAX][48];
    for (uint8_t i = 0; i < history->count; ++i) {
        char when[24];
        ago(history->entries[i].at_us, when, sizeof(when));
        (void)snprintf(details[i], sizeof(details[i]), "%s · %s", words[history->entries[i].kind], when);
        items[i] = (xiaotai_view_item_t){.label = history->entries[i].label, .detail = details[i],
            .icon = history->entries[i].kind == DOOR_LOG_OPENED ? XIAOTAI_ICON_KEY : XIAOTAI_ICON_PHONE};
    }
    const xiaotai_view_action_t back = {
        .label = "返回", .role = XIAOTAI_ROLE_BACK, .intent = XIAOTAI_INTENT_APP_DOOR_BACK,
    };
    const xiaotai_view_t view = {
        .kind = XIAOTAI_VIEW_CUSTOM, .id = XIAOTAI_APP_DOOR_PAGE_LOG, .custom = XIAOTAI_APP_DOOR_TEMPLATE_DEVICES,
        .title = "来访记录", .text = history->count ? NULL : "还没有来访记录",
        .actions = &back, .action_count = 1, .items = items, .item_count = history->count,
    };
    door_show_list(&view);
}

static void log_intent(const xiaotai_args_t *args, void *ctx)
{
    (void)args;
    (void)ctx;
    if (!door.foreground || door.sender || door.bell_indoor || door.overlay || door.setup || door_busy()) return;
    if (door.page != DOOR_HOME && door.page != DOOR_SETTINGS) return;
    door.missed = 0;
    door_page(DOOR_LOG);
}

xiaotai_err_t door_init_mode(void)
{
    history = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*history));
    if (!history) return XIAOTAI_ERR_NO_MEM;
    static const struct { const char *intent; xiaotai_intent_fn_t handler; } intents[] = {
        {XIAOTAI_INTENT_APP_DOOR_MODE, mode_intent}, {XIAOTAI_INTENT_APP_DOOR_SETTINGS, settings_intent},
        {XIAOTAI_INTENT_APP_DOOR_REMOVE, remove_intent}, {XIAOTAI_INTENT_APP_DOOR_ASK, ask_intent},
        {XIAOTAI_INTENT_APP_DOOR_LOG, log_intent},
    };
    for (size_t i = 0; i < sizeof(intents) / sizeof(*intents); ++i) {
        const xiaotai_err_t err = xiaotai_intent_handle(intents[i].intent, intents[i].handler, NULL);
        if (err != XIAOTAI_OK) return err;
    }
    return XIAOTAI_OK;
}
