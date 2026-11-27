// SPDX-License-Identifier: MIT
/* Sound recognition for the person in front of the device: get ready, listen,
 * wait, read, done. Every screen says what happens, what can be done now and
 * how to go on when something is missing; the module keeps the task. */
#include "companion_internal.h"
#include "companion_recognition_text.h"

#include <stdio.h>
#include <string.h>

#include "xiaotai/media.h"
#include "xiaotai/modules/binding.h"
#include "xiaotai/modules/recognition.h"
#include "xiaotai/modules/settings.h"
#include "xiaotai/platform.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"

#define PAGE XIAOTAI_APP_COMPANION_PAGE_RECOGNITION
#define INTENT XIAOTAI_INTENT_APP_COMPANION_RECOGNITION
#define ACTION_INTENT "companion.recognition.action"
#define SECONDS_INTENT "companion.recognition.seconds"
#define RECORD XIAOTAI_APP_COMPANION_OVERLAY_RECORD
#define RC(name) XIAOTAI_RECOGNITION_##name
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define RECEIPT_MS 5000U
#define TICK_MS 1000U
#define ASK_AFTER_S 15U         /* waiting longer than this asks the service itself */
#define QUIET_LEVEL 160U        /* clean microphone amplitude a sound worth sending exceeds */

typedef enum {
    CLOSE, PREVIOUS, NEXT, START, LENGTH, DETAIL, REFRESH, STOP, PLAY, SKIP, FINISH, UNMUTE, RECORD_AGAIN, COMMANDS
} command_t;

typedef enum {
    READY, BLOCKED, RECORDING, WORKING, WAITING, RESULT, UNSURE, FAILED,
} screen_t;

typedef struct {
    char request[XIAOTAI_RECOGNITION_ID_MAX], item[XIAOTAI_RECOGNITION_ID_MAX];
    uint64_t revision;
    uint32_t token;
} target_t;

static struct {
    struct { char id[40], title[64], status[64], lead[160], body[XIAOTAI_VIEW_TEXT_MAX]; } *text;
    target_t shown;
    screen_t screen;
    uint64_t seen_revision;     /* the result that was on the screen */
    size_t item, items;
    bool details, has_result, has_text, can_play, playing, final, retained, unseen, finishing, length_open;
    uint32_t seconds, epoch, timer, tick, decision_serial, waited;
    uint16_t loudest;
    xiaotai_recognition_action_t waiting;
} s_ui;

static bool visible(void) { return companion_on_page(PAGE) && !companion.status.setup; }

/* ------------------------------------------------------------ what is shown */

static screen_t screen_of(const xiaotai_recognition_status_t *status)
{
    switch (status->phase) {
    case RC(RECORDING): return RECORDING;
    case RC(CONFIG): case RC(UPLOAD): case RC(CANCELLING): return WORKING;
    default: break;
    }
    if (s_ui.has_result) return RESULT;
    if (status->request_id[0]) {
        if (status->rejected_without_task || status->phase == RC(RECOVERY)) return UNSURE;
        return WAITING;
    }
    if (status->cleanup_pending) return WORKING;
    if (status->phase == RC(FAILED) && status->error != XIAOTAI_OK && status->error != XIAOTAI_ERR_NOT_FOUND) return FAILED;
    return READY;
}

/* Why nothing can be recorded now, and what helps; NULL: it can. */
static const char *blocked(const xiaotai_recognition_status_t *status, command_t *remedy)
{
    *remedy = REFRESH;
    xiaotai_platform_status_t platform;
    xiaotai_platform_status(&platform);
    if (!companion_home_network_up()) return "设备没有联网。\n连上 Wi-Fi 后再试。";
    if (platform.phase != XIAOTAI_PLATFORM_ONLINE) return "设备还没连上服务器，请稍等。\n还没绑定的话，先到小程序里绑定。";
    int32_t muted = 0;
    if (xiaotai_setting_get_int(XIAOTAI_SETTING_AUDIO_MIC_MUTED, &muted) == XIAOTAI_OK && muted) {
        *remedy = UNMUTE;
        return "麦克风关着，听不到声音。";
    }
    if (xiaotai_activity_active()) return "设备正在通话或对话，结束后再来。";
    xiaotai_recognition_policy_t policy;
    if (xiaotai_recognition_policy(&policy) != XIAOTAI_OK || !policy.valid) {
        if (status->config_error != XIAOTAI_OK && status->config_error != XIAOTAI_ERR_NOT_FOUND)
            return "没有问到识别服务，可能是网络不好。\n点右上角刷新再试。";
        return "正在查看能不能识别…";
    }
    if (!policy.allowed) return "这台设备不能用声音识别。\n请联系服务方开通，再刷新。";
    if (!policy.enabled || !policy.effective_enabled)
        return "声音识别还没有打开。\n到 xiaotai.chat 找到这台设备，点 AI识别，打开开关、添加音频类型，再点右上角刷新。";
    if (!policy.scheme_count) return "还没有选要听什么。\n到 xiaotai.chat 找到这台设备，点 AI识别，添加音频类型，再点右上角刷新。";
    if (policy.monthly_limit && policy.used + policy.scheme_count > policy.monthly_limit)
        return "这个月的识别次数用完了。\n下个月恢复，也可以在小钛控制台调高上限。";
    return NULL;
}

