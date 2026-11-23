// SPDX-License-Identifier: MIT
#include "fakes.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "xiaotai/modules/recognition.h"
#include "xiaotai/port/os.h"

#define RC(name) XIAOTAI_RECOGNITION_##name
#define PAGE XIAOTAI_APP_COMPANION_PAGE_RECOGNITION
#define COMMAND XIAOTAI_INTENT_APP_COMPANION_RECOGNITION

static atomic_int_fast64_t clock_us;
int64_t __wrap_xiaotai_time_us(void);
int64_t __wrap_xiaotai_time_us(void) { return atomic_load(&clock_us); }
static xiaotai_signal_t synced;
static bool fail_status, long_summary, fail_detail, timeout_result;
static bool legacy_items;
static xiaotai_err_t post_error;
static xiaotai_recognition_status_t snapshot;
static struct xiaotai_recognition_result { bool active; unsigned leases; } result;
static struct {
    char title[64], status[64], text[XIAOTAI_VIEW_TEXT_MAX], subtitle[104], toast[112];
    char view_id[40];
    char overlay[24], disclosure[128];
    xiaotai_view_action_t page_actions[8], overlay_actions[8];
    xiaotai_view_item_t task[4];
    xiaotai_view_field_t field;
    unsigned shows, toasts;
} shown;
static struct {
    unsigned count;
    xiaotai_recognition_action_t action;
    uint64_t revision;
    uint32_t value;
    char request[65], item[65];
} requested;

xiaotai_err_t xiaotai_recognition_status(xiaotai_recognition_status_t *out)
{
    *out = snapshot;
    return fail_status ? XIAOTAI_ERR_TIMEOUT : XIAOTAI_OK;
}

xiaotai_err_t xiaotai_recognition_result_acquire(xiaotai_recognition_result_t **out)
{
    if (!result.active) return XIAOTAI_ERR_NOT_FOUND;
    if (timeout_result) return XIAOTAI_ERR_TIMEOUT;
    *out = &result;
    ++result.leases;
    return XIAOTAI_OK;
}

void xiaotai_recognition_result_release(xiaotai_recognition_result_t *held)
{
    if (held) { assert(held == &result && result.leases > 0); --result.leases; }
}

