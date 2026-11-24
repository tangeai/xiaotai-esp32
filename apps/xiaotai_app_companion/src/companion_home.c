// SPDX-License-Identifier: MIT
#include "companion_internal.h"

#include <string.h>
#include "xiaotai/media.h"
#include "xiaotai/modules/provisioning.h"
#include "xiaotai/modules/settings.h"
#include "xiaotai/platform.h"
#include "xiaotai/port/os.h"
#ifdef COMPANION_WITH_TRANSPORT
#include "xiaotai/transport.h"
#endif

#define DAYDREAM_MS 45000U
#define SLEEPY_MS 180000U
#define VOICE_MS 4500U
#define REST_MS 2500U
#define NETWORK_NOTICE_MS 3200U

static struct {
    uint32_t timer, generation;
    int64_t interacted, voice_until, rest_until, notice_until;
    const char *notice;
    uint8_t voice;
    bool had_session, session_notice, return_pending;
} home;

bool companion_home_network_up(void)
{
    xiaotai_net_status_t net;
    xiaotai_net_status(&net);
    return net.state == XIAOTAI_NET_UP;
}

uint8_t companion_home_voice_level(void) { return home.voice; }

static companion_home_session_t shown_session(void)
{
    companion_home_session_t out = {0};
    for (size_t i = 0; xiaotai_app_companion_parts[i]; ++i) {
        const xiaotai_app_companion_part_t *part = xiaotai_app_companion_parts[i];
        if (part->home_session && part->home_session(&out)) return out;
    }
    const char *activity = NULL;
    if (!xiaotai_activity_current(&activity) || !activity) return out;
    if (!strcmp(activity, "ai")) out.mode = COMPANION_HOME_AI_ACTIVE;
    else if (!strcmp(activity, "call") || !strcmp(activity, "wechat")) out.mode = COMPANION_HOME_CALL_CONNECT;
    else if (!strcmp(activity, "stream")) out.mode = COMPANION_HOME_STREAM_AUDIO;
    else if (!strcmp(activity, "room")) out.mode = COMPANION_HOME_ROOM_CONNECT;
    return out;
}

static bool session_running(void)
{
    return xiaotai_activity_active() || shown_session().mode != COMPANION_HOME_IDLE;
}

bool companion_page_may_sleep(void)
{
    return companion.style->menu_entries && !session_running();
}

/* Waking up starts from the home screen, not from the page somebody left open
 * before the screen went dark. A result nobody has read yet stays. */
void companion_home_screen_dark(void)
{
    if (!companion_page_may_sleep() || companion.status.setup || companion.code_visible ||
        companion_on_page(XIAOTAI_APP_COMPANION_PAGE_HOME) || companion_on_page(XIAOTAI_APP_COMPANION_PAGE_RESULT) ||
        companion_on_page(XIAOTAI_APP_COMPANION_PAGE_CALL)) return;
    (void)companion_reset_home(0);
}

bool companion_finish_presentation(void)
{
    if (!companion.status.foreground || xiaotai_app_entry() != XIAOTAI_APP_ENTRY_PRESENT) return false;
    /* A setup screen and a new session can overlap the end of the last one.
     * Keep the framework's return ticket until there is nothing left to show. */
    home.return_pending = companion.status.setup || companion_on_page(XIAOTAI_APP_COMPANION_PAGE_RESULT) ||
                          session_running();
    return !home.return_pending && xiaotai_app_return() == XIAOTAI_OK;
}

static void timer_expired(void *unused);

static void include_due(int64_t candidate, int64_t now, int64_t *due)
{
    if (candidate > now && (!*due || candidate < *due)) *due = candidate;
}

static void schedule(void)
{
    if (home.timer) {
        xiaotai_timer_cancel(home.timer);
        home.timer = 0;
    }
    if (!companion_on_page(XIAOTAI_APP_COMPANION_PAGE_HOME)) return;
    const int64_t now = xiaotai_time_us();
    int64_t due = 0;
    include_due(home.notice_until, now, &due);
    include_due(home.voice_until, now, &due);
    if (!session_running()) {
        include_due(home.interacted + DAYDREAM_MS * INT64_C(1000), now, &due);
        include_due(home.interacted + SLEEPY_MS * INT64_C(1000), now, &due);
    }
    if (due) (void)xiaotai_timer_start((uint32_t)((due - now + 999) / 1000), timer_expired, NULL, &home.timer);
}