static const char *failure(const xiaotai_recognition_status_t *status)
{
    switch (status->error) {
    case XIAOTAI_ERR_NO_MEM: return "内存不够，没有录成。\n结束别的功能后再试。";
    case XIAOTAI_ERR_TIMEOUT: return "等太久没有回应，没有录成。\n看看网络，再试一次。";
    case XIAOTAI_ERR_PLATFORM_OFFLINE: return "网络断开了，没有录成。\n连上以后再试。";
    default: break;
    }
    if (status->config_error == XIAOTAI_OK && status->config_revision && !status->config_enabled)
        return "声音识别没有打开。\n到 xiaotai.chat 这台设备的 AI识别 里打开后再试。";
    if (status->code || status->http_status) {
        (void)snprintf(s_ui.text->lead, sizeof(s_ui.text->lead), "服务没有受理这次录音（%d）。\n没有产生结果，可以再录一次。",
                       (int)(status->code ? status->code : status->http_status));
        return s_ui.text->lead;
    }
    return "这次没有录成，没有上传，也不消耗次数。\n可以再录一次。";
}

static const char *item_lead(const xiaotai_recognition_item_t *item)
{
    const char *speech = "";
    if (item->output != RC(OUTPUT_TEXT)) {
        if (item->consumption == RC(PLAYED) || item->consumption == RC(PLAY_STARTED)) speech = "（语音已播放）";
        else if (item->speech == RC(SPEECH_PENDING) || item->speech == RC(SPEECH_SYNTHESIZING)) speech = "（语音生成中）";
        else if (item->speech == RC(SPEECH_FAILED)) speech = "（语音没有生成）";
    }
    const char *lead;
    switch (item->state) {
    case RC(ITEM_SUCCEEDED): lead = item->alarm ? "结论：可能有异常，请留意" : "结论：分析完成，没有提示异常"; break;
    case RC(ITEM_FAILED): lead = "结论：这一项没有分析出来"; break;
    case RC(ITEM_CANCELLED): lead = "结论：这一项已取消"; break;
    case RC(ITEM_UNKNOWN): lead = "结论：还没有定论，请刷新"; break;
    default: lead = "正在分析这一项…"; break;
    }
    (void)snprintf(s_ui.text->lead, sizeof(s_ui.text->lead), "%s%s", lead, speech);
    return s_ui.text->lead;
}

static bool playable(const xiaotai_recognition_item_t *item)
{
    return item->state == RC(ITEM_SUCCEEDED) && item->output != RC(OUTPUT_TEXT) && item->consumption == RC(UNTOUCHED) &&
           item->speech != RC(SPEECH_FAILED) && item->speech != RC(SPEECH_SKIPPED);
}

