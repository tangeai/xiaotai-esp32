// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <string.h>
#include "../main/xmini_product.c"
#include "../components/xmini_settings/xmini_settings.c"

static unsigned failures, save_calls, restarts, prompt_calls, take_calls, stop_calls, platform_starts, portal_starts;
static bool saved_wifi, take_verified, fail_save, hold_confirmation, hold_binding, fail_confirmation;
static int64_t time_us;
static xmini_audio_status_t audio_status;
static platform_status_t network_status;
static xmini_prompt_t last_prompt;
static bool preference_present;
static int preference_commit_error;
static unsigned preference_commits;
static settings_record_t preference_record, preference_staged;
static void (*wait_hook)(TickType_t wait);
static void (*clear_hook)(void);
static int64_t binding_started_us;
static bool fail_binding;
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #condition); ++failures; } } while (0)

QueueHandle_t xQueueCreateStatic(unsigned length, unsigned size, uint8_t *buffer, StaticQueue_t *storage)
{ (void)length; *storage = (StaticQueue_t){.item_size = size, .buffer = buffer}; return storage; }
int xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait)
{ (void)wait; if (queue->count) return pdFALSE; memcpy(queue->buffer, item, queue->item_size); queue->count = 1; return pdTRUE; }
int xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait)
{ (void)wait; if (!queue->count) return pdFALSE; memcpy(item, queue->buffer, queue->item_size); queue->count = 0; return pdTRUE; }
EventGroupHandle_t xEventGroupCreateStatic(StaticEventGroup_t *storage) { storage->bits = 0; return storage; }
EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits) { return group->bits |= bits; }
EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits)
{
    EventBits_t old = group->bits;
    group->bits &= ~bits;
    if (clear_hook) clear_hook();
    return old;
}
EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits, int clear, int all, TickType_t wait)
{
    (void)all;
    if (!(group->bits & bits)) {
        if (wait_hook) wait_hook(wait);
        else time_us += (int64_t)wait * 1000;
    }
    EventBits_t ready = group->bits;
    if (clear) group->bits &= ~bits;
    return ready;
}
int64_t esp_timer_get_time(void) { return time_us; }
void esp_restart(void) { ++restarts; }
esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{ CHECK(!strcmp(name, "xmini_ui")); *handle = 1; return mode == NVS_READONLY && !preference_present ? ESP_ERR_NVS_NOT_FOUND : ESP_OK; }
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *size)
{ CHECK(handle == 1 && !strcmp(key, "audio") && *size == sizeof(preference_record)); memcpy(value, &preference_record, sizeof(preference_record)); return ESP_OK; }
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t size)
{ CHECK(handle == 1 && !strcmp(key, "audio") && size == sizeof(preference_record)); memcpy(&preference_staged, value, size); return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t handle)
{ CHECK(handle == 1); ++preference_commits; if (!preference_commit_error) { preference_record = preference_staged; preference_present = true; } return preference_commit_error; }
void nvs_close(nvs_handle_t handle) { CHECK(handle == 1); }
const char *esp_err_to_name(esp_err_t error) { (void)error; return "mock"; }
bool xmini_runtime_ready(void) { return true; }
bool xmini_business_platform_event(const platform_event_t *event) { (void)event; return true; }
xmini_audio_status_t xmini_audio_status(void) { return audio_status; }
esp_err_t xmini_audio_request(xmini_audio_request_t request)
{ if (request.kind == XMINI_AUDIO_STOP) { ++stop_calls; audio_status.busy = false; audio_status.stopped = true; } return ESP_OK; }
esp_err_t xmini_audio_prompt(xmini_prompt_t prompt, const char *code)
{
    (void)code; last_prompt = prompt; ++prompt_calls;
    if ((fail_confirmation && prompt == XMINI_PROMPT_WIFI_CONNECTED) ||
        (fail_binding && prompt == XMINI_PROMPT_BINDING)) {
        audio_status.error = true;
        return ESP_FAIL;
    }
    audio_status = (xmini_audio_status_t){.ready = true, .owner = XMINI_AUDIO_GUIDANCE,
        .prompt = prompt, .frames = 1, .expected_frames = 1,
        .busy = (hold_confirmation && prompt == XMINI_PROMPT_WIFI_CONNECTED) ||
                (hold_binding && prompt == XMINI_PROMPT_BINDING)};
    if (prompt == XMINI_PROMPT_BINDING) {
        binding_started_us = time_us;
        if (hold_binding) audio_status.expected_frames = 1049;
    }
    return ESP_OK;
}
platform_status_t platform_status(void) { return network_status; }
esp_err_t platform_set_binding_prompt(const platform_binding_prompt_t *prompt) { (void)prompt; return ESP_OK; }
bool platform_load_wifi(char *ssid, size_t ssid_size, char *password, size_t password_size)
{ if (saved_wifi) { snprintf(ssid, ssid_size, "fixture"); snprintf(password, password_size, "fixture-pass"); } return saved_wifi; }
bool platform_wifi_provisioning_requested(void) { return false; }
esp_err_t platform_start(const char *ssid, const char *password, const char *url, platform_observer_t handler)
{ (void)ssid; (void)password; (void)url; (void)handler; ++platform_starts; return ESP_OK; }
esp_err_t platform_retry(void) { return ESP_OK; }
esp_err_t platform_save_wifi(const char *ssid, const char *password)
{
    CHECK(!strcmp(ssid, "verified-fixture") && !strcmp(password, "fixture-only"));
    ++save_calls; return fail_save ? ESP_FAIL : ESP_OK;
}
esp_err_t wifi_provisioning_start(void) { ++portal_starts; return ESP_OK; }
bool wifi_provisioning_take_credentials(wifi_provisioning_credentials_t *credentials)
{
    ++take_calls;
    if (!take_verified) return false;
    take_verified = false;
    strcpy(credentials->ssid, "verified-fixture"); strcpy(credentials->password, "fixture-only");
    return true;
}
wifi_provisioning_status_t wifi_provisioning_status(void)
{ return (wifi_provisioning_status_t){.ssid = "XiaoTai-TEST"}; }

