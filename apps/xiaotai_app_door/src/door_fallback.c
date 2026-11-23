// SPDX-License-Identifier: MIT
#include "door_internal.h"

#include "xiaotai/feedback.h"
#include "xiaotai/modules/binding.h"
#include "xiaotai/modules/provisioning.h"
#include "xiaotai/net.h"
#include "xiaotai/port/os.h"
#include "xiaotai/ui.h"
#ifdef XIAOTAI_DOOR_WITH_WECHAT
#include "xiaotai/modules/wechat.h"
#endif

#include <string.h>

#define AI_STATE "ai_talk.state"
#define TALK_STOP "talk.stop"
#define REPLAY "door.fallback.replay"
#define BINDING_PAGE "door.binding"
#define ANNOUNCE_WAIT_MS 100U

typedef enum {CALL_INCOMING, CALL_CONNECTING, CALL_OUTGOING, CALL_ACTIVE} phase_t;
typedef enum {SETUP_NONE, SETUP_PREPARE, SETUP_WIFI, SETUP_BINDING} setup_t;
typedef struct {
    const char *accept, *reject, *hangup, *mute;
} call_actions_t;

static struct {
    xiaotai_net_prepare_t prepare;
    xiaotai_provisioning_phase_t provision;
    xiaotai_binding_phase_t binding;
    xiaotai_call_status_t device_result;
    char code[XIAOTAI_BINDING_CODE_MAX + 1U];
    uint32_t binding_generation, code_token, announce_timer;
    int64_t announce_deadline;
    bool announced, wechat_result, environment_boot_failed;
#ifdef XIAOTAI_DOOR_WITH_WECHAT
    xiaotai_wechat_status_t wechat;
#endif
} fallback;

static void update_rings(void)
{
#ifdef XIAOTAI_DOOR_WITH_WECHAT
    if (!door.session && fallback.wechat.session) {
        const bool show = door.foreground && door.page == DOOR_OTHER_CALL && !door.setup;
        (void)xiaotai_feedback_set(XIAOTAI_APP_DOOR_STATE_INCOMING, show && fallback.wechat.state == XIAOTAI_WECHAT_INCOMING);
        (void)xiaotai_feedback_set(XIAOTAI_APP_DOOR_STATE_OUTGOING, show && fallback.wechat.outgoing &&
                                  (fallback.wechat.state == XIAOTAI_WECHAT_OUTGOING ||
                                   fallback.wechat.state == XIAOTAI_WECHAT_CONNECTING));
        return;
    }
#endif
    door_call_ring_update();
}

static setup_t setup_source(void)
{
    if (fallback.prepare.stage != XIAOTAI_NET_PREPARE_NONE && fallback.prepare.stage != XIAOTAI_NET_PREPARE_READY)
        return SETUP_PREPARE;
    if (fallback.provision != XIAOTAI_PROVISIONING_NONE) return SETUP_WIFI;
    if (fallback.environment_boot_failed) {
        xiaotai_net_status_t net;
        xiaotai_net_status(&net);
        if (net.state != XIAOTAI_NET_UP) return SETUP_WIFI;
    }
    return fallback.binding >= XIAOTAI_BINDING_NEEDED ? SETUP_BINDING : SETUP_NONE;
}

static bool code_valid(const xiaotai_binding_status_t *status)
{
    return status->phase == XIAOTAI_BINDING_WAITING && status->seconds_left && status->verification[0] &&
           memchr(status->verification, '\0', sizeof(status->verification));
}

static void cancel_announce(void)
{
    if (fallback.announce_timer) xiaotai_timer_cancel(fallback.announce_timer);
    fallback.announce_timer = 0;
}