/* Reads the result that is held into the buffers. False: there is none. */
static void read_result(const xiaotai_recognition_status_t *status)
{
    xiaotai_recognition_result_t *result = NULL;
    xiaotai_recognition_result_info_t info;
    xiaotai_recognition_item_t item;
    const xiaotai_err_t access = xiaotai_recognition_result_acquire(&result);
    s_ui.retained = access == XIAOTAI_ERR_TIMEOUT && s_ui.has_result && status->task_revision &&
        status->task_revision == s_ui.shown.revision && !strcmp(status->request_id, s_ui.shown.request);
    if (s_ui.retained) return;
    const bool task = access == XIAOTAI_OK && xiaotai_recognition_result_info(result, &info) == XIAOTAI_OK;
    s_ui.items = task ? info.items : 0;
    if (s_ui.item >= s_ui.items) s_ui.item = 0;
    if (!task || strcmp(info.request_id, s_ui.shown.request)) s_ui.details = false;
    s_ui.final = task && info.state >= RC(TASK_SUCCEEDED) && info.state != RC(TASK_UNKNOWN);
    bool any = false;
    for (size_t i = 0; task && i < info.items; ++i)
        any |= xiaotai_recognition_result_item(result, i, &item) == XIAOTAI_OK && item.state >= RC(ITEM_SUCCEEDED);
    s_ui.has_result = task && info.items && (any || s_ui.final) &&
        xiaotai_recognition_result_item(result, s_ui.item, &item) == XIAOTAI_OK;
    s_ui.has_text = s_ui.has_result && item.has_text;
    s_ui.can_play = s_ui.has_result && playable(&item) && item.speech == RC(SPEECH_READY);
    s_ui.shown = (target_t){.token = status->token};
    strcpy(s_ui.shown.request, task ? info.request_id : status->request_id);
    if (s_ui.has_result) {
        strcpy(s_ui.shown.item, item.item_id);
        s_ui.shown.revision = info.revision;
        (void)item_lead(&item);
        s_ui.text->body[0] = '\0';
        if (s_ui.details && s_ui.has_text) {
            if (companion_recognition_details(result, s_ui.item, s_ui.text->body, sizeof(s_ui.text->body)) != XIAOTAI_OK)
                strcpy(s_ui.text->body, "详细内容暂时读不出来。");
        } else {
            const xiaotai_err_t error = xiaotai_recognition_result_summary(result, s_ui.item, s_ui.text->body,
                                                                           sizeof(s_ui.text->body), NULL);
            if (error != XIAOTAI_OK) strcpy(s_ui.text->body, item.output == RC(OUTPUT_AUDIO) ? "这一项用语音回答，没有文字。" :
                item.error_code ? "" : "没有文字结果。");
            if (error != XIAOTAI_OK && item.error_code) (void)snprintf(s_ui.text->body, sizeof(s_ui.text->body),
                "服务返回了错误 %ld，没有消耗结果。可以再录一次试试。", (long)item.error_code);
        }
    }
    xiaotai_recognition_result_release(result);
}

static int32_t key(command_t cmd) { return (int32_t)(s_ui.epoch * COMMANDS + (unsigned)cmd); }

static void add(xiaotai_view_t *view, command_t cmd, const char *label, const char *icon, uint8_t style, bool enabled)
{
    companion.buffer->actions[view->action_count++] = (xiaotai_view_action_t){.label = label, .icon = icon,
        .intent = ACTION_INTENT, .value = key(cmd), .style = style, .state = enabled && !s_ui.waiting ? 0 : XIAOTAI_STATE_DISABLED};
}

static void add_primary(xiaotai_view_t *view, command_t cmd, const char *label, bool enabled)
{
    add(view, cmd, label, NULL, XIAOTAI_STYLE_NORMAL, enabled);
    companion.buffer->actions[view->action_count - 1U].role = XIAOTAI_ROLE_PRIMARY;
}

static void add_back(xiaotai_view_t *view)
{
    companion.buffer->actions[view->action_count++] = (xiaotai_view_action_t){.icon = XIAOTAI_ICON_BACK,
        .role = XIAOTAI_ROLE_BACK, .intent = XIAOTAI_INTENT_APP_COMPANION_BACK};
}

static void build_ready(xiaotai_view_t *view, const xiaotai_recognition_status_t *status)
{
    command_t remedy;
    const char *why = blocked(status, &remedy);
    s_ui.screen = why ? BLOCKED : READY;
    view->subtitle = "录一小段身边的声音，让云端听听有没有要留意的，比如孩子在哭、有人吵架或呼救。";
    view->text = why;
    xiaotai_recognition_policy_t policy;
    if (!why && xiaotai_recognition_policy(&policy) == XIAOTAI_OK && policy.valid) {
        if (policy.monthly_limit)
            (void)snprintf(s_ui.text->body, sizeof(s_ui.text->body),
                "录 %lu 秒，录音会上传。这个月用了 %llu 次，上限 %lu 次；录一次用 %u 次。", (unsigned long)s_ui.seconds,
                (unsigned long long)policy.used, (unsigned long)policy.monthly_limit, policy.scheme_count);
        else
            (void)snprintf(s_ui.text->body, sizeof(s_ui.text->body),
                "录 %lu 秒，录音会上传，录一次用 %u 次额度。", (unsigned long)s_ui.seconds, policy.scheme_count);
        view->text = s_ui.text->body;
    }
    if (why && remedy == UNMUTE) add_primary(view, UNMUTE, "打开麦克风", true);
    else if (!why) {
        add_primary(view, START, "开始录音", true);
        add(view, LENGTH, "时长", NULL, XIAOTAI_STYLE_NORMAL, true);
    }
    add(view, REFRESH, "刷新", XIAOTAI_ICON_REFRESH, XIAOTAI_STYLE_NORMAL, companion_home_network_up());
}

