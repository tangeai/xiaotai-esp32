// SPDX-License-Identifier: MIT
#include "xmini_audio.h"
#include "xmini_board.h"
#include "xmini_echo_guard.h"

#include <stdatomic.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_attr.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_g711_enc.h"
#include "esp_g711_dec.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "assets/guidance_index.inc"

extern const uint8_t s_guidance_data[] asm("_binary_guidance_alaw_start");

#if CONFIG_AUDIO_ENCODER_OPUS_SUPPORT || CONFIG_AUDIO_DECODER_OPUS_SUPPORT
#error "Xmini-C3-V3 must not enable Opus"
#endif

#define DMA_DESCRIPTORS 4U
#define RECORD_FRAMES 50U
#define TONE_FRAMES 25U

static const char *TAG = "xmini_audio";
static i2s_chan_handle_t s_tx, s_rx;
static bool s_tx_enabled, s_rx_enabled;
static const audio_codec_data_if_t *s_data;
static const audio_codec_ctrl_if_t *s_ctrl;
static const audio_codec_gpio_if_t *s_gpio;
static const audio_codec_if_t *s_codec;
static esp_codec_dev_handle_t s_dev;
static void *s_encoder, *s_decoder;
static QueueHandle_t s_requests;
static TaskHandle_t s_worker_task;
static atomic_bool s_pending, s_stop;
static atomic_uint s_stream_generation;
static atomic_uint s_tone_remaining;
typedef struct { uint32_t generation; uint8_t packet[XMINI_PCMA_SAMPLES]; } downlink_t;
static QueueHandle_t s_downlink;
static StaticQueue_t s_downlink_storage;
static uint8_t s_downlink_buffer[8 * sizeof(downlink_t)];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static xmini_audio_status_t s_status = {.volume = 40, .gain_db = 24, .prompt = -1, .echo_enabled = true};
static RTC_FAST_ATTR uint8_t s_recording[RECORD_FRAMES][XMINI_PCMA_SAMPLES];
static unsigned s_recording_offset;
static int16_t s_pcm[XMINI_PCMA_SAMPLES];
static uint8_t s_packet[XMINI_PCMA_SAMPLES];
static int s_applied_volume, s_applied_gain;
static xmini_echo_guard_t s_echo;

static esp_err_t codec_result(int result, const char *stage)
{
    if (result == ESP_CODEC_DEV_OK) return ESP_OK;
    ESP_LOGE(TAG, "%s: codec result=%d", stage, result);
    return ESP_FAIL;
}

xmini_audio_status_t xmini_audio_status(void)
{
    portENTER_CRITICAL(&s_lock);
    xmini_audio_status_t status = s_status;
    status.busy |= atomic_load(&s_pending);
    portEXIT_CRITICAL(&s_lock);
    return status;
}

bool xmini_audio_stack_watermark(unsigned *bytes)
{
    if (!bytes) return false;
    portENTER_CRITICAL(&s_lock);
    TaskHandle_t task = s_worker_task;
    portEXIT_CRITICAL(&s_lock);
    // This worker is never deleted after successful creation.
    *bytes = task ? (unsigned)uxTaskGetStackHighWaterMark(task) : 0;
    return task != NULL;
}

static void release_resources(void)
{
    gpio_set_level(XMINI_PA_GPIO, 0);
    if (s_dev) {
        esp_codec_dev_delete(s_dev);
        s_dev = NULL;
    }
    if (s_codec) audio_codec_delete_codec_if(s_codec);
    if (s_ctrl) audio_codec_delete_ctrl_if(s_ctrl);
    if (s_gpio) audio_codec_delete_gpio_if(s_gpio);
    if (s_data) audio_codec_delete_data_if(s_data);
    s_codec = NULL;
    s_ctrl = NULL;
    s_gpio = NULL;
    s_data = NULL;
    if (s_tx) {
        if (s_tx_enabled) i2s_channel_disable(s_tx);
        i2s_del_channel(s_tx);
        s_tx = NULL;
    }
    if (s_rx) {
        if (s_rx_enabled) i2s_channel_disable(s_rx);
        i2s_del_channel(s_rx);
        s_rx = NULL;
    }
    s_tx_enabled = s_rx_enabled = false;
    if (s_encoder) esp_g711_enc_close(s_encoder);
    if (s_decoder) esp_g711_dec_close(s_decoder);
    s_encoder = s_decoder = NULL;
    if (s_requests) vQueueDelete(s_requests);
    s_requests = NULL;
}

