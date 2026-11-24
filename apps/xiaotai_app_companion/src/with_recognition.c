// SPDX-License-Identifier: MIT
#include "companion_internal.h"

#include <stdio.h>
#include <string.h>

#include "xiaotai/modules/recognition.h"
#include "xiaotai/port/mem.h"

#define PAGE XIAOTAI_APP_COMPANION_PAGE_RECOGNITION
#define INTENT XIAOTAI_INTENT_APP_COMPANION_RECOGNITION
#define ACTION_INTENT "companion.recognition.action"
#define SECONDS_INTENT "companion.recognition.seconds"
#define RECORD XIAOTAI_APP_COMPANION_OVERLAY_RECORD
#define TASK XIAOTAI_APP_COMPANION_OVERLAY_TASK
#define RC(name) XIAOTAI_RECOGNITION_##name
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define DETAIL_BYTES 512U
#define RECEIPT_MS 5000U

typedef enum {
    CLOSE, PREVIOUS, NEXT, OPEN_RECORD, DETAIL, OPEN_TASK, REFRESH, SUBMIT,
    STOP, RESUME, SKIP, ARCHIVE, COMMANDS
} command_t;

typedef enum { NO_MODAL, RECORD_MODAL, TASK_MODAL, SKIP_MODAL, ARCHIVE_MODAL } modal_t;

typedef struct {
    char request[XIAOTAI_RECOGNITION_ID_MAX], item[XIAOTAI_RECOGNITION_ID_MAX];
    uint64_t revision;
    uint32_t token;
} target_t;

static struct {
    struct { char id[40], title[64], status[64], prefix[104], body[XIAOTAI_VIEW_TEXT_MAX]; } *text;
    target_t displayed, captured;
    uint64_t detail_revision;
    size_t item, items, page, offset, next, total;
    bool details, has_result, has_text, long_summary, can_previous, can_next, retained_projection;
    uint32_t seconds, epoch, timer, decision_serial, projection;
    modal_t modal;
    xiaotai_recognition_action_t waiting;
} s_ui;

static bool visible(void)
{
    return companion_on_page(PAGE) && !companion.status.setup;
}

static bool offline(void)
{
    return (companion.style->policy & XIAOTAI_APP_COMPANION_OFFLINE_GATE) && !companion_home_network_up();
}

static bool working(const xiaotai_recognition_status_t *status)
{
    if (status->cleanup_pending) return true;
    switch (status->phase) {
    case RC(CONFIG): case RC(RECORDING): case RC(UPLOAD): case RC(CANCELLING):
    case RC(QUERY): case RC(REFRESH): case RC(DOWNLOAD): case RC(PLAYBACK): return true;
    default: return false;
    }
}

static bool available(const xiaotai_recognition_status_t *status, command_t cmd)
{
    if (!status || s_ui.waiting || offline()) return false;
    const bool busy = working(status);
    const target_t *const target = &s_ui.captured;
    const bool matching = !busy && target->request[0] && !strcmp(target->request, status->request_id);
    switch (cmd) {
    case OPEN_RECORD: case SUBMIT: return !busy && !status->request_id[0] && !s_ui.has_result;
    case STOP: return busy && target->token && target->token == status->token;
    case RESUME: return matching && target->revision;
    case SKIP: return matching && target->revision && target->item[0];
    case ARCHIVE: return matching && (target->revision || status->rejected_without_task);
    case REFRESH: return true;
    default: return false;
    }
}