static void build_recording(xiaotai_view_t *view, const xiaotai_recognition_status_t *status)
{
    const uint32_t rate = status->capture_rate_hz;
    const uint32_t total = rate ? status->target_samples / rate : s_ui.seconds;
    const uint32_t done = rate ? status->samples / rate : 0;
    (void)snprintf(s_ui.text->status, sizeof(s_ui.text->status), "正在听，还有 %lu 秒", (unsigned long)(total > done ? total - done : 0));
    view->kind = XIAOTAI_VIEW_TALK;
    view->status = s_ui.text->status;
    view->emotion = XIAOTAI_EMOTION_LISTENING;
    view->phase = XIAOTAI_TALK_LISTENING;
    view->level = companion_home_voice_level();
    view->text = done >= 2U && s_ui.loudest < QUIET_LEVEL ? "声音有点小，靠近一点" : "把设备靠近要听的声音\n取消不会上传，也不消耗次数";
    add(view, STOP, "取消", XIAOTAI_ICON_STOP, XIAOTAI_STYLE_DESTRUCTIVE, status->token != 0);
}

static void build_working(xiaotai_view_t *view, const xiaotai_recognition_status_t *status)
{
    const bool waiting = s_ui.screen == WAITING;
    view->kind = XIAOTAI_VIEW_TALK;
    view->emotion = XIAOTAI_EMOTION_THINKING;
    view->phase = XIAOTAI_TALK_THINKING;
    if (waiting) {
        (void)snprintf(s_ui.text->status, sizeof(s_ui.text->status), "云端在分析，%lu 秒", (unsigned long)s_ui.waited);
        view->status = s_ui.text->status;
        view->text = s_ui.waited < 40U ? "云端正在听这段录音\n可以先去做别的，好了会提示" : "还在分析，可以先离开\n好了会在首页提示";
        add(view, REFRESH, "问一下结果", XIAOTAI_ICON_REFRESH, XIAOTAI_STYLE_NORMAL, true);
    } else {
        view->status = status->phase == RC(CONFIG) ? "正在确认额度" : status->phase == RC(UPLOAD) ? "正在上传" : "正在停止";
        view->text = status->phase == RC(UPLOAD) ? "正在上传录音，别断网" :
                     status->phase == RC(CONFIG) ? "马上开始录音，先别出声" : "请稍等";
        if (status->phase != RC(CANCELLING) && status->token)
            add(view, STOP, "取消", XIAOTAI_ICON_STOP, XIAOTAI_STYLE_DESTRUCTIVE, true);
    }
}

static void build_result(xiaotai_view_t *view, const xiaotai_recognition_status_t *status)
{
    s_ui.playing = (status->phase == RC(DOWNLOAD) || status->phase == RC(PLAYBACK)) && status->token;
    if (s_ui.items > 1U) (void)snprintf(s_ui.text->title, sizeof(s_ui.text->title), "结果 %u / %u",
                                        (unsigned)s_ui.item + 1U, (unsigned)s_ui.items);
    else strcpy(s_ui.text->title, "识别结果");
    view->title = s_ui.text->title;
    view->status = s_ui.playing ? (status->phase == RC(DOWNLOAD) ? "取语音" : "播放中") : s_ui.final ? NULL : "还在分析";
    view->subtitle = s_ui.details ? NULL : s_ui.text->lead;
    view->text = s_ui.text->body;
    if (s_ui.playing) add(view, STOP, "停止播放", XIAOTAI_ICON_STOP, XIAOTAI_STYLE_NORMAL, true);
    else if (s_ui.can_play) add(view, PLAY, "听一遍", XIAOTAI_ICON_PLAY, XIAOTAI_STYLE_NORMAL, true);
    if (s_ui.has_text) add(view, DETAIL, s_ui.details ? "收起" : "详细", NULL, XIAOTAI_STYLE_NORMAL, !s_ui.retained);
    if (s_ui.items > 1U) add(view, NEXT, "下一项", NULL, XIAOTAI_STYLE_NORMAL, !s_ui.retained);
    if (s_ui.final) add_primary(view, FINISH, "完成", true);
    add(view, REFRESH, "刷新", XIAOTAI_ICON_REFRESH, XIAOTAI_STYLE_NORMAL, true);
    s_ui.unseen = false;
    s_ui.seen_revision = s_ui.shown.revision;
}

