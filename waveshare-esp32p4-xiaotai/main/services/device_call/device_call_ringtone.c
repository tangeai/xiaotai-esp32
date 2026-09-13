#include "device_call_ringtone.h"

#include <limits.h>
#include <stdint.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "app_memory_policy.h"
#include "app_task_affinity.h"
#include "audio_device.h"

static const char *TAG = "call_ringtone";

#define DEVICE_CALL_RINGTONE_TASK_STACK_BYTES (12U * 1024U)
#define DEVICE_CALL_RINGTONE_TASK_PRIORITY    4
#define DEVICE_CALL_RINGTONE_CHUNK_MS         20U
#define DEVICE_CALL_RINGTONE_PERIOD_MS        3200U
#define DEVICE_CALL_RINGTONE_ATTACK_MS        8U
#define DEVICE_CALL_RINGTONE_RELEASE_MS       48U
#define DEVICE_CALL_RINGTONE_STOP_TIMEOUT_MS  500U
#define DEVICE_CALL_RINGTONE_MAX_SAMPLES      \
    (16000U * 2U * DEVICE_CALL_RINGTONE_CHUNK_MS / 1000U)

static portMUX_TYPE s_ringtone_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_ringtone_task;
static bool s_ringtone_stop_requested;
static EXT_RAM_BSS_ATTR int16_t s_ringtone_pcm[DEVICE_CALL_RINGTONE_MAX_SAMPLES];

typedef struct {
    uint16_t start_ms;
    uint16_t duration_ms;
    uint16_t frequency_hz;
} device_call_ringtone_note_t;

typedef struct {
    uint32_t start_frame;
    uint32_t duration_frames;
    uint32_t phase_step;
} device_call_ringtone_runtime_note_t;

/* Two ascending three-note phrases form an original compact mobile chime.
 * The idle tail keeps repeated ringing noticeable without sounding harsh. */
static const device_call_ringtone_note_t s_ringtone_notes[] = {
    {   0U, 160U,  659U },
    { 190U, 160U,  784U },
    { 380U, 300U, 1047U },
    { 820U, 160U,  659U },
    {1010U, 160U,  784U },
    {1200U, 420U, 1047U },
};

#define DEVICE_CALL_RINGTONE_NOTE_COUNT \
    (sizeof(s_ringtone_notes) / sizeof(s_ringtone_notes[0]))

/* A compact sine table avoids a WAV asset and floating-point work in the
 * realtime playback loop. */
static const int16_t s_sine_lut[32] = {
    0, 6393, 12539, 18204, 23170, 27245, 30273, 32137,
    32767, 32137, 30273, 27245, 23170, 18204, 12539, 6393,
    0, -6393, -12539, -18204, -23170, -27245, -30273, -32137,
    -32767, -32137, -30273, -27245, -23170, -18204, -12539, -6393,
};

static bool device_call_ringtone_stop_requested(void)
{
    bool stop = false;

    taskENTER_CRITICAL(&s_ringtone_lock);
    stop = s_ringtone_stop_requested;
    taskEXIT_CRITICAL(&s_ringtone_lock);
    return stop;
}

static int16_t device_call_ringtone_sample(uint32_t phase,
                                           uint32_t envelope_q15)
{
    int32_t fundamental = s_sine_lut[phase >> 27];
    int32_t harmonic = s_sine_lut[(phase * 2U) >> 27];
    int32_t mixed = (fundamental * 12000 + harmonic * 3600) / 32767;

    mixed = (mixed * (int32_t)envelope_q15) / 32767;
    if (mixed > INT16_MAX) {
        mixed = INT16_MAX;
    } else if (mixed < INT16_MIN) {
        mixed = INT16_MIN;
    }
    return (int16_t)mixed;
}

static uint32_t device_call_ringtone_note_envelope(uint32_t note_frame,
                                                   uint32_t note_frames,
                                                   uint32_t attack_frames,
                                                   uint32_t release_frames)
{
    if (note_frame >= note_frames) {
        return 0U;
    }

    if (attack_frames > 0U && note_frame < attack_frames) {
        return (note_frame * 32767U) / attack_frames;
    }
    if (release_frames > 0U && note_frame + release_frames >= note_frames) {
        return ((note_frames - note_frame) * 32767U) / release_frames;
    }
    return 32767U;
}