xiaotai_err_t xiaotai_recognition_result_info(const xiaotai_recognition_result_t *held,
                                               xiaotai_recognition_result_info_t *info)
{
    assert(held == &result && result.leases);
    *info = (xiaotai_recognition_result_info_t){.revision = snapshot.task_revision, .items = legacy_items ? 8U : 2U,
        .state = RC(TASK_SUCCEEDED)};
    strcpy(info->request_id, snapshot.request_id);
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_recognition_result_item(const xiaotai_recognition_result_t *held,
                                              size_t index, xiaotai_recognition_item_t *item)
{
    assert(held == &result && result.leases && index < (legacy_items ? 8U : 2U));
    const bool audio = index == 1U;
    const bool failed = index == 2U;
    const bool mixed = legacy_items && !audio && !failed;
    *item = (xiaotai_recognition_item_t){.state = failed ? RC(ITEM_FAILED) : RC(ITEM_SUCCEEDED),
        .alarm = !audio && !failed, .output = audio ? RC(OUTPUT_AUDIO) : mixed ? RC(OUTPUT_TEXT_AUDIO) : RC(OUTPUT_TEXT),
        .speech = audio ? RC(SPEECH_READY) : mixed ? RC(SPEECH_FAILED) : RC(SPEECH_SKIPPED),
        .has_text = !audio && !failed};
    snprintf(item->item_id, sizeof(item->item_id), "item_%u", (unsigned)index + 1U);
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_recognition_result_summary(const xiaotai_recognition_result_t *held,
                                                 size_t index, char *out, size_t capacity, size_t *size)
{
    assert(held == &result && result.leases && index < (legacy_items ? 8U : 2U));
    const char text[] = "这是一段识别结果。";
    const bool absent = index == 1U || index == 2U;
    const size_t length = long_summary ? 2000U : legacy_items ? 1500U : sizeof(text) - 1U;
    if (size) *size = absent ? 0 : length;
    if (absent) return XIAOTAI_ERR_NOT_FOUND;
    if (capacity <= length) return XIAOTAI_ERR_INVALID_SIZE;
    if (long_summary) {
        for (size_t off = 0; off < length; off += 4U) memcpy(out + off, "\xf0\x9f\x8e\xa7", 4U);
        out[length] = '\0';
    } else if (legacy_items) {
        for (size_t off = 0; off < length; off += 3U) memcpy(out + off, "声", 3U);
        out[length] = '\0';
    } else memcpy(out, text, sizeof(text));
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_recognition_result_detail_page(const xiaotai_recognition_result_t *held,
                                                     size_t index, size_t offset, char *out,
                                                     size_t capacity, size_t *next, size_t *previous,
                                                     size_t *total)
{
    assert(held == &result && result.leases && index < (legacy_items ? 8U : 2U) &&
           index != 1U && index != 2U && capacity == 512U);
    if (fail_detail) return XIAOTAI_ERR_NO_MEM;
    const char tail[] = "中字尾";
    assert(offset == 0 || offset == 510U);
    if (!offset) { memset(out, 'a', 510U); out[510] = '\0'; }
    else strcpy(out, tail);
    *total = 510U + sizeof(tail) - 1U;
    *next = offset ? *total : 510U;
    if (previous) *previous = 0;
    return XIAOTAI_OK;
}

static xiaotai_err_t request(xiaotai_recognition_action_t action, const char *id, const char *item,
                              uint64_t revision, uint32_t value)
{
    assert(xiaotai_on_owner());
    ++requested.count;
    requested.action = action;
    strcpy(requested.request, id ? id : "");
    strcpy(requested.item, item ? item : "");
    requested.revision = revision;
    requested.value = value;
    return post_error;
}

xiaotai_err_t xiaotai_recognition_record(uint32_t seconds)
{ return request(RC(ACTION_RECORD), NULL, NULL, 0, seconds); }
xiaotai_err_t xiaotai_recognition_cancel(uint32_t token)
{ return request(RC(ACTION_CANCEL), NULL, NULL, 0, token); }
xiaotai_err_t xiaotai_recognition_query(void)
{ return request(RC(ACTION_QUERY), NULL, NULL, 0, 0); }
xiaotai_err_t xiaotai_recognition_resume(const char *id)
{ return request(RC(ACTION_RESUME), id, NULL, 0, 0); }
xiaotai_err_t xiaotai_recognition_dismiss(const char *id, const char *item)
{ return request(RC(ACTION_DISMISS), id, item, 0, 0); }
xiaotai_err_t xiaotai_recognition_archive(const char *id, uint64_t revision)
{ return request(RC(ACTION_ARCHIVE), id, NULL, revision, 0); }

static void show(const xiaotai_view_t *view, void *unused)
{
    (void)unused;
    assert(xiaotai_on_owner());
    if (view->layer == XIAOTAI_LAYER_TOAST) {
        snprintf(shown.toast, sizeof(shown.toast), "%s", view->text ? view->text : "");
        ++shown.toasts;
    } else if (view->layer == XIAOTAI_LAYER_OVERLAY) {
        if (view->flags & XIAOTAI_VIEW_DISMISS) {
            if (!view->id || !strcmp(view->id, shown.overlay)) shown.overlay[0] = '\0';
            return;
        }
        strcpy(shown.overlay, view->id);
        snprintf(shown.disclosure, sizeof(shown.disclosure), "%s", view->text ? view->text : "");
        if (view->action_count) memcpy(shown.overlay_actions, view->actions,
            view->action_count * sizeof(*view->actions));
        if (view->field_count) shown.field = view->fields[0];
        if (view->item_count) {
            assert(view->item_count == 4U);
            memcpy(shown.task, view->items, sizeof(shown.task));
        }
    } else if (!strncmp(view->id, PAGE, sizeof(PAGE) - 1U)) {
        assert(view->kind == XIAOTAI_VIEW_NOTICE && view->action_count == 7U && result.leases == 0);
        if (strcmp(shown.view_id, view->id)) shown.overlay[0] = '\0';
        strcpy(shown.view_id, view->id);
        strcpy(shown.title, view->title);
        strcpy(shown.status, view->status);
        strcpy(shown.text, view->text);
        strcpy(shown.subtitle, view->subtitle ? view->subtitle : "");
        memcpy(shown.page_actions, view->actions, 7U * sizeof(*view->actions));
        ++shown.shows;
    }
}

static xiaotai_err_t init_probe(void)
{
    const xiaotai_err_t err = companion_fixture_init();
    if (err == XIAOTAI_OK) xiaotai_view_set_sink(show, NULL);
    return err;
}

static void barrier(void *ctx) { xiaotai_signal_give(ctx); }
static void flush(void)
{
    for (unsigned i = 0; i < 3U; ++i) {
        assert(xiaotai_run(barrier, &synced) == XIAOTAI_OK);
        assert(xiaotai_signal_take(&synced, 3000U));
    }
}

static void send(const char *intent, const char *text, int32_t value)
{
    xiaotai_args_t args = {.value = value};
    if (text) strcpy(args.text, text);
    assert(xiaotai_intent_post(intent, &args) == XIAOTAI_OK);
    flush();
}

static void state(const char *event)
{
    assert(xiaotai_event_emit(event, NULL, NULL, 0) == XIAOTAI_OK);
    flush();
}

static void receipt(uint32_t serial, xiaotai_recognition_action_t action, xiaotai_err_t error)
{
    const xiaotai_recognition_receipt_t msg = {.serial = serial, .action = action, .error = error};
    snapshot.decision_serial = serial;
    assert(xiaotai_event_emit(XIAOTAI_EVENT_RECOGNITION_DECISION, NULL, &msg, sizeof(msg)) == XIAOTAI_OK);
    flush();
}

static void confirm(bool yes)
{
    const xiaotai_view_action_t action = shown.overlay_actions[yes ? 0 : 1];
    assert(!strcmp(action.intent, XIAOTAI_INTENT_APP_COMPANION_CONFIRM));
    send(action.intent, NULL, action.value);
}

static bool confirming(void) { return !strncmp(shown.overlay, "confirm.", sizeof("confirm.") - 1U); }

int main(void)
{
    static const xiaotai_module_t probe = {.name = "probe", .init = init_probe};
    static const xiaotai_module_t *const modules[] = {&probe};
    static const xiaotai_application_t app = {.name = "companion", .init = xiaotai_app_companion_init,
        .start = xiaotai_app_companion_start, .foreground = xiaotai_app_companion_foreground};
    static const xiaotai_application_t *const apps[] = {&app};
    const uint32_t needs = XIAOTAI_NEED_DISPLAY | XIAOTAI_NEED_TOUCH | XIAOTAI_NEED_KEY;
    fixture = (companion_test_fixture_t){.needs = needs, .navigation_pages = true,
        .net.state = XIAOTAI_NET_UP, .binding.phase = XIAOTAI_BINDING_BOUND, .platform.phase = XIAOTAI_PLATFORM_ONLINE};
    const xiaotai_framework_config_t config = {.modules = modules, .module_count = 1,
        .applications = apps, .application_count = 1, .available_needs = needs};
    assert(xiaotai_signal_init(&synced) == XIAOTAI_OK);
    assert(xiaotai_framework_start(&config) == XIAOTAI_OK);
    flush();
    send(XIAOTAI_INTENT_APP_COMPANION_SHOW, PAGE, 0);
    assert(shown.shows && !strcmp(shown.title, "识别结果") && !strcmp(shown.status, "等待结果"));
    fail_status = true;
    state(XIAOTAI_EVENT_RECOGNITION_STATE);
    assert(!strcmp(shown.status, "状态暂不可用"));
    fail_status = false;
    state(XIAOTAI_EVENT_RECOGNITION_STATE);
    send(COMMAND, "record", 0);
    assert(!strcmp(shown.overlay, "record") && shown.field.kind == XIAOTAI_FIELD_STEPPER);
    assert(shown.field.min == 1 && shown.field.max == 60 && shown.field.value == 10 && shown.field.step == 1);
    assert(!strcmp(shown.disclosure, "录音将上传到识别服务\n可能消耗账户额度"));
    send(shown.field.intent, NULL, 61);
    assert(shown.field.value == 10 && !requested.count);
    send(shown.field.intent, NULL, 12);
    assert(shown.field.value == 12);
    send(COMMAND, "record", 0);
    assert(requested.count == 1 && requested.action == RC(ACTION_RECORD) && requested.value == 12);
    assert(!shown.overlay[0] && !strcmp(shown.toast, "请求已提交"));
    receipt(0U, RC(ACTION_RECORD), XIAOTAI_OK);
    assert(!strcmp(shown.toast, "请求已提交"));
    atomic_store(&clock_us, 4999000);
    flush();
    assert(!strcmp(shown.toast, "请求已提交"));
    atomic_store(&clock_us, 5000000);
    flush();
    assert(!strcmp(shown.toast, "未收到操作结果，请核对状态"));

    snapshot.phase = RC(RECORDING);
    snapshot.samples = 8000;
    snapshot.target_samples = 96000;
    snapshot.capture_rate_hz = 8000;
    snapshot.token = 191;
    state(XIAOTAI_EVENT_RECOGNITION_STATE);
    assert(!strcmp(shown.status, "正在录音 1 / 12 秒"));
    snapshot.samples = 16000;
    state(XIAOTAI_EVENT_RECOGNITION_PROGRESS);
    assert(!strcmp(shown.status, "正在录音 2 / 12 秒"));
    send(shown.page_actions[4].intent, NULL, shown.page_actions[4].value);
    assert(!strcmp(shown.overlay, "task") && shown.task[0].state == 0 && shown.task[1].state != 0);
    const xiaotai_view_item_t stale_stop = shown.task[0];
    snapshot.token = 192;
    state(XIAOTAI_EVENT_RECOGNITION_STATE);
    send(stale_stop.intent, NULL, stale_stop.value);
    assert(requested.count == 1);
    assert(shown.task[0].state & XIAOTAI_STATE_DISABLED);
    send(shown.overlay_actions[0].intent, NULL, shown.overlay_actions[0].value);
    send(COMMAND, "stop", 0);
    assert(requested.count == 2 && requested.action == RC(ACTION_CANCEL) && requested.value == 192);
    receipt(1U, RC(ACTION_CANCEL), XIAOTAI_OK);
    assert(!strcmp(shown.toast, "操作已受理"));
    snapshot.phase = RC(CANCELLED);
    state(XIAOTAI_EVENT_RECOGNITION_STATE);
    assert(!strcmp(shown.status, "本地操作已取消"));

    result.active = true;
    snapshot.phase = RC(RESULT);
    snapshot.task_state = RC(TASK_SUCCEEDED);
    snapshot.token = 0;
    snapshot.task_revision = 9007199254740988ULL;
    strcpy(snapshot.request_id, "request");
    state(XIAOTAI_EVENT_RECOGNITION_RESULT);
    assert(!strcmp(shown.title, "类型 1 / 2") && !strcmp(shown.status, "任务完成"));
    assert(!strcmp(shown.text, "分析成功 / 仅文字\n模型提示异常，待确认\n这是一段识别结果。"));
    long_summary = true;
    state(XIAOTAI_EVENT_RECOGNITION_RESULT);
    assert(strlen(shown.text) == 2000U && !memcmp(shown.text + 1996U, "\xf0\x9f\x8e\xa7", 4U));
    assert(strstr(shown.subtitle, "模型提示异常，待确认"));
    long_summary = false;
    state(XIAOTAI_EVENT_RECOGNITION_RESULT);
    send(COMMAND, "next", 0);
    assert(!strcmp(shown.title, "类型 2 / 2") && strstr(shown.text, "本条返回语音"));
    send(COMMAND, "previous", 0);
    fail_detail = true;
    send(COMMAND, "detail", 0);
    assert(!strcmp(shown.title, "类型 1 / 2") && !strcmp(shown.toast, "详细结果暂不可用"));
    fail_detail = false;
    send(COMMAND, "detail", 0);
    assert(!strcmp(shown.title, "详情 · 第 1 页") && strlen(shown.text) == 510U);
    send(COMMAND, "next", 0);
    assert(!strcmp(shown.title, "详情 · 第 2 页") && !strcmp(shown.text, "中字尾"));
    assert(!strcmp(companion.status.page, PAGE) && strstr(shown.view_id, ".d1"));
    char second_page[sizeof(shown.view_id)];
    strcpy(second_page, shown.view_id);
    timeout_result = true;
    state(XIAOTAI_EVENT_RECOGNITION_RESULT);
    assert(!strcmp(shown.title, "详情 · 第 2 页") && !strcmp(shown.text, "中字尾") && result.leases == 0);
    assert(!strcmp(shown.view_id, second_page));
    timeout_result = false;
    state(XIAOTAI_EVENT_RECOGNITION_RESULT);
    send(COMMAND, "previous", 0);
    assert(!strcmp(shown.title, "详情 · 第 1 页") && strlen(shown.text) == 510U);
    assert(!strcmp(companion.status.page, PAGE) && strcmp(shown.view_id, second_page) && strstr(shown.view_id, ".d0"));
    send(COMMAND, "detail", 0);
    assert(!strcmp(shown.title, "类型 1 / 2"));
    send(COMMAND, "skip", 0);
    assert(confirming() && requested.count == 2);
    confirm(false);
    assert(!shown.overlay[0] && requested.count == 2);
    send(COMMAND, "skip", 0);
    confirm(true);
    assert(requested.action == RC(ACTION_DISMISS) && !strcmp(requested.request, "request") &&
           !strcmp(requested.item, "item_1") && requested.count == 3);
    receipt(2U, RC(ACTION_RESUME), XIAOTAI_OK);
    assert(!strcmp(shown.toast, "收到其他操作结果，请核对状态"));
    send(COMMAND, "archive", 0);
    assert(confirming() && requested.count == 3);
    strcpy(second_page, shown.view_id);
    const uint64_t seen = snapshot.task_revision++;
    state(XIAOTAI_EVENT_RECOGNITION_RESULT);
    assert(confirming() && !strcmp(second_page, shown.view_id));
    confirm(true);
    assert(requested.action == RC(ACTION_ARCHIVE) && requested.revision == seen && requested.count == 4);
    assert(strcmp(second_page, shown.view_id));
    receipt(3U, RC(ACTION_ARCHIVE), XIAOTAI_ERR_INVALID_STATE);
    assert(strstr(shown.toast, "操作未执行"));
    send(COMMAND, "resume", 0);
    assert(requested.action == RC(ACTION_RESUME) && requested.count == 5 && !strcmp(requested.request, "request"));
    receipt(4U, RC(ACTION_RESUME), XIAOTAI_OK);
    post_error = XIAOTAI_ERR_NO_MEM;
    send(COMMAND, "refresh", 0);
    assert(requested.count == 6 && !strcmp(shown.toast, "操作未提交，请检查状态"));
    post_error = XIAOTAI_OK;
    send(COMMAND, "archive", 0);
    assert(confirming());
    const xiaotai_view_action_t obsolete = shown.overlay_actions[0];
    send(XIAOTAI_INTENT_APP_COMPANION_BACK, NULL, 0);
    assert(!shown.overlay[0]);
    send(obsolete.intent, NULL, obsolete.value);
    assert(requested.count == 6);

    send(XIAOTAI_INTENT_APP_COMPANION_SHOW, PAGE, 0);
    send(COMMAND, "refresh", 0);
    assert(requested.count == 7 && requested.action == RC(ACTION_QUERY));
    send(XIAOTAI_INTENT_APP_COMPANION_BACK, NULL, 0);
    const unsigned notices = shown.toasts;
    atomic_store(&clock_us, 20000000);
    receipt(5U, RC(ACTION_QUERY), XIAOTAI_OK);
    assert(shown.toasts == notices && result.leases == 0);

    send(XIAOTAI_INTENT_APP_COMPANION_SHOW, PAGE, 0);
    assert(strstr(shown.text, "这是一段识别结果"));
    timeout_result = true;
    snapshot.task_revision = 0;
    snapshot.request_id[0] = '\0';
    state(XIAOTAI_EVENT_RECOGNITION_RESULT);
    assert(!strcmp(shown.title, "识别结果") && !strcmp(shown.text, "暂无识别结果"));
    timeout_result = false;

    result.active = false;
    snapshot = (xiaotai_recognition_status_t){.phase = RC(RECOVERY), .decision_serial = 5U};
    strcpy(snapshot.request_id, "unaccepted");
    state(XIAOTAI_EVENT_RECOGNITION_STATE);
    assert(!strcmp(shown.status, "任务状态待确认"));
    send(shown.page_actions[4].intent, NULL, shown.page_actions[4].value);
    assert(!strcmp(shown.overlay, "task") && (shown.task[3].state & XIAOTAI_STATE_DISABLED));
    send(shown.overlay_actions[0].intent, NULL, shown.overlay_actions[0].value);
    snapshot.rejected_without_task = true;
    state(XIAOTAI_EVENT_RECOGNITION_STATE);
    send(COMMAND, "archive", 0);
    assert(confirming() && shown.overlay_actions[0].state == 0);
    snapshot.rejected_without_task = false;
    state(XIAOTAI_EVENT_RECOGNITION_STATE);
    assert(confirming() && (shown.overlay_actions[0].state & XIAOTAI_STATE_DISABLED));
    confirm(true);
    assert(confirming() && requested.count == 7U);
    snapshot.rejected_without_task = true;
    state(XIAOTAI_EVENT_RECOGNITION_STATE);
    assert(shown.overlay_actions[0].state == 0);
    post_error = XIAOTAI_ERR_NO_MEM;
    confirm(true);
    assert(requested.count == 8U && requested.action == RC(ACTION_ARCHIVE) &&
           !strcmp(requested.request, "unaccepted") && requested.revision == 0);
    assert(confirming() && !strcmp(shown.toast, "操作未提交，请检查状态"));
    post_error = XIAOTAI_OK;
    confirm(true);
    assert(!confirming() && requested.count == 9U && requested.revision == 0);

    xiaotai_framework_reset();
    xiaotai_app_companion_reset();
    xiaotai_signal_deinit(&synced);
    return 0;
}