static void build(xiaotai_view_t *view, int32_t arg)
{
    (void)arg;
    if (++s_ui.epoch > INT32_MAX / COMMANDS - 1U) s_ui.epoch = 1U;
    xiaotai_recognition_status_t status;
    const bool readable = xiaotai_recognition_status(&status) == XIAOTAI_OK;
    view->kind = XIAOTAI_VIEW_NOTICE;
    view->actions = companion.buffer->actions;
    if (!readable) {
        view->text = "正在读取识别状态…";
        add_back(view);
        return;
    }
    read_result(&status);
    s_ui.screen = screen_of(&status);
    switch (s_ui.screen) {
    case RECORDING: build_recording(view, &status); break;
    case WORKING: case WAITING: build_working(view, &status); break;
    case RESULT: build_result(view, &status); break;
    case UNSURE:
        if (status.rejected_without_task) {
            view->text = "云端没有收到这次录音，没有消耗次数。\n可以重新录一次。";
            add_primary(view, RECORD_AGAIN, "重新开始", true);
        } else {
            view->text = "不确定录音有没有送到，设备记着这一次，不会重复录。\n点“问一下结果”向云端确认。";
            add_primary(view, REFRESH, "问一下结果", true);
        }
        break;
    case FAILED: {
        command_t remedy;
        view->text = failure(&status);
        add_primary(view, START, "再录一次", blocked(&status, &remedy) == NULL);
        break;
    }
    default: build_ready(view, &status); break;
    }
    /* A page for each step: what was pressed on one is not pressed on the next. */
    (void)snprintf(s_ui.text->id, sizeof(s_ui.text->id), PAGE ".%u.%u%c", (unsigned)(s_ui.screen == BLOCKED ? READY : s_ui.screen),
                   (unsigned)s_ui.item, s_ui.details ? 'd' : 's');
    view->id = s_ui.text->id;
    add_back(view);
}

/* ------------------------------------------------------------------ actions */

static void length_overlay(void)
{
    const xiaotai_view_field_t field = {.kind = XIAOTAI_FIELD_STEPPER, .label = "录音时长", .text = "秒",
        .intent = SECONDS_INTENT, .min = 5, .max = XIAOTAI_RECOGNITION_SECONDS_MAX, .step = 5, .value = (int32_t)s_ui.seconds};
    xiaotai_view_action_t close = {.label = "好了", .role = XIAOTAI_ROLE_BACK, .intent = ACTION_INTENT, .value = key(CLOSE)};
    const xiaotai_view_t view = {.kind = XIAOTAI_VIEW_FORM, .layer = XIAOTAI_LAYER_OVERLAY, .id = RECORD, .title = "录音时长",
        .text = "声音短的用 5 到 10 秒；断断续续的声音录久一点。", .fields = &field, .field_count = 1,
        .actions = &close, .action_count = 1};
    s_ui.length_open = true;
    xiaotai_view_show(&view);
}

static void close_length(void)
{
    if (!s_ui.length_open) return;
    s_ui.length_open = false;
    xiaotai_view_dismiss(XIAOTAI_LAYER_OVERLAY, RECORD);
}

static void clear_wait(void)
{
    if (s_ui.timer) xiaotai_timer_cancel(s_ui.timer);
    s_ui.timer = 0;
    s_ui.waiting = RC(ACTION_NONE);
}

static void expire(void *unused)
{
    (void)unused;
    s_ui.timer = 0;
    s_ui.waiting = RC(ACTION_NONE);
    s_ui.finishing = false;
    if (!visible()) return;
    xiaotai_view_toast("没有等到回应，请看看现在的状态", companion_toast_ms());
    companion_render();
}

static xiaotai_err_t submit(command_t cmd, const xiaotai_recognition_status_t *status)
{
    const target_t *const target = &s_ui.shown;
    xiaotai_recognition_action_t expected = RC(ACTION_NONE);
    if (s_ui.waiting) return XIAOTAI_ERR_BUSY;
    xiaotai_err_t error = xiaotai_timer_start(RECEIPT_MS, expire, NULL, &s_ui.timer);
    if (error == XIAOTAI_OK) {
        switch (cmd) {
        case START: expected = RC(ACTION_RECORD); s_ui.loudest = 0; error = xiaotai_recognition_record(s_ui.seconds); break;
        case STOP: expected = RC(ACTION_CANCEL); error = xiaotai_recognition_cancel(target->token); break;
        case PLAY: expected = RC(ACTION_RESUME); error = xiaotai_recognition_resume(target->request); break;
        case SKIP: expected = RC(ACTION_DISMISS); error = xiaotai_recognition_dismiss(target->request, target->item); break;
        case FINISH: expected = RC(ACTION_ARCHIVE); error = xiaotai_recognition_archive(target->request, target->revision); break;
        case RECORD_AGAIN: expected = RC(ACTION_ARCHIVE); error = xiaotai_recognition_archive(target->request, 0); break;
        case REFRESH: expected = RC(ACTION_QUERY); error = xiaotai_recognition_query(); break;
        default: error = XIAOTAI_ERR_INVALID_ARG; break;
        }
    }
    if (error != XIAOTAI_OK) {
        clear_wait();
        companion_refuse_text(INTENT, error, "现在做不了，请稍后再试");
        return error;
    }
    s_ui.waiting = expected;
    s_ui.decision_serial = status->decision_serial;
    companion_render();
    return XIAOTAI_OK;
}