static void status_text(const xiaotai_recognition_status_t *status, const xiaotai_recognition_result_info_t *info)
{
    const char *text = "等待结果";
    if (!status) text = "状态暂不可用";
    else if (status->phase == RC(RECORDING) && status->capture_rate_hz) {
        snprintf(s_ui.text->status, sizeof(s_ui.text->status), "正在录音 %lu / %lu 秒",
            (unsigned long)(status->samples / status->capture_rate_hz),
            (unsigned long)(status->target_samples / status->capture_rate_hz));
        return;
    } else if (status->phase == RC(RECORDING)) text = "正在录音";
    else if (status->phase == RC(CONFIG)) text = "正在检查识别配置";
    else if (status->phase == RC(UPLOAD)) text = "正在提交录音";
    else if (status->phase == RC(CANCELLING)) text = "正在停止并回收资源";
    else if (status->phase == RC(DOWNLOAD)) text = "正在下载语音";
    else if (status->phase == RC(PLAYBACK)) text = "正在播放语音";
    else if (status->phase == RC(QUERY)) text = "正在更新";
    else if (status->error != XIAOTAI_OK && status->config_revision &&
             status->config_error == XIAOTAI_OK && !status->config_enabled) text = "当前识别配置未开启";
    else if (status->error == XIAOTAI_ERR_NO_MEM) text = "内存不足，操作未完成";
    else if (status->error == XIAOTAI_ERR_TIMEOUT) text = "请求超时，任务待确认";
    else if (status->error != XIAOTAI_OK && status->error != XIAOTAI_ERR_NOT_FOUND) {
        snprintf(s_ui.text->status, sizeof(s_ui.text->status), "操作失败 (%d)", (int)status->error);
        return;
    } else if (status->phase == RC(CANCELLED)) text = "本地操作已取消";
    else if (status->phase == RC(RECOVERY)) text = "任务状态待确认";
    else if (info) {
        static const char *const states[] = {"排队中", "处理中", "任务完成", "部分失败",
                                            "任务失败", "结果待确认", "任务已取消"};
        text = (unsigned)info->state < COUNT(states) ? states[info->state] : "结果待确认";
    }
    strcpy(s_ui.text->status, text);
}

static void reset_detail(void)
{
    s_ui.details = false;
    s_ui.page = s_ui.offset = s_ui.next = s_ui.total = 0;
}

static const char *summary(const xiaotai_recognition_result_t *result, const xiaotai_recognition_item_t *item)
{
    static const char *const states[] = {"排队中", "分析中", "分析成功", "分析失败", "结果待确认", "已取消"};
    static const char *const voices[] = {"等待语音", "合成中", "语音已就绪", "语音失败", "语音待确认", "仅文字"};
    const char *state = (unsigned)item->state < COUNT(states) ? states[item->state] : states[RC(ITEM_UNKNOWN)];
    const char *speech = (unsigned)item->speech < COUNT(voices) ? voices[item->speech] : voices[RC(SPEECH_UNKNOWN)];
    const char *alarm = item->alarm ? "模型提示异常，待确认\n" : "";
    const char *fallback = item->output == RC(OUTPUT_AUDIO) ? "本条返回语音" : "尚无文字结果";
    const size_t prefix = (size_t)snprintf(s_ui.text->prefix, sizeof(s_ui.text->prefix), "%s / %s\n%s",
        state, speech, alarm);
    memcpy(s_ui.text->body, s_ui.text->prefix, prefix);
    const xiaotai_err_t error = xiaotai_recognition_result_summary(result, s_ui.item, s_ui.text->body + prefix,
        sizeof(s_ui.text->body) - prefix, NULL);
    if (error == XIAOTAI_ERR_INVALID_SIZE && xiaotai_recognition_result_summary(result, s_ui.item,
        s_ui.text->body, sizeof(s_ui.text->body), NULL) == XIAOTAI_OK) return s_ui.text->prefix;
    if (error != XIAOTAI_OK) strcpy(s_ui.text->body + prefix, fallback);
    return NULL;
}

static int32_t key(command_t cmd) { return (int32_t)(s_ui.epoch * COMMANDS + (unsigned)cmd); }

static xiaotai_view_action_t action(command_t cmd, const char *label, const char *icon, bool enabled)
{
    return (xiaotai_view_action_t){.label = label, .icon = icon, .intent = ACTION_INTENT, .value = key(cmd),
        .state = enabled ? 0 : XIAOTAI_STATE_DISABLED};
}