static void remember_code(const xiaotai_binding_status_t *status)
{
    if (!code_valid(status)) {
        cancel_announce();
        fallback.code[0] = '\0';
        fallback.code_token = 0;
        return;
    }
    if (fallback.code_token && fallback.binding_generation == status->generation &&
        strcmp(status->verification, fallback.code) == 0) return;
    cancel_announce();
    fallback.binding_generation = status->generation;
    memcpy(fallback.code, status->verification, sizeof(fallback.code));
    fallback.code_token = xiaotai_generation_next();
    fallback.announced = false;
    const int64_t now = xiaotai_time_us(), span = (int64_t)status->seconds_left * 1000000;
    fallback.announce_deadline = now > INT64_MAX - span ? INT64_MAX : now + span;
}

static bool current_code(xiaotai_binding_status_t *status)
{
    if (!door.foreground || door.page != DOOR_SETUP || setup_source() != SETUP_BINDING || !fallback.code_token)
        return false;
    xiaotai_binding_status(status);
    return code_valid(status) && status->generation == fallback.binding_generation &&
           strcmp(status->verification, fallback.code) == 0;
}

static void try_announce(void);

static void announce_tick(void *ctx)
{
    if ((uint32_t)(uintptr_t)ctx != fallback.code_token) return;
    fallback.announce_timer = 0;
    try_announce();
}

static void try_announce(void)
{
    xiaotai_binding_status_t status;
    if (!door_config.announce_on_code || fallback.announce_timer || fallback.announced || xiaotai_quiesced() ||
        xiaotai_time_us() >= fallback.announce_deadline || !current_code(&status)) return;
    if (status.announcing) {
        fallback.announced = true;
        return;
    }
    if (!xiaotai_feedback_sound_pending() && xiaotai_binding_announce() != XIAOTAI_ERR_BUSY) {
        fallback.announced = true;
        return;
    }
    // Wait only within this code's original display window.
    (void)xiaotai_timer_start(ANNOUNCE_WAIT_MS, announce_tick, (void *)(uintptr_t)fallback.code_token,
                             &fallback.announce_timer);
}

static void replay(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    xiaotai_binding_status_t status;
    if (!args || (uint32_t)args->value != fallback.code_token || !current_code(&status)) return;
    if (xiaotai_quiesced() || xiaotai_feedback_sound_pending() || xiaotai_binding_announce() != XIAOTAI_OK) {
        xiaotai_view_toast("播报未受理，请重试", 0);
        return;
    }
    fallback.announced = true;
    cancel_announce();
}

static void render_binding(void)
{
    xiaotai_view_t view = {.id = BINDING_PAGE, .title = "正在准备绑定", .kind = XIAOTAI_VIEW_PROGRESS};
    xiaotai_view_action_t actions[2];
    xiaotai_binding_status_t status;
    if (fallback.environment_boot_failed) {
        static const xiaotai_view_item_t failed = {.label = "绑定未完成，请点击刷新"};
        static const xiaotai_view_item_t waiting = {.label = "正在获取验证码"};
        view.kind = XIAOTAI_VIEW_CODE;
        view.title = "绑定设备";
        view.text = "------";
        view.status = "Wi-Fi 已连接";
        view.items = fallback.binding == XIAOTAI_BINDING_FAILED ? &failed : &waiting;
        view.item_count = 1;
        if (fallback.binding == XIAOTAI_BINDING_FAILED) {
            actions[view.action_count++] = (xiaotai_view_action_t){.label = "刷新验证码",
                .intent = XIAOTAI_INTENT_BINDING_RETRY, .role = XIAOTAI_ROLE_PRIMARY};
        }
        actions[view.action_count++] = (xiaotai_view_action_t){.label = "更换网络",
            .intent = XIAOTAI_INTENT_PROVISIONING_BEGIN, .value = XIAOTAI_PROVISIONING_PREEMPT};
        view.actions = actions;
    } else if (current_code(&status)) {
        actions[0] = (xiaotai_view_action_t){.label = "重播", .intent = REPLAY, .icon = XIAOTAI_ICON_VOLUME,
                                           .role = XIAOTAI_ROLE_PRIMARY, .value = (int32_t)fallback.code_token};
        view.kind = XIAOTAI_VIEW_CODE;
        view.title = "绑定设备";
        view.text = fallback.code;
        view.timer_s = status.seconds_left > INT32_MAX ? INT32_MAX : (int32_t)status.seconds_left;
        view.timer_mode = XIAOTAI_TIMER_REMAINING;
        view.actions = actions;
        view.action_count = 1;
    } else if (fallback.binding == XIAOTAI_BINDING_FAILED || fallback.binding == XIAOTAI_BINDING_EXPIRED) {
        actions[0] = (xiaotai_view_action_t){.label = "重试", .intent = XIAOTAI_INTENT_BINDING_RETRY,
                                           .role = XIAOTAI_ROLE_PRIMARY};
        view.kind = XIAOTAI_VIEW_NOTICE;
        view.title = fallback.binding == XIAOTAI_BINDING_EXPIRED ? "绑定码已过期" : "绑定未完成";
        view.actions = actions;
        view.action_count = 1;
    }
    door_register_view(&view);
    if (view.kind == XIAOTAI_VIEW_CODE) try_announce();
}