/* Letting a result go: what was not played is let go first, one by one, then
 * the task. Each step is asked for when the one before was carried out. */
static void finish_step(void)
{
    xiaotai_recognition_status_t status;
    if (!s_ui.finishing || s_ui.waiting || xiaotai_recognition_status(&status) != XIAOTAI_OK) return;
    if (status.phase == RC(DOWNLOAD) || status.phase == RC(PLAYBACK)) {
        s_ui.shown.token = status.token;
        if (submit(STOP, &status) != XIAOTAI_OK) s_ui.finishing = false;
        return;
    }
    if (status.cleanup_pending || status.phase == RC(CANCELLING)) return;
    xiaotai_recognition_result_t *result = NULL;
    xiaotai_recognition_result_info_t info;
    xiaotai_recognition_item_t item;
    char waiting[XIAOTAI_RECOGNITION_ID_MAX] = "";
    xiaotai_err_t error = xiaotai_recognition_result_acquire(&result);
    if (error == XIAOTAI_ERR_TIMEOUT) return;       /* the next event tries again */
    if (error == XIAOTAI_OK) error = xiaotai_recognition_result_info(result, &info);
    for (size_t i = 0; error == XIAOTAI_OK && i < info.items && !waiting[0]; ++i)
        if (xiaotai_recognition_result_item(result, i, &item) == XIAOTAI_OK && playable(&item)) strcpy(waiting, item.item_id);
    xiaotai_recognition_result_release(result);
    if (error != XIAOTAI_OK) { s_ui.finishing = false; return; }
    strcpy(s_ui.shown.request, info.request_id);
    s_ui.shown.revision = info.revision;
    if (waiting[0]) strcpy(s_ui.shown.item, waiting);
    else s_ui.finishing = false;
    if (submit(waiting[0] ? SKIP : FINISH, &status) != XIAOTAI_OK) s_ui.finishing = false;
}

static void resolved(bool accept)
{
    if (!visible() || !accept) return;
    s_ui.finishing = true;
    finish_step();
}

static bool confirming(void) { return companion.confirm_resolved == resolved; }

static void run(command_t cmd)
{
    if (!visible()) { ++companion.status.stale; return; }
    if (cmd == CLOSE) { close_length(); companion_render(); return; }
    xiaotai_recognition_status_t status;
    if (xiaotai_recognition_status(&status) != XIAOTAI_OK) {
        xiaotai_view_toast("正在读取状态，请稍后", companion_toast_ms());
        return;
    }
    switch (cmd) {
    case NEXT: case PREVIOUS:
        if (s_ui.screen != RESULT || s_ui.retained || s_ui.items < 2U) return;
        s_ui.item = (s_ui.item + (cmd == NEXT ? 1U : s_ui.items - 1U)) % s_ui.items;
        s_ui.details = false;
        companion_render();
        return;
    case DETAIL:
        if (s_ui.screen != RESULT || !s_ui.has_text || s_ui.retained) return;
        s_ui.details = !s_ui.details;
        companion_render();
        return;
    case LENGTH:
        if (s_ui.screen == READY) length_overlay();
        return;
    case UNMUTE: {
        const xiaotai_err_t error = xiaotai_setting_set_int(XIAOTAI_SETTING_AUDIO_MIC_MUTED, 0);
        if (error != XIAOTAI_OK) companion_refuse(INTENT, error);
        companion_render();
        return;
    }
    case REFRESH:
        if (status.request_id[0]) { (void)submit(REFRESH, &status); return; }
        if (xiaotai_recognition_policy_refresh() != XIAOTAI_OK)
            companion_refuse_text(INTENT, XIAOTAI_ERR_BUSY, "现在做不了，请稍后再试");
        else xiaotai_view_toast("正在刷新", companion_toast_ms());
        return;
    case START: {
        command_t remedy;
        if (status.request_id[0] || (s_ui.screen != READY && s_ui.screen != FAILED)) return;
        if (blocked(&status, &remedy)) { companion_render(); return; }
        close_length();
        (void)submit(START, &status);
        return;
    }
    case FINISH:
        if (s_ui.screen != RESULT || !s_ui.final || confirming()) return;
        if (!companion_confirm_open("完成这次识别", "这次的结果不再保留在设备上，之后可以录新的。", resolved))
            companion_refuse(INTENT, XIAOTAI_ERR_BUSY);
        return;
    case STOP: s_ui.shown.token = status.token; if (status.token) (void)submit(STOP, &status); return;
    case PLAY: if (s_ui.can_play) (void)submit(PLAY, &status); return;
    case SKIP: if (s_ui.screen == RESULT && s_ui.shown.item[0]) (void)submit(SKIP, &status); return;
    case RECORD_AGAIN: if (status.rejected_without_task) (void)submit(RECORD_AGAIN, &status); return;
    default: return;
    }
}

