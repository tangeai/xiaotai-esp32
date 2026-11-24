// SPDX-License-Identifier: MIT
#include "companion_internal.h"

#include <stdio.h>
#include <string.h>
#include "xiaotai/modules/binding.h"
#include "xiaotai/modules/provisioning.h"
#include "xiaotai/platform.h"
#include "xiaotai/port/mem.h"

typedef enum { TAKE_NONE, TAKE_MAINTENANCE, TAKE_NETWORK, TAKE_WIFI, TAKE_BINDING, TAKE_SERVICE, TAKE_AUDIO } takeover_t;
typedef enum { RETRY_NONE, RETRY_PROVISIONING, RETRY_SERVICE, RETRY_BINDING, REPLAY_BINDING } setup_action_t;

typedef struct {
    takeover_t page;
    setup_action_t primary;
    const char *message;
    const char *title;
    bool online, offer_begin, offer_cancel, busy, percent, room_navigation;
    uint8_t progress, binding_retry_control;
    uint32_t seconds;
    char code[XIAOTAI_BINDING_CODE_MAX + 1U];
    char ssid[XIAOTAI_NET_SSID_MAX + 1U], url[XIAOTAI_NET_URL_MAX], error[16];
} setup_view_t;

typedef struct {
    xiaotai_provisioning_status_t provision;
    xiaotai_binding_status_t binding;
    xiaotai_platform_status_t platform;
} setup_snapshots_t;

static setup_snapshots_t *s_snapshots;
static setup_view_t s_view;
static bool s_ever_online, s_services_ready;
static const char *s_maintenance;

static void copy(char *dst, size_t capacity, const char *src)
{
    const size_t count = strnlen(src, capacity - 1U);
    memcpy(dst, src, count);
    dst[count] = '\0';
}

static bool keys(void) { return !strcmp(companion.style->name, XIAOTAI_APP_COMPANION_STYLE_KEYS); }

static bool key_cancel_available(void)
{
    return companion.config.provisioning_cancel_from_keys && keys() &&
           xiaotai_quiesced_for(XIAOTAI_QUIESCE_PROVISIONING) && xiaotai_provisioning_begin_cancellable();
}

static void format_error(setup_view_t *out, xiaotai_err_t error)
{
    if (error != XIAOTAI_OK) snprintf(out->error, sizeof(out->error), "ERR %04X", (unsigned)error & 0xffffU);
}

static const char *maintenance_text(const char *reason)
{
    if (!strcmp(reason, XIAOTAI_QUIESCE_ENVIRONMENT)) return "正在切换服务环境";
    if (!strcmp(reason, XIAOTAI_QUIESCE_PROVISIONING)) return "正在准备重新配网";
    if (!strcmp(reason, XIAOTAI_QUIESCE_BINDING)) return "正在更新绑定";
    if (!strcmp(reason, XIAOTAI_QUIESCE_COPROCESSOR)) return "正在维护联网模块";
    if (!strcmp(reason, "network") || !strcmp(reason, "intent")) return "设备即将重启";
    return "请稍候";
}

static void network_step(setup_view_t *out, const xiaotai_net_status_t *net)
{
    const xiaotai_net_prepare_t *step = &net->prepare;
    out->page = TAKE_NETWORK;
    out->title = "联网模块";
    if (net->state == XIAOTAI_NET_BLOCKED) {
        if (!step->retry_blocked) out->primary = RETRY_PROVISIONING;
        switch (step->fault) {
            case XIAOTAI_NET_FAULT_INTERRUPTED: out->message = "上次更新未完成"; break;
            case XIAOTAI_NET_FAULT_BAD_IMAGE: out->message = "联网模块镜像无效"; break;
            case XIAOTAI_NET_FAULT_UNSUPPORTED: out->message = "不支持自动升级"; break;
            case XIAOTAI_NET_FAULT_POWER_CYCLE: out->message = "请断电后重新启动"; break;
            default: out->message = "联网模块未就绪"; break;
        }
        format_error(out, step->error);
        return;
    }
    switch (step->stage) {
        case XIAOTAI_NET_PREPARE_CHECKING: out->message = "正在检查联网模块"; break;
        case XIAOTAI_NET_PREPARE_VERIFYING: out->message = "正在校验联网模块镜像"; break;
        case XIAOTAI_NET_PREPARE_WRITING: out->message = "正在更新联网模块"; break;
        case XIAOTAI_NET_PREPARE_ACTIVATING: out->message = "正在激活联网模块"; break;
        case XIAOTAI_NET_PREPARE_RESTARTING: out->message = "设备即将重启"; break;
        case XIAOTAI_NET_PREPARE_READY: out->message = "联网模块已就绪"; break;
        default: out->message = "正在准备联网模块"; break;
    }
    out->percent = step->stage >= XIAOTAI_NET_PREPARE_CHECKING && step->stage <= XIAOTAI_NET_PREPARE_READY;
    out->progress = step->percent <= 100U ? step->percent : 100U;
}