static void reset_product(void)
{
    s_first = true; s_portal = s_saw_binding = false; s_init_error = ESP_OK;
    s_pending_prompt = s_binding_pending = s_binding_playing = false;
    s_pending_prompt_feedback = false;
    s_pending_prompt_explicit = s_active_prompt_explicit = false;
    memset(&s_binding_wait, 0, sizeof(s_binding_wait));
    s_binding_pending_id = s_binding_playing_id = 0;
    memset(s_active_binding_code, 0, sizeof(s_active_binding_code));
    s_restart_pending = false; atomic_store(&s_cancel_binding, false);
    s_verified_wifi_pending = s_wifi_saved = false;
    s_wifi_save_error = s_wifi_confirmation_error = ESP_OK;
    s_wifi_settings_held = false;
    memset(&s_verified_wifi, 0, sizeof(s_verified_wifi));
    memset(&s_guide, 0, sizeof(s_guide)); memset(&s_previous_guide, 0, sizeof(s_previous_guide));
    memset(s_prompt_code, 0, sizeof(s_prompt_code));
    save_calls = restarts = prompt_calls = take_calls = stop_calls = platform_starts = portal_starts = 0;
    time_us = 1000000;
    wait_hook = NULL; clear_hook = NULL; binding_started_us = 0; fail_binding = false;
    preference_present = false; preference_commit_error = 0; preference_commits = 0;
    memset(&s_state, 0, sizeof(s_state)); s_loading = s_quiesced = false;
    int volume, gain;
    CHECK(xmini_settings_load(&volume, &gain) == ESP_OK && volume == 40 && gain == 24);
    saved_wifi = true; take_verified = fail_save = hold_confirmation = hold_binding = fail_confirmation = false;
    audio_status = (xmini_audio_status_t){.ready = true};
    network_status = (platform_status_t){.phase = PLATFORM_PHASE_BINDING, .expires_at_ms = 60000};
    strcpy(network_status.code, "012345");
    CHECK(xmini_product_init(ESP_OK) == ESP_OK);
}

static void test_binding_feedback_once(void)
{
    reset_product(); xmini_ui_t ui = {0};
    xmini_product_poll(&ui);
    network_status.phase = PLATFORM_PHASE_ONLINE; network_status.code[0] = 0;
    xmini_product_poll(&ui); CHECK(last_prompt == XMINI_PROMPT_BOUND);
    network_status.phase = PLATFORM_PHASE_OFFLINE; xmini_product_poll(&ui);
    network_status.phase = PLATFORM_PHASE_ONLINE; xmini_product_poll(&ui);
    CHECK(last_prompt == XMINI_PROMPT_ONLINE);
    network_status.phase = PLATFORM_PHASE_BINDING; strcpy(network_status.code, "001122");
    xmini_product_poll(&ui);
    network_status.phase = PLATFORM_PHASE_ONLINE; network_status.code[0] = 0;
    xmini_product_poll(&ui); CHECK(last_prompt == XMINI_PROMPT_BOUND && !s_saw_binding);
}