static void invoke(const xiaotai_args_t *args, void *unused)
{
    (void)unused;
    static const struct { const char *text; command_t cmd; } commands[] = {
        {"record", START}, {"stop", STOP}, {"resume", PLAY}, {"skip", SKIP}, {"archive", FINISH},
        {"detail", DETAIL}, {"next", NEXT}, {"previous", PREVIOUS}, {"refresh", REFRESH},
    };
    for (size_t i = 0; i < COUNT(commands); ++i) {
        if (strcmp(commands[i].text, args->text)) continue;
        run(commands[i].cmd);
        return;
    }
    companion_report_refusal(INTENT, XIAOTAI_ERR_INVALID_ARG);
}

static void local_action(const xiaotai_args_t *args, void *unused)
{
    (void)unused;
    if (args->value < 0 || (uint32_t)args->value / COMMANDS != s_ui.epoch) { ++companion.status.stale; return; }
    run((command_t)((uint32_t)args->value % COMMANDS));
}

static void seconds(const xiaotai_args_t *args, void *unused)
{
    (void)unused;
    if (!visible() || !s_ui.length_open || args->value < 1 ||
        args->value > (int32_t)XIAOTAI_RECOGNITION_SECONDS_MAX) { ++companion.status.stale; return; }
    s_ui.seconds = (uint32_t)args->value;
    length_overlay();
}

/* ------------------------------------------------------------------- events */

static const char *receipt_text(xiaotai_recognition_action_t action, xiaotai_err_t error)
{
    if (error == XIAOTAI_OK) return action == RC(ACTION_ARCHIVE) ? "这次识别已完成" : NULL;
    switch (action) {
    case RC(ACTION_RECORD):
        return error == XIAOTAI_ERR_BUSY ? "设备正忙，稍后再录" : error == XIAOTAI_ERR_PLATFORM_OFFLINE ? "还没连上服务器，稍后再录" :
               error == XIAOTAI_ERR_NO_MEM ? "内存不够，稍后再录" : "没有开始录音，请看页面上的说明";
    case RC(ACTION_RESUME): return "语音已经播过，或者还没准备好";
    case RC(ACTION_ARCHIVE): case RC(ACTION_DISMISS): return "结果刚刚有变化，请再点一次";
    case RC(ACTION_QUERY): return "没有问到结果，请稍后再试";
    default: return "已经停下了";
    }
}

static void changed(const xiaotai_event_t *event, void *unused)
{
    (void)unused;
    if (!strcmp(event->name, XIAOTAI_EVENT_RECOGNITION_PROGRESS)) {
        xiaotai_media_loudness_t loudness;
        xiaotai_media_loudness(&loudness);
        if (loudness.capture > s_ui.loudest) s_ui.loudest = loudness.capture;
    }
    xiaotai_recognition_status_t status;
    if (!visible()) {
        /* Told once, on the home screen, when the answer is there. */
        if (xiaotai_recognition_status(&status) == XIAOTAI_OK && status.task_revision &&
            status.task_state >= RC(TASK_SUCCEEDED) && status.task_state != RC(TASK_UNKNOWN)) {
            const bool news = status.task_revision != s_ui.seen_revision;
            if (news && !s_ui.unseen) {
                s_ui.unseen = true;
                if (companion.status.foreground && companion_on_page(XIAOTAI_APP_COMPANION_PAGE_HOME)) companion_render();
            }
        } else if (xiaotai_recognition_status(&status) == XIAOTAI_OK && !status.request_id[0]) {
            s_ui.unseen = false;
        }
        return;
    }
    if (s_ui.waiting && !strcmp(event->name, XIAOTAI_EVENT_RECOGNITION_DECISION) &&
        event->data && event->size == sizeof(xiaotai_recognition_receipt_t)) {
        xiaotai_recognition_receipt_t receipt;
        memcpy(&receipt, event->data, sizeof(receipt));
        if (receipt.serial && (int32_t)(receipt.serial - s_ui.decision_serial) > 0) {
            const bool mine = receipt.action == s_ui.waiting;
            const char *text = mine ? receipt_text(receipt.action, receipt.error) : NULL;
            clear_wait();
            if (text) xiaotai_view_toast(text, companion_toast_ms());
            if (!mine || (receipt.error != XIAOTAI_OK && receipt.action != RC(ACTION_CANCEL))) s_ui.finishing = false;
        }
    }
    finish_step();
    companion_render();
}

