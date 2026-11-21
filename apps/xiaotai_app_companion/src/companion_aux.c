// SPDX-License-Identifier: MIT
#include "companion_internal.h"

#include <stdio.h>
#include <string.h>
#include "xiaotai/modules/binding.h"
#include "xiaotai/modules/provisioning.h"
#include "xiaotai/modules/settings.h"
#include "xiaotai/platform.h"

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static uint32_t timer;
static const char *const diagnostic_names[] = {"framework", "net", "platform", "media", "settings", "tasks"};
static const char *const diagnostic_labels[] = {"框架", "网络", "平台", "媒体", "设置", "任务"};

static void back(xiaotai_view_t *view)
{
    xiaotai_view_action_t *action = &companion.buffer->actions[view->action_count++];
    *action = (xiaotai_view_action_t){.role = XIAOTAI_ROLE_BACK, .icon = XIAOTAI_ICON_BACK,
        .intent = XIAOTAI_INTENT_APP_COMPANION_BACK};
    view->actions = companion.buffer->actions;
}

static void emotion_list(xiaotai_view_t *view, int32_t arg)
{
    (void)arg;
    int32_t selected = 0;
    (void)xiaotai_setting_get_int(XIAOTAI_SETTING_APP_COMPANION_EMOTION, &selected);
    view->kind = XIAOTAI_VIEW_LIST;
    view->flags |= XIAOTAI_VIEW_GRID;
    view->items = companion.buffer->items;
    for (size_t i = 0; i < XIAOTAI_APP_COMPANION_EMOTIONS; ++i) {
        companion.buffer->items[view->item_count++] = (xiaotai_view_item_t){
            .label = xiaotai_app_companion_emotion(i), .intent = XIAOTAI_INTENT_APP_COMPANION_EMOTION,
            .value = (int32_t)i, .state = (size_t)selected == i ? XIAOTAI_STATE_CHECKED : 0};
    }
    back(view);
}

static void emotion_preview(xiaotai_view_t *view, int32_t index)
{
    view->kind = XIAOTAI_VIEW_TALK;
    view->flags |= XIAOTAI_VIEW_EMOTION_PREVIEW;
    view->emotion = xiaotai_app_companion_emotion((size_t)index);
    view->level = 1;
    companion.buffer->actions[view->action_count++] = (xiaotai_view_action_t){.label = "应用到主页",
        .role = XIAOTAI_ROLE_PRIMARY, .intent = XIAOTAI_INTENT_APP_COMPANION_EMOTION_APPLY, .value = index};
    back(view);
}

static void choose_emotion(const xiaotai_args_t *args, void *unused)
{
    (void)unused;
    if (!companion_on_page(XIAOTAI_APP_COMPANION_PAGE_EMOTIONS)) { ++companion.status.stale; return; }
    const xiaotai_err_t err = companion_show_page(XIAOTAI_APP_COMPANION_PAGE_EMOTION, args->value);
    if (err != XIAOTAI_OK) companion_refuse(XIAOTAI_INTENT_APP_COMPANION_EMOTION, err);
}

static xiaotai_err_t apply_emotion(int32_t value)
{
    const char *const name = XIAOTAI_SETTING_APP_COMPANION_EMOTION;
    if (companion.config.save_emotion_on_reapply) {
        int32_t current;
        const xiaotai_err_t err = xiaotai_setting_get_int(name, &current);
        if (err != XIAOTAI_OK) return err;
        if (value == current) return xiaotai_settings_confirm_int_selection(name, value);
    }
    return xiaotai_setting_set_int(name, value);
}

static void store_emotion(const xiaotai_args_t *args, void *unused)
{
    (void)unused;
    if (!companion_on_page(XIAOTAI_APP_COMPANION_PAGE_EMOTION) ||
        args->value != companion.stack[companion.depth - 1].arg) { ++companion.status.stale; return; }
    const xiaotai_err_t err = apply_emotion(args->value);
    if (err != XIAOTAI_OK) companion_refuse(XIAOTAI_INTENT_APP_COMPANION_EMOTION_APPLY, err);
    else (void)companion_reset_home(0);
}

