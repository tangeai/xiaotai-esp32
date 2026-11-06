// SPDX-License-Identifier: MIT
#include "xmini_product.h"
#include "demo_platform.h"
#include "wifi_provisioning.h"
#include "xmini_audio.h"
#include "xmini_business.h"
#include "xmini_runtime.h"
#include "xmini_settings.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "nvs_flash.h"

#define PROMPT_DONE BIT0
#define PROMPT_FAILED BIT1
#define PROMPT_CANCELLED BIT2

typedef struct { char code[7]; uint32_t request_id; } binding_prompt_t;
static xmini_guide_t s_guide;
static xmini_guide_t s_previous_guide;
static bool s_first = true, s_portal, s_saw_binding;
static esp_err_t s_init_error;
static bool s_pending_prompt, s_binding_pending, s_binding_playing;
static bool s_pending_prompt_feedback;
static bool s_pending_prompt_explicit, s_active_prompt_explicit;
static xmini_prompt_t s_prompt;
static char s_prompt_code[7];
static atomic_bool s_cancel_binding;
static portMUX_TYPE s_binding_lock = portMUX_INITIALIZER_UNLOCKED;
static struct {
    uint32_t sequence, waiter, cancel_generation;
    EventBits_t result;
} s_binding_wait;
static uint32_t s_binding_pending_id, s_binding_playing_id;
static char s_active_binding_code[7];
static StaticQueue_t s_prompt_queue_storage;
static uint8_t s_prompt_queue_buffer[sizeof(binding_prompt_t)];
static QueueHandle_t s_prompt_queue;
static StaticEventGroup_t s_prompt_event_storage;
static EventGroupHandle_t s_prompt_events;
static bool s_restart_pending;
static wifi_provisioning_credentials_t s_verified_wifi;
static bool s_verified_wifi_pending, s_wifi_saved;
static esp_err_t s_wifi_save_error, s_wifi_confirmation_error;
static bool s_wifi_settings_held;

static bool automatic_guidance_enabled(void)
{
    xmini_settings_status_t settings = xmini_settings_status();
    return !settings.loaded || settings.guidance_enabled;
}

static bool spoken_guidance(xmini_prompt_t prompt)
{
    return prompt == XMINI_PROMPT_STARTUP || prompt == XMINI_PROMPT_WIFI_SETUP ||
        prompt == XMINI_PROMPT_WIFI_CONNECTING || prompt == XMINI_PROMPT_WIFI_CONNECTED ||
        prompt == XMINI_PROMPT_BINDING || prompt == XMINI_PROMPT_BOUND || prompt == XMINI_PROMPT_ONLINE;
}

static bool platform_event(const platform_event_t *event)
{
    return xmini_runtime_ready() && xmini_business_platform_event(event);
}

static bool binding_code_current(const char *code)
{
    platform_status_t status = platform_status();
    return status.phase == PLATFORM_PHASE_BINDING &&
        xmini_guide_valid_code(code) && !strcmp(status.code, code) &&
        status.expires_at_ms > esp_timer_get_time() / 1000;
}

static bool binding_request_live(uint32_t request_id)
{
    portENTER_CRITICAL(&s_binding_lock);
    bool live = request_id && s_binding_wait.waiter == request_id &&
        !s_binding_wait.result && !atomic_load(&s_cancel_binding);
    portEXIT_CRITICAL(&s_binding_lock);
    return live;
}

static void finish_binding_request(uint32_t request_id, EventBits_t result)
{
    portENTER_CRITICAL(&s_binding_lock);
    bool notify = request_id && s_binding_wait.waiter == request_id && !s_binding_wait.result;
    if (notify) s_binding_wait.result = result;
    portEXIT_CRITICAL(&s_binding_lock);
    // The mailbox owns the result; a delayed event is only a wake-up hint.
    if (notify) xEventGroupSetBits(s_prompt_events, PROMPT_DONE);
}