static esp_err_t device_open(void)
{
    esp_codec_dev_sample_info_t format = {
        .sample_rate = XMINI_PCMA_RATE,
        .bits_per_sample = 16,
        .channel = 1,
        .mclk_multiple = 256,
    };
    ESP_RETURN_ON_ERROR(codec_result(esp_codec_dev_open(s_dev, &format), "open"), TAG, "open");
    xmini_audio_status_t status = xmini_audio_status();
    ESP_RETURN_ON_ERROR(codec_result(esp_codec_dev_set_out_vol(s_dev, status.volume), "volume"), TAG, "volume");
    ESP_RETURN_ON_ERROR(codec_result(esp_codec_dev_set_in_gain(s_dev, status.gain_db), "gain"), TAG, "gain");
    s_applied_volume = status.volume;
    s_applied_gain = status.gain_db;
    return ESP_OK;
}

static esp_err_t encode_frame(uint8_t packet[XMINI_PCMA_SAMPLES])
{
    esp_audio_enc_in_frame_t input = {.buffer = (uint8_t *)s_pcm, .len = sizeof(s_pcm)};
    esp_audio_enc_out_frame_t output = {.buffer = packet, .len = XMINI_PCMA_SAMPLES};
    int result = esp_g711_enc_process(s_encoder, &input, &output);
    if (result != ESP_AUDIO_ERR_OK || output.encoded_bytes != XMINI_PCMA_SAMPLES) {
        ESP_LOGE(TAG, "PCMA encode result=%d bytes=%lu", result, (unsigned long)output.encoded_bytes);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t decode_frame(uint8_t packet[XMINI_PCMA_SAMPLES])
{
    esp_audio_dec_in_raw_t raw = {.buffer = packet, .len = XMINI_PCMA_SAMPLES};
    esp_audio_dec_out_frame_t output = {.buffer = (uint8_t *)s_pcm, .len = sizeof(s_pcm)};
    esp_audio_dec_info_t info = {0};
    int result = esp_g711a_dec_decode(s_decoder, &raw, &output, &info);
    if (result != ESP_AUDIO_ERR_OK || raw.consumed != XMINI_PCMA_SAMPLES ||
        output.decoded_size != sizeof(s_pcm)) {
        ESP_LOGE(TAG, "PCMA decode result=%d consumed=%lu pcm=%lu", result,
                 (unsigned long)raw.consumed, (unsigned long)output.decoded_size);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void progress(unsigned frames)
{
    uint32_t peak = 0;
    for (unsigned i = 0; i < XMINI_PCMA_SAMPLES; ++i) {
        int32_t value = s_pcm[i];
        uint32_t magnitude = value < 0 ? (uint32_t)-value : (uint32_t)value;
        if (magnitude > peak) peak = magnitude;
    }
    portENTER_CRITICAL(&s_lock);
    s_status.frames = frames;
    s_status.peak = peak;
    portEXIT_CRITICAL(&s_lock);
}

static esp_err_t run_audio(const xmini_audio_request_t *request)
{
    xmini_audio_command_t kind = request->kind;
    static const int16_t tone[20] = {
        0,927,1763,2427,2853,3000,2853,2427,1763,927,
        0,-927,-1763,-2427,-2853,-3000,-2853,-2427,-1763,-927,
    };
    unsigned count = kind == XMINI_AUDIO_STREAM ? UINT32_MAX : kind == XMINI_AUDIO_TONE ? TONE_FRAMES : RECORD_FRAMES;
    unsigned clips[7] = {(unsigned)request->value};
    unsigned clip_count = 1;
    if (kind == XMINI_AUDIO_PROMPT) {
        count = s_guidance[clips[0]].bytes / XMINI_PCMA_SAMPLES;
        if (request->value == XMINI_PROMPT_BINDING) {
            for (unsigned i = 0; i < 6; ++i) {
                clips[clip_count++] = XMINI_PROMPT_COUNT + (unsigned)(request->code[i] - '0');
                count += s_guidance[clips[i + 1]].bytes / XMINI_PCMA_SAMPLES;
            }
        }
    }
    portENTER_CRITICAL(&s_lock);
    s_status.expected_frames = count;
    portEXIT_CRITICAL(&s_lock);
    esp_err_t ret = device_open();
    if (ret == ESP_OK && kind == XMINI_AUDIO_STREAM) xmini_echo_guard_reset(&s_echo, s_applied_volume, s_applied_gain);
    bool output = kind != XMINI_AUDIO_RECORD;
    if (ret == ESP_OK && output) {
        memset(s_pcm, 0, sizeof(s_pcm));
        ret = codec_result(esp_codec_dev_write(s_dev, s_pcm, sizeof(s_pcm)), "prime output");
        if (ret == ESP_OK) ret = gpio_set_level(XMINI_PA_GPIO, 1);
    }
    portENTER_CRITICAL(&s_lock);
    s_status.device_open = ret == ESP_OK;
    portEXIT_CRITICAL(&s_lock);
    unsigned completed = 0;
    unsigned received_frames = 0;
    while (ret == ESP_OK && completed < count && !atomic_load(&s_stop)) {
        xmini_audio_status_t controls = xmini_audio_status();
        if (controls.volume != s_applied_volume) {
            ret = codec_result(esp_codec_dev_set_out_vol(s_dev, controls.volume), "live volume");
            if (ret == ESP_OK) {
                s_applied_volume = controls.volume;
                xmini_echo_guard_levels(&s_echo, s_applied_volume, s_applied_gain);
            }
        }
        if (ret == ESP_OK && controls.gain_db != s_applied_gain) {
            ret = codec_result(esp_codec_dev_set_in_gain(s_dev, controls.gain_db), "live gain");
            if (ret == ESP_OK) {
                s_applied_gain = controls.gain_db;
                xmini_echo_guard_levels(&s_echo, s_applied_volume, s_applied_gain);
            }
        }
        if (ret != ESP_OK) break;
        if (kind == XMINI_AUDIO_STREAM) {
            ret = codec_result(esp_codec_dev_read(s_dev, s_pcm, sizeof(s_pcm)), "stream capture");
            if (atomic_load(&s_stop)) break;
            if (ret == ESP_OK && controls.echo_enabled) {
                xmini_echo_guard_uplink(&s_echo, s_pcm, XMINI_PCMA_SAMPLES);
                portENTER_CRITICAL(&s_lock);
                s_status.echo_closed_frames = s_echo.closed_frames;
                s_status.echo_dt_frames = s_echo.dt_frames;
                s_status.echo_open_frames = s_echo.open_frames;
                s_status.echo_gain_q15 = s_echo.gain_q15;
                portEXIT_CRITICAL(&s_lock);
            }
            if (ret == ESP_OK && atomic_load(&s_tone_remaining)) {
                for (unsigned i = 0; i < XMINI_PCMA_SAMPLES; ++i) s_pcm[i] = tone[i % 20];
                atomic_fetch_sub(&s_tone_remaining, 1);
                portENTER_CRITICAL(&s_lock);
                ++s_status.injected_frames;
                portEXIT_CRITICAL(&s_lock);
            }
            if (ret == ESP_OK) ret = encode_frame(s_packet);
            if (atomic_load(&s_stop)) break;
            if (ret == ESP_OK) {
                bool sent = request->transmit(request->generation, s_packet, sizeof(s_packet),
                                               (uint64_t)(esp_timer_get_time() / 1000), request->context);
                portENTER_CRITICAL(&s_lock);
                if (sent) ++s_status.tx_frames;
                else ++s_status.tx_dropped;
                portEXIT_CRITICAL(&s_lock);
                downlink_t downlink;
                bool received = false;
                for (unsigned i = 0; i < 8 && xQueueReceive(s_downlink, &downlink, 0) == pdTRUE; ++i) {
                    if (downlink.generation == request->generation) { received = true; break; }
                }
                if (received) {
                    memcpy(s_packet, downlink.packet, sizeof(s_packet));
                    memcpy(s_recording[received_frames++ % RECORD_FRAMES], s_packet, sizeof(s_packet));
                    ret = decode_frame(s_packet);
                    portENTER_CRITICAL(&s_lock);
                    ++s_status.rx_frames;
                    portEXIT_CRITICAL(&s_lock);
                } else {
                    memset(s_pcm, 0, sizeof(s_pcm));
                    portENTER_CRITICAL(&s_lock);
                    ++s_status.underruns;
                    portEXIT_CRITICAL(&s_lock);
                }
                if (ret == ESP_OK) {
                    xmini_echo_guard_downlink(&s_echo, s_pcm, XMINI_PCMA_SAMPLES);
                    ret = codec_result(esp_codec_dev_write(s_dev, s_pcm, sizeof(s_pcm)), "stream playback");
                }
            }
        } else if (kind == XMINI_AUDIO_RECORD) {
            ret = codec_result(esp_codec_dev_read(s_dev, s_pcm, sizeof(s_pcm)), "capture");
            if (ret == ESP_OK) ret = encode_frame(s_recording[completed]);
        } else {
            if (kind == XMINI_AUDIO_PROMPT) {
                unsigned frame = completed;
                for (unsigned i = 0; i < clip_count; ++i) {
                    unsigned frames = s_guidance[clips[i]].bytes / XMINI_PCMA_SAMPLES;
                    if (frame < frames) {
                        memcpy(s_packet, s_guidance_data + s_guidance[clips[i]].offset +
                               frame * XMINI_PCMA_SAMPLES, sizeof(s_packet));
                        break;
                    }
                    frame -= frames;
                }
                ret = decode_frame(s_packet);
            } else if (kind == XMINI_AUDIO_TONE) {
                for (unsigned i = 0; i < XMINI_PCMA_SAMPLES; ++i) s_pcm[i] = tone[i % 20];
                ret = encode_frame(s_packet);
                if (ret == ESP_OK) ret = decode_frame(s_packet);
            } else {
                ret = decode_frame(s_recording[(s_recording_offset + completed) % RECORD_FRAMES]);
            }
            if (ret == ESP_OK) ret = codec_result(esp_codec_dev_write(s_dev, s_pcm, sizeof(s_pcm)), "playback");
        }
        if (ret == ESP_OK) progress(++completed);
    }
    // Flush one DMA ring plus a frame; cancellation skips this normal tail drain.
    if (ret == ESP_OK && output && !atomic_load(&s_stop)) {
        memset(s_pcm, 0, sizeof(s_pcm));
        for (unsigned i = 0; i < DMA_DESCRIPTORS + 1 && ret == ESP_OK && !atomic_load(&s_stop); ++i) {
            ret = codec_result(esp_codec_dev_write(s_dev, s_pcm, sizeof(s_pcm)), "drain output");
        }
    }
    esp_err_t pa_result = gpio_set_level(XMINI_PA_GPIO, 0);
    esp_err_t close_result = codec_result(esp_codec_dev_close(s_dev), "close");
    if (ret == ESP_OK) ret = pa_result;
    if (ret == ESP_OK) ret = close_result;
    portENTER_CRITICAL(&s_lock);
    s_status.device_open = false;
    if (kind == XMINI_AUDIO_RECORD) {
        s_status.recording_ready = ret == ESP_OK && completed == RECORD_FRAMES && !atomic_load(&s_stop);
        s_status.recording_source = 0;
        s_recording_offset = 0;
    } else if (kind == XMINI_AUDIO_STREAM) {
        s_status.recording_ready = ret == ESP_OK && received_frames >= RECORD_FRAMES;
        s_status.recording_source = 1;
        s_recording_offset = received_frames % RECORD_FRAMES;
    }
    if (close_result != ESP_OK || pa_result != ESP_OK) s_status.ready = false;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "operation=%d frames=%u stopped=%d result=%s", kind,
             completed, atomic_load(&s_stop), esp_err_to_name(ret));
    return ret;
}

static void worker(void *arg)
{
    (void)arg;
    xmini_audio_request_t request;
    for (;;) {
        if (xQueueReceive(s_requests, &request, portMAX_DELAY) != pdTRUE) continue;
        portENTER_CRITICAL(&s_lock);
        s_status.busy = true;
        s_status.error = false;
        s_status.frames = 0;
        s_status.peak = 0;
        s_status.prompt = request.kind == XMINI_AUDIO_PROMPT ? request.value : -1;
        s_status.expected_frames = 0;
        s_status.stopped = false;
        s_status.owner = request.owner;
        s_status.generation = request.generation;
        s_status.tx_frames = s_status.rx_frames = s_status.tx_dropped = s_status.rx_dropped = s_status.underruns = 0;
        s_status.injected_frames = 0;
        s_status.echo_closed_frames = s_status.echo_dt_frames = s_status.echo_open_frames = s_status.echo_gain_q15 = 0;
        if (request.kind == XMINI_AUDIO_RECORD || request.kind == XMINI_AUDIO_STREAM) s_status.recording_ready = false;
        portEXIT_CRITICAL(&s_lock);
        esp_err_t result = ESP_OK;
        if (request.kind == XMINI_AUDIO_VOLUME || request.kind == XMINI_AUDIO_GAIN) {
            // Hardware is closed in standby; apply these values on the next open.
            portENTER_CRITICAL(&s_lock);
            if (request.kind == XMINI_AUDIO_VOLUME) s_status.volume = request.value;
            else s_status.gain_db = request.value;
            portEXIT_CRITICAL(&s_lock);
        } else if (!atomic_load(&s_stop)) {
            result = run_audio(&request);
        }
        portENTER_CRITICAL(&s_lock);
        s_status.error = result != ESP_OK;
        s_status.stopped = atomic_load(&s_stop);
        s_status.busy = false;
        if (request.kind == XMINI_AUDIO_STREAM && atomic_load(&s_stream_generation) == request.generation)
            atomic_store(&s_stream_generation, 0);
        atomic_store(&s_pending, false);
        portEXIT_CRITICAL(&s_lock);
    }
}

esp_err_t xmini_audio_request(xmini_audio_request_t request)
{
    if (request.kind == XMINI_AUDIO_STOP) {
        portENTER_CRITICAL(&s_lock);
        // RTC publishes its generation under this same reservation lock.
        // Only stream_stop(generation) may cancel that owner, even before open.
        if (atomic_load(&s_stream_generation)) {
            portEXIT_CRITICAL(&s_lock);
            return ESP_ERR_INVALID_STATE;
        }
        atomic_store(&s_stop, true);
        portEXIT_CRITICAL(&s_lock);
        return ESP_OK;
    }
    xmini_audio_status_t status = xmini_audio_status();
    if (!status.ready || !s_requests) return ESP_ERR_INVALID_STATE;
    if (request.owner < XMINI_AUDIO_DIAGNOSTIC || request.owner > XMINI_AUDIO_RTC ||
        request.kind < XMINI_AUDIO_VOLUME ||
        (request.kind > XMINI_AUDIO_PLAY && request.kind != XMINI_AUDIO_PROMPT && request.kind != XMINI_AUDIO_STREAM) ||
        (request.kind == XMINI_AUDIO_VOLUME && (request.value < 0 || request.value > 100)) ||
        (request.kind == XMINI_AUDIO_GAIN && (request.value < 0 || request.value > 36)) ||
        (request.kind == XMINI_AUDIO_PROMPT && (request.value < 0 || request.value >= XMINI_PROMPT_COUNT)) ||
        (request.kind == XMINI_AUDIO_STREAM && (!request.generation || !request.transmit || request.owner != XMINI_AUDIO_RTC))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (request.kind == XMINI_AUDIO_PROMPT && request.value == XMINI_PROMPT_BINDING) {
        for (unsigned i = 0; i < 6; ++i)
            if (request.code[i] < '0' || request.code[i] > '9') return ESP_ERR_INVALID_ARG;
        if (request.code[6]) return ESP_ERR_INVALID_ARG;
    }
    if (request.kind == XMINI_AUDIO_PLAY && !status.recording_ready) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_lock);
    if ((request.kind == XMINI_AUDIO_VOLUME || request.kind == XMINI_AUDIO_GAIN) && s_status.ready) {
        if (request.kind == XMINI_AUDIO_VOLUME) s_status.volume = request.value;
        else s_status.gain_db = request.value;
        portEXIT_CRITICAL(&s_lock);
        return ESP_OK;
    }
    if (atomic_load(&s_pending) || !s_status.ready ||
        (request.kind == XMINI_AUDIO_PLAY && !s_status.recording_ready)) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store(&s_pending, true);
    atomic_store(&s_stop, false);
    atomic_store(&s_tone_remaining, 0);
    atomic_store(&s_stream_generation, request.kind == XMINI_AUDIO_STREAM ? request.generation : 0);
    portEXIT_CRITICAL(&s_lock);
    if (request.kind == XMINI_AUDIO_STREAM) {
        xQueueReset(s_downlink);
    }
    if (xQueueSend(s_requests, &request, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_lock);
        atomic_store(&s_pending, false);
        if (request.kind == XMINI_AUDIO_STREAM) atomic_store(&s_stream_generation, 0);
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t xmini_audio_initial_settings(int volume, int gain_db)
{
    if (s_requests) return ESP_ERR_INVALID_STATE;
    if (volume < 0 || volume > 100 || gain_db < 0 || gain_db > 36) return ESP_ERR_INVALID_ARG;
    s_status.volume = volume;
    s_status.gain_db = gain_db;
    return ESP_OK;
}

esp_err_t xmini_audio_echo_guard_set(bool enabled)
{
    portENTER_CRITICAL(&s_lock);
    s_status.echo_enabled = enabled;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

bool xmini_audio_echo_guard_enabled(void)
{
    return xmini_audio_status().echo_enabled;
}

esp_err_t xmini_audio_stream_start(uint32_t generation, xmini_audio_tx_t transmit, void *context)
{
    return xmini_audio_request((xmini_audio_request_t){.kind = XMINI_AUDIO_STREAM,
        .owner = XMINI_AUDIO_RTC, .generation = generation, .transmit = transmit, .context = context});
}

esp_err_t xmini_audio_stream_stop(uint32_t generation)
{
    portENTER_CRITICAL(&s_lock);
    if (!generation || atomic_load(&s_stream_generation) != generation) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store(&s_stop, true);
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

bool xmini_audio_receive(uint32_t generation, const uint8_t *packet, size_t length)
{
    if (!packet || length != XMINI_PCMA_SAMPLES || !generation ||
        atomic_load(&s_stream_generation) != generation || atomic_load(&s_stop)) return false;
    downlink_t frame = {.generation = generation};
    memcpy(frame.packet, packet, sizeof(frame.packet));
    if (xQueueSend(s_downlink, &frame, 0) == pdTRUE) return true;
    portENTER_CRITICAL(&s_lock);
    if (atomic_load(&s_stream_generation) == generation) ++s_status.rx_dropped;
    portEXIT_CRITICAL(&s_lock);
    return false;
}

bool xmini_audio_stream_busy(uint32_t generation)
{
    portENTER_CRITICAL(&s_lock);
    bool busy = generation && atomic_load(&s_stream_generation) == generation && atomic_load(&s_pending);
    portEXIT_CRITICAL(&s_lock);
    return busy;
}

void xmini_audio_stream_flush(uint32_t generation)
{
    portENTER_CRITICAL(&s_lock);
    if (generation && atomic_load(&s_stream_generation) == generation) xQueueReset(s_downlink);
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t xmini_audio_stream_tone(uint32_t generation)
{
    portENTER_CRITICAL(&s_lock);
    bool valid = generation && atomic_load(&s_stream_generation) == generation &&
                 atomic_load(&s_pending) && !atomic_load(&s_stop);
    if (valid) atomic_store(&s_tone_remaining, TONE_FRAMES);
    portEXIT_CRITICAL(&s_lock);
    return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t xmini_audio_recording_copy(uint8_t *out, size_t capacity)
{
    if (!out || capacity < sizeof(s_recording)) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    if (atomic_load(&s_pending) || !s_status.recording_ready) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    for (unsigned i = 0; i < RECORD_FRAMES; ++i)
        memcpy(out + i * XMINI_PCMA_SAMPLES, s_recording[(s_recording_offset + i) % RECORD_FRAMES], XMINI_PCMA_SAMPLES);
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t xmini_audio_prompt(xmini_prompt_t prompt, const char *code)
{
    xmini_audio_request_t request = {.kind = XMINI_AUDIO_PROMPT, .owner = XMINI_AUDIO_GUIDANCE, .value = prompt};
    if (code) {
        if (strlen(code) != 6) return ESP_ERR_INVALID_ARG;
        memcpy(request.code, code, sizeof(request.code));
    }
    return xmini_audio_request(request);
}

esp_err_t xmini_audio_init(void)
{
    if (s_requests || !xmini_board_i2c()) return ESP_ERR_INVALID_STATE;
    esp_err_t ret;
    TaskHandle_t worker_task = NULL;
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel.dma_desc_num = DMA_DESCRIPTORS;
    channel.dma_frame_num = XMINI_PCMA_SAMPLES;
    channel.auto_clear_after_cb = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel, &s_tx, &s_rx), TAG, "I2S channels");
    i2s_std_config_t standard = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(XMINI_PCMA_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = XMINI_I2S_MCLK, .bclk = XMINI_I2S_BCLK, .ws = XMINI_I2S_WS,
            .din = XMINI_I2S_DIN, .dout = XMINI_I2S_DOUT,
        },
    };
    ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(s_tx, &standard), fail, TAG, "TX init");
    ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(s_rx, &standard), fail, TAG, "RX init");
    ESP_GOTO_ON_ERROR(i2s_channel_enable(s_tx), fail, TAG, "TX enable");
    s_tx_enabled = true;
    ESP_GOTO_ON_ERROR(i2s_channel_enable(s_rx), fail, TAG, "RX enable");
    s_rx_enabled = true;
    audio_codec_i2s_cfg_t data = {.port = I2S_NUM_0, .rx_handle = s_rx, .tx_handle = s_tx};
    s_data = audio_codec_new_i2s_data(&data);
    audio_codec_i2c_cfg_t control = {
        .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = xmini_board_i2c(), .clock_speed_hz = 400000,
    };
    s_ctrl = audio_codec_new_i2c_ctrl(&control);
    s_gpio = audio_codec_new_gpio();
    ESP_GOTO_ON_FALSE(s_data && s_ctrl && s_gpio, ESP_ERR_NO_MEM, fail, TAG, "codec interfaces");
    es8311_codec_cfg_t codec = {
        .ctrl_if = s_ctrl, .gpio_if = s_gpio,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = -1, .use_mclk = true, .mclk_div = 256,
        .hw_gain = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3},
    };
    s_codec = es8311_codec_new(&codec);
    ESP_GOTO_ON_FALSE(s_codec, ESP_ERR_NO_MEM, fail, TAG, "ES8311");
    esp_codec_dev_cfg_t device = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT, .codec_if = s_codec, .data_if = s_data,
    };
    s_dev = esp_codec_dev_new(&device);
    ESP_GOTO_ON_FALSE(s_dev, ESP_ERR_NO_MEM, fail, TAG, "codec device");
    ESP_GOTO_ON_ERROR(device_open(), close_fail, TAG, "initial audio format");
    ESP_GOTO_ON_ERROR(codec_result(esp_codec_dev_close(s_dev), "initial close"), fail, TAG, "initial close");
    s_tx_enabled = s_rx_enabled = false;
    esp_g711_enc_config_t encoder = ESP_G711_ENC_CONFIG_DEFAULT();
    encoder.frame_duration = XMINI_PCMA_FRAME_MS;
    ESP_GOTO_ON_FALSE(esp_g711a_enc_open(&encoder, sizeof(encoder), &s_encoder) == ESP_AUDIO_ERR_OK,
                      ESP_FAIL, fail, TAG, "PCMA encoder");
    ESP_GOTO_ON_FALSE(esp_g711_dec_open(NULL, 0, &s_decoder) == ESP_AUDIO_ERR_OK,
                      ESP_FAIL, fail, TAG, "PCMA decoder");
    s_requests = xQueueCreate(1, sizeof(xmini_audio_request_t));
    s_downlink = xQueueCreateStatic(8, sizeof(downlink_t), s_downlink_buffer, &s_downlink_storage);
    ESP_GOTO_ON_FALSE(s_requests, ESP_ERR_NO_MEM, fail, TAG, "audio queue");
    ESP_GOTO_ON_FALSE(xTaskCreate(worker, "xmini_audio", 6144, NULL, 5, &worker_task) == pdPASS,
                      ESP_ERR_NO_MEM, fail, TAG, "audio worker");
    portENTER_CRITICAL(&s_lock);
    s_worker_task = worker_task;
    s_status.ready = true;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "PCMA 8000 Hz mono, 160 samples/20 ms; idle codec closed, PA off");
    return ESP_OK;

close_fail:
    codec_result(esp_codec_dev_close(s_dev), "failed-open close");
fail:
    release_resources();
    return ret;
}