static void build(xiaotai_view_t *view, int32_t arg)
{
    (void)arg;
    if (++s_ui.epoch > INT32_MAX / COMMANDS - 1U) s_ui.epoch = 1U;
    xiaotai_recognition_status_t status;
    const bool readable = xiaotai_recognition_status(&status) == XIAOTAI_OK;
    xiaotai_recognition_result_t *result = NULL;
    xiaotai_recognition_result_info_t info;
    xiaotai_recognition_item_t item;
    const xiaotai_err_t access = xiaotai_recognition_result_acquire(&result);
    const bool task = access == XIAOTAI_OK && xiaotai_recognition_result_info(result, &info) == XIAOTAI_OK;
    // The status reader clears the identity and revision when binding retires.
    const bool unchanged = access == XIAOTAI_ERR_TIMEOUT && readable && s_ui.has_result &&
        status.task_revision && status.task_revision == s_ui.displayed.revision &&
        !strcmp(status.request_id, s_ui.displayed.request);
    s_ui.retained_projection = unchanged;
    view->kind = XIAOTAI_VIEW_NOTICE;
    view->title = s_ui.text->title;
    view->status = s_ui.text->status;
    view->text = s_ui.text->body;
    view->id = s_ui.text->id;
    if (unchanged) {
        info.state = status.task_state;
        status_text(&status, &info);
        view->subtitle = s_ui.long_summary ? s_ui.text->prefix : NULL;
        goto actions;
    }
    s_ui.items = task ? info.items : 0;
    if (s_ui.item >= s_ui.items) s_ui.item = 0;
    if (!task || info.revision != s_ui.detail_revision || strcmp(info.request_id, s_ui.displayed.request)) {
        s_ui.detail_revision = task ? info.revision : 0;
        ++s_ui.projection;
        reset_detail();
    }
    s_ui.has_result = task && info.items && xiaotai_recognition_result_item(result, s_ui.item, &item) == XIAOTAI_OK;
    s_ui.has_text = s_ui.has_result && item.has_text;
    s_ui.displayed = (target_t){.token = readable ? status.token : 0};
    status_text(readable ? &status : NULL, task ? &info : NULL);
    s_ui.can_previous = s_ui.can_next = s_ui.long_summary = false;
    if (!s_ui.has_result) {
        strcpy(s_ui.text->title, "识别结果");
        strcpy(s_ui.text->body, "暂无识别结果");
        if (readable && status.rejected_without_task) strcpy(s_ui.displayed.request, status.request_id);
    } else {
        strcpy(s_ui.displayed.request, info.request_id);
        strcpy(s_ui.displayed.item, item.item_id);
        s_ui.displayed.revision = info.revision;
        if (s_ui.details && xiaotai_recognition_result_detail_page(result, s_ui.item, s_ui.offset,
            s_ui.text->body, DETAIL_BYTES, &s_ui.next, NULL, &s_ui.total) != XIAOTAI_OK) reset_detail();
        if (s_ui.details) {
            snprintf(s_ui.text->title, sizeof(s_ui.text->title), "详情 · 第 %u 页", (unsigned)s_ui.page + 1);
            s_ui.can_previous = s_ui.offset != 0;
            s_ui.can_next = s_ui.next < s_ui.total;
        } else {
            snprintf(s_ui.text->title, sizeof(s_ui.text->title), "类型 %u / %u", (unsigned)s_ui.item + 1, info.items);
            view->subtitle = summary(result, &item);
            s_ui.long_summary = view->subtitle != NULL;
            s_ui.can_previous = s_ui.item != 0;
            s_ui.can_next = s_ui.item + 1 < info.items;
        }
    }
    // A new UI identity clears its overlay. Keep the displayed confirmation
    // while underlying revisions change; apply the scroll reset on dismissal.
    if (s_ui.modal == NO_MODAL) {
        if (!s_ui.has_result) strcpy(s_ui.text->id, PAGE);
        else snprintf(s_ui.text->id, sizeof(s_ui.text->id), PAGE ".%08lx.%u.%c%u",
            (unsigned long)s_ui.projection, (unsigned)s_ui.item, s_ui.details ? 'd' : 's',
            (unsigned)s_ui.page);
    }
    xiaotai_recognition_result_release(result);

actions:;
    xiaotai_view_action_t *const actions = companion.buffer->actions;
    const bool enabled = !offline();
    const bool navigate = enabled && !s_ui.retained_projection;
    actions[0] = action(PREVIOUS, "上一条", XIAOTAI_ICON_PREVIOUS, navigate && s_ui.can_previous);
    actions[1] = action(NEXT, "下一条", XIAOTAI_ICON_NEXT, navigate && s_ui.can_next);
    actions[2] = action(OPEN_RECORD, "录音", NULL, available(readable ? &status : NULL, OPEN_RECORD));
    actions[3] = action(DETAIL, s_ui.details ? "返回摘要" : "完整结果", NULL, navigate && s_ui.has_text);
    actions[4] = action(OPEN_TASK, "任务操作", NULL, enabled && readable && !s_ui.waiting);
    actions[5] = action(REFRESH, "刷新", XIAOTAI_ICON_REFRESH, available(readable ? &status : NULL, REFRESH));
    actions[6] = (xiaotai_view_action_t){.icon = XIAOTAI_ICON_BACK, .role = XIAOTAI_ROLE_BACK,
        .intent = XIAOTAI_INTENT_APP_COMPANION_BACK};
    view->actions = actions;
    view->action_count = 7;
}