static bool can_cancel(const xiaotai_provisioning_status_t *s)
{
    return !s->exclusive && !s->restart_pending && !s->link.control_pending &&
           (s->phase == XIAOTAI_PROVISIONING_WAITING || s->phase == XIAOTAI_PROVISIONING_REJECTED) &&
           xiaotai_net_has_saved_credentials();
}

static void wifi_step(setup_view_t *out, const xiaotai_provisioning_status_t *s)
{
    out->page = TAKE_WIFI;
    out->online = s->link.state == XIAOTAI_NET_UP;
    const bool open = s->provision.ap_ssid[0] != '\0';
    const xiaotai_err_t err = s->error ? s->error : s->link.control_error;
    copy(out->ssid, sizeof(out->ssid), s->provision.ap_ssid);
    copy(out->url, sizeof(out->url), s->provision.url);
    out->offer_cancel = can_cancel(s);
    out->busy = s->link.control_pending || s->phase == XIAOTAI_PROVISIONING_STARTING ||
                s->phase == XIAOTAI_PROVISIONING_VERIFYING || s->phase == XIAOTAI_PROVISIONING_STORING ||
                s->phase == XIAOTAI_PROVISIONING_DONE || s->phase == XIAOTAI_PROVISIONING_RESTARTING;
    out->offer_begin = !open && !out->busy;
    switch (s->phase) {
        case XIAOTAI_PROVISIONING_STARTING: out->message = "正在开启热点配网"; break;
        case XIAOTAI_PROVISIONING_WAITING: out->message = open ? "手机连接设备热点" : "等待提交 Wi-Fi 配置"; break;
        case XIAOTAI_PROVISIONING_VERIFYING: out->message = "正在验证 Wi-Fi 连接"; break;
        case XIAOTAI_PROVISIONING_REJECTED:
            out->message = "连接失败，请重试";
            if (s->link.state == XIAOTAI_NET_WAITING || s->link.state == XIAOTAI_NET_HELD) {
                out->primary = RETRY_PROVISIONING;
            }
            format_error(out, s->provision.error);
            break;
        case XIAOTAI_PROVISIONING_STORING: out->message = "正在保存 Wi-Fi 设置"; break;
        case XIAOTAI_PROVISIONING_STORE_FAILED:
            out->message = "Wi-Fi 设置未保存，请重试";
            out->primary = RETRY_PROVISIONING;
            format_error(out, s->provision.error);
            break;
        case XIAOTAI_PROVISIONING_DONE:
            if (s->packed_retry_required) {
                out->message = "设备设置未保存，请重试";
                out->primary = RETRY_PROVISIONING;
                format_error(out, s->error);
            } else {
                out->message = s->restart_pending ? "Wi-Fi 已保存，等待重启" : "Wi-Fi 已连接";
            }
            break;
        case XIAOTAI_PROVISIONING_RESTARTING: out->message = "设备即将重启"; break;
        default:
            if (s->link.state == XIAOTAI_NET_WAITING || s->link.state == XIAOTAI_NET_HELD) out->primary = RETRY_PROVISIONING;
            out->message = s->link.failing ? "正在重连已保存的 Wi-Fi" : "正在连接 Wi-Fi";
            break;
    }
    if (err && (s->phase == XIAOTAI_PROVISIONING_NONE || s->phase == XIAOTAI_PROVISIONING_STARTING ||
                s->phase == XIAOTAI_PROVISIONING_WAITING)) {
        out->message = out->online ? "断开失败，请重试" : "热点启动失败，请点击热点配网重试";
    }
}