static esp_err_t play_binding(const char *code, void *context)
{
    (void)context;
    if (!xmini_guide_valid_code(code)) return ESP_ERR_INVALID_ARG;
    if (!binding_code_current(code)) return ESP_ERR_INVALID_STATE;
    if (!automatic_guidance_enabled()) return ESP_OK;
    binding_prompt_t request = {0};
    memcpy(request.code, code, sizeof(request.code));
    portENTER_CRITICAL(&s_binding_lock);
    if (s_binding_wait.waiter || atomic_load(&s_cancel_binding)) {
        portEXIT_CRITICAL(&s_binding_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (!++s_binding_wait.sequence) ++s_binding_wait.sequence;
    request.request_id = s_binding_wait.waiter = s_binding_wait.sequence;
    s_binding_wait.result = 0;
    portEXIT_CRITICAL(&s_binding_lock);
    xEventGroupClearBits(s_prompt_events, PROMPT_DONE | PROMPT_FAILED | PROMPT_CANCELLED);
    esp_err_t error = ESP_OK;
    EventBits_t bits = 0;
    if (!binding_code_current(code) || !binding_request_live(request.request_id)) {
        error = ESP_ERR_INVALID_STATE;
    } else if (xQueueSend(s_prompt_queue, &request, 0) != pdTRUE) {
        error = ESP_ERR_NO_MEM;
    } else {
        int64_t deadline = esp_timer_get_time() / 1000 + 30000;
        for (;;) {
            portENTER_CRITICAL(&s_binding_lock);
            bits = s_binding_wait.result;
            portEXIT_CRITICAL(&s_binding_lock);
            if (bits) break;
            int64_t remaining = deadline - esp_timer_get_time() / 1000;
            if (remaining <= 0) break;
            xEventGroupWaitBits(s_prompt_events, PROMPT_DONE, pdTRUE, pdFALSE,
                                pdMS_TO_TICKS((uint32_t)remaining));
        }
    }
    portENTER_CRITICAL(&s_binding_lock);
    s_binding_wait.waiter = 0;
    portEXIT_CRITICAL(&s_binding_lock);
    if (error != ESP_OK) return error;
    if (bits & PROMPT_DONE) return binding_code_current(code) ? ESP_OK : ESP_ERR_INVALID_STATE;
    return bits & PROMPT_CANCELLED ? ESP_ERR_INVALID_STATE : bits & PROMPT_FAILED ? ESP_FAIL : ESP_ERR_TIMEOUT;
}

static void cancel_binding(void *context)
{
    (void)context;
    portENTER_CRITICAL(&s_binding_lock);
    atomic_store(&s_cancel_binding, true);
    ++s_binding_wait.cancel_generation;
    if (s_binding_wait.waiter && !s_binding_wait.result) s_binding_wait.result = PROMPT_CANCELLED;
    portEXIT_CRITICAL(&s_binding_lock);
    xEventGroupSetBits(s_prompt_events, PROMPT_DONE);
}

static void schedule(xmini_prompt_t prompt, const char *code, bool binding)
{
    s_pending_prompt = true;
    s_prompt = prompt;
    s_binding_pending = binding;
    s_binding_pending_id = 0;
    s_pending_prompt_explicit = false;
    s_pending_prompt_feedback = false;
    memset(s_prompt_code, 0, sizeof(s_prompt_code));
    if (code) memcpy(s_prompt_code, code, sizeof(s_prompt_code));
}

static void schedule_explicit(xmini_prompt_t prompt, const char *code)
{
    schedule(prompt, code, false);
    s_pending_prompt_explicit = true;
}

esp_err_t xmini_product_feedback(int prompt)
{
    if (prompt == -1) {
        if (s_pending_prompt && s_pending_prompt_feedback) {
            s_pending_prompt = false;
            s_pending_prompt_feedback = false;
        }
        return ESP_OK;
    }
    if (prompt != XMINI_PROMPT_INCOMING && prompt != XMINI_PROMPT_DIALING &&
        prompt != XMINI_PROMPT_ENDED && prompt != XMINI_PROMPT_FAILED)
        return ESP_ERR_INVALID_ARG;
    if (s_restart_pending || s_binding_pending || s_binding_playing ||
        s_guide.phase == XMINI_GUIDE_BINDING)
        return ESP_ERR_INVALID_STATE;
    schedule((xmini_prompt_t)prompt, NULL, false);
    s_pending_prompt_feedback = true;
    return ESP_OK;
}

static esp_err_t prepare_wifi_confirmation(void)
{
    if (!s_wifi_settings_held) {
        esp_err_t error = xmini_settings_quiesce_begin();
        if (error) { s_wifi_confirmation_error = error; return error; }
        s_wifi_settings_held = true;
    }
    s_wifi_confirmation_error = ESP_OK;
    s_restart_pending = true;
    schedule(XMINI_PROMPT_WIFI_CONNECTED, NULL, false);
    return ESP_OK;
}

static esp_err_t persist_verified_wifi(void)
{
    if (!s_verified_wifi_pending || s_wifi_saved || s_restart_pending) return ESP_ERR_INVALID_STATE;
    esp_err_t err = platform_save_wifi(s_verified_wifi.ssid, s_verified_wifi.password);
    s_wifi_save_error = err;
    if (err == ESP_OK) {
        volatile unsigned char *bytes = (volatile unsigned char *)&s_verified_wifi;
        for (size_t i = 0; i < sizeof(s_verified_wifi); ++i) bytes[i] = 0;
        s_verified_wifi_pending = false;
        s_wifi_saved = true;
        return prepare_wifi_confirmation();
    } else {
        ESP_LOGE("xmini_product", "verified Wi-Fi save failed: %s; credentials retained for explicit retry",
                 esp_err_to_name(err));
    }
    return err;
}

static void poll_prompt(void)
{
    xmini_audio_status_t audio = xmini_audio_status();
    bool enabled = automatic_guidance_enabled();
    bool stopping_binding = false;
    portENTER_CRITICAL(&s_binding_lock);
    uint32_t cancel_generation = s_binding_wait.cancel_generation;
    bool cancelled = atomic_load(&s_cancel_binding);
    portEXIT_CRITICAL(&s_binding_lock);
    if (s_pending_prompt && s_prompt == XMINI_PROMPT_BINDING) {
        if (cancelled || !binding_code_current(s_prompt_code)) {
            finish_binding_request(s_binding_pending_id, PROMPT_CANCELLED);
            s_pending_prompt = s_binding_pending = false;
            s_binding_pending_id = 0;
        } else if (s_binding_pending_id && !binding_request_live(s_binding_pending_id)) {
            if (!s_pending_prompt_explicit) s_pending_prompt = false;
            s_binding_pending = false;
            s_binding_pending_id = 0;
        }
    }
    if (s_binding_playing_id && !binding_request_live(s_binding_playing_id)) {
        s_binding_playing = false;
        s_binding_playing_id = 0;
        stopping_binding = !s_active_prompt_explicit;
    }
    if (audio.busy && audio.owner == XMINI_AUDIO_GUIDANCE && audio.prompt == XMINI_PROMPT_BINDING &&
        (cancelled || !binding_code_current(s_active_binding_code) || stopping_binding)) {
        finish_binding_request(s_binding_playing_id, PROMPT_CANCELLED);
        s_binding_playing = false;
        s_binding_playing_id = 0;
        stopping_binding = true;
        xmini_audio_request((xmini_audio_request_t){.kind = XMINI_AUDIO_STOP});
    }
    if (!enabled && audio.busy && audio.owner == XMINI_AUDIO_GUIDANCE &&
        !s_active_prompt_explicit && spoken_guidance(audio.prompt)) {
        if (s_binding_playing) {
            finish_binding_request(s_binding_playing_id, PROMPT_DONE);
            s_binding_playing = false;
            s_binding_playing_id = 0;
        }
        xmini_audio_request((xmini_audio_request_t){.kind = XMINI_AUDIO_STOP});
        stopping_binding = audio.prompt == XMINI_PROMPT_BINDING;
    }
    if (s_binding_playing && !audio.busy) {
        bool complete = !audio.error && !audio.stopped && audio.owner == XMINI_AUDIO_GUIDANCE && audio.prompt == XMINI_PROMPT_BINDING &&
                        audio.frames == audio.expected_frames && audio.frames > 0;
        finish_binding_request(s_binding_playing_id, !binding_code_current(s_active_binding_code) ?
                               PROMPT_CANCELLED : complete ? PROMPT_DONE : PROMPT_FAILED);
        s_binding_playing = false;
        s_binding_playing_id = 0;
    }
    binding_prompt_t request;
    if (xQueueReceive(s_prompt_queue, &request, 0) == pdTRUE && binding_request_live(request.request_id)) {
        if (!binding_code_current(request.code)) {
            finish_binding_request(request.request_id, PROMPT_CANCELLED);
        } else if (!stopping_binding && audio.busy && audio.owner == XMINI_AUDIO_GUIDANCE &&
                   audio.prompt == XMINI_PROMPT_BINDING && !strcmp(s_active_binding_code, request.code)) {
            s_binding_playing = true;
            s_binding_playing_id = request.request_id;
        } else {
            bool same_pending = s_pending_prompt && s_prompt == XMINI_PROMPT_BINDING &&
                !strcmp(s_prompt_code, request.code);
            if (!same_pending) schedule(XMINI_PROMPT_BINDING, request.code, true);
            s_binding_pending = true;
            s_binding_pending_id = request.request_id;
        }
    }
    if (cancelled && !audio.busy) {
        portENTER_CRITICAL(&s_binding_lock);
        if (s_binding_wait.cancel_generation == cancel_generation) atomic_store(&s_cancel_binding, false);
        portEXIT_CRITICAL(&s_binding_lock);
    }
    if (!s_pending_prompt) return;
    if (!enabled && !s_pending_prompt_explicit && spoken_guidance(s_prompt)) {
        if (s_binding_pending) finish_binding_request(s_binding_pending_id, PROMPT_DONE);
        s_pending_prompt = s_binding_pending = false;
        s_binding_pending_id = 0;
        return;
    }
    if (s_prompt == XMINI_PROMPT_BINDING &&
        !xmini_guide_seconds_left(&s_guide, (uint32_t)(esp_timer_get_time() / 1000))) {
        if (s_binding_pending) finish_binding_request(s_binding_pending_id, PROMPT_FAILED);
        s_pending_prompt = s_binding_pending = false;
        s_binding_pending_id = 0;
        return;
    }
    if (audio.busy) return;
    esp_err_t err = xmini_audio_prompt(s_prompt, s_prompt_code[0] ? s_prompt_code : NULL);
    if (err == ESP_OK) {
        s_active_prompt_explicit = s_pending_prompt_explicit;
        s_binding_playing = s_binding_pending;
        s_binding_playing_id = s_binding_pending_id;
        if (s_prompt == XMINI_PROMPT_BINDING) memcpy(s_active_binding_code, s_prompt_code, sizeof(s_active_binding_code));
        s_pending_prompt = s_binding_pending = false;
        s_binding_pending_id = 0;
        s_pending_prompt_feedback = false;
    } else {
        ESP_LOGE("xmini_product", "guide rejected: prompt=%d result=%s", s_prompt, esp_err_to_name(err));
        if (s_binding_pending) finish_binding_request(s_binding_pending_id, PROMPT_FAILED);
        s_pending_prompt = s_binding_pending = false;
        s_binding_pending_id = 0;
        s_pending_prompt_feedback = false;
    }
}

esp_err_t xmini_product_init(esp_err_t runtime_error)
{
    s_prompt_queue = xQueueCreateStatic(1, sizeof(binding_prompt_t), s_prompt_queue_buffer, &s_prompt_queue_storage);
    s_prompt_events = xEventGroupCreateStatic(&s_prompt_event_storage);
    if (runtime_error != ESP_OK) return s_init_error = runtime_error;
    if (!xmini_audio_status().ready) return s_init_error = ESP_ERR_INVALID_STATE;
    s_init_error = automatic_guidance_enabled() ? xmini_audio_prompt(XMINI_PROMPT_STARTUP, NULL) : ESP_OK;
    if (s_init_error != ESP_OK) return s_init_error;
    s_init_error = nvs_flash_init();
    if (s_init_error != ESP_OK) {
        ESP_LOGE("xmini_product", "NVS init failed: %s; original data retained, no erase", esp_err_to_name(s_init_error));
        return s_init_error;
    }
    platform_binding_prompt_t prompt = {.play_code = play_binding, .cancel = cancel_binding};
    s_init_error = platform_set_binding_prompt(&prompt);
    if (s_init_error != ESP_OK) return s_init_error;
    char ssid[33] = {0}, password[65] = {0};
    bool saved = platform_load_wifi(ssid, sizeof(ssid), password, sizeof(password));
    if (!saved || platform_wifi_provisioning_requested()) {
        s_init_error = wifi_provisioning_start();
        s_portal = s_init_error == ESP_OK;
    } else {
        s_init_error = platform_start(ssid, password, "https://ep-open.tangeopen.com/services", platform_event);
    }
    memset(password, 0, sizeof(password));
    return s_init_error;
}

static void update_guide(void)
{
    s_guide = (xmini_guide_t){.phase = XMINI_GUIDE_STARTUP};
    if (s_init_error != ESP_OK) {
        s_guide.phase = XMINI_GUIDE_ERROR;
        s_guide.error = s_init_error;
        s_guide.startup_failed = true;
    } else if (s_portal) {
        wifi_provisioning_status_t wifi = wifi_provisioning_status();
        esp_err_t operation_error = s_wifi_save_error ? s_wifi_save_error : s_wifi_confirmation_error;
        s_guide.phase = operation_error || wifi.failed ? XMINI_GUIDE_ERROR : wifi.connecting || s_restart_pending ?
                        XMINI_GUIDE_CONNECTING : XMINI_GUIDE_WIFI_SETUP;
        if (operation_error || wifi.failed) s_guide.error = operation_error ? operation_error : wifi.error;
        s_guide.retryable = operation_error != ESP_OK;
        memcpy(s_guide.ssid, wifi.ssid, sizeof(s_guide.ssid));
    } else {
        platform_status_t status = platform_status();
        static const xmini_guide_phase_t phases[] = {
            XMINI_GUIDE_STARTUP, XMINI_GUIDE_CONNECTING, XMINI_GUIDE_SERVICES,
            XMINI_GUIDE_SERVICES, XMINI_GUIDE_SERVICES, XMINI_GUIDE_BINDING,
            XMINI_GUIDE_VERIFYING, XMINI_GUIDE_ONLINE, XMINI_GUIDE_OFFLINE, XMINI_GUIDE_ERROR,
        };
        s_guide.phase = phases[status.phase];
        s_guide.error = status.error;
        memcpy(s_guide.code, status.code, sizeof(s_guide.code));
        s_guide.expires_at_ms = (uint32_t)status.expires_at_ms;
    }
}

void xmini_product_poll(xmini_ui_t *ui)
{
    if (s_portal && !s_restart_pending && !s_verified_wifi_pending && !s_wifi_saved) {
        if (wifi_provisioning_take_credentials(&s_verified_wifi)) {
            s_verified_wifi_pending = true;
            persist_verified_wifi();
        }
    }
    update_guide();
    if (s_guide.phase == XMINI_GUIDE_BINDING &&
        !xmini_guide_seconds_left(&s_guide, (uint32_t)(esp_timer_get_time() / 1000)))
        cancel_binding(NULL);
    if (s_first || s_guide.phase != s_previous_guide.phase || strcmp(s_guide.code, s_previous_guide.code)) {
        ui->page = s_guide.phase == XMINI_GUIDE_ONLINE ? XMINI_PAGE_HOME : XMINI_PAGE_GUIDE;
        ui->selected = 0;
        ++ui->epoch;
        switch (s_guide.phase) {
        case XMINI_GUIDE_WIFI_SETUP: schedule(XMINI_PROMPT_WIFI_SETUP, NULL, false); break;
        case XMINI_GUIDE_CONNECTING:
            if (!s_restart_pending) schedule(XMINI_PROMPT_WIFI_CONNECTING, NULL, false);
            break;
        case XMINI_GUIDE_BINDING: s_saw_binding = true; break;
        case XMINI_GUIDE_ONLINE:
            schedule(s_saw_binding ? XMINI_PROMPT_BOUND : XMINI_PROMPT_ONLINE, NULL, false);
            s_saw_binding = false;
            break;
        case XMINI_GUIDE_OFFLINE: schedule(XMINI_PROMPT_OFFLINE, NULL, false); break;
        case XMINI_GUIDE_ERROR: schedule(s_portal ? XMINI_PROMPT_WIFI_FAILED : XMINI_PROMPT_FAILED, NULL, false); break;
        default: break;
        }
        ESP_LOGI("xmini_product", "guide phase=%u error=%u", s_guide.phase, s_guide.error);
        s_previous_guide = s_guide;
        s_first = false;
    }
    poll_prompt();
    xmini_settings_status_t settings = xmini_settings_status();
    if (s_restart_pending && settings.error && !settings.attempt_pending) {
        s_wifi_confirmation_error = settings.error;
        s_restart_pending = false;
        if (s_pending_prompt && s_prompt == XMINI_PROMPT_WIFI_CONNECTED) s_pending_prompt = false;
        xmini_audio_status_t audio = xmini_audio_status();
        if (audio.busy && audio.owner == XMINI_AUDIO_GUIDANCE && audio.prompt == XMINI_PROMPT_WIFI_CONNECTED)
            xmini_audio_request((xmini_audio_request_t){.kind = XMINI_AUDIO_STOP});
        ESP_LOGE("xmini_product", "Wi-Fi saved but settings failed: %s; explicit retry required",
                 esp_err_to_name(settings.error));
    }
    if (s_restart_pending && s_wifi_settings_held && settings.loaded &&
        settings.saved && !settings.pending && !settings.error &&
        !s_pending_prompt && !xmini_audio_status().busy) {
        // Wait for resource release, not a successful spoken confirmation.
        s_restart_pending = false;
        ESP_LOGI("xmini_product", "verified Wi-Fi and settings persisted; restarting once into station mode");
        esp_restart();
    }
}

esp_err_t xmini_product_confirm(void)
{
    if (s_init_error != ESP_OK) return s_init_error;
    if (s_portal) {
        if (s_verified_wifi_pending && s_wifi_save_error) return persist_verified_wifi();
        if (s_wifi_saved && s_wifi_confirmation_error && !s_restart_pending) {
            xmini_settings_status_t settings = xmini_settings_status();
            if (s_wifi_settings_held && settings.error) {
                esp_err_t error = xmini_settings_quiesce_retry();
                if (error) return error;
            }
            return prepare_wifi_confirmation();
        }
        return ESP_ERR_INVALID_STATE;
    }
    if (s_guide.phase == XMINI_GUIDE_BINDING && xmini_guide_seconds_left(&s_guide, (uint32_t)(esp_timer_get_time() / 1000))) {
        if (s_binding_pending || s_binding_playing || s_pending_prompt) return ESP_ERR_INVALID_STATE;
        schedule_explicit(XMINI_PROMPT_BINDING, s_guide.code);
        return ESP_OK;
    } else if (!s_portal && (s_guide.phase == XMINI_GUIDE_ERROR || s_guide.phase == XMINI_GUIDE_OFFLINE ||
                           s_guide.phase == XMINI_GUIDE_BINDING)) {
        return platform_retry();
    }
    return ESP_ERR_INVALID_STATE;
}

xmini_guide_t xmini_product_guide(void) { return s_guide; }