static void test_feedback_waits_and_coalesces(void)
{
    reset_product(); xmini_ui_t ui = {0};
    network_status.phase = PLATFORM_PHASE_ONLINE; network_status.code[0] = 0;
    audio_status.busy = true;
    xmini_product_poll(&ui);
    unsigned before = prompt_calls;
    CHECK(xmini_product_feedback(XMINI_PROMPT_INCOMING) == ESP_OK);
    CHECK(xmini_product_feedback(XMINI_PROMPT_DIALING) == ESP_OK);
    for (unsigned i = 0; i < 20; ++i) xmini_product_poll(&ui);
    CHECK(!stop_calls && prompt_calls == before && audio_status.busy);
    CHECK(s_pending_prompt && s_pending_prompt_feedback && s_prompt == XMINI_PROMPT_DIALING);
    audio_status.busy = false;
    xmini_product_poll(&ui);
    CHECK(prompt_calls == before + 1 && last_prompt == XMINI_PROMPT_DIALING);
    CHECK(!s_pending_prompt && !s_pending_prompt_feedback);
    for (unsigned i = 0; i < 20; ++i) xmini_product_poll(&ui);
    CHECK(prompt_calls == before + 1 && !stop_calls);
}

static void test_feedback_cancel_preserves_critical_prompts(void)
{
    reset_product(); xmini_ui_t ui = {0};
    network_status.phase = PLATFORM_PHASE_ONLINE; network_status.code[0] = 0;
    xmini_product_poll(&ui);
    CHECK(xmini_product_feedback(XMINI_PROMPT_INCOMING) == ESP_OK);
    CHECK(s_pending_prompt && s_pending_prompt_feedback);
    CHECK(xmini_product_feedback(-1) == ESP_OK && !s_pending_prompt);
    CHECK(xmini_product_feedback(XMINI_PROMPT_STARTUP) == ESP_ERR_INVALID_ARG);
    network_status.phase = PLATFORM_PHASE_BINDING; strcpy(network_status.code, "001122");
    xmini_product_poll(&ui);
    CHECK(xmini_product_confirm() == ESP_OK);
    CHECK(s_pending_prompt && !s_pending_prompt_feedback && s_prompt == XMINI_PROMPT_BINDING);
    CHECK(xmini_product_feedback(-1) == ESP_OK);
    CHECK(s_pending_prompt && s_prompt == XMINI_PROMPT_BINDING && !strcmp(s_prompt_code, "001122"));
    CHECK(xmini_product_feedback(XMINI_PROMPT_INCOMING) == ESP_ERR_INVALID_STATE);
    CHECK(s_pending_prompt && s_prompt == XMINI_PROMPT_BINDING);
    s_pending_prompt = false;
    network_status.phase = PLATFORM_PHASE_ONLINE; network_status.code[0] = 0;
    xmini_product_poll(&ui);
    s_restart_pending = true;
    CHECK(xmini_product_feedback(XMINI_PROMPT_FAILED) == ESP_ERR_INVALID_STATE);
}

static void test_verified_save_explicit_retry(void)
{
    reset_product(); saved_wifi = false; s_portal = true;
    xmini_ui_t ui = {0}; take_verified = fail_save = true;
    xmini_product_poll(&ui);
    CHECK(save_calls == 1 && restarts == 0 && xmini_product_guide().phase == XMINI_GUIDE_ERROR);
    for (unsigned i = 0; i < 50; ++i) xmini_product_poll(&ui);
    CHECK(save_calls == 1 && take_calls == 1);
    CHECK(s_verified_wifi_pending && s_verified_wifi.password[0] && !s_wifi_saved);
    CHECK(xmini_product_confirm() == ESP_FAIL && save_calls == 2);
    xmini_product_poll(&ui);
    CHECK(save_calls == 2 && s_verified_wifi_pending);
    fail_save = false; hold_confirmation = true;
    CHECK(xmini_product_confirm() == ESP_OK);
    CHECK(save_calls == 3 && restarts == 0);
    CHECK(!s_verified_wifi_pending && !s_verified_wifi.ssid[0] && !s_verified_wifi.password[0] && s_wifi_saved);
    xmini_product_poll(&ui);
    CHECK(last_prompt == XMINI_PROMPT_WIFI_CONNECTED);
    CHECK(restarts == 0 && audio_status.busy);
    CHECK(xmini_product_confirm() == ESP_ERR_INVALID_STATE);
    CHECK(save_calls == 3);
    audio_status.busy = false; xmini_product_poll(&ui);
    CHECK(restarts == 1);
}