static bool binding_code_visible(const xiaotai_binding_status_t *status)
{
    if (!status->verification[0] || !status->seconds_left) return false;
    if (!companion.config.binding_guide_six_digits) return true;
    for (size_t i = 0; i < 6U; ++i) {
        if (status->verification[i] < '0' || status->verification[i] > '9') return false;
    }
    return status->verification[6] == '\0';
}

static void binding_step(setup_view_t *out, const xiaotai_binding_status_t *s)
{
    out->page = TAKE_BINDING;
    switch (s->phase) {
        case XIAOTAI_BINDING_WAITING:
            out->message = "等待绑定";
            if (binding_code_visible(s)) {
                copy(out->code, sizeof(out->code), s->verification);
                out->seconds = s->seconds_left;
                out->primary = REPLAY_BINDING;
            }
            break;
        case XIAOTAI_BINDING_EXPIRED:
            if (companion.config.binding_retry_control == XIAOTAI_COMPANION_BINDING_RETRY_REFRESH) {
                out->binding_retry_control = XIAOTAI_COMPANION_BINDING_RETRY_REFRESH;
                out->message = "验证码已过期，请点击刷新";
            } else out->message = "验证码已过期";
            out->primary = RETRY_BINDING;
            break;
        case XIAOTAI_BINDING_FAILED:
            out->binding_retry_control = companion.config.binding_retry_control;
            if (!out->binding_retry_control && s->environment_boot_failed)
                out->binding_retry_control = XIAOTAI_COMPANION_BINDING_RETRY_REFRESH;
            switch (out->binding_retry_control) {
                case XIAOTAI_COMPANION_BINDING_RETRY_HEADER: out->message = "绑定未完成，请点击右上角重试"; break;
                case XIAOTAI_COMPANION_BINDING_RETRY_REFRESH: out->message = "绑定未完成，请点击刷新"; break;
                default: out->message = "绑定未完成"; break;
            }
            out->primary = RETRY_BINDING;
            break;
        case XIAOTAI_BINDING_STORING: case XIAOTAI_BINDING_CONFIRMING:
            out->message = "正在验证授权";
            break;
        default:
            out->message = "正在获取验证码";
            break;
    }
}

static bool service_preparing(void)
{
    for (size_t i = 0; xiaotai_app_companion_parts[i]; ++i) {
        const xiaotai_app_companion_part_t *part = xiaotai_app_companion_parts[i];
        if (part->service_preparing && part->service_preparing()) return true;
    }
    return false;
}

static void service_step(setup_view_t *out, const xiaotai_platform_status_t *s, bool rtc)
{
    out->page = TAKE_SERVICE;
    out->title = "设备初始化";
    if (s->phase == XIAOTAI_PLATFORM_PAUSED) {
        out->primary = RETRY_SERVICE;
        out->title = keys() ? "操作未完成" : out->title;
        out->message = "初始化失败";
        format_error(out, s->last_error);
    } else if (rtc && s->phase == XIAOTAI_PLATFORM_ONLINE) {
        out->message = "正在初始化通话服务";
    } else if (s->phase == XIAOTAI_PLATFORM_LOGIN || s->phase == XIAOTAI_PLATFORM_CONNECTING) {
        out->message = "正在连接设备服务";
    } else {
        out->message = "正在准备设备服务";
    }
}

static bool defer_offline(void)
{
    for (size_t i = 0; xiaotai_app_companion_parts[i]; ++i) {
        const xiaotai_app_companion_part_t *part = xiaotai_app_companion_parts[i];
        if (part->defer_offline_setup && part->defer_offline_setup()) return true;
    }
    return false;
}