static bool emotion_arg(int32_t index) { return index >= 0 && (size_t)index < XIAOTAI_APP_COMPANION_EMOTIONS; }
static bool diag_arg(int32_t index) { return index >= 0 && (size_t)index < COUNT(diagnostic_names); }

static void network(xiaotai_view_t *view, int32_t unused)
{
    (void)unused;
    xiaotai_net_status_t net;
    xiaotai_net_status(&net);
    xiaotai_binding_status_t binding;
    xiaotai_binding_status(&binding);
    enum { IP = 0, SIGNAL = 48, SSID = 64, DEVICE = 97, PLATFORM = 162, MESSAGE = 801 };
    char *const scratch = companion.buffer->details;
    if (net.ipv4) {
        const uint8_t *const address = (const uint8_t *)&net.ipv4;
        snprintf(scratch + IP, SIGNAL - IP, "%u.%u.%u.%u", address[0], address[1], address[2], address[3]);
    } else if (net.ipv6_valid) {
        uint16_t segments[8];
        for (size_t i = 0; i < COUNT(segments); ++i) segments[i] = (uint16_t)(net.ipv6[i * 2] << 8) | net.ipv6[i * 2 + 1];
        snprintf(scratch + IP, SIGNAL - IP, "%x:%x:%x:%x:%x:%x:%x:%x", segments[0], segments[1], segments[2],
            segments[3], segments[4], segments[5], segments[6], segments[7]);
    } else strcpy(scratch + IP, "—");
    if (net.signal_valid) snprintf(scratch + SIGNAL, SSID - SIGNAL, "%d dBm", net.signal_dbm);
    else strcpy(scratch + SIGNAL, "—");
    snprintf(scratch + SSID, DEVICE - SSID, "%s", net.ssid);
    snprintf(scratch + DEVICE, PLATFORM - DEVICE, "%s", binding.device_id);
    if (xiaotai_platform_endpoint(XIAOTAI_PLATFORM_SERVICE_DEVICE, scratch + PLATFORM, MESSAGE - PLATFORM) != XIAOTAI_OK)
        strcpy(scratch + PLATFORM, "—");
    if (xiaotai_platform_endpoint(XIAOTAI_PLATFORM_SERVICE_MQTT, scratch + MESSAGE,
        sizeof(companion.buffer->details) - MESSAGE) != XIAOTAI_OK) strcpy(scratch + MESSAGE, "—");
    const char *const labels[] = {"SSID", "IP", "信号", "设备号", "平台服务", "消息服务"};
    const unsigned offsets[] = {SSID, IP, SIGNAL, DEVICE, PLATFORM, MESSAGE};
    view->kind = XIAOTAI_VIEW_NOTICE;
    view->items = companion.buffer->items;
    for (size_t i = 0; i < COUNT(labels); ++i)
        companion.buffer->items[view->item_count++] = (xiaotai_view_item_t){.label = labels[i], .detail = scratch + offsets[i]};
    companion.buffer->actions[view->action_count++] = (xiaotai_view_action_t){.label = "断开当前连接",
        .role = XIAOTAI_ROLE_PRIMARY, .intent = XIAOTAI_INTENT_PROVISIONING_BEGIN, .value = XIAOTAI_PROVISIONING_PREEMPT};
    back(view);
}

static void append_report(void *ctx, const char *line)
{
    size_t *const written = ctx;
    char *const text = companion.buffer->details;
    const size_t left = sizeof(companion.buffer->details) - *written;
    const size_t count = strnlen(line, left);
    if (left < 2 || count >= left - 1) return;
    memcpy(text + *written, line, count);
    *written += count;
    text[(*written)++] = '\n';
    text[*written] = '\0';
}

static const char *const diag_tab_intents[] = {
    "companion.status.framework", "companion.status.net", "companion.status.platform",
    "companion.status.media", "companion.status.settings", "companion.status.tasks",
};
_Static_assert(COUNT(diag_tab_intents) == COUNT(diagnostic_names) &&
               COUNT(diag_tab_intents) < XIAOTAI_VIEW_ACTIONS_MAX, "diagnostic tabs and BACK fit");