static void test_saved_restart_independent_of_audio(void)
{
    for (unsigned mode = 0; mode < 4; ++mode) {
        reset_product(); s_portal = true; take_verified = true;
        hold_confirmation = mode == 0 || mode == 3;
        fail_confirmation = mode == 1;
        if (mode == 2) {
            audio_status.busy = true;
            audio_status.owner = XMINI_AUDIO_DIAGNOSTIC;
        }
        xmini_ui_t ui = {0};
        xmini_product_poll(&ui);
        CHECK(save_calls == 1);
        CHECK(s_wifi_saved && s_wifi_settings_held && xmini_settings_status().saved);
        CHECK(!s_verified_wifi_pending && !s_verified_wifi.password[0]);
        CHECK(!s_wifi_confirmation_error);
        if (mode == 0 || mode == 3) {
            CHECK(audio_status.busy && last_prompt == XMINI_PROMPT_WIFI_CONNECTED);
            CHECK(!restarts && s_restart_pending && !s_pending_prompt);
        }
        if (mode == 1) CHECK(audio_status.error && restarts == 1);
        if (mode == 2) CHECK(audio_status.busy && audio_status.owner == XMINI_AUDIO_DIAGNOSTIC);
        CHECK(xmini_product_confirm() == ESP_ERR_INVALID_STATE);
        for (unsigned i = 0; i < 20; ++i) xmini_product_poll(&ui);
        if (mode != 1) {
            CHECK(!restarts && s_restart_pending && audio_status.busy);
            audio_status.busy = false;
            if (mode == 0) {
                audio_status.error = true;
                audio_status.frames = 0;
            } else if (mode == 3) {
                audio_status.stopped = true;
                audio_status.frames = 0;
            }
            xmini_product_poll(&ui);
        }
        CHECK(restarts == 1 && !s_restart_pending);
        for (unsigned i = 0; i < 20; ++i) xmini_product_poll(&ui);
        CHECK(save_calls == 1 && restarts == 1);
    }
}

static void test_binding_expiry_and_diagnostic_owner(void)
{
    reset_product(); xmini_ui_t ui = {0}; hold_binding = true;
    xmini_product_poll(&ui);
    CHECK(xmini_product_confirm() == ESP_OK);
    xmini_product_poll(&ui);
    CHECK(audio_status.busy && last_prompt == XMINI_PROMPT_BINDING);
    time_us = 60001000;
    xmini_product_poll(&ui);
    CHECK(stop_calls == 1 && audio_status.stopped && !audio_status.busy);
    audio_status.owner = XMINI_AUDIO_DIAGNOSTIC;
    audio_status.busy = true; audio_status.stopped = false;
    xmini_product_poll(&ui);
    CHECK(stop_calls == 1 && audio_status.busy && !audio_status.stopped);
}

static void test_runtime_failure_visible(void)
{
    const esp_err_t errors[] = {ESP_ERR_NO_MEM, ESP_ERR_TIMEOUT, ESP_FAIL};
    for (unsigned i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        reset_product(); xmini_ui_t ui = {0};
        platform_starts = portal_starts = 0;
        CHECK(xmini_product_init(errors[i]) == errors[i]);
        network_status.phase = PLATFORM_PHASE_ONLINE;
        xmini_product_poll(&ui);
        xmini_guide_t guide = xmini_product_guide();
        CHECK(guide.phase == XMINI_GUIDE_ERROR && guide.error == (unsigned)errors[i] && guide.startup_failed);
        CHECK(ui.page == XMINI_PAGE_GUIDE && !platform_starts && !portal_starts);
        CHECK(xmini_product_confirm() == errors[i]);
        uint8_t failed[1024], generic[1024];
        xmini_guide_render(&guide, 1000, failed);
        guide.startup_failed = false; xmini_guide_render(&guide, 1000, generic);
        CHECK(memcmp(failed, generic, sizeof(failed)) != 0);
    }
}

static void test_portal_restart_pending_preferences(void)
{
    reset_product(); s_portal = true; take_verified = hold_confirmation = true;
    xmini_ui_t ui = {0};
    CHECK(xmini_settings_schedule(65, 24) == ESP_OK);
    xmini_product_poll(&ui);
    audio_status.busy = false;
    xmini_product_poll(&ui);
    CHECK(xmini_settings_status().pending && !preference_commits && !restarts);
    CHECK(xmini_settings_status().quiesced && s_wifi_settings_held);
    CHECK(xmini_settings_schedule(90, 36) == ESP_ERR_INVALID_STATE);
    time_us = 1999000; xmini_settings_process(); xmini_product_poll(&ui);
    CHECK(!preference_commits && !restarts && xmini_settings_status().volume == 65);
    time_us = 2000000; xmini_settings_process(); xmini_product_poll(&ui);
    CHECK(preference_commits == 1 && preference_record.volume == 65 && preference_record.gain_db == 24);
    CHECK(xmini_settings_status().saved && restarts == 1 && save_calls == 1);
}