bool companion_room_maintenance(void)
{
    return companion.config.room_navigation_during_environment &&
        xiaotai_quiesced_for(XIAOTAI_QUIESCE_ENVIRONMENT) &&
        companion_page_exists(XIAOTAI_APP_COMPANION_PAGE_ROOM);
}

static void classify(setup_view_t *out, bool rtc)
{
    const xiaotai_provisioning_status_t *prov = &s_snapshots->provision;
    const xiaotai_binding_status_t *binding = &s_snapshots->binding;
    const xiaotai_platform_status_t *platform = &s_snapshots->platform;
    const bool online = prov->link.state == XIAOTAI_NET_UP;
    const bool session = xiaotai_activity_active();
    const xiaotai_err_t storage = xiaotai_storage_boot_error();
    if (storage != XIAOTAI_OK) {
        out->page = TAKE_MAINTENANCE;
        out->title = "存储错误";
        out->message = "设备仅可本地使用";
        format_error(out, storage);
    } else if (prov->packed_retry_required) {
        wifi_step(out, prov);
    } else if (xiaotai_quiesced()) {
        out->page = TAKE_MAINTENANCE;
        out->message = s_maintenance ? s_maintenance : "请稍候";
        out->room_navigation = companion_room_maintenance();
        out->offer_cancel = key_cancel_available();
        if (out->offer_cancel && prov->error != XIAOTAI_OK) {
            out->message = "配网准备失败";
            format_error(out, prov->error);
        }
    } else if (companion_feedback_audio_error()) {
        out->page = TAKE_AUDIO;
        out->message = "音频设备未就绪";
    } else if (prov->link.state == XIAOTAI_NET_PREPARING || prov->link.state == XIAOTAI_NET_BLOCKED) {
        network_step(out, &prov->link);
    } else if (!(online && binding->phase == XIAOTAI_BINDING_BOUND && session)) {
        if (prov->phase != XIAOTAI_PROVISIONING_NONE || (!online && !session)) {
            if (prov->phase != XIAOTAI_PROVISIONING_NONE || !defer_offline()) wifi_step(out, prov);
        } else if (binding->phase != XIAOTAI_BINDING_BOUND &&
                   (binding->phase != XIAOTAI_BINDING_UNKNOWN || !binding->device_id[0])) {
            binding_step(out, binding);
            out->online = online;
        } else if (!s_services_ready && (!s_ever_online || rtc)) service_step(out, platform, rtc);
    }
}

static void action(xiaotai_view_t *view, const char *label, const char *intent, int32_t value, uint8_t role)
{
    companion.buffer->actions[view->action_count++] = (xiaotai_view_action_t){
        .label = label, .intent = intent, .value = value, .role = role,
    };
    view->actions = companion.buffer->actions;
}

static void primary(xiaotai_view_t *view)
{
    const char *retry = keys() ? "双击重试" : "重试";
    switch (s_view.primary) {
        case RETRY_PROVISIONING: action(view, retry, XIAOTAI_INTENT_PROVISIONING_RETRY, 0, XIAOTAI_ROLE_PRIMARY); break;
        case RETRY_SERVICE: action(view, retry, XIAOTAI_INTENT_APP_COMPANION_SESSION, 0, XIAOTAI_ROLE_PRIMARY); break;
        case RETRY_BINDING:
            action(view, s_view.binding_retry_control == XIAOTAI_COMPANION_BINDING_RETRY_REFRESH ? "刷新验证码" : retry,
                   XIAOTAI_INTENT_BINDING_RETRY, 0, XIAOTAI_ROLE_PRIMARY);
            if (s_view.binding_retry_control == XIAOTAI_COMPANION_BINDING_RETRY_HEADER)
                companion.buffer->actions[view->action_count - 1U].icon = XIAOTAI_ICON_REFRESH;
            break;
        case REPLAY_BINDING: action(view, "重播验证码", XIAOTAI_INTENT_BINDING_ANNOUNCE, 0, XIAOTAI_ROLE_PRIMARY); break;
        default: break;
    }
}