static void resolved(bool accept);

static void dismiss_modal(void)
{
    const modal_t previous = s_ui.modal;
    s_ui.modal = NO_MODAL;
    if (previous == RECORD_MODAL) xiaotai_view_dismiss(XIAOTAI_LAYER_OVERLAY, RECORD);
    else if (previous == TASK_MODAL) xiaotai_view_dismiss(XIAOTAI_LAYER_OVERLAY, TASK);
    else if (previous != NO_MODAL) companion_confirm_cancel(resolved);
}

static void overlay(void)
{
    if (s_ui.modal == NO_MODAL) return;
    xiaotai_recognition_status_t status;
    const xiaotai_recognition_status_t *const snapshot =
        xiaotai_recognition_status(&status) == XIAOTAI_OK ? &status : NULL;
    if (s_ui.modal == ARCHIVE_MODAL || s_ui.modal == SKIP_MODAL) {
        companion_confirm_enabled(resolved, available(snapshot, s_ui.modal == ARCHIVE_MODAL ? ARCHIVE : SKIP));
        return;
    }
    xiaotai_view_t view = {.layer = XIAOTAI_LAYER_OVERLAY};
    xiaotai_view_action_t actions[2] = {action(CLOSE, "取消", XIAOTAI_ICON_BACK, true)};
    actions[0].role = XIAOTAI_ROLE_BACK;
    view.actions = actions;
    view.action_count = 1;
    if (s_ui.modal == RECORD_MODAL) {
        const bool enabled = available(snapshot, SUBMIT);
        xiaotai_view_field_t field = {.kind = XIAOTAI_FIELD_STEPPER, .label = "录音时长", .text = "秒",
            .intent = SECONDS_INTENT, .min = 1, .max = XIAOTAI_RECOGNITION_SECONDS_MAX, .step = 1,
            .value = (int32_t)s_ui.seconds, .state = enabled ? 0 : XIAOTAI_STATE_DISABLED};
        view.id = RECORD;
        view.kind = XIAOTAI_VIEW_FORM;
        view.title = "录音时长";
        view.text = "录音将上传到识别服务\n可能消耗账户额度";
        view.fields = &field;
        view.field_count = 1;
        actions[1] = action(SUBMIT, "录音并提交", NULL, enabled);
        actions[1].role = XIAOTAI_ROLE_PRIMARY;
        view.action_count = 2;
        xiaotai_view_show(&view);
    } else {
        static const char *const labels[] = {"停止", "恢复未播", "略过本条", "归档任务"};
        xiaotai_view_item_t items[COUNT(labels)];
        for (unsigned i = 0; i < COUNT(labels); ++i) {
            const command_t cmd = (command_t)(STOP + i);
            items[i] = (xiaotai_view_item_t){.label = labels[i], .intent = ACTION_INTENT, .value = key(cmd),
                .state = available(snapshot, cmd) ? 0 : XIAOTAI_STATE_DISABLED};
        }
        view.id = TASK;
        view.kind = XIAOTAI_VIEW_LIST;
        view.title = "任务操作";
        view.items = items;
        view.item_count = COUNT(items);
        xiaotai_view_show(&view);
    }
}

static void refresh(void)
{
    companion_render();
    if (visible()) overlay();
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
    if (!visible()) return;
    xiaotai_view_toast("未收到操作结果，请核对状态", companion_toast_ms());
    refresh();
}

