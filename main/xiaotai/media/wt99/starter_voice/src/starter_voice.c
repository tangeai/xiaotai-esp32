/* Dedicated Voicute wake only. One inference owner; capture never invokes it. */
#include "starter_voice.h"
#include "wake_backend.h"
#include "wake_window.h"
#include "wake_audio_queue.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <stdatomic.h>
#include <ctype.h>
#include <string.h>

#define WAKE_THRESHOLD_MILLI 950U

static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static atomic_bool s_started, s_ready, s_gap;
static atomic_bool s_accepting;
static atomic_uint s_input_epoch;
static wake_audio_queue_t *s_input;
static uint32_t s_window_input_epoch;
static atomic_uint s_dropped;
static atomic_int s_init_error;
static wake_window_t s_window;
static starter_voice_status_t s_status = {.threshold_milli = WAKE_THRESHOLD_MILLI, .ns_vad = true, .last_raw_id = -1};
static starter_voice_result_handler_t s_handler;
static void *s_user;
static bool s_enabled;
static int64_t s_armed_ms, s_cooldown_ms;
static uint64_t s_detect_total_us;

static void reset_locked(void)
{
    /* In-flight producer copies keep their old epoch and are discarded by the
     * consumer. Never reset queue indices while capture can be writing. */
    s_window_input_epoch = atomic_fetch_add(&s_input_epoch, 1) + 1U;
    wake_window_reset(&s_window);
    ++s_status.resets;
}