static void row(xiaotai_view_t *view, const char *label, const char *detail)
{
    companion.buffer->items[view->item_count++] = (xiaotai_view_item_t){.label = label, .detail = detail};
    view->items = companion.buffer->items;
}

static void build_progress(xiaotai_view_t *view, int32_t arg)
{
    (void)arg;
    view->kind = XIAOTAI_VIEW_PROGRESS;
    if (s_view.title) view->title = s_view.title;
    view->text = s_view.message;
    view->status = s_view.error[0] ? s_view.error : NULL;
    if (s_view.percent) {
        view->flags |= XIAOTAI_VIEW_PERCENT;
        view->progress = s_view.progress;
    }
    primary(view);
    if (s_view.page == TAKE_MAINTENANCE && s_view.offer_cancel)
        action(view, "长按或双击取消", XIAOTAI_INTENT_APP_COMPANION_BACK, COMPANION_SETUP_BACK, XIAOTAI_ROLE_PRIMARY);
}

static void build_wifi(xiaotai_view_t *view, int32_t arg)
{
    (void)arg;
    view->kind = XIAOTAI_VIEW_NOTICE;
    view->text = s_view.message;
    view->status = s_view.error[0] ? s_view.error : NULL;
    if (keys()) {
        /* A key-operated NOTICE must leave confirm on PRIMARY; these lines
         * are information, not selectable rows. */
        if (s_view.ssid[0]) {
            view->title = "无线网络";
            view->subtitle = s_view.ssid;
        }
        if (!view->status && s_view.url[0]) view->status = s_view.url;
    } else {
        /* Touch screens spell the hotspot set-up out as three steps; the
         * current state is already the page text above them. */
        if (s_view.ssid[0] || s_view.url[0]) view->subtitle = "用手机按下面三步配网";
        if (s_view.ssid[0]) row(view, "1. 连接热点", s_view.ssid);
        if (s_view.url[0]) row(view, "2. 打开网址", s_view.url);
        if (s_view.ssid[0] || s_view.url[0]) row(view, "3. 选择网络", "家里的 Wi-Fi 和密码");
    }
    if (s_view.offer_begin) {
        action(view, s_view.online ? "断开当前连接" : "开启热点配网", XIAOTAI_INTENT_PROVISIONING_BEGIN,
               XIAOTAI_PROVISIONING_PREEMPT, s_view.primary == RETRY_NONE ? XIAOTAI_ROLE_PRIMARY : XIAOTAI_ROLE_NONE);
    }
    primary(view);
    if (s_view.offer_cancel) action(view, "取消", XIAOTAI_INTENT_PROVISIONING_CANCEL, 0, XIAOTAI_ROLE_BACK);
}

static void build_binding(xiaotai_view_t *view, int32_t arg)
{
    (void)arg;
    view->kind = XIAOTAI_VIEW_CODE;
    view->text = s_view.code[0] ? s_view.code : "------";
    view->subtitle = companion.config.binding_url ? companion.config.binding_url : "微信搜索小钛";
    view->status = s_view.online ? "网络已连接" : NULL;
    if (s_view.code[0]) {
        view->timer_mode = XIAOTAI_TIMER_REMAINING;
        view->timer_s = s_view.seconds > INT32_MAX ? INT32_MAX : (int32_t)s_view.seconds;
    }
    if (keys()) {
        view->title = "设备绑定";
        view->status = s_view.message;
    } else {
        row(view, s_view.message, NULL);
        row(view, companion.config.binding_url ? "用手机打开网址，输入验证码" : "打开小程序，输入验证码绑定", NULL);
    }
    primary(view);
    if (s_view.online) action(view, "更换网络", XIAOTAI_INTENT_PROVISIONING_BEGIN, XIAOTAI_PROVISIONING_PREEMPT, XIAOTAI_ROLE_NONE);
}