static void test_portal_preferences_failure_explicit_retry(void)
{
    reset_product(); s_portal = true; take_verified = hold_confirmation = true;
    xmini_ui_t ui = {0};
    CHECK(xmini_settings_schedule(65, 24) == ESP_OK);
    xmini_product_poll(&ui);
    preference_commit_error = ESP_FAIL;
    time_us = 2000000; xmini_settings_process(); xmini_product_poll(&ui);
    CHECK(preference_commits == 1 && !restarts && !s_restart_pending);
    CHECK(s_wifi_saved && s_wifi_settings_held && xmini_settings_status().quiesced);
    CHECK(xmini_settings_status().error == ESP_FAIL && !xmini_settings_status().attempt_pending);
    xmini_product_poll(&ui);
    CHECK(xmini_product_guide().phase == XMINI_GUIDE_ERROR && xmini_product_guide().retryable);
    CHECK(xmini_product_guide().error == (unsigned)ESP_FAIL);
    struct { uint8_t before[16], frame[1024], after[16]; } canvas;
    memset(&canvas, 0xA5, sizeof(canvas));
    xmini_guide_t guide = xmini_product_guide();
    xmini_guide_render(&guide, 2000, canvas.frame);
    for (unsigned i = 0; i < 16; ++i) CHECK(canvas.before[i] == 0xA5 && canvas.after[i] == 0xA5);
    uint8_t without_retry[1024]; guide.retryable = false;
    xmini_guide_render(&guide, 2000, without_retry);
    CHECK(memcmp(canvas.frame, without_retry, sizeof(without_retry)) != 0);
    for (unsigned i = 0; i < 50; ++i) {
        time_us += 1000000; xmini_settings_process(); xmini_product_poll(&ui);
    }
    CHECK(preference_commits == 1 && !restarts && save_calls == 1);
    CHECK(xmini_settings_schedule(90, 36) == ESP_ERR_INVALID_STATE);
    CHECK(xmini_product_confirm() == ESP_OK);
    CHECK(xmini_settings_status().error == ESP_FAIL && xmini_settings_status().attempt_pending);
    CHECK(xmini_product_confirm() == ESP_ERR_INVALID_STATE);
    xmini_product_poll(&ui); audio_status.busy = false;
    xmini_product_poll(&ui);
    CHECK(s_restart_pending && !restarts && preference_commits == 1);
    time_us += 999000; xmini_settings_process(); xmini_product_poll(&ui);
    CHECK(s_restart_pending && !restarts && preference_commits == 1);
    time_us += 1000; xmini_settings_process(); xmini_product_poll(&ui);
    CHECK(!s_restart_pending && !restarts && preference_commits == 2);
    CHECK(xmini_settings_status().error == ESP_FAIL && !xmini_settings_status().attempt_pending);
    for (unsigned i = 0; i < 50; ++i) { ++time_us; xmini_settings_process(); xmini_product_poll(&ui); }
    CHECK(preference_commits == 2 && !restarts);
    preference_commit_error = 0;
    CHECK(xmini_product_confirm() == ESP_OK);
    xmini_product_poll(&ui); audio_status.busy = false;
    time_us += 1000000; xmini_settings_process(); xmini_product_poll(&ui);
    CHECK(preference_commits == 3 && restarts == 1 && save_calls == 1);
    CHECK(xmini_settings_status().saved && !xmini_settings_status().pending && !xmini_settings_status().error);
    CHECK(preference_record.volume == 65 && preference_record.gain_db == 24);
}

static void test_guidance_disabled_and_explicit_replay(void)
{
    reset_product(); xmini_ui_t ui = {0};
    CHECK(xmini_settings_schedule_guidance(false) == ESP_OK);
    unsigned prompts = prompt_calls;
    CHECK(xmini_product_init(ESP_OK) == ESP_OK && prompt_calls == prompts);
    xmini_product_poll(&ui);
    CHECK(ui.page == XMINI_PAGE_GUIDE && xmini_product_guide().phase == XMINI_GUIDE_BINDING);
    CHECK(!strcmp(xmini_product_guide().code, "012345"));
    CHECK(play_binding("012345", NULL) == ESP_OK && !s_prompt_queue->count && prompt_calls == prompts);
    hold_binding = true;
    CHECK(xmini_product_confirm() == ESP_OK); xmini_product_poll(&ui);
    CHECK(prompt_calls == prompts + 1 && last_prompt == XMINI_PROMPT_BINDING && audio_status.busy);
    xmini_product_poll(&ui);
    CHECK(audio_status.busy && !stop_calls);
    network_status.phase = PLATFORM_PHASE_ONLINE; network_status.code[0] = 0;
    audio_status.busy = false; xmini_product_poll(&ui);
    CHECK(ui.page == XMINI_PAGE_HOME && prompt_calls == prompts + 1);
    network_status.phase = PLATFORM_PHASE_OFFLINE; xmini_product_poll(&ui);
    CHECK(last_prompt == XMINI_PROMPT_OFFLINE && prompt_calls == prompts + 2);
    network_status.phase = PLATFORM_PHASE_ERROR; xmini_product_poll(&ui);
    CHECK(last_prompt == XMINI_PROMPT_FAILED && prompt_calls == prompts + 3);

    reset_product(); xmini_product_poll(&ui);
    binding_prompt_t automatic = {.code = "012345", .request_id = 1}; hold_binding = true;
    s_binding_wait.sequence = s_binding_wait.waiter = 1;
    CHECK(xQueueSend(s_prompt_queue, &automatic, 0) == pdTRUE);
    xmini_product_poll(&ui); CHECK(s_binding_playing && audio_status.busy);
    CHECK(xmini_settings_schedule_guidance(false) == ESP_OK);
    xmini_product_poll(&ui);
    CHECK(stop_calls == 1 && !audio_status.busy && !s_binding_playing);
    CHECK(s_prompt_events->bits & PROMPT_DONE);
    audio_status = (xmini_audio_status_t){.ready = true, .busy = true, .owner = XMINI_AUDIO_RTC,
        .prompt = XMINI_PROMPT_BINDING};
    xmini_product_poll(&ui); CHECK(stop_calls == 1 && audio_status.busy);
    audio_status.owner = XMINI_AUDIO_DIAGNOSTIC;
    xmini_product_poll(&ui); CHECK(stop_calls == 1 && audio_status.busy);
}