static void wake_task(void *arg)
{
    (void)arg;
    int16_t *snapshot = NULL;
    unsigned retry_ms = 5000;
    for (;;) {
        s_window.pcm = heap_caps_malloc(WAKE_WINDOW_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        snapshot = heap_caps_malloc(WAKE_WINDOW_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_input = heap_caps_malloc(sizeof(*s_input), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_input) wake_audio_queue_init(s_input);
        esp_err_t err = (!s_window.pcm || !snapshot || !s_input) ? ESP_ERR_NO_MEM : wake_backend_init();
        if (err == ESP_OK) {
            // Exercise actual operators before advertising ready; no callback on test audio.
            memset(snapshot, 0, WAKE_WINDOW_SAMPLES * sizeof(int16_t));
            float probability = 0;
            int64_t start = esp_timer_get_time();
            err = wake_backend_run(snapshot, &probability);
            ESP_LOGI("starter_voice", "wake self-test=%s silence-probability=%.3f elapsed-ms=%lu",
                     esp_err_to_name(err), probability, (unsigned long)((esp_timer_get_time() - start) / 1000));
        }
        atomic_store(&s_init_error, err);
        if (err == ESP_OK) break;
        ESP_LOGE("starter_voice", "wake init failed: %s; retry in %u ms; manual AI remains available",
                 esp_err_to_name(err), retry_ms);
        heap_caps_free(s_window.pcm); s_window.pcm = NULL;
        heap_caps_free(snapshot);
        heap_caps_free(s_input); s_input = NULL;
        wake_backend_deinit();
        vTaskDelay(pdMS_TO_TICKS(retry_ms));
        retry_ms = retry_ms < 15000 ? retry_ms * 2 : 30000;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.ready = true;
    s_armed_ms = esp_timer_get_time() / 1000 + 4000;
    reset_locked();
    xSemaphoreGive(s_mutex);
    atomic_store(&s_ready, true);
    ESP_LOGI("starter_voice", "Voicute wake ready: 你好小钛; clean16k window=%u hop=%u threshold=%u/1000 core=0",
             WAKE_WINDOW_SAMPLES, WAKE_HOP_SAMPLES, (unsigned)s_status.threshold_milli);
    ESP_LOGI("starter_voice", "capture handoff: PSRAM=%u bytes blocks=%u nonblocking producer",
             (unsigned)sizeof(*s_input), WAKE_AUDIO_QUEUE_BLOCKS);
    for (;;) {
        // Bounded blocking even when inference is slower than the hop interval.
        vTaskDelay(pdMS_TO_TICKS(20));
        int64_t captured_ms = 0;
        uint32_t epoch = 0;
        uint32_t input_epoch = 0;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        int64_t now = esp_timer_get_time() / 1000;
        if (atomic_exchange(&s_gap, false)) reset_locked();
        input_epoch = atomic_load(&s_input_epoch);
        if (s_window_input_epoch != input_epoch) {
            s_window_input_epoch = input_epoch;
            wake_window_reset(&s_window);
            ++s_status.resets;
        }
        s_enabled = atomic_load(&s_accepting);
        /* Drain a bounded backlog before selecting the newest continuous
         * window. Slow inference no longer holds up or drops capture frames. */
        for (unsigned n = 0; n < WAKE_AUDIO_QUEUE_BLOCKS; ++n) {
            const wake_audio_block_t *b = wake_audio_queue_peek(s_input);
            if (!b) break;
            if (b->epoch != input_epoch || !s_enabled ||
                b->captured_ms < s_armed_ms || b->captured_ms < s_cooldown_ms) {
                ++s_status.skipped_unarmed;
            } else {
                uint32_t previous_epoch = s_window.epoch;
                wake_window_append(&s_window, b->pcm, b->samples, b->captured_ms);
                if (previous_epoch != s_window.epoch) { ++s_status.resets; ++s_status.stale; }
                ++s_status.fed;
            }
            wake_audio_queue_release(s_input);
        }
        bool available = s_enabled && now >= s_armed_ms && now >= s_cooldown_ms &&
                         wake_window_snapshot(&s_window, snapshot);
        if (available) {
            captured_ms = s_window.captured_ms; epoch = s_window.epoch;
        }
        xSemaphoreGive(s_mutex);
        if (!available) continue;
        float probability = 0;
        int64_t start_us = esp_timer_get_time();
        esp_err_t err = wake_backend_run(snapshot, &probability);
        uint32_t elapsed_us = (uint32_t)(esp_timer_get_time() - start_us);
        bool dispatch = false;
        unsigned accepted_threshold = 0;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        now = esp_timer_get_time() / 1000;
        ++s_status.model_frames;
        s_detect_total_us += elapsed_us;
        s_status.detect_avg_us = s_detect_total_us / s_status.model_frames;
        if (elapsed_us > s_status.max_detect_us) s_status.max_detect_us = elapsed_us;
        if (elapsed_us > 160000) ++s_status.detect_over_budget;
        uint32_t age = now > captured_ms ? (uint32_t)(now - captured_ms) : 0;
        if (age > s_status.max_age_ms) s_status.max_age_ms = age;
        if (err != ESP_OK) {
            ++s_status.reject_id;
        } else {
            s_status.last_raw_probability_milli = (uint32_t)(probability * 1000);
            s_status.last_raw_at_ms = (uint32_t)now;
            if (probability >= (float)s_status.threshold_milli / 1000.0f) {
                ++s_status.model_detected;
                s_status.last_raw_id = 1;
                wake_verdict_t verdict = wake_result_check(atomic_load(&s_accepting),
                    atomic_load(&s_gap) || input_epoch != atomic_load(&s_input_epoch), epoch,
                    s_window.epoch, captured_ms, now, s_cooldown_ms);
                if (verdict == WAKE_REJECT_EPOCH) ++s_status.reject_epoch;
                else if (verdict == WAKE_REJECT_AGE) ++s_status.reject_age;
                else if (verdict == WAKE_REJECT_COOLDOWN) ++s_status.skipped_cooldown;
                else {
                    ++s_status.detections;
                    s_cooldown_ms = now + 2500;
                    reset_locked();
                    dispatch = true;
                    accepted_threshold = s_status.threshold_milli;
                }
            }
        }
        xSemaphoreGive(s_mutex);
        if (dispatch) {
            ESP_LOGI("starter_voice", "Voicute wake detected: 你好小钛 probability=%.3f inference=%lu ms",
                     probability, (unsigned long)(elapsed_us / 1000));
            const starter_voice_result_t result = {
                .intent = STARTER_VOICE_INTENT_WAKE_ONLY, .captured_ms = captured_ms,
                .acoustic_score_valid = true,
                .probability_milli = (uint16_t)(probability * 1000.0f + 0.5f),
                .threshold_milli = (uint16_t)accepted_threshold,
            };
            // Product/runtime revalidate foreground ownership; never start RTC here.
            s_handler(&result, s_user);
        }
    }
}

esp_err_t starter_voice_start(starter_voice_result_handler_t handler, void *user_data)
{
    if (!handler) return ESP_ERR_INVALID_ARG;
    if (atomic_exchange(&s_started, true)) return ESP_ERR_INVALID_STATE;
    s_handler = handler; s_user = user_data;
    s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
    if (xTaskCreatePinnedToCoreWithCaps(wake_task, "voicute_wake", 12288, NULL, 3, NULL,
                                      0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        atomic_store(&s_init_error, ESP_ERR_NO_MEM);
        atomic_store(&s_started, false);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK; /* asynchronous: ready becomes true only after tensor validation */
}

esp_err_t starter_voice_feed_pcm16k(const int16_t *pcm, size_t samples, int64_t captured_ms)
{
    if (!pcm || samples == 0 || samples > 1024 || captured_ms <= 0) return ESP_ERR_INVALID_ARG;
    if (!atomic_load(&s_ready)) return ESP_ERR_INVALID_STATE;
    if (!atomic_load(&s_accepting)) return ESP_ERR_INVALID_STATE;
    uint32_t epoch = atomic_load(&s_input_epoch);
    if (!wake_audio_queue_push(s_input, pcm, samples, captured_ms, epoch)) {
        atomic_store(&s_gap, true);
        atomic_fetch_add(&s_dropped, 1);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void starter_voice_set_listening(bool enabled)
{
    if (!atomic_load(&s_ready)) return;
    /* Called by capture on every frame. Only the model owner resets its window;
     * this immediate epoch change also invalidates already-running inference. */
    if (atomic_exchange(&s_accepting, enabled) != enabled)
        atomic_fetch_add(&s_input_epoch, 1);
}
bool starter_voice_ready(void) { return atomic_load(&s_ready); }

starter_voice_status_t starter_voice_status(void)
{
    if (!atomic_load(&s_ready)) return (starter_voice_status_t){.threshold_milli = WAKE_THRESHOLD_MILLI,
        .ns_vad = true, .last_raw_id = -1, .init_error = atomic_load(&s_init_error)};
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    starter_voice_status_t out = s_status;
    int64_t now = esp_timer_get_time() / 1000;
    out.listening = atomic_load(&s_accepting) && now >= s_armed_ms && now >= s_cooldown_ms;
    out.dropped = atomic_load(&s_dropped);
    xSemaphoreGive(s_mutex);
    return out;
}

void starter_voice_trace(starter_voice_trace_t *out)
{
    if (out != NULL) *out = (starter_voice_trace_t){0};
}

esp_err_t starter_voice_configure(unsigned threshold_milli, bool ns_vad)
{
    if (threshold_milli < 50 || threshold_milli > 990 || !ns_vad) return ESP_ERR_INVALID_ARG;
    if (!atomic_load(&s_ready)) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.threshold_milli = threshold_milli;
    reset_locked();
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

/* Console text injection only; never consumes microphone audio. */
esp_err_t starter_voice_parse(const char *text, starter_voice_result_t *result)
{
    if (text == NULL || result == NULL) return ESP_ERR_INVALID_ARG;
    while (*text != '\0' && isspace((unsigned char)*text)) ++text;
    const char *phrases[] = {"你好小钛", "小钛小钛", "小钛同学"};
    for (size_t i = 0; i < sizeof(phrases) / sizeof(phrases[0]); ++i) {
        if (strncmp(text, phrases[i], strlen(phrases[i])) == 0) {
            *result = (starter_voice_result_t){.intent = STARTER_VOICE_INTENT_WAKE_ONLY};
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

const char *starter_voice_intent_name(starter_voice_intent_t intent)
{
    return intent == STARTER_VOICE_INTENT_WAKE_ONLY ? "wake-only" : "none";
}