static const companion_page_t s_pages[] = {
    [TAKE_MAINTENANCE] = {.id = XIAOTAI_APP_COMPANION_PAGE_MAINTENANCE, .title = "维护中", .build = build_progress},
    [TAKE_NETWORK] = {.id = XIAOTAI_APP_COMPANION_PAGE_PREPARING, .title = "联网模块", .build = build_progress},
    [TAKE_WIFI] = {.id = XIAOTAI_APP_COMPANION_PAGE_SETUP, .title = "连接 Wi-Fi", .build = build_wifi},
    [TAKE_BINDING] = {.id = XIAOTAI_APP_COMPANION_PAGE_BINDING, .title = "绑定设备", .build = build_binding},
    [TAKE_SERVICE] = {.id = XIAOTAI_APP_COMPANION_PAGE_PREPARING, .title = "设备初始化", .build = build_progress},
    [TAKE_AUDIO] = {.id = XIAOTAI_APP_COMPANION_PAGE_MAINTENANCE, .title = "音频错误", .build = build_progress},
};

const companion_page_t *companion_setup_page(void)
{
    /* The room owner may render on release before the setup event arrives. */
    const bool room_navigation = xiaotai_quiesced() ? companion_room_maintenance() : s_view.room_navigation;
    if (room_navigation && companion_room_navigation_here()) return NULL;
    return s_view.page != TAKE_NONE ? &s_pages[s_view.page] : NULL;
}

void companion_setup_visible(void)
{
    if (!companion.has_display || !companion.status.setup) return;
    companion_home_interaction();
    companion_user_activity();
}

bool companion_setup_claim(const xiaotai_event_t *event)
{
    if (!s_snapshots) return false;
    /* Only a current, actionable change may bring a background app forward. */
    if (!strcmp(event->name, XIAOTAI_EVENT_PROVISIONING_CHANGED)) {
        if (!event->data || event->size != sizeof(s_snapshots->provision)) return false;
        const xiaotai_provisioning_status_t *reported = event->data;
        xiaotai_provisioning_status(&s_snapshots->provision);
        const xiaotai_provisioning_status_t *current = &s_snapshots->provision;
        return reported->revision == current->revision && reported->phase == current->phase &&
            current->phase != XIAOTAI_PROVISIONING_NONE &&
            (current->phase != XIAOTAI_PROVISIONING_DONE || current->packed_retry_required) &&
            current->phase != XIAOTAI_PROVISIONING_RESTARTING;
    }
    const bool code = !strcmp(event->name, XIAOTAI_EVENT_BINDING_CODE);
    if (!code && strcmp(event->name, XIAOTAI_EVENT_BINDING_CHANGED)) return false;
    xiaotai_binding_status(&s_snapshots->binding);
    const xiaotai_binding_status_t *current = &s_snapshots->binding;
    if (current->phase != XIAOTAI_BINDING_WAITING || !binding_code_visible(current))
        return false;
    if (code) return event->args.value > 0 &&
        !strncmp(event->args.text, current->verification, sizeof(current->verification));
    if (!event->data || event->size != sizeof(*current)) return false;
    const xiaotai_binding_status_t *reported = event->data;
    return reported->phase == current->phase && reported->revision == current->revision &&
           reported->generation == current->generation;
}

static void refresh(bool keep_settings)
{
    if (!s_snapshots) return;
    xiaotai_provisioning_status(&s_snapshots->provision);
    /* Net preparation can post its own progress without transitioning the
     * provisioning reducer; read the freshest authoritative link snapshot. */
    xiaotai_net_status(&s_snapshots->provision.link);
    xiaotai_binding_status(&s_snapshots->binding);
    xiaotai_platform_status(&s_snapshots->platform);
    const bool rtc = service_preparing();
    if (s_snapshots->platform.phase == XIAOTAI_PLATFORM_ONLINE) {
        s_ever_online = true;
        if (!rtc) s_services_ready = true;
    }
    setup_view_t next;
    memset(&next, 0, sizeof(next));
    classify(&next, rtc);
    memset(s_snapshots, 0, sizeof(*s_snapshots));
    if (!memcmp(&s_view, &next, sizeof(next))) return;
    const takeover_t before = s_view.page;
    const bool restore_room_page = s_view.room_navigation && companion_room_navigation_here();
    const bool page_changed = before != next.page || s_view.room_navigation != next.room_navigation;
    memcpy(&s_view, &next, sizeof(s_view));
    companion.status.setup = next.page != TAKE_NONE;
    if (page_changed) {
        if (companion.status.foreground) {
            companion_session_refresh_context();
            companion_setup_visible();
        }
        if (next.page == TAKE_NONE && !restore_room_page && companion_finish_presentation()) return;
        if (next.page != TAKE_NONE || restore_room_page ||
            (keep_settings && companion_on_page(XIAOTAI_APP_COMPANION_PAGE_SETTINGS)) ||
            !companion_reset_home(0)) companion_presentation_changed();
    } else {
        companion_render();
    }
}