static void render_wifi(void)
{
    xiaotai_provisioning_status_t status;
    xiaotai_provisioning_status(&status);
    xiaotai_view_action_t retry = {.label = "重试", .intent = XIAOTAI_INTENT_PROVISIONING_RETRY,
                                  .role = XIAOTAI_ROLE_PRIMARY};
    xiaotai_view_t view = {.id = "door.network", .title = "网络设置", .kind = XIAOTAI_VIEW_PROGRESS};
    switch (fallback.provision) {
    case XIAOTAI_PROVISIONING_STARTING: view.text = "正在开启热点配网"; break;
    case XIAOTAI_PROVISIONING_WAITING:
        view.status = status.provision.ap_ssid[0] ? "手机连接设备热点" : "等待提交 Wi-Fi 配置";
        view.subtitle = status.provision.ap_ssid;
        view.text = status.provision.url;
        break;
    case XIAOTAI_PROVISIONING_VERIFYING: view.text = "正在验证 Wi-Fi 连接"; break;
    case XIAOTAI_PROVISIONING_STORING: view.text = "正在保存 Wi-Fi 设置"; break;
    case XIAOTAI_PROVISIONING_REJECTED:
    case XIAOTAI_PROVISIONING_STORE_FAILED:
        view.kind = XIAOTAI_VIEW_NOTICE;
        view.text = fallback.provision == XIAOTAI_PROVISIONING_REJECTED ? "连接失败，请重试" : "Wi-Fi 设置未保存，请重试";
        view.actions = &retry;
        view.action_count = 1;
        break;
    case XIAOTAI_PROVISIONING_DONE:
        view.text = status.restart_pending ? "Wi-Fi 已保存，等待重启" : "Wi-Fi 已连接";
        break;
    case XIAOTAI_PROVISIONING_RESTARTING: view.text = "设备即将重启"; break;
    default:
        if (fallback.environment_boot_failed) view.text = "正在连接 Wi-Fi";
        break;
    }
    door_register_view(&view);
}

static bool can_retry_prepare(void)
{
    if (fallback.prepare.fault != XIAOTAI_NET_FAULT_NO_LINK &&
        fallback.prepare.fault != XIAOTAI_NET_FAULT_UNSUPPORTED) return false;
    xiaotai_net_status_t link;
    xiaotai_net_status(&link);
    return link.state == XIAOTAI_NET_BLOCKED && link.prepare.fault == fallback.prepare.fault &&
           !link.prepare.retry_blocked;
}