static void refresh(void)
{
    if (companion_on_page(XIAOTAI_APP_COMPANION_PAGE_HOME)) companion_render();
    schedule();
}

static void timer_expired(void *unused)
{
    (void)unused;
    home.timer = 0;
    refresh();
}

void companion_home_visible(void)
{
    if (!companion.status.foreground) home.return_pending = false;
    if (!home.interacted) home.interacted = xiaotai_time_us();
    schedule();
}

void companion_home_interaction(void)
{
    if (!companion.status.foreground) return;
    home.interacted = xiaotai_time_us();
    home.voice_until = 0;
    refresh();
}

void companion_home_notice(const char *text, uint32_t ms)
{
    /* Text must have static lifetime. */
    home.session_notice = false;
    home.notice = text;
    home.notice_until = text ? xiaotai_time_us() + (int64_t)ms * 1000 : 0;
    refresh();
}

void companion_home_session_notice(const char *text, uint32_t ms)
{
    home.session_notice = true;
    home.notice = text;
    home.notice_until = xiaotai_time_us() + (int64_t)ms * 1000;
    refresh();
}

void companion_home_clear_session_notice(void)
{
    if (home.session_notice) companion_home_notice(NULL, 0);
}

void companion_home_ack(void)
{
    companion_home_interaction();
    companion_user_activity();
    companion_home_session_notice("在呢。", 1700);
}

static const char *status_text(companion_home_session_t session, bool muted)
{
    if (companion.style->policy & XIAOTAI_APP_COMPANION_HOME_NETWORK_STATUS) {
        if (xiaotai_quiesced()) return "维护中";
        if (!companion_home_network_up()) return "离线";
        xiaotai_platform_status_t platform;
        xiaotai_platform_status(&platform);
        bool online = platform.phase == XIAOTAI_PLATFORM_ONLINE;
#ifdef COMPANION_WITH_TRANSPORT
        if (online && companion.config.home_online_requires_transport)
            online = xiaotai_transport_state() == XIAOTAI_TRANSPORT_ONLINE;
#endif
        return online ? "在线" : "连接中";
    }
    const char *save = companion_save_failure_home();
    if (save) return save;
    if (xiaotai_quiesced()) return "维护中，设置暂不可修改";
    if (home.session_notice && session.mode >= COMPANION_HOME_AI_CONNECT &&
        session.mode <= COMPANION_HOME_AI_ACTIVE && xiaotai_time_us() < home.notice_until) return home.notice;
    switch (session.mode) {
        case COMPANION_HOME_AI_CONNECT: return "正在连接 AI";
        case COMPANION_HOME_AI_STOP: return "结束对话中";
        case COMPANION_HOME_AI_ACTIVE:
            if (session.phase == XIAOTAI_TALK_LISTENING) return "正在聆听";
            if (session.phase == XIAOTAI_TALK_THINKING) return "AI 正在思考";
            if (session.phase == XIAOTAI_TALK_SPEAKING) return "AI 回复中";
            return "小钛在这儿";
        case COMPANION_HOME_STREAM_AUDIO: return "远程对讲中";
        case COMPANION_HOME_STREAM_VIEW: return "远程查看中";
        case COMPANION_HOME_STREAM_CAMERA_START: return "摄像头启动中";
        case COMPANION_HOME_STREAM_CAMERA_ON: return "视频查看中";
        case COMPANION_HOME_STREAM_CAMERA_ERROR: return "摄像头异常";
        case COMPANION_HOME_RINGING: return "语音来电";
        case COMPANION_HOME_CALL_CONNECT: return "正在建立通话";
        case COMPANION_HOME_CALL: return "通话中";
        case COMPANION_HOME_ROOM_CONNECT: return "正在加入房间";
        case COMPANION_HOME_ROOM: return "房间对讲中";
        default:
            if (home.notice && xiaotai_time_us() < home.notice_until) return home.notice;
            return muted ? "麦克风关闭" : "待唤醒";
    }
}