void companion_setup_refresh(void) { refresh(false); }

bool companion_setup_cancel_from_key(void)
{
    /* A key can reach the owner after the view was rendered. Never let that
     * cancel fall through to closing a portal the engine now owns. */
    if (s_view.page != TAKE_MAINTENANCE || !s_view.offer_cancel || !key_cancel_available()) return false;
    const xiaotai_err_t err = xiaotai_provisioning_cancel();
    if (err != XIAOTAI_OK) {
        companion_refuse(XIAOTAI_INTENT_APP_COMPANION_BACK, err);
    } else {
        companion_settings_reprovision_cancelled();
        refresh(true);
    }
    return true;
}

static void changed(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    if (!strcmp(event->name, XIAOTAI_EVENT_QUIESCE)) s_maintenance = event->args.value ? maintenance_text(event->args.text) : NULL;
    if (!strcmp(event->name, XIAOTAI_EVENT_PLATFORM_ONLINE)) s_ever_online = true;
    companion_setup_refresh();
}

bool companion_setup_can_retry(void) { return s_view.primary == RETRY_SERVICE; }

xiaotai_err_t companion_setup_init(void)
{
    static const xiaotai_subscriber_descriptor_t events[] = {
        {XIAOTAI_EVENT_STARTED, changed, NULL},
        {XIAOTAI_EVENT_QUIESCE, changed, NULL},
        {XIAOTAI_EVENT_ACTIVITY, changed, NULL},
        {XIAOTAI_EVENT_NET_STATE, changed, NULL},
        {XIAOTAI_EVENT_NET_PREPARE, changed, NULL},
        {XIAOTAI_EVENT_NET_UP, changed, NULL},
        {XIAOTAI_EVENT_NET_DOWN, changed, NULL},
        {XIAOTAI_EVENT_NET_CREDENTIALS_CHANGED, changed, NULL},
        {XIAOTAI_EVENT_PLATFORM_CLOCK, changed, NULL},
        {XIAOTAI_EVENT_PLATFORM_DISCOVERED, changed, NULL},
        {XIAOTAI_EVENT_PLATFORM_ONLINE, changed, NULL},
        {XIAOTAI_EVENT_PLATFORM_OFFLINE, changed, NULL},
        {XIAOTAI_EVENT_PLATFORM_PAUSED, changed, NULL},
        {XIAOTAI_EVENT_PROVISIONING_CHANGED, changed, NULL},
        {XIAOTAI_EVENT_BINDING_CHANGED, changed, NULL},
        {XIAOTAI_EVENT_BINDING_CODE, changed, NULL},
    };
    s_snapshots = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1U, sizeof(*s_snapshots));
    if (!s_snapshots) return XIAOTAI_ERR_NO_MEM;
    xiaotai_err_t err = XIAOTAI_OK;
    for (size_t n = 0; err == XIAOTAI_OK && n < sizeof(events) / sizeof(events[0]); ++n) {
        err = xiaotai_event_subscribe_static(&events[n]);
    }
    if (err != XIAOTAI_OK) companion_setup_reset();
    return err;
}

void companion_setup_reset(void)
{
    xiaotai_mem_free(s_snapshots);
    s_snapshots = NULL;
    memset(&s_view, 0, sizeof(s_view));
    s_maintenance = NULL;
    s_ever_online = s_services_ready = false;
}