static void render_setup(void)
{
    switch (setup_source()) {
    case SETUP_PREPARE: {
        const bool failed = fallback.prepare.stage == XIAOTAI_NET_PREPARE_FAILED;
        const bool can_retry = failed && can_retry_prepare();
        const xiaotai_view_action_t retry = {.label = "重新检查", .intent = XIAOTAI_INTENT_PROVISIONING_RETRY,
            .role = XIAOTAI_ROLE_PRIMARY};
        const xiaotai_view_t view = {
            .id = "door.prepare", .title = failed ? "网络零件未就绪" : "正在准备网络",
            .text = failed && fallback.prepare.fault == XIAOTAI_NET_FAULT_POWER_CYCLE ? "请断电后重新启动" : NULL,
            .kind = failed ? XIAOTAI_VIEW_NOTICE : XIAOTAI_VIEW_PROGRESS,
            .flags = failed ? 0 : XIAOTAI_VIEW_PERCENT,
            .progress = fallback.prepare.percent <= 100 ? fallback.prepare.percent : 100,
            .actions = can_retry ? &retry : NULL, .action_count = can_retry ? 1U : 0U,
        };
        door_register_view(&view);
        break;
    }
    case SETUP_WIFI: render_wifi(); break;
    case SETUP_BINDING: render_binding(); break;
    default: break;
    }
}

static void refresh_setup(void)
{
    if (!door.foreground) return;
    const setup_t source = setup_source();
    if (source != SETUP_BINDING) cancel_announce();
    door.setup = source != SETUP_NONE;
    if (door.setup) door_page(DOOR_SETUP);
    else if (door.page == DOOR_SETUP) {
        if (door.session) door_page(door.classified_door ? DOOR_CALL : DOOR_OTHER_CALL);
#ifdef XIAOTAI_DOOR_WITH_WECHAT
        else if (fallback.wechat.session) door_page(DOOR_OTHER_CALL);
#endif
        else door_page(DOOR_HOME);
    }
    update_rings();
}

static void on_setup(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    if (strcmp(event->name, XIAOTAI_EVENT_NET_STATE) == 0) {
        if (!fallback.environment_boot_failed && fallback.prepare.stage != XIAOTAI_NET_PREPARE_FAILED) return;
    } else if (strcmp(event->name, XIAOTAI_EVENT_PROVISIONING_CHANGED) == 0) {
        const xiaotai_provisioning_status_t *status =
            event->size == sizeof(*status) ? event->data : NULL;
        fallback.provision = status ? status->phase : (xiaotai_provisioning_phase_t)event->args.value;
    } else if (strcmp(event->name, XIAOTAI_EVENT_NET_PREPARE) == 0) {
        const xiaotai_net_prepare_t *prepare = event->size == sizeof(*prepare) ? event->data : NULL;
        if (!prepare) return;
        fallback.prepare = *prepare;
    } else {
        xiaotai_binding_status_t status;
        xiaotai_binding_status(&status);
        if (strcmp(event->name, XIAOTAI_EVENT_BINDING_CODE) == 0 &&
            (!code_valid(&status) || !memchr(event->args.text, '\0', sizeof(event->args.text)) ||
             strcmp(event->args.text, status.verification) != 0)) return;
        fallback.binding = status.phase;
        fallback.environment_boot_failed = status.environment_boot_failed;
        remember_code(&status);
    }
    refresh_setup();
}

static void on_ai(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    if (!door.foreground) return;
    if (event->args.value && door.page == DOOR_HOME && !door.setup && !door.overlay && !door.session &&
        !door.stream_generation && !door_fallback_busy()) door_page(DOOR_ASSISTANT);
    else if (!event->args.value && door.page == DOOR_ASSISTANT) door_page(DOOR_HOME);
}