static xiaotai_err_t submit(command_t cmd, const xiaotai_recognition_status_t *status)
{
    const target_t *const target = &s_ui.captured;
    xiaotai_recognition_action_t expected = RC(ACTION_NONE);
    if (!available(status, cmd)) return XIAOTAI_ERR_INVALID_STATE;
    xiaotai_err_t error = xiaotai_timer_start(RECEIPT_MS, expire, NULL, &s_ui.timer);
    if (error == XIAOTAI_OK) {
        switch (cmd) {
        case SUBMIT: expected = RC(ACTION_RECORD); error = xiaotai_recognition_record(s_ui.seconds); break;
        case STOP: expected = RC(ACTION_CANCEL); error = xiaotai_recognition_cancel(target->token); break;
        case RESUME: expected = RC(ACTION_RESUME); error = xiaotai_recognition_resume(target->request); break;
        case SKIP: expected = RC(ACTION_DISMISS); error = xiaotai_recognition_dismiss(target->request, target->item); break;
        case ARCHIVE: expected = RC(ACTION_ARCHIVE); error = xiaotai_recognition_archive(target->request, target->revision); break;
        case REFRESH: expected = RC(ACTION_QUERY); error = xiaotai_recognition_query(); break;
        default: error = XIAOTAI_ERR_INVALID_ARG; break;
        }
    }
    if (error != XIAOTAI_OK) {
        clear_wait();
        return error;
    }
    s_ui.waiting = expected;
    s_ui.decision_serial = status->decision_serial;
    dismiss_modal();
    xiaotai_view_toast("请求已提交", companion_toast_ms());
    refresh();
    return XIAOTAI_OK;
}

static bool confirm_command(command_t cmd)
{
    if (!companion_confirm_open(cmd == ARCHIVE ? "归档任务" : "略过本条",
        cmd == ARCHIVE ? "归档后可开始新的录音\n本机不保留这次结果" : "确认略过当前这一条结果？", resolved))
        return false;
    s_ui.modal = cmd == ARCHIVE ? ARCHIVE_MODAL : SKIP_MODAL;
    return true;
}

static void resolved(bool accept)
{
    const modal_t modal = s_ui.modal;
    s_ui.modal = NO_MODAL;
    if (!visible() || (modal != ARCHIVE_MODAL && modal != SKIP_MODAL)) return;
    if (!accept) { refresh(); return; }
    xiaotai_recognition_status_t status;
    if (xiaotai_recognition_status(&status) != XIAOTAI_OK) {
        xiaotai_view_toast("状态暂不可用", companion_toast_ms());
        return;
    }
    const command_t cmd = modal == ARCHIVE_MODAL ? ARCHIVE : SKIP;
    const xiaotai_err_t error = submit(cmd, &status);
    if (error != XIAOTAI_OK) {
        if (confirm_command(cmd)) overlay();
        companion_refuse_text(INTENT, error, "操作未提交，请检查状态");
    }
}

static xiaotai_err_t detail_page(size_t offset, size_t *previous)
{
    xiaotai_recognition_result_t *result = NULL;
    xiaotai_recognition_result_info_t info;
    xiaotai_err_t error = xiaotai_recognition_result_acquire(&result);
    if (error == XIAOTAI_OK) error = xiaotai_recognition_result_info(result, &info);
    if (error == XIAOTAI_OK && (info.revision != s_ui.detail_revision ||
        strcmp(info.request_id, s_ui.displayed.request))) error = XIAOTAI_ERR_INVALID_STATE;
    if (error == XIAOTAI_OK) error = xiaotai_recognition_result_detail_page(result, s_ui.item, offset,
        s_ui.text->body, DETAIL_BYTES, &s_ui.next, previous, &s_ui.total);
    xiaotai_recognition_result_release(result);
    return error;
}

static void step_detail(command_t cmd)
{
    if (cmd == NEXT && s_ui.next < s_ui.total) { s_ui.offset = s_ui.next; ++s_ui.page; }
    else if (cmd == PREVIOUS && s_ui.offset) {
        size_t previous;
        if (detail_page(s_ui.offset, &previous) == XIAOTAI_OK) {
            s_ui.offset = previous;
            --s_ui.page;
        }
    }
}

