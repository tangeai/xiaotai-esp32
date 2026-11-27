// SPDX-License-Identifier: MIT
#include "fakes.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "xiaotai/media.h"
#include "xiaotai/modules/recognition.h"
#include "xiaotai/port/os.h"

#define RC(name) XIAOTAI_RECOGNITION_##name
#define PAGE XIAOTAI_APP_COMPANION_PAGE_RECOGNITION
#define COMMAND XIAOTAI_INTENT_APP_COMPANION_RECOGNITION

static atomic_int_fast64_t clock_us;
int64_t __wrap_xiaotai_time_us(void);
int64_t __wrap_xiaotai_time_us(void) { return atomic_load(&clock_us); }
static xiaotai_signal_t synced;
static bool fail_status, timeout_result, dismissed;
static unsigned policy_reads;
static uint16_t heard;
static xiaotai_recognition_policy_t policy;
static char fixture_home_status[64];
static xiaotai_err_t post_error;
static xiaotai_recognition_status_t snapshot;
static struct xiaotai_recognition_result { bool active; unsigned leases; } result;
static struct {
    char title[64], status[64], text[XIAOTAI_VIEW_TEXT_MAX], subtitle[160], toast[112];
    unsigned kind, page_action_count;
    char view_id[40];
    char overlay[24], disclosure[128];
    xiaotai_view_action_t page_actions[8], overlay_actions[8];
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

xiaotai_err_t xiaotai_recognition_policy(xiaotai_recognition_policy_t *out) { *out = policy; return XIAOTAI_OK; }
xiaotai_err_t xiaotai_recognition_policy_refresh(void) { ++policy_reads; return XIAOTAI_OK; }
void xiaotai_media_loudness(xiaotai_media_loudness_t *out) { *out = (xiaotai_media_loudness_t){.capture = heard}; }

xiaotai_err_t xiaotai_recognition_result_info(const xiaotai_recognition_result_t *held,
                                               xiaotai_recognition_result_info_t *info)
{
    assert(held == &result && result.leases);
    *info = (xiaotai_recognition_result_info_t){.revision = snapshot.task_revision, .items = 2U, .state = RC(TASK_SUCCEEDED)};
    strcpy(info->request_id, snapshot.request_id);
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_recognition_result_item(const xiaotai_recognition_result_t *held,
                                              size_t index, xiaotai_recognition_item_t *item)
{
    assert(held == &result && result.leases && index < 2U);
    const bool audio = index == 1U;
    *item = (xiaotai_recognition_item_t){.state = RC(ITEM_SUCCEEDED), .alarm = !audio,
        .output = audio ? RC(OUTPUT_AUDIO) : RC(OUTPUT_TEXT), .speech = audio ? RC(SPEECH_READY) : RC(SPEECH_SKIPPED),
        .consumption = audio && dismissed ? RC(DISMISSED) : RC(UNTOUCHED), .has_text = !audio};
    snprintf(item->item_id, sizeof(item->item_id), "item_%u", (unsigned)index + 1U);
    return XIAOTAI_OK;
}

static xiaotai_err_t give(const char *text, char *out, size_t capacity, size_t *size)
{
    const size_t length = strlen(text);
    if (size) *size = length;
    if (capacity <= length) return XIAOTAI_ERR_INVALID_SIZE;
    memcpy(out, text, length + 1U);
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_recognition_result_summary(const xiaotai_recognition_result_t *held,
                                                 size_t index, char *out, size_t capacity, size_t *size)
{
    assert(held == &result && result.leases && index < 2U);
    return index ? XIAOTAI_ERR_NOT_FOUND : give("这是一段识别结果。", out, capacity, size);
}

xiaotai_err_t xiaotai_recognition_result_detail(const xiaotai_recognition_result_t *held, size_t index,
                                                char *out, size_t capacity, size_t *size)
{
    assert(held == &result && result.leases && !index);
    return give("{\"summary\":\"这是一段识别结果。\",\"alarm\":true,\"labels\":[\"哭声\",\"婴儿\"],"
                "\"details\":{\"次数\":3,\"持续\":true,\"依据\":{\"音高\":\"高\"},\"备注\":null}}", out, capacity, size);
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
    } else if (!strcmp(view->id, XIAOTAI_APP_COMPANION_PAGE_HOME)) {
        snprintf(fixture_home_status, sizeof(fixture_home_status), "%s", view->status ? view->status : "");
    } else if (!strncmp(view->id, PAGE, sizeof(PAGE) - 1U)) {
        assert(view->action_count && view->action_count <= 8U && result.leases == 0);
        if (strcmp(shown.view_id, view->id) || shown.kind != view->kind) shown.overlay[0] = '\0';
        shown.kind = view->kind;
        shown.page_action_count = (unsigned)view->action_count;
        strcpy(shown.view_id, view->id);
        strcpy(shown.title, view->title);
        snprintf(shown.status, sizeof(shown.status), "%s", view->status ? view->status : "");
        snprintf(shown.text, sizeof(shown.text), "%s", view->text ? view->text : "");
        snprintf(shown.subtitle, sizeof(shown.subtitle), "%s", view->subtitle ? view->subtitle : "");
        memcpy(shown.page_actions, view->actions, view->action_count * sizeof(*view->actions));
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

static const xiaotai_view_action_t *button(const char *label)
{
    for (unsigned i = 0; i < shown.page_action_count; ++i)
        if (shown.page_actions[i].label && !strcmp(shown.page_actions[i].label, label)) return &shown.page_actions[i];
    return NULL;
}

static void press(const char *label)
{
    const xiaotai_view_action_t *found = button(label);
    assert(found && !(found->state & XIAOTAI_STATE_DISABLED));
    send(found->intent, NULL, found->value);
}

static void blocked_by(const char *why)
{
    state(XIAOTAI_EVENT_RECOGNITION_POLICY);
    assert(strstr(shown.text, why) && !button("开始录音"));
}

int main(void)
{
    static const xiaotai_module_t probe = {.name = "probe", .init = init_probe};
    static const xiaotai_module_t *const modules[] = {&probe};
    static const xiaotai_application_t app = {.name = "companion", .init = xiaotai_app_companion_init,
        .start = xiaotai_app_companion_start, .foreground = xiaotai_app_companion_foreground};
    static const xiaotai_application_t *const apps[] = {&app};
    const uint32_t needs = XIAOTAI_NEED_DISPLAY | XIAOTAI_NEED_TOUCH | XIAOTAI_NEED_KEY;
    fixture = (companion_test_fixture_t){.needs = needs, .navigation_pages = true, .board_settings = true,
        .net.state = XIAOTAI_NET_UP, .binding.phase = XIAOTAI_BINDING_BOUND, .platform.phase = XIAOTAI_PLATFORM_ONLINE};
    const xiaotai_framework_config_t config = {.modules = modules, .module_count = 1,
        .applications = apps, .application_count = 1, .available_needs = needs};
    assert(xiaotai_signal_init(&synced) == XIAOTAI_OK);
    assert(xiaotai_framework_start(&config) == XIAOTAI_OK);
    flush();

    /* Coming in: what it is, what it needs, and how to get that. */
    send(XIAOTAI_INTENT_APP_COMPANION_SHOW, PAGE, 0);
    assert(policy_reads == 1 && !strcmp(shown.title, "声音识别") && strstr(shown.subtitle, "云端"));
    assert(strstr(shown.text, "正在查看") && !button("开始录音"));
    policy = (xiaotai_recognition_policy_t){.valid = true, .scheme_count = 2, .monthly_limit = 100, .used = 3};
    blocked_by("不能用声音识别");
    policy.allowed = true;
    blocked_by("还没有打开");
    policy.enabled = policy.effective_enabled = true;
    policy.scheme_count = 0;
    blocked_by("添加音频类型");
    policy.scheme_count = 2;
    policy.used = 99;
    blocked_by("用完了");
    policy.used = 3;
    fixture.platform.phase = XIAOTAI_PLATFORM_LOGIN;
    blocked_by("还没连上服务器");
    fixture.platform.phase = XIAOTAI_PLATFORM_ONLINE;
    fixture.net.state = XIAOTAI_NET_CONNECTING;
    blocked_by("没有联网");
    fixture.net.state = XIAOTAI_NET_UP;
    state(XIAOTAI_EVENT_RECOGNITION_POLICY);
    assert(strstr(shown.text, "用了 3 次，上限 100 次") && strstr(shown.text, "录一次用 2 次") && button("开始录音"));

    press("时长");
    assert(!strcmp(shown.overlay, "record") && shown.field.kind == XIAOTAI_FIELD_STEPPER &&
           shown.field.value == 10 && shown.field.max == 60);
    send(shown.field.intent, NULL, 61);
    assert(shown.field.value == 10);
    send(shown.field.intent, NULL, 15);
    assert(shown.field.value == 15);
    send(shown.overlay_actions[0].intent, NULL, shown.overlay_actions[0].value);
    assert(!shown.overlay[0] && strstr(shown.text, "录 15 秒") && !requested.count);

    press("开始录音");
    assert(requested.count == 1 && requested.action == RC(ACTION_RECORD) && requested.value == 15);
    assert(button("开始录音")->state & XIAOTAI_STATE_DISABLED);     /* once, until it is answered */
    atomic_store(&clock_us, 5000000);
    flush();
    assert(!strcmp(shown.toast, "没有等到回应，请看看现在的状态"));
    press("开始录音");
    receipt(1U, RC(ACTION_RECORD), XIAOTAI_OK);

    snapshot.phase = RC(CONFIG);
    snapshot.token = 190;
    state(XIAOTAI_EVENT_RECOGNITION_STATE);
    assert(shown.kind == XIAOTAI_VIEW_TALK && !strcmp(shown.status, "正在确认额度") && button("取消"));
    snapshot.phase = RC(RECORDING);
    snapshot.samples = 8000;
    snapshot.target_samples = 120000;
    snapshot.capture_rate_hz = 8000;
    snapshot.token = 191;
    state(XIAOTAI_EVENT_RECOGNITION_STATE);
    assert(shown.kind == XIAOTAI_VIEW_TALK && !strcmp(shown.status, "正在听，还有 14 秒"));
    snapshot.samples = 24000;
    state(XIAOTAI_EVENT_RECOGNITION_PROGRESS);
    assert(!strcmp(shown.status, "正在听，还有 12 秒") && strstr(shown.text, "靠近一点"));
    heard = 4000;
    state(XIAOTAI_EVENT_RECOGNITION_PROGRESS);
    assert(strstr(shown.text, "不消耗次数"));
    press("取消");
    assert(requested.count == 3 && requested.action == RC(ACTION_CANCEL) && requested.value == 191);
    receipt(2U, RC(ACTION_CANCEL), XIAOTAI_OK);
    snapshot.phase = RC(CANCELLED);
    snapshot.token = 0;
    state(XIAOTAI_EVENT_RECOGNITION_STATE);
    assert(shown.kind == XIAOTAI_VIEW_NOTICE && button("开始录音"));

    /* A recording that failed: why, and once more. */
    snapshot.phase = RC(FAILED);
    snapshot.error = XIAOTAI_ERR_TIMEOUT;
    state(XIAOTAI_EVENT_RECOGNITION_STATE);
    assert(strstr(shown.text, "等太久") && button("再录一次"));
    snapshot.error = XIAOTAI_FAIL;
    snapshot.code = 40321;
    state(XIAOTAI_EVENT_RECOGNITION_STATE);
    assert(strstr(shown.text, "40321"));
    snapshot.error = XIAOTAI_OK;
    snapshot.code = 0;

    /* Uploading, then waiting for the service; the page may be left. */
    snapshot.phase = RC(UPLOAD);
    snapshot.token = 193;
    strcpy(snapshot.request_id, "request");
    state(XIAOTAI_EVENT_RECOGNITION_STATE);
    assert(!strcmp(shown.status, "正在上传") && button("取消"));
    snapshot.phase = RC(ACCEPTED);
    snapshot.token = 0;
    state(XIAOTAI_EVENT_RECOGNITION_STATE);
    assert(shown.kind == XIAOTAI_VIEW_TALK && strstr(shown.status, "云端在分析") && strstr(shown.text, "先去做别的"));
    press("问一下结果");
    assert(requested.count == 4 && requested.action == RC(ACTION_QUERY));
    receipt(3U, RC(ACTION_QUERY), XIAOTAI_OK);
    send(XIAOTAI_INTENT_APP_COMPANION_BACK, NULL, 0);
    result.active = true;
    snapshot.phase = RC(RESULT);
    snapshot.task_state = RC(TASK_SUCCEEDED);
    snapshot.task_revision = 9007199254740988ULL;
    state(XIAOTAI_EVENT_RECOGNITION_RESULT);
    send(XIAOTAI_INTENT_APP_COMPANION_HOME, NULL, 0);
    assert(!strcmp(fixture_home_status, "声音识别有结果了"));

    /* The answer first, then the details in words. */
    send(XIAOTAI_INTENT_APP_COMPANION_SHOW, PAGE, 0);
    assert(!strcmp(shown.title, "结果 1 / 2") && !strcmp(shown.subtitle, "结论：可能有异常，请留意") &&
           !strcmp(shown.text, "这是一段识别结果。") && result.leases == 0);
    press("详细");
    assert(strstr(shown.text, "标签：哭声、婴儿") && strstr(shown.text, "次数：3") && strstr(shown.text, "持续：是") &&
           !strchr(shown.text, '{') && !shown.subtitle[0]);
    press("收起");
    timeout_result = true;
    state(XIAOTAI_EVENT_RECOGNITION_RESULT);
    assert(!strcmp(shown.text, "这是一段识别结果。") && (button("下一项")->state & XIAOTAI_STATE_DISABLED));
    timeout_result = false;
    state(XIAOTAI_EVENT_RECOGNITION_RESULT);
    press("下一项");
    assert(!strcmp(shown.title, "结果 2 / 2") && strstr(shown.text, "用语音回答") && !button("详细"));
    press("听一遍");
    assert(requested.count == 5 && requested.action == RC(ACTION_RESUME) && !strcmp(requested.request, "request"));
    receipt(4U, RC(ACTION_RESUME), XIAOTAI_OK);
    snapshot.phase = RC(PLAYBACK);
    snapshot.token = 200;
    state(XIAOTAI_EVENT_RECOGNITION_STATE);
    assert(!strcmp(shown.status, "播放中"));
    press("停止播放");
    assert(requested.count == 6 && requested.action == RC(ACTION_CANCEL) && requested.value == 200);
    receipt(5U, RC(ACTION_CANCEL), XIAOTAI_OK);
    snapshot.phase = RC(RESULT);
    snapshot.token = 0;
    state(XIAOTAI_EVENT_RECOGNITION_STATE);

    /* Done: asked once; what was not heard is let go first. */
    press("完成");
    assert(confirming() && requested.count == 6);
    confirm(false);
    assert(!shown.overlay[0] && requested.count == 6);
    press("完成");
    const uint64_t seen = ++snapshot.task_revision;
    confirm(true);
    assert(requested.count == 7 && requested.action == RC(ACTION_DISMISS) && !strcmp(requested.item, "item_2"));
    dismissed = true;
    receipt(6U, RC(ACTION_DISMISS), XIAOTAI_OK);
    assert(requested.count == 8 && requested.action == RC(ACTION_ARCHIVE) && requested.revision == seen);
    result.active = false;
    snapshot = (xiaotai_recognition_status_t){.phase = RC(IDLE), .decision_serial = 6U};
    receipt(7U, RC(ACTION_ARCHIVE), XIAOTAI_OK);
    assert(!strcmp(shown.toast, "这次识别已完成") && button("开始录音"));

    /* A refused request says so and leaves the page as it is. */
    post_error = XIAOTAI_ERR_NO_MEM;
    press("开始录音");
    assert(!strcmp(shown.toast, "现在做不了，请稍后再试") && !(button("开始录音")->state & XIAOTAI_STATE_DISABLED));
    post_error = XIAOTAI_OK;

    /* After a restart the stored request is still there: never recorded again. */
    snapshot = (xiaotai_recognition_status_t){.phase = RC(RECOVERY), .decision_serial = 7U};
    strcpy(snapshot.request_id, "unaccepted");
    state(XIAOTAI_EVENT_RECOGNITION_STATE);
    assert(strstr(shown.text, "不会重复录") && button("问一下结果") && !button("重新开始"));
    snapshot.rejected_without_task = true;
    state(XIAOTAI_EVENT_RECOGNITION_STATE);
    assert(strstr(shown.text, "没有收到"));
    const unsigned before = requested.count;
    press("重新开始");
    assert(requested.count == before + 1 && requested.action == RC(ACTION_ARCHIVE) && requested.revision == 0 &&
           !strcmp(requested.request, "unaccepted"));
    const xiaotai_view_action_t stale = shown.page_actions[0];
    state(XIAOTAI_EVENT_RECOGNITION_STATE);
    send(stale.intent, NULL, stale.value);
    assert(requested.count == before + 1);

    fail_status = true;
    state(XIAOTAI_EVENT_RECOGNITION_STATE);
    assert(strstr(shown.text, "正在读取"));

    xiaotai_framework_reset();
    xiaotai_app_companion_reset();
    xiaotai_signal_deinit(&synced);
    return 0;
}