static void ticked(void *unused)
{
    (void)unused;
    s_ui.tick = 0;
    if (!visible()) return;
    if (s_ui.screen == WAITING) {
        ++s_ui.waited;
        /* The service tells when it is done; a lost notice must not leave anybody waiting. */
        if (s_ui.waited % ASK_AFTER_S == 0 && !s_ui.waiting) (void)xiaotai_recognition_query();
    } else {
        s_ui.waited = 0;
    }
    companion_render();
    (void)xiaotai_timer_start(TICK_MS, ticked, NULL, &s_ui.tick);
}

static void foreground(bool shown)
{
    if (shown) return;
    close_length();
    companion_confirm_cancel(resolved);
    clear_wait();
    if (s_ui.tick) xiaotai_timer_cancel(s_ui.tick);
    s_ui.tick = 0;
    s_ui.details = s_ui.finishing = s_ui.has_result = false;
    s_ui.item = 0;
    s_ui.waited = 0;
    s_ui.seconds = 10;
}

static void page_visible(const char *id)
{
    if (strcmp(id, PAGE)) { foreground(false); return; }
    if (s_ui.tick) return;      /* another step of the page that shows already */
    /* The limits may have changed in the console since they were read. */
    xiaotai_recognition_status_t status;
    if (xiaotai_recognition_status(&status) == XIAOTAI_OK && !status.request_id[0] && companion_home_network_up())
        (void)xiaotai_recognition_policy_refresh();
    (void)xiaotai_timer_start(TICK_MS, ticked, NULL, &s_ui.tick);
}

static bool home_notice(const char **text)
{
    if (!s_ui.unseen) return false;
    *text = "声音识别有结果了";
    return true;
}

static xiaotai_err_t init(void)
{
    if (!companion.has_display) return XIAOTAI_OK;
    static const companion_page_t page = {.id = PAGE, .title = "声音识别", .build = build, .icon = XIAOTAI_ICON_SOUND};
    s_ui.text = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, sizeof(*s_ui.text));
    if (!s_ui.text) return XIAOTAI_ERR_NO_MEM;
    s_ui.seconds = 10;
    xiaotai_err_t error = companion_page_register(&page);
    if (error == XIAOTAI_OK) error = xiaotai_intent_handle(INTENT, invoke, NULL);
    if (error == XIAOTAI_OK) error = xiaotai_intent_handle_ex(ACTION_INTENT, local_action, NULL, XIAOTAI_INTENT_LOCAL);
    if (error == XIAOTAI_OK) error = xiaotai_intent_handle_ex(SECONDS_INTENT, seconds, NULL, XIAOTAI_INTENT_LOCAL);
    static const char *const events[] = {XIAOTAI_EVENT_RECOGNITION_STATE, XIAOTAI_EVENT_RECOGNITION_RESULT,
        XIAOTAI_EVENT_RECOGNITION_DECISION, XIAOTAI_EVENT_RECOGNITION_PROGRESS, XIAOTAI_EVENT_RECOGNITION_POLICY};
    for (size_t i = 0; error == XIAOTAI_OK && i < COUNT(events); ++i)
        error = xiaotai_event_subscribe(events[i], changed, NULL);
    return error;
}

static void reset(void)
{
    foreground(false);
    xiaotai_mem_free(s_ui.text);
    memset(&s_ui, 0, sizeof(s_ui));
}

const xiaotai_app_companion_part_t xiaotai_app_companion_with_recognition = {.name = "recognition",
    .init = init, .foreground = foreground, .reset = reset, .page_visible = page_visible, .home_notice = home_notice};