static void render_call(phase_t phase, const char *source, const char *peer, uint32_t session, bool muted,
                        int64_t active_since, const call_actions_t *intents)
{
    xiaotai_view_action_t actions[2];
    size_t count = 0;
    if (phase == CALL_INCOMING) {
        actions[count++] = (xiaotai_view_action_t){.label = "拒绝", .intent = intents->reject,
            .icon = XIAOTAI_ICON_HANGUP, .role = XIAOTAI_ROLE_BACK, .style = XIAOTAI_STYLE_DESTRUCTIVE, .value = (int32_t)session};
        actions[count++] = (xiaotai_view_action_t){.label = "接听", .intent = intents->accept,
            .icon = XIAOTAI_ICON_ACCEPT, .role = XIAOTAI_ROLE_PRIMARY, .style = XIAOTAI_STYLE_POSITIVE, .value = (int32_t)session};
    } else {
        if (phase == CALL_ACTIVE) {
            actions[count++] = (xiaotai_view_action_t){.label = muted ? "取消静音" : "静音", .intent = intents->mute,
                .icon = muted ? XIAOTAI_ICON_MIC_OFF : XIAOTAI_ICON_MIC, .role = XIAOTAI_ROLE_AUX, .value = (int32_t)session};
        }
        actions[count++] = (xiaotai_view_action_t){.label = phase == CALL_OUTGOING ? "取消" : "挂断", .intent = intents->hangup,
            .icon = XIAOTAI_ICON_HANGUP, .role = XIAOTAI_ROLE_BACK, .style = XIAOTAI_STYLE_DESTRUCTIVE, .value = (int32_t)session};
    }
    const int64_t now = xiaotai_time_us();
    const int64_t elapsed = active_since > 0 && now > active_since ? (now - active_since) / 1000000 : 0;
    const xiaotai_view_t view = {
        .id = "door.other_call", .kind = XIAOTAI_VIEW_CALL, .title = source, .subtitle = peer,
        .text = phase == CALL_INCOMING ? "待接听" : phase == CALL_ACTIVE ? "正在通话" :
                phase == CALL_OUTGOING ? "正在呼叫" : "正在连接",
        .timer_mode = phase == CALL_ACTIVE ? XIAOTAI_TIMER_ELAPSED : XIAOTAI_TIMER_NONE,
        .timer_s = elapsed <= INT32_MAX ? (int32_t)elapsed : INT32_MAX,
        .actions = actions, .action_count = count,
    };
    door_register_view(&view);
}

static phase_t device_phase(xiaotai_call_state_t state)
{
    if (state == XIAOTAI_CALL_INCOMING) return CALL_INCOMING;
    if (state == XIAOTAI_CALL_ACTIVE) return CALL_ACTIVE;
    return state == XIAOTAI_CALL_OUTGOING ? CALL_OUTGOING : CALL_CONNECTING;
}

static const char *device_result(const xiaotai_call_status_t *call)
{
    switch (call->end) {
    case XIAOTAI_CALL_END_PEER_HANGUP: return "对方已挂断";
    case XIAOTAI_CALL_END_PEER_REJECT: return "对方拒接";
    case XIAOTAI_CALL_END_PEER_CANCEL: return "对方已取消";
    case XIAOTAI_CALL_END_NO_ANSWER: return "无人接听";
    case XIAOTAI_CALL_END_MISSED: return "来电已结束";
    case XIAOTAI_CALL_END_BUSY: return "呼叫繁忙";
    case XIAOTAI_CALL_END_FAILED: return "呼叫失败";
    case XIAOTAI_CALL_END_NETWORK: return "网络中断";
    case XIAOTAI_CALL_END_NO_MEMORY: return "内存不足";
    case XIAOTAI_CALL_END_AUDIO: return "音频启动失败";
    default: return call->error == XIAOTAI_OK ? "通话已结束" : "通话未完成";
    }
}

#ifdef XIAOTAI_DOOR_WITH_WECHAT
static phase_t wechat_phase(xiaotai_wechat_state_t state)
{
    if (state == XIAOTAI_WECHAT_INCOMING) return CALL_INCOMING;
    if (state == XIAOTAI_WECHAT_ACTIVE) return CALL_ACTIVE;
    return state == XIAOTAI_WECHAT_OUTGOING ? CALL_OUTGOING : CALL_CONNECTING;
}