static void test_portal_silent_confirmation_waits_settings(void)
{
    reset_product(); s_portal = true; take_verified = true;
    CHECK(xmini_settings_schedule_guidance(false) == ESP_OK);
    xmini_ui_t ui = {0}; unsigned prompts = prompt_calls;
    xmini_product_poll(&ui);
    CHECK(save_calls == 1 && s_restart_pending && !restarts);
    CHECK(prompt_calls == prompts && xmini_settings_status().quiesced);
    CHECK(xmini_settings_schedule_guidance(true) == ESP_ERR_INVALID_STATE);
    time_us = 1999000; xmini_settings_process(); xmini_product_poll(&ui);
    CHECK(!preference_commits && !restarts);
    time_us = 2000000; xmini_settings_process(); xmini_product_poll(&ui);
    CHECK(preference_commits == 1 && restarts == 1 && preference_record.reserved == 0);
    CHECK(preference_record.version == 2 && xmini_settings_status().saved);
}

enum binding_wait_mode { BIND_PROGRESS, BIND_HOLD, BIND_LATE_RESULT, BIND_CANCEL,
    BIND_EXPIRE, BIND_CHANGE_CODE, BIND_GUIDANCE_OFF, BIND_SPURIOUS, BIND_AUDIO_ERROR };
static enum binding_wait_mode binding_mode;
static xmini_ui_t binding_ui;
static uint32_t stale_binding_id;
static unsigned wait_steps, late_attempts;

static void binding_wait_step(TickType_t wait)
{
    ++wait_steps;
    time_us += (int64_t)(wait < 50 ? wait : 50) * 1000;
    if (audio_status.busy && audio_status.owner == XMINI_AUDIO_GUIDANCE &&
        audio_status.prompt == XMINI_PROMPT_BINDING) {
        if (binding_mode != BIND_HOLD && binding_mode != BIND_SPURIOUS &&
            !(binding_mode == BIND_LATE_RESULT && s_binding_playing_id == stale_binding_id)) {
            unsigned frames = (unsigned)((time_us - binding_started_us) / 20000);
            audio_status.frames = frames < 1049 ? frames : 1049;
            if (frames >= 1049) audio_status.busy = false;
        }
        if (audio_status.frames >= 100) {
            if (binding_mode == BIND_CANCEL) {
                cancel_binding(NULL);
                network_status.phase = PLATFORM_PHASE_VERIFYING;
                network_status.code[0] = 0;
            } else if (binding_mode == BIND_EXPIRE) {
                network_status.expires_at_ms = time_us / 1000;
            } else if (binding_mode == BIND_CHANGE_CODE) {
                strcpy(network_status.code, "654321");
            } else if (binding_mode == BIND_GUIDANCE_OFF) {
                CHECK(xmini_settings_schedule_guidance(false) == ESP_OK);
            } else if (binding_mode == BIND_AUDIO_ERROR) {
                audio_status.error = true;
                audio_status.busy = false;
            }
        }
    }
    xmini_product_poll(&binding_ui);
    if (binding_mode == BIND_SPURIOUS) xEventGroupSetBits(s_prompt_events, PROMPT_DONE);
    if (binding_mode == BIND_LATE_RESULT && s_binding_playing_id &&
        s_binding_playing_id != stale_binding_id && late_attempts == 0) {
        ++late_attempts;
        finish_binding_request(stale_binding_id, PROMPT_DONE);
        finish_binding_request(stale_binding_id, PROMPT_FAILED);
        finish_binding_request(stale_binding_id, PROMPT_CANCELLED);
        CHECK(s_binding_wait.result == 0 && binding_request_live(s_binding_playing_id));
        // An old SetBits may arrive after the new request cleared notifications.
        xEventGroupSetBits(s_prompt_events, PROMPT_DONE);
    }
}

static void prepare_binding_wait(void)
{
    reset_product();
    memset(&binding_ui, 0, sizeof(binding_ui));
    hold_binding = true;
    network_status.expires_at_ms = 240000;
    xmini_product_poll(&binding_ui);
    wait_hook = binding_wait_step;
    binding_mode = BIND_PROGRESS;
    stale_binding_id = 0; wait_steps = late_attempts = 0;
}