void companion_home_status(xiaotai_view_t *view, int32_t screen)
{
    const int64_t now = xiaotai_time_us();
    const companion_home_session_t session = shown_session();
    const bool idle = session.mode == COMPANION_HOME_IDLE && !xiaotai_activity_active();
    int32_t muted = 0, emotion = 0;
    (void)xiaotai_setting_get_int(XIAOTAI_SETTING_AUDIO_MIC_MUTED, &muted);
    (void)xiaotai_setting_get_int(XIAOTAI_SETTING_APP_COMPANION_EMOTION, &emotion);
    view->emotion = xiaotai_app_companion_emotion((size_t)emotion);
    view->status = status_text(session, muted != 0);
    view->level = home.voice;
    xiaotai_net_status_t net;
    xiaotai_net_status(&net);
    int32_t bars = -1;
    if (net.state == XIAOTAI_NET_UP) {
        const int32_t signal = net.signal_dbm;
        bars = !net.signal_valid ? 0 : signal >= -60 ? 3 : signal >= -72 ? 2 : 1;
    }
    companion.buffer->indicators[0] = (xiaotai_view_indicator_t){.icon = XIAOTAI_ICON_WIFI, .level = bars};
    view->indicators = companion.buffer->indicators;
    view->indicator_count = 1;
    if (idle) {
        view->flags |= XIAOTAI_VIEW_MAY_SLEEP;
        if (!screen && (companion.style->policy & XIAOTAI_APP_COMPANION_TALK_ON_HOME)) {
            const bool tap = xiaotai_intent_exists(COMPANION_HOME_START_INTENT);
            const bool wake = xiaotai_intent_exists("wake.enable");
            if (muted) view->text = tap ? "麦克风已关闭\n点击表情唤醒对话" : "麦克风已关闭";
            else if (wake) view->text = tap ? "尝试说“你好小钛”\n或者点击表情唤醒对话" : "尝试说“你好小钛”";
            else if (tap) view->text = "点击表情唤醒对话";
        }
        const int64_t quiet = home.interacted ? now - home.interacted : 0;
        if (now < home.voice_until) view->emotion = home.voice >= 2 ? XIAOTAI_EMOTION_LISTENING : XIAOTAI_EMOTION_AMBIENT;
        else if (quiet >= SLEEPY_MS * INT64_C(1000)) view->emotion = XIAOTAI_EMOTION_SLEEPY;
        else if (quiet >= DAYDREAM_MS * INT64_C(1000)) view->emotion = XIAOTAI_EMOTION_AMBIENT;
    } else if (session.mode < COMPANION_HOME_STREAM_AUDIO || session.mode > COMPANION_HOME_STREAM_CAMERA_ERROR) {
        view->text = session.caption;
        view->speaker = session.speaker;
        view->phase = session.phase;
        if (session.mode == COMPANION_HOME_AI_ACTIVE) view->emotion = xiaotai_app_companion_emotion_face(session.emotion);
    }
}

static void rest(const xiaotai_args_t *args, void *unused)
{
    (void)args;
    (void)unused;
    if (!companion.status.foreground) { ++companion.status.stale; return; }
    if (companion.status.setup) { companion_refuse(XIAOTAI_INTENT_APP_COMPANION_REST, XIAOTAI_ERR_BUSY); return; }
    if (xiaotai_intent_exists("talk.stop")) {
        const xiaotai_err_t err = xiaotai_intent_post("talk.stop", NULL);
        if (err != XIAOTAI_OK) { companion_refuse(XIAOTAI_INTENT_APP_COMPANION_REST, err); return; }
    }
    home.rest_until = xiaotai_time_us() + REST_MS * INT64_C(1000);
    home.voice_until = 0;
    companion_home_notice(NULL, 0);
    (void)companion_reset_home(0);
    for (size_t i = 0; xiaotai_app_companion_parts[i]; ++i) {
        const xiaotai_app_companion_part_t *part = xiaotai_app_companion_parts[i];
        if (part->screen_sleep) {
            const xiaotai_err_t err = part->screen_sleep();
            if (err != XIAOTAI_OK) companion_refuse(XIAOTAI_INTENT_APP_COMPANION_REST, err);
            break;
        }
    }
}