static const char *wechat_result(const xiaotai_wechat_status_t *call)
{
    switch (call->end) {
    case XIAOTAI_WECHAT_END_PEER_HANGUP: return "对方已挂断";
    case XIAOTAI_WECHAT_END_PEER_REJECT: return "对方拒接";
    case XIAOTAI_WECHAT_END_PEER_CANCEL: return "来电已取消";
    case XIAOTAI_WECHAT_END_NO_ANSWER: return "无人接听";
    case XIAOTAI_WECHAT_END_MISSED: return "来电已结束";
    case XIAOTAI_WECHAT_END_FAILED: return "呼叫失败";
    case XIAOTAI_WECHAT_END_NETWORK: return "网络中断";
    case XIAOTAI_WECHAT_END_NO_MEMORY: return "内存不足";
    case XIAOTAI_WECHAT_END_AUDIO: return "音频启动失败";
    default: return call->error == XIAOTAI_OK ? "通话已结束" : "通话未完成";
    }
}

static void on_wechat(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    if (!door.foreground) return;
    xiaotai_wechat_status_t status;
    xiaotai_wechat_status(&status);
    if (strcmp(event->name, XIAOTAI_EVENT_WECHAT_ENDED) == 0) {
        if (!fallback.wechat.session || status.session || status.state != XIAOTAI_WECHAT_IDLE) return;
        if (door_station_wechat(fallback.wechat.session)) {
            /* The door station's own call: back to its home screen with a word, no result page. */
            fallback.wechat = (xiaotai_wechat_status_t){0};
            update_rings();
            door_station_wechat_ended(&status);
            return;
        }
        fallback.wechat = status;
        if (!door.session && door.page == DOOR_OTHER_CALL) {
            fallback.wechat_result = true;
            door_page(DOOR_OTHER_RESULT);
        }
    } else if (!door.session && status.session && status.state != XIAOTAI_WECHAT_IDLE) {
        fallback.wechat = status;
        if (!door.setup) door_page(DOOR_OTHER_CALL);
    }
    update_rings();
}
#endif

static void render_result(void)
{
    const xiaotai_call_status_t *result = &fallback.device_result;
    const char *peer = result->peer_name[0] ? result->peer_name : result->peer_id;
    const char *title = device_result(result);
#ifdef XIAOTAI_DOOR_WITH_WECHAT
    if (fallback.wechat_result) {
        peer = fallback.wechat.peer_name;
        title = wechat_result(&fallback.wechat);
    }
#endif
    const xiaotai_view_action_t back = {.label = "返回", .intent = XIAOTAI_INTENT_APP_DOOR_BACK,
                                       .icon = XIAOTAI_ICON_BACK, .role = XIAOTAI_ROLE_BACK};
    const xiaotai_view_t view = {
        .id = "door.other_result", .kind = XIAOTAI_VIEW_NOTICE,
        .title = title, .subtitle = peer,
        .actions = &back, .action_count = 1,
    };
    door_register_view(&view);
}

void door_fallback_device_ended(const xiaotai_call_status_t *ended)
{
    fallback.device_result = *ended;
    fallback.wechat_result = false;
    door_page(DOOR_OTHER_RESULT);
}