static void test_binding_explicit_automatic_overlap(void)
{
    prepare_binding_wait();
    CHECK(play_binding("012345", NULL) == ESP_OK);
    CHECK(audio_status.frames == 1049 && !audio_status.busy && !s_binding_wait.waiter);
    CHECK(xmini_product_confirm() == ESP_OK);
    xmini_product_poll(&binding_ui);
    int64_t explicit_start = binding_started_us;
    unsigned before = prompt_calls;
    time_us += 8900000;
    CHECK(play_binding("012345", NULL) == ESP_OK);
    CHECK(prompt_calls == before && binding_started_us == explicit_start);
    CHECK(time_us - explicit_start >= 20980000 && time_us - explicit_start < 21100000);
    CHECK(!audio_status.busy && audio_status.frames == 1049 && !audio_status.stopped);
    CHECK(!s_pending_prompt && !s_binding_playing && !s_prompt_queue->count && !stop_calls);
}

static void test_binding_timeout_and_late_completion(void)
{
    prepare_binding_wait(); binding_mode = BIND_HOLD;
    int64_t begin = time_us;
    CHECK(play_binding("012345", NULL) == ESP_ERR_TIMEOUT);
    CHECK(time_us - begin == 30000000 && audio_status.busy && s_binding_playing_id);
    stale_binding_id = s_binding_playing_id;
    binding_mode = BIND_LATE_RESULT;
    CHECK(play_binding("012345", NULL) == ESP_OK);
    CHECK(late_attempts == 1 && prompt_calls == 3 && stop_calls == 1);
    CHECK(!audio_status.busy && audio_status.frames == 1049 && !s_binding_wait.waiter);
    CHECK(time_us - binding_started_us >= 20980000);
}

static void test_binding_queued_timeout_does_not_start_stale_audio(void)
{
    prepare_binding_wait(); binding_mode = BIND_HOLD;
    audio_status = (xmini_audio_status_t){.ready = true, .busy = true, .owner = XMINI_AUDIO_DIAGNOSTIC};
    unsigned before = prompt_calls;
    CHECK(play_binding("012345", NULL) == ESP_ERR_TIMEOUT);
    uint32_t old = s_binding_pending_id;
    CHECK(old && s_pending_prompt && prompt_calls == before && !stop_calls);
    audio_status.busy = false;
    binding_mode = BIND_PROGRESS;
    CHECK(play_binding("012345", NULL) == ESP_OK);
    CHECK(prompt_calls == before + 1 && !s_pending_prompt && !stop_calls);
    finish_binding_request(old, PROMPT_DONE);
    CHECK(!s_binding_wait.waiter);
}

static void test_binding_timeout_preserves_explicit_intent(void)
{
    for (unsigned pending = 0; pending < 2; ++pending) {
        prepare_binding_wait(); binding_mode = BIND_HOLD;
        if (pending) audio_status = (xmini_audio_status_t){.ready = true, .busy = true, .owner = XMINI_AUDIO_DIAGNOSTIC};
        CHECK(xmini_product_confirm() == ESP_OK);
        if (!pending) xmini_product_poll(&binding_ui);
        CHECK(play_binding("012345", NULL) == ESP_ERR_TIMEOUT);
        xmini_product_poll(&binding_ui);
        CHECK(!stop_calls && !s_binding_playing && !s_binding_pending);
        if (pending) {
            CHECK(s_pending_prompt && s_pending_prompt_explicit);
            audio_status.busy = false; xmini_product_poll(&binding_ui);
        }
        CHECK(audio_status.busy && s_active_prompt_explicit && !audio_status.stopped);
    }
}

static void cancel_during_registration(void)
{
    clear_hook = NULL;
    cancel_binding(NULL);
}

static void change_code_during_registration(void)
{
    clear_hook = NULL;
    strcpy(network_status.code, "654321");
}

static void test_binding_registration_cancellation_and_new_code(void)
{
    prepare_binding_wait();
    unsigned before = prompt_calls;
    clear_hook = cancel_during_registration;
    CHECK(play_binding("012345", NULL) == ESP_ERR_INVALID_STATE);
    CHECK(!s_prompt_queue->count && prompt_calls == before && atomic_load(&s_cancel_binding));
    CHECK(play_binding("012345", NULL) == ESP_ERR_INVALID_STATE);
    xmini_product_poll(&binding_ui);
    CHECK(!atomic_load(&s_cancel_binding));
    CHECK(play_binding("012345", NULL) == ESP_OK);
    clear_hook = change_code_during_registration;
    CHECK(play_binding("012345", NULL) == ESP_ERR_INVALID_STATE);
    CHECK(!s_prompt_queue->count && !s_binding_wait.waiter);
    xmini_product_poll(&binding_ui);
    CHECK(play_binding("654321", NULL) == ESP_OK);
}