static void changed(const xiaotai_event_t *event, void *unused)
{
    (void)unused;
    if (!strcmp(event->name, XIAOTAI_EVENT_MEDIA_VOICE)) {
        const uint8_t level = (uint8_t)event->args.value;
        if (home.voice < 2 && level >= 2 && xiaotai_time_us() >= home.rest_until &&
            !session_running() && companion_on_page(XIAOTAI_APP_COMPANION_PAGE_HOME)) {
            home.interacted = xiaotai_time_us();
            home.voice_until = home.interacted + VOICE_MS * INT64_C(1000);
            companion_user_activity();
        }
        const bool changed_level = home.voice != level;
        home.voice = level;
        if (changed_level && companion_on_page(XIAOTAI_APP_COMPANION_PAGE_TALK)) companion_render();
    } else if (!strcmp(event->name, XIAOTAI_EVENT_INPUT_BEGAN)) {
        companion_home_interaction();
        companion_user_activity();
    } else if (!strcmp(event->name, XIAOTAI_EVENT_NET_UP) ||
               !strcmp(event->name, XIAOTAI_EVENT_PROVISIONING_FAILED)) {
        const bool up = !strcmp(event->name, XIAOTAI_EVENT_NET_UP);
        const uint32_t generation = (uint32_t)event->args.value;
        if (up && generation && home.generation == generation) return;
        if (up) home.generation = generation;
        if (companion.status.foreground) {
            companion_home_interaction();
            companion_user_activity();
            if (!session_running() && companion_on_page(XIAOTAI_APP_COMPANION_PAGE_HOME)) {
                companion_home_notice(up ? "Wi-Fi 连接成功" : "Wi-Fi 连接失败", NETWORK_NOTICE_MS);
            }
        }
    } else if (!strcmp(event->name, XIAOTAI_EVENT_ACTIVITY)) {
        const bool active = session_running();
        if (active || home.had_session) {
            companion_home_interaction();
            if (xiaotai_time_us() >= home.rest_until) companion_user_activity();
        }
        home.had_session = active;
        if (home.return_pending && companion_finish_presentation()) return;
    }
    refresh();
}

#ifdef COMPANION_WITH_TRANSPORT
static void transport_changed(const xiaotai_event_t *event, void *unused)
{
    (void)event;
    (void)unused;
    if (companion_on_page(XIAOTAI_APP_COMPANION_PAGE_HOME)) companion_render();
}
#endif

xiaotai_err_t companion_home_init(void)
{
    if (!companion.has_display) return XIAOTAI_OK;
    static const xiaotai_subscriber_descriptor_t events[] = {
        {XIAOTAI_EVENT_MEDIA_VOICE, changed, NULL},
        {XIAOTAI_EVENT_INPUT_BEGAN, changed, NULL},
        {XIAOTAI_EVENT_ACTIVITY, changed, NULL},
        {XIAOTAI_EVENT_NET_UP, changed, NULL},
        {XIAOTAI_EVENT_NET_DOWN, changed, NULL},
        {XIAOTAI_EVENT_NET_SIGNAL, changed, NULL},
        {XIAOTAI_EVENT_PLATFORM_ONLINE, changed, NULL},
        {XIAOTAI_EVENT_PLATFORM_OFFLINE, changed, NULL},
        {XIAOTAI_EVENT_PROVISIONING_FAILED, changed, NULL},
        {XIAOTAI_EVENT_SETTINGS_CHANGED, changed, NULL},
    };
    xiaotai_err_t err = xiaotai_intent_handle(XIAOTAI_INTENT_APP_COMPANION_REST, rest, NULL);
    for (size_t i = 0; err == XIAOTAI_OK && i < sizeof(events) / sizeof(events[0]); ++i)
        err = xiaotai_event_subscribe_static(&events[i]);
#ifdef COMPANION_WITH_TRANSPORT
    if (err == XIAOTAI_OK && companion.config.home_online_requires_transport &&
        (companion.style->policy & XIAOTAI_APP_COMPANION_HOME_NETWORK_STATUS))
        err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_TRANSPORT_STATE, transport_changed, NULL);
#endif
    return err;
}

void companion_home_reset(void) { memset(&home, 0, sizeof(home)); }