static void device_call_ringtone_task(void *ctx)
{
    const audio_format_t *format = speaker_get_playback_format();
    device_call_ringtone_runtime_note_t runtime_notes[DEVICE_CALL_RINGTONE_NOTE_COUNT];
    uint64_t rendered_frames = 0U;
    esp_err_t result = ESP_OK;

    (void)ctx;
    if (format == NULL || format->bits_per_sample != 16U ||
        (format->channels != 1U && format->channels != 2U) ||
        format->sample_rate_hz == 0U) {
        result = ESP_ERR_NOT_SUPPORTED;
        goto done;
    }

    const size_t chunk_frames =
        ((size_t)format->sample_rate_hz * DEVICE_CALL_RINGTONE_CHUNK_MS) / 1000U;
    const size_t chunk_samples = chunk_frames * format->channels;
    if (chunk_frames == 0U || chunk_samples > DEVICE_CALL_RINGTONE_MAX_SAMPLES) {
        result = ESP_ERR_INVALID_SIZE;
        goto done;
    }

    result = speaker_prepare_playback_path();
    if (result != ESP_OK) {
        goto done;
    }

    const uint32_t attack_frames =
        (format->sample_rate_hz * DEVICE_CALL_RINGTONE_ATTACK_MS) / 1000U;
    const uint32_t release_frames =
        (format->sample_rate_hz * DEVICE_CALL_RINGTONE_RELEASE_MS) / 1000U;
    const uint32_t period_frames =
        (format->sample_rate_hz * DEVICE_CALL_RINGTONE_PERIOD_MS) / 1000U;
    for (size_t note = 0U; note < DEVICE_CALL_RINGTONE_NOTE_COUNT; ++note) {
        runtime_notes[note].start_frame =
            (format->sample_rate_hz * s_ringtone_notes[note].start_ms) / 1000U;
        runtime_notes[note].duration_frames =
            (format->sample_rate_hz * s_ringtone_notes[note].duration_ms) / 1000U;
        runtime_notes[note].phase_step =
            (uint32_t)(((uint64_t)s_ringtone_notes[note].frequency_hz << 32) /
                       format->sample_rate_hz);
    }

    ESP_LOGI(TAG,
             "ringtone started: rate=%luHz channels=%u style=mobile-chime notes=%u period=%ums stack=PSRAM",
             (unsigned long)format->sample_rate_hz,
             (unsigned)format->channels,
             (unsigned)DEVICE_CALL_RINGTONE_NOTE_COUNT,
             (unsigned)DEVICE_CALL_RINGTONE_PERIOD_MS);

    while (!device_call_ringtone_stop_requested()) {
        for (size_t frame = 0; frame < chunk_frames; ++frame) {
            uint32_t pattern_frame = (uint32_t)((rendered_frames + frame) % period_frames);
            int16_t sample = 0;

            for (size_t note = 0U; note < DEVICE_CALL_RINGTONE_NOTE_COUNT; ++note) {
                const device_call_ringtone_runtime_note_t *runtime_note = &runtime_notes[note];
                if (pattern_frame < runtime_note->start_frame ||
                    pattern_frame >= runtime_note->start_frame + runtime_note->duration_frames) {
                    continue;
                }

                uint32_t note_frame = pattern_frame - runtime_note->start_frame;
                uint32_t envelope_q15 =
                    device_call_ringtone_note_envelope(note_frame,
                                                       runtime_note->duration_frames,
                                                       attack_frames,
                                                       release_frames);
                sample = device_call_ringtone_sample(note_frame * runtime_note->phase_step,
                                                      envelope_q15);
                break;
            }

            for (uint8_t channel = 0U; channel < format->channels; ++channel) {
                s_ringtone_pcm[frame * format->channels + channel] = sample;
            }
        }

        result = speaker_play_pcm_frame((const uint8_t *)s_ringtone_pcm,
                                        chunk_samples * sizeof(int16_t),
                                        format);
        if (result != ESP_OK) {
            break;
        }
        rendered_frames += chunk_frames;
    }

done:
    speaker_stop_playback();
    taskENTER_CRITICAL(&s_ringtone_lock);
    if (s_ringtone_task == xTaskGetCurrentTaskHandle()) {
        s_ringtone_task = NULL;
    }
    s_ringtone_stop_requested = false;
    taskEXIT_CRITICAL(&s_ringtone_lock);

    if (result == ESP_OK) {
        ESP_LOGI(TAG, "ringtone stopped");
    } else {
        ESP_LOGW(TAG, "ringtone stopped after playback error: %s", esp_err_to_name(result));
    }
    vTaskDeleteWithCaps(NULL);
}

esp_err_t device_call_ringtone_start(void)
{
    BaseType_t task_result = pdFAIL;

    taskENTER_CRITICAL(&s_ringtone_lock);
    if (s_ringtone_task != NULL) {
        taskEXIT_CRITICAL(&s_ringtone_lock);
        return ESP_OK;
    }
    s_ringtone_stop_requested = false;
    taskEXIT_CRITICAL(&s_ringtone_lock);

    task_result = xTaskCreatePinnedToCoreWithCaps(device_call_ringtone_task,
                                                  "call_ring",
                                                  DEVICE_CALL_RINGTONE_TASK_STACK_BYTES,
                                                  NULL,
                                                  DEVICE_CALL_RINGTONE_TASK_PRIORITY,
                                                  &s_ringtone_task,
                                                  APP_TASK_CORE_AUDIO,
                                                  APP_TASK_STACK_CAPS_BACKGROUND);
    if (task_result != pdPASS) {
        taskENTER_CRITICAL(&s_ringtone_lock);
        s_ringtone_task = NULL;
        s_ringtone_stop_requested = false;
        taskEXIT_CRITICAL(&s_ringtone_lock);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t device_call_ringtone_stop(void)
{
    TickType_t started = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(DEVICE_CALL_RINGTONE_STOP_TIMEOUT_MS);

    taskENTER_CRITICAL(&s_ringtone_lock);
    if (s_ringtone_task == NULL) {
        s_ringtone_stop_requested = false;
        taskEXIT_CRITICAL(&s_ringtone_lock);
        return ESP_OK;
    }
    s_ringtone_stop_requested = true;
    taskEXIT_CRITICAL(&s_ringtone_lock);

    while (device_call_ringtone_is_active()) {
        if ((xTaskGetTickCount() - started) >= timeout) {
            ESP_LOGE(TAG, "ringtone stop timed out");
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_OK;
}

bool device_call_ringtone_is_active(void)
{
    bool active = false;

    taskENTER_CRITICAL(&s_ringtone_lock);
    active = s_ringtone_task != NULL;
    taskEXIT_CRITICAL(&s_ringtone_lock);
    return active;
}