static void diagnostics(xiaotai_view_t *view, int32_t arg)
{
    if (!diag_arg(arg)) return;
    view->kind = XIAOTAI_VIEW_NOTICE;
    view->status = diagnostic_labels[arg];
    companion.buffer->details[0] = '\0';
    size_t written = 0;
    const xiaotai_reply_t sink = {.write = append_report, .ctx = &written};
    xiaotai_diag_report(diagnostic_names[arg], &sink);
    view->text = written ? companion.buffer->details : "暂无信息";
    for (size_t i = 0; i < COUNT(diag_tab_intents); ++i)
        companion.buffer->actions[view->action_count++] = (xiaotai_view_action_t){.label = diagnostic_labels[i],
            .intent = diag_tab_intents[i], .value = (int32_t)i, .state = (size_t)arg == i ? XIAOTAI_STATE_CHECKED : 0};
    back(view);
}

static void tab(const xiaotai_args_t *args, void *unused)
{
    (void)unused;
    if (!companion_on_page(XIAOTAI_APP_COMPANION_PAGE_STATUS)) { ++companion.status.stale; return; }
    if (!diag_arg(args->value)) { companion_refuse(XIAOTAI_APP_COMPANION_PAGE_STATUS, XIAOTAI_ERR_INVALID_ARG); return; }
    /* Switching tabs doesn't push more pages onto the stack. */
    companion.stack[companion.depth - 1].arg = args->value;
    companion_render();
}

static void tick(void *unused)
{
    (void)unused;
    timer = 0;
    if (companion_on_page(XIAOTAI_APP_COMPANION_PAGE_NETWORK) || companion_on_page(XIAOTAI_APP_COMPANION_PAGE_STATUS)) {
        companion_render();
        companion_aux_visible();
    }
}

void companion_aux_visible(void)
{
    if (timer) { xiaotai_timer_cancel(timer); timer = 0; }
    if (companion_on_page(XIAOTAI_APP_COMPANION_PAGE_NETWORK) || companion_on_page(XIAOTAI_APP_COMPANION_PAGE_STATUS))
        (void)xiaotai_timer_start(1000, tick, NULL, &timer);
}

xiaotai_err_t companion_aux_init(void)
{
    if (!companion.has_display) return XIAOTAI_OK;
    static const companion_page_t pages[] = {
        {.id = XIAOTAI_APP_COMPANION_PAGE_EMOTIONS, .title = "表情", .build = emotion_list},
        {.id = XIAOTAI_APP_COMPANION_PAGE_EMOTION, .title = "表情预览", .build = emotion_preview, .arg_valid = emotion_arg},
        {.id = XIAOTAI_APP_COMPANION_PAGE_NETWORK, .title = "网络信息", .build = network, .icon = XIAOTAI_ICON_WIFI},
        {.id = XIAOTAI_APP_COMPANION_PAGE_STATUS, .title = "运行状态", .build = diagnostics, .arg_valid = diag_arg},
    };
    xiaotai_err_t err = XIAOTAI_OK;
    for (size_t i = 0; err == XIAOTAI_OK && i < COUNT(pages); ++i) err = companion_page_register(&pages[i]);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle_ex(XIAOTAI_INTENT_APP_COMPANION_EMOTION, choose_emotion, NULL, XIAOTAI_INTENT_LOCAL);
    if (err == XIAOTAI_OK) err = xiaotai_intent_handle_ex(XIAOTAI_INTENT_APP_COMPANION_EMOTION_APPLY, store_emotion, NULL, XIAOTAI_INTENT_LOCAL);
    for (size_t i = 0; err == XIAOTAI_OK && i < COUNT(diag_tab_intents); ++i)
        err = xiaotai_intent_handle_ex(diag_tab_intents[i], tab, NULL, XIAOTAI_INTENT_LOCAL);
    return err;
}

void companion_aux_reset(void) { timer = 0; }