static void test_binding_cancel_expiry_code_and_failure(void)
{
    const enum binding_wait_mode modes[] = {BIND_CANCEL, BIND_EXPIRE, BIND_CHANGE_CODE};
    for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
        prepare_binding_wait(); binding_mode = modes[i];
        CHECK(play_binding("012345", NULL) == ESP_ERR_INVALID_STATE);
        CHECK(stop_calls == 1 && audio_status.stopped && !s_binding_wait.waiter);
    }
    prepare_binding_wait(); fail_binding = true;
    CHECK(play_binding("012345", NULL) == ESP_FAIL);
    CHECK(audio_status.error && !s_binding_wait.waiter && !s_pending_prompt);
    prepare_binding_wait(); binding_mode = BIND_AUDIO_ERROR;
    CHECK(play_binding("012345", NULL) == ESP_FAIL);
    CHECK(audio_status.error && audio_status.frames < audio_status.expected_frames);
}

static void test_binding_guidance_change_preserves_explicit_audio(void)
{
    prepare_binding_wait(); binding_mode = BIND_GUIDANCE_OFF;
    CHECK(play_binding("012345", NULL) == ESP_OK);
    CHECK(stop_calls == 1 && audio_status.stopped && !audio_status.busy);
    prepare_binding_wait(); binding_mode = BIND_GUIDANCE_OFF;
    CHECK(xmini_product_confirm() == ESP_OK);
    xmini_product_poll(&binding_ui);
    unsigned before = prompt_calls;
    time_us += 8900000;
    CHECK(play_binding("012345", NULL) == ESP_OK);
    CHECK(!stop_calls && prompt_calls == before && !audio_status.busy && audio_status.frames == 1049);
    CHECK(!xmini_settings_status().guidance_enabled && s_active_prompt_explicit);
}

static void test_binding_pending_context_change(void)
{
    for (unsigned change = 0; change < 3; ++change) {
        prepare_binding_wait();
        audio_status = (xmini_audio_status_t){.ready = true, .busy = true, .owner = XMINI_AUDIO_DIAGNOSTIC};
        CHECK(xmini_product_confirm() == ESP_OK);
        unsigned before = prompt_calls;
        if (change == 0) strcpy(network_status.code, "654321");
        else if (change == 1) network_status.phase = PLATFORM_PHASE_VERIFYING;
        else network_status.expires_at_ms = time_us / 1000;
        xmini_product_poll(&binding_ui);
        CHECK(!s_pending_prompt && !stop_calls && audio_status.busy);
        audio_status.busy = false; xmini_product_poll(&binding_ui);
        CHECK(prompt_calls == before && !s_pending_prompt);
    }
}

static void test_binding_spurious_events_and_full_queue(void)
{
    prepare_binding_wait(); binding_mode = BIND_SPURIOUS;
    int64_t begin = time_us;
    CHECK(play_binding("012345", NULL) == ESP_ERR_TIMEOUT);
    CHECK(time_us - begin == 30000000 && wait_steps > 1 && !s_binding_wait.waiter);
    prepare_binding_wait();
    binding_prompt_t old = {.code = "012345", .request_id = 99};
    CHECK(xQueueSend(s_prompt_queue, &old, 0) == pdTRUE);
    CHECK(play_binding("012345", NULL) == ESP_ERR_NO_MEM);
    CHECK(!s_binding_wait.waiter);
    unsigned before = prompt_calls;
    xmini_product_poll(&binding_ui);
    CHECK(prompt_calls == before && !s_prompt_queue->count);
}

int main(void)
{
    test_binding_feedback_once();
    test_feedback_waits_and_coalesces();
    test_feedback_cancel_preserves_critical_prompts();
    test_verified_save_explicit_retry();
    test_saved_restart_independent_of_audio();
    test_binding_expiry_and_diagnostic_owner();
    test_runtime_failure_visible();
    test_portal_restart_pending_preferences();
    test_portal_preferences_failure_explicit_retry();
    test_guidance_disabled_and_explicit_replay();
    test_portal_silent_confirmation_waits_settings();
    test_binding_explicit_automatic_overlap();
    test_binding_timeout_and_late_completion();
    test_binding_queued_timeout_does_not_start_stale_audio();
    test_binding_timeout_preserves_explicit_intent();
    test_binding_registration_cancellation_and_new_code();
    test_binding_cancel_expiry_code_and_failure();
    test_binding_guidance_change_preserves_explicit_audio();
    test_binding_pending_context_change();
    test_binding_spurious_events_and_full_queue();
    if (failures) fprintf(stderr, "product failures=%u\n", failures);
    else puts("product: 20 groups; binding replay ownership/deadline, verified Wi-Fi/settings restart PASS");
    return failures ? 1 : 0;
}