void door_fallback_render(void)
{
    if (door.page == DOOR_SETUP) {
        render_setup();
    } else if (door.page == DOOR_OTHER_RESULT) {
        render_result();
    } else if (door.page == DOOR_ASSISTANT) {
        const xiaotai_view_action_t stop = {.label = "结束", .intent = TALK_STOP, .role = XIAOTAI_ROLE_BACK};
        const bool can_stop = xiaotai_intent_exists(TALK_STOP);
        const xiaotai_view_t view = {.id = "door.assistant", .kind = XIAOTAI_VIEW_NOTICE, .title = "AI 对话中",
                                     .actions = can_stop ? &stop : NULL, .action_count = can_stop ? 1 : 0};
        door_register_view(&view);
    } else {
        const xiaotai_call_status_t *call = door_shown_call();
        if (call->session && door.session == call->session) {
            static const call_actions_t intents = {
                XIAOTAI_INTENT_CALL_ACCEPT, XIAOTAI_INTENT_CALL_REJECT, XIAOTAI_INTENT_CALL_HANGUP, XIAOTAI_INTENT_CALL_MIC_TOGGLE,
            };
            const char *peer = call->peer_name[0] ? call->peer_name : call->peer_id;
            render_call(device_phase(call->state), "设备通话", peer, call->session, call->muted, call->active_since_us, &intents);
        }
#ifdef XIAOTAI_DOOR_WITH_WECHAT
        else if (fallback.wechat.session) {
            static const call_actions_t intents = {
                XIAOTAI_INTENT_WECHAT_ACCEPT, XIAOTAI_INTENT_WECHAT_REJECT, XIAOTAI_INTENT_WECHAT_HANGUP, XIAOTAI_INTENT_WECHAT_MIC_TOGGLE,
            };
            const xiaotai_wechat_status_t *call = &fallback.wechat;
            if (door_station_wechat(call->session)) {
                door_station_render_wechat(call);
                return;
            }
            render_call(wechat_phase(call->state), "微信通话", call->peer_name, call->session, call->muted,
                        call->active_since_us, &intents);
        }
#endif
    }
}

bool door_fallback_busy(void)
{
#ifdef XIAOTAI_DOOR_WITH_WECHAT
    if (fallback.wechat.session) return true;
#endif
    return door.page == DOOR_ASSISTANT;
}

void door_fallback_foreground(bool foreground)
{
    if (!foreground) {
        cancel_announce();
#ifdef XIAOTAI_DOOR_WITH_WECHAT
        fallback.wechat = (xiaotai_wechat_status_t){0};
#endif
        return;
    }
    /* A different presenter may have received the terminal setup event. */
    xiaotai_provisioning_status_t provision;
    xiaotai_provisioning_status(&provision);
    fallback.provision = provision.phase;
    xiaotai_binding_status_t binding;
    xiaotai_binding_status(&binding);
    fallback.binding = binding.phase;
    fallback.environment_boot_failed = binding.environment_boot_failed;
    remember_code(&binding);
    refresh_setup();
}

xiaotai_err_t door_fallback_init(void)
{
    static const struct {const char *name; xiaotai_event_fn_t handler;} events[] = {
        {AI_STATE, on_ai},
        {XIAOTAI_EVENT_PROVISIONING_CHANGED, on_setup}, {XIAOTAI_EVENT_NET_PREPARE, on_setup},
        {XIAOTAI_EVENT_NET_STATE, on_setup},
        {XIAOTAI_EVENT_BINDING_CHANGED, on_setup}, {XIAOTAI_EVENT_BINDING_CODE, on_setup},
#ifdef XIAOTAI_DOOR_WITH_WECHAT
        {XIAOTAI_EVENT_WECHAT_INCOMING, on_wechat}, {XIAOTAI_EVENT_WECHAT_STATE, on_wechat},
        {XIAOTAI_EVENT_WECHAT_CONNECTED, on_wechat}, {XIAOTAI_EVENT_WECHAT_ENDED, on_wechat},
#endif
    };
    xiaotai_err_t err = xiaotai_intent_handle_ex(REPLAY, replay, NULL, XIAOTAI_INTENT_LOCAL);
    for (size_t i = 0; err == XIAOTAI_OK && i < sizeof(events) / sizeof(*events); ++i)
        err = xiaotai_event_subscribe(events[i].name, events[i].handler, NULL);
    return err;
}

void door_fallback_reset(void)
{
    cancel_announce();
    memset(&fallback, 0, sizeof(fallback));
}