static void run(command_t cmd)
{
    if (!visible()) { ++companion.status.stale; return; }
    if (cmd == CLOSE) { dismiss_modal(); refresh(); return; }
    if (offline()) return;
    if (cmd == NEXT || cmd == PREVIOUS || cmd == DETAIL) {
        if (s_ui.modal != NO_MODAL || s_ui.retained_projection) return;
        if (cmd == DETAIL && s_ui.has_text) {
            if (s_ui.details) reset_detail();
            else if (detail_page(0, NULL) == XIAOTAI_OK) s_ui.details = true;
            else xiaotai_view_toast("详细结果暂不可用", companion_toast_ms());
        } else if (cmd != DETAIL && s_ui.details) step_detail(cmd);
        else if (cmd == NEXT && s_ui.item + 1U < s_ui.items) ++s_ui.item;
        else if (cmd == PREVIOUS && s_ui.item) --s_ui.item;
        refresh();
        return;
    }
    xiaotai_recognition_status_t status;
    if (xiaotai_recognition_status(&status) != XIAOTAI_OK) {
        xiaotai_view_toast("状态暂不可用", companion_toast_ms());
        return;
    }
    if (cmd == OPEN_TASK || cmd == OPEN_RECORD) {
        if (s_ui.waiting || s_ui.modal != NO_MODAL || (cmd == OPEN_RECORD && !available(&status, cmd))) return;
        s_ui.modal = cmd == OPEN_RECORD ? RECORD_MODAL : TASK_MODAL;
        s_ui.captured = s_ui.displayed;
        overlay();
        return;
    }
    if (cmd == SKIP || cmd == ARCHIVE) {
        if (s_ui.modal == NO_MODAL) s_ui.captured = s_ui.displayed;
        if ((s_ui.modal != TASK_MODAL && s_ui.modal != NO_MODAL) || !available(&status, cmd)) return;
        dismiss_modal();
        if (!confirm_command(cmd)) companion_refuse(INTENT, XIAOTAI_ERR_BUSY);
        return;
    }
    if (cmd == STOP || cmd == RESUME) {
        if (s_ui.modal == NO_MODAL) s_ui.captured = s_ui.displayed;
        else if (s_ui.modal != TASK_MODAL) return;
    } else if ((cmd == SUBMIT && s_ui.modal != RECORD_MODAL) ||
               (cmd == REFRESH && s_ui.modal != NO_MODAL)) return;
    if (!available(&status, cmd)) return;
    const xiaotai_err_t error = submit(cmd, &status);
    if (error != XIAOTAI_OK) companion_refuse_text(INTENT, error, "操作未提交，请检查状态");
}

static void invoke(const xiaotai_args_t *args, void *unused)
{
    (void)unused;
    static const struct { const char *text; command_t cmd; } commands[] = {
        {"record", OPEN_RECORD}, {"stop", STOP}, {"resume", RESUME}, {"skip", SKIP}, {"archive", ARCHIVE},
        {"detail", DETAIL}, {"next", NEXT}, {"previous", PREVIOUS}, {"refresh", REFRESH},
    };
    for (size_t i = 0; i < COUNT(commands); ++i) {
        if (!strcmp(commands[i].text, args->text)) {
            const command_t cmd = commands[i].cmd;
            run(cmd == OPEN_RECORD && s_ui.modal == RECORD_MODAL ? SUBMIT : cmd);
            return;
        }
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
    if (!visible() || s_ui.modal != RECORD_MODAL || offline() || args->value < 1 ||
        args->value > (int32_t)XIAOTAI_RECOGNITION_SECONDS_MAX) { ++companion.status.stale; return; }
    s_ui.seconds = (uint32_t)args->value;
    overlay();
}

static void changed(const xiaotai_event_t *event, void *unused)
{
    (void)unused;
    if (!visible()) return;
    if (s_ui.waiting && !strcmp(event->name, XIAOTAI_EVENT_RECOGNITION_DECISION) &&
        event->data && event->size == sizeof(xiaotai_recognition_receipt_t)) {
        xiaotai_recognition_receipt_t receipt;
        memcpy(&receipt, event->data, sizeof(receipt));
        if (receipt.serial && (int32_t)(receipt.serial - s_ui.decision_serial) > 0) {
            const char *text = "操作已受理";
            if (receipt.action != s_ui.waiting) text = "收到其他操作结果，请核对状态";
            else if (receipt.error != XIAOTAI_OK) text = "操作未执行，请稍后重试";
            clear_wait();
            xiaotai_view_toast(text, companion_toast_ms());
        }
    }
    refresh();
}

static void foreground(bool foreground)
{
    if (foreground) return;
    dismiss_modal();
    clear_wait();
    reset_detail();
    s_ui.has_result = false;
    s_ui.item = 0;
    s_ui.seconds = 10;
}

static void page_visible(const char *id)
{
    if (strcmp(id, PAGE)) foreground(false);
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
        XIAOTAI_EVENT_RECOGNITION_DECISION, XIAOTAI_EVENT_RECOGNITION_PROGRESS};
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
    .init = init, .foreground = foreground, .reset = reset, .page_visible = page_visible};
