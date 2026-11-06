/*
 * CoreS3-SE media adapter.
 *
 * 上行：ES7210 MIC1 + MIC3 扬声器回采（MIC2 仅测电平）-> ESP-SR AFE MR 16 kHz ->
 *       H5/AI/Room A-law 8 kHz，设备互呼/微信 Opus 16 kHz，均为单声道 20 ms。
 * 下行：按业务解码到 16 kHz PCM -> AW88298。
 *
 * Standard TX and four-slot TDM RX share one clock.
 */
#include "starter_media.h"
#include "starter_board.h"
#include "starter_camera.h"
#include "starter_aec.h"
#include "starter_agc.h"
#include "starter_audio_resampler.h"
#include "starter_voice.h"
#include "starter_preroll.h"
#include "starter_playout.h"
#include "starter_log.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "decoder/impl/esp_g711_dec.h"
#include "decoder/impl/esp_opus_dec.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "encoder/impl/esp_g711_enc.h"
#include "encoder/impl/esp_opus_enc.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es7210_adc.h"
#include "aw88298_dac.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#if CONFIG_XIAOTAI_DEVELOPMENT_CONSOLE
#include "esp_freertos_hooks.h"
#endif

#define BOARD_I2C_PORT I2C_NUM_0
#define BOARD_I2C_SDA GPIO_NUM_12
#define BOARD_I2C_SCL GPIO_NUM_11
#define BOARD_I2C_HZ 400000U

#define AUDIO_TRANSPORT_SAMPLE_RATE_HZ STARTER_TIRTC_AUDIO_SAMPLE_RATE_HZ
#define AUDIO_HW_SAMPLE_RATE_HZ STARTER_AUDIO_DMA_SAMPLE_RATE_HZ
#define AUDIO_PACKET_MS 20U
#define AUDIO_PACKET_SAMPLES \
    ((AUDIO_TRANSPORT_SAMPLE_RATE_HZ * AUDIO_PACKET_MS) / 1000U)
#define AUDIO_H5_PACKET_SAMPLES \
    ((STARTER_H5_AUDIO_SAMPLE_RATE_HZ * STARTER_H5_AUDIO_PACKET_MS) / 1000U)
#define AUDIO_MIC_CHANNELS 4U
#define AUDIO_MCLK_MULTIPLE 256U
#define AUDIO_MCLK_HZ (AUDIO_HW_SAMPLE_RATE_HZ * AUDIO_MCLK_MULTIPLE)
#define AUDIO_ENCODED_MAX_BYTES 1500U
#define AUDIO_RX_BYTES AUDIO_ENCODED_MAX_BYTES
#define AUDIO_DECODE_PCM_CAPACITY (AUDIO_RX_BYTES * 2U)
#define AUDIO_RX_QUEUE_DEPTH STARTER_PLAYOUT_CAPACITY
#define AUDIO_TX_QUEUE_DEPTH 12U
#define AUDIO_OPUS_DECODER_STACK_BYTES (24U * 1024U)
#define AUDIO_OPUS_ENCODER_STACK_BYTES (48U * 1024U)
#define AUDIO_OPUS_ENCODER_COMPLEXITY 0
#define AUDIO_RTC_I2S_VALUES_PER_INPUT 2U
#define AUDIO_PCM8K_I2S_VALUES_PER_INPUT 4U
#define AUDIO_OUTPUT_LOCK_WAIT_MS 500U
#define AUDIO_PLAYOUT_POLL_MS 5U
#define MEDIA_CPU_YIELD_MS 1U
#define MEDIA_REALTIME_CORE 1
#define PREROLL_CAPTURE_LOCK_WAIT_MS 2U

_Static_assert(AUDIO_HW_SAMPLE_RATE_HZ == STARTER_AEC_SAMPLE_RATE_HZ,
               "I2S and AEC sample rates must match");
_Static_assert(AUDIO_TRANSPORT_SAMPLE_RATE_HZ ==
                   STARTER_AEC_TRANSPORT_RATE_HZ,
               "AEC output and TiRTC sample rates must match");
_Static_assert(AUDIO_PACKET_SAMPLES == STARTER_PLAYOUT_PCM_SAMPLES,
               "playout quantum must be 20 ms at 16 kHz");
_Static_assert(AUDIO_H5_PACKET_SAMPLES * 2U == AUDIO_PACKET_SAMPLES,
               "H5 PCMA packet must map to one 16 kHz playout quantum");
_Static_assert(STARTER_ROOM_AUDIO_SAMPLE_RATE_HZ == STARTER_H5_AUDIO_SAMPLE_RATE_HZ &&
                   STARTER_ROOM_AUDIO_CHANNELS == STARTER_H5_AUDIO_CHANNELS,
                "Room shares the mono PCMA/8k resampler");
_Static_assert(STARTER_AI_AUDIO_SAMPLE_RATE_HZ == STARTER_H5_AUDIO_SAMPLE_RATE_HZ &&
                   STARTER_AI_AUDIO_CHANNELS == STARTER_H5_AUDIO_CHANNELS,
                "AI shares the mono PCMA/8k resampler");
_Static_assert(STARTER_AEC_CAPTURE_DMA_CHANNELS == 4U,
               "logical AFE adapter layout is mic1/ref/mic2/unused");

#define I2S_MCLK GPIO_NUM_0
#define I2S_BCLK GPIO_NUM_34
#define I2S_WS GPIO_NUM_33
#define I2S_ADC_DIN GPIO_NUM_14
#define I2S_DAC_DOUT GPIO_NUM_13
#define I2S_AUDIO_PORT I2S_NUM_0

typedef struct {
    _Alignas(8) uint64_t received_us;
    starter_tirtc_mode_t mode;
    uint32_t generation;
    uint32_t epoch;
    starter_tirtc_frame_t frame;
    uint8_t payload[AUDIO_RX_BYTES];
} audio_rx_item_t;

_Static_assert(sizeof(audio_rx_item_t) == 1536U, "S3 RX PSRAM budget changed");

/*
 * 采集/AEC 是硬实时任务，不能直接进入 TiRTC SDK。每个项目固定为一个
 * 20 ms、16 kHz、16-bit 单声道 PCM 包；池放在 PSRAM，队列只传递索引。
 */
typedef struct {
    starter_tirtc_mode_t mode;
    uint32_t generation;
    uint32_t timestamp_ms;
    uint32_t enqueued_us; /* Monotonic low 32 bits; queue budget is far below its wrap. */
    uint32_t captured_ms; /* Latest source frame's capture time, low 32 bits. */
    unsigned mute_epoch;
    int16_t pcm[AUDIO_PACKET_SAMPLES];
} audio_tx_item_t;

static const char *TAG = "starter_media";

static atomic_bool s_ready;
static atomic_bool s_wake_allowed = true;
static SemaphoreHandle_t s_preroll_mutex;
static starter_preroll_t s_preroll;
static uint32_t s_expected_wake_token; /* protected by preroll mutex */
static atomic_uint s_mute_epoch;
enum { PREROLL_GAP_CAPTURE = 1U, PREROLL_GAP_FETCH = 2U,
       PREROLL_GAP_LOCK = 4U, PREROLL_GAP_TIME = 8U,
       PREROLL_GAP_CAPACITY = 16U, PREROLL_GAP_DEADLINE = 32U };
static atomic_uint s_preroll_gap;
static atomic_uint s_capture_epoch;
static atomic_bool s_active;
static atomic_int s_mode;
static atomic_uint_fast32_t s_generation;
/* EOS closes ingress, not playback. The sink acknowledges only after all
 * admitted PCM and the bounded I2S output horizon have drained. */
static atomic_uint s_audio_rx_writers;
static atomic_uint s_drain_generation;
static atomic_uint s_drained_generation;
static atomic_uint_fast32_t s_audio_sent;
static atomic_uint_fast32_t s_audio_received;
static atomic_uint_fast32_t s_audio_dropped;
static atomic_uint_fast32_t s_audio_decoded;
static atomic_uint_fast32_t s_audio_played;
static atomic_uint_fast32_t s_audio_decode_failed;
static atomic_uint_fast32_t s_audio_playback_blocked;
static atomic_uint_fast32_t s_audio_write_failed;
/* Playback-only diagnostics, separate from capture and the mixed TX/RX drop
 * total. Counters are lifetime values; only the FIFO snapshot resets per epoch. */
EXT_RAM_BSS_ATTR static struct {
    atomic_uint_fast32_t epoch, owner;
    atomic_uint_fast32_t generation, target_ms, queued_ms, queued_peak_ms, buffering;
    atomic_uint_fast32_t jitter_us, gap_max_us, residence_max_us, late_bursts;
    atomic_uint_fast32_t untimed_pairs, timestamp_breaks, starts;
    atomic_uint_fast32_t invalid, overflow, stale, slot_errors;
    atomic_uint_fast32_t lock_timeouts, lock_max_us, last_lock_owner;
    atomic_uint_fast32_t write_max_us, slow_writes;
    atomic_uint_fast32_t decoded_samples, written_samples, discarded_samples;
    atomic_uint_fast32_t pending_samples, chunks, tails, decode_max_us;
    atomic_uint_fast32_t ingress_max_us, write_gap_max_us, service_gap_max_us;
    atomic_uint_fast32_t pcm_epoch, pcm_in_hash, pcm_out_hash;
    atomic_uint_fast32_t output_samples, slow_chunks, fast_chunks, fade_starts, dma_estimate_us;
    atomic_int rate_mode;
} s_playout_diag;

EXT_RAM_BSS_ATTR static struct {
    atomic_uint_fast32_t read_frames, read_errors, read_gap_max_us, read_max_us;
    atomic_uint_fast32_t afe_age_max_ms, fetch_wait_max_us, agc_max_us;
    atomic_uint_fast32_t tx_queue_peak, tx_age_max_us, tx_gain_max_us, encode_max_us, send_max_us;
    atomic_uint_fast32_t post_agc_peak, post_agc_rms, tx_peak, tx_rms, tx_clipped, tx_frames, tx_measure_max_us;
} s_pipeline_diag;
_Static_assert(sizeof(s_pipeline_diag) == 76U, "S3 pipeline diagnostics PSRAM budget changed");

#if CONFIG_XIAOTAI_DEVELOPMENT_CONSOLE
EXT_RAM_BSS_ATTR static starter_rx_trace_t s_rx_trace;
EXT_RAM_BSS_ATTR static struct {
    atomic_uint stage, at_us, buffering_count;
} s_rx_trace_sink;
_Static_assert(sizeof(s_rx_trace_sink) == 12U, "RX trace sink PSRAM budget changed");

static void rx_trace_stage(unsigned stage, unsigned count, bool buffering)
{
    if (!atomic_load_explicit(&s_rx_trace.active, memory_order_relaxed)) return;
    atomic_store_explicit(&s_rx_trace_sink.at_us, (uint32_t)esp_timer_get_time(), memory_order_relaxed);
    atomic_store_explicit(&s_rx_trace_sink.buffering_count, (count << 8) | buffering, memory_order_relaxed);
    atomic_store_explicit(&s_rx_trace_sink.stage, stage, memory_order_relaxed);
}

/* Each role has one worker. Only 32-bit published fields are read elsewhere. */
EXT_RAM_BSS_ATTR static struct {
    uint64_t total_us;
    atomic_uint_fast32_t frames, avg_us;
} s_codec_diag[2][2];
_Static_assert(sizeof(s_codec_diag) == 64U, "Codec timing PSRAM budget changed");

/* Tick ISR touches only these 24 DRAM bytes. Summed running samples across
 * both cores exclude other tasks, but still include codec memory stalls. */
DRAM_ATTR static struct {
    atomic_uintptr_t owner[2];
    atomic_uint running[2][2];
} s_codec_cpu;
_Static_assert(sizeof(s_codec_cpu) == 24U, "Codec ISR DRAM budget changed");
EXT_RAM_BSS_ATTR static atomic_bool s_codec_tick_ready;

static void IRAM_ATTR codec_cpu_tick(void)
{
    unsigned core = (unsigned)xPortGetCoreID();
    if (core >= 2U) return;
    uintptr_t current = (uintptr_t)xTaskGetCurrentTaskHandle();
    for (unsigned role = 0; role < 2U; ++role) {
        uintptr_t owner = atomic_load_explicit(&s_codec_cpu.owner[role], memory_order_relaxed);
        if (owner && owner == current)
            atomic_fetch_add_explicit(&s_codec_cpu.running[role][core], 1U, memory_order_relaxed);
    }
}

static void codec_cpu_tick_init(void)
{
    if (atomic_load(&s_codec_tick_ready)) return;
    esp_err_t err = esp_register_freertos_tick_hook_for_cpu(codec_cpu_tick, 0);
    if (err == ESP_OK) {
        err = esp_register_freertos_tick_hook_for_cpu(codec_cpu_tick, 1);
        if (err != ESP_OK)
            esp_deregister_freertos_tick_hook_for_cpu(codec_cpu_tick, 0);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Opus CPU sampling unavailable: %s", esp_err_to_name(err));
        return;
    }
    atomic_store(&s_codec_tick_ready, true);
}

static void record_codec_timing(unsigned role, unsigned core, uint32_t elapsed)
{
    if (role >= 2U || core >= 2U) return;
    uint32_t frames = atomic_load_explicit(&s_codec_diag[role][core].frames,
                                          memory_order_relaxed);
    if (frames == UINT32_MAX) return;
    s_codec_diag[role][core].total_us += elapsed;
    ++frames;
    atomic_store_explicit(&s_codec_diag[role][core].avg_us,
        (uint32_t)(s_codec_diag[role][core].total_us / frames), memory_order_relaxed);
    atomic_store_explicit(&s_codec_diag[role][core].frames, frames, memory_order_release);
}
#endif

/* Room-only observation, no pacing/gain/gate changes. Counters are lifetime
 * values. TX/RX each own their last-* fields; runtime owns last_log_ms. No
 * allocation, payload logging, SDK query or serial output on the hot paths. */
EXT_RAM_BSS_ATTR static struct {
    atomic_uint_fast32_t tx_enqueued, tx_full, tx_stale, tx_encode_error, tx_send_error;
    atomic_uint_fast32_t tx_pairs, tx_burst, tx_gaps, tx_gap_max_us, tx_accepted, tx_gain_error;
    atomic_uint_fast32_t ptt_down, ptt_up, ptt_drained, capture_stale;
    atomic_uint_fast32_t rx_frames, rx_bytes, rx_pairs, rx_gap_max_us, rx_burst, rx_gaps;
    atomic_uint_fast32_t rx_repeat_ts, rx_forward_gap, rx_back_ts;
    atomic_uint rx_observer_busy;
    atomic_uint_fast32_t rx_observer_skipped;
    uint32_t tx_last_us, tx_generation, tx_epoch;
    uint32_t rx_last_us, rx_last_timestamp, rx_last_len, rx_generation, last_log_ms;
} s_room_audio_diag;
_Static_assert(sizeof(s_room_audio_diag) == 136U, "Room diagnostics PSRAM budget changed");

typedef enum {
    OUTPUT_OWNER_NONE, OUTPUT_OWNER_RX, OUTPUT_OWNER_VOLUME, OUTPUT_OWNER_MUTE,
    OUTPUT_OWNER_LCD, OUTPUT_OWNER_PROMPT, OUTPUT_OWNER_START, OUTPUT_OWNER_STOP,
} audio_output_owner_t;
static atomic_uint_fast32_t s_aec_processed;
/* [DEBUG-wake49] Capture-owned accumulator, fixed-size published snapshot. */
static starter_media_audio_diagnostics_t s_audio_diag_acc, s_audio_diag;
static portMUX_TYPE s_audio_diag_lock = portMUX_INITIALIZER_UNLOCKED;

static void update_audio_diagnostics(const starter_aec_output_t *frame,
                                     uint32_t aec_us, uint32_t captured_ms)
{
    for (unsigned i = 0; i < 3; ++i) {
        s_audio_diag_acc.rms_avg[i] += frame->level[i].rms;
        s_audio_diag_acc.dc_avg[i] += frame->level[i].dc;
        if (frame->level[i].rms > s_audio_diag_acc.rms_max[i])
            s_audio_diag_acc.rms_max[i] = frame->level[i].rms;
        if (frame->level[i].peak > s_audio_diag_acc.peak[i])
            s_audio_diag_acc.peak[i] = frame->level[i].peak;
    }
    s_audio_diag_acc.aec_avg_us += aec_us;
    s_audio_diag_acc.measurement_avg_us += frame->measurement_us;
    if (frame->measurement_us > s_audio_diag_acc.measurement_max_us)
        s_audio_diag_acc.measurement_max_us = frame->measurement_us;
    if (++s_audio_diag_acc.frames < 32U) return;
    for (unsigned i = 0; i < 3; ++i) {
        s_audio_diag_acc.rms_avg[i] /= 32U;
        s_audio_diag_acc.dc_avg[i] /= 32;
    }
    s_audio_diag_acc.aec_avg_us /= 32U;
    s_audio_diag_acc.measurement_avg_us /= 32U;
    s_audio_diag_acc.at_ms = captured_ms;
    portENTER_CRITICAL(&s_audio_diag_lock);
    s_audio_diag_acc.window = s_audio_diag.window + 1U;
    s_audio_diag = s_audio_diag_acc;
    portEXIT_CRITICAL(&s_audio_diag_lock);
    memset(&s_audio_diag_acc, 0, sizeof(s_audio_diag_acc));
}

void starter_media_audio_diagnostics(starter_media_audio_diagnostics_t *out)
{
    if (out == NULL) return;
    portENTER_CRITICAL(&s_audio_diag_lock);
    *out = s_audio_diag;
    portEXIT_CRITICAL(&s_audio_diag_lock);
}
static atomic_uint_fast32_t s_aec_errors;
static atomic_uint_fast32_t s_aec_mic_clipped;
static atomic_uint_fast32_t s_aec_reference_clipped;
static atomic_uint_fast32_t s_aec_max_process_us;
static atomic_uint_fast32_t s_aec_deadline_misses;
static atomic_uchar s_audio_activity_level;
static atomic_bool s_voice_active;
static atomic_uchar s_speaker_volume = 7;
static atomic_bool s_speaker_muted;
static atomic_bool s_microphone_muted;
static atomic_uint s_room_ptt_generation;
/* PTT release closes Room admission at its capture time. Frames captured up to
 * the release still drain for a bounded time; release_ms is esp_timer ms, low
 * 32 bits, published before the generation. */
#define ROOM_PTT_DRAIN_MS 600U
static atomic_uint s_room_ptt_drain_generation;
static atomic_uint s_room_ptt_release_ms;
/* 单调序号避免“清除取消标志”与新铃声启动之间的竞态。 */
static atomic_uint_fast32_t s_pcm8k_cancel_sequence;

static audio_rx_item_t *s_audio_rx_pool;
static audio_tx_item_t *s_audio_tx_pool;
static QueueHandle_t s_audio_rx_ready_queue;
static QueueHandle_t s_audio_rx_free_queue;
static QueueHandle_t s_audio_tx_ready_queue;
static QueueHandle_t s_audio_tx_free_queue;

#if CONFIG_XIAOTAI_DEVELOPMENT_CONSOLE
static void rx_trace_event(starter_rx_trace_result_t result, uint64_t received_us,
                           starter_tirtc_mode_t mode, uint32_t generation,
                           uint32_t epoch, const starter_tirtc_frame_t *frame)
{
    if (!atomic_load_explicit(&s_rx_trace.active, memory_order_relaxed)) return;
    uint32_t begin = (uint32_t)esp_timer_get_time();
    starter_rx_trace_entry_t entry = {
        .at_us = (uint32_t)received_us, .timestamp_ms = frame->timestamp_ms,
        .generation = generation, .epoch = epoch, .result = result,
        .format = ((uint32_t)mode << 16) | ((uint32_t)frame->media << 8) | frame->stream_id,
        .length = frame->length,
        .queues = uxQueueMessagesWaiting(s_audio_rx_free_queue) |
                  (uxQueueMessagesWaiting(s_audio_rx_ready_queue) << 8),
        .queued_ms = atomic_load_explicit(&s_playout_diag.queued_ms, memory_order_relaxed),
        .pending_samples = atomic_load_explicit(&s_playout_diag.pending_samples, memory_order_relaxed),
        .decoded = atomic_load_explicit(&s_audio_decoded, memory_order_relaxed),
        .played = atomic_load_explicit(&s_audio_played, memory_order_relaxed),
        .sink_stage = atomic_load_explicit(&s_rx_trace_sink.stage, memory_order_relaxed),
        .sink_at_us = atomic_load_explicit(&s_rx_trace_sink.at_us, memory_order_relaxed),
        .owner = atomic_load_explicit(&s_playout_diag.owner, memory_order_relaxed),
        .buffering_count = atomic_load_explicit(&s_rx_trace_sink.buffering_count, memory_order_relaxed),
    };
    (void)starter_rx_trace_record(&s_rx_trace, &entry);
    uint32_t elapsed = (uint32_t)esp_timer_get_time() - begin;
    uint32_t prior = atomic_load(&s_rx_trace.max_us);
    if (elapsed > prior)
        (void)atomic_compare_exchange_strong(&s_rx_trace.max_us, &prior, elapsed);
}
#else
#define rx_trace_stage(stage, count, buffering) ((void)0)
#define rx_trace_event(result, received, mode, generation, epoch, frame) ((void)0)
#endif
static SemaphoreHandle_t s_audio_output_mutex;
static i2s_chan_handle_t s_i2s_rx;
static i2s_chan_handle_t s_i2s_tx;
static bool s_amp_enabled;
/* Codec devices own the shared duplex I2S configuration. */
static const audio_codec_data_if_t *s_i2s_data_if;
static const audio_codec_ctrl_if_t *s_es7210_ctrl_if;
static const audio_codec_ctrl_if_t *s_amp_ctrl_if;
static const audio_codec_if_t *s_es7210_codec_if;
static const audio_codec_if_t *s_amp_codec_if;
static esp_codec_dev_handle_t s_microphone_dev;
static esp_codec_dev_handle_t s_speaker_dev;
static i2c_master_bus_handle_t s_i2c_bus;
static void *s_opus_encoder;
static void *s_opus_decoder;
static void *s_g711_encoder;
static void *s_g711_decoder;

/* Codec scratch stays in PSRAM. H5 may carry up to AUDIO_RX_BYTES A-law
 * samples, which expand twofold when normalized to the 16 kHz playout clock. */
EXT_RAM_BSS_ATTR static int16_t s_decode_h5_pcm[AUDIO_RX_BYTES];
EXT_RAM_BSS_ATTR static int16_t s_decode_pcm[AUDIO_DECODE_PCM_CAPACITY];
EXT_RAM_BSS_ATTR static int16_t s_encode_h5_pcm[AUDIO_H5_PACKET_SAMPLES];
EXT_RAM_BSS_ATTR static uint8_t s_encode_alaw[AUDIO_H5_PACKET_SAMPLES];
EXT_RAM_BSS_ATTR static uint8_t s_encode_opus[AUDIO_ENCODED_MAX_BYTES];
EXT_RAM_BSS_ATTR static starter_playout_pcm_t s_rx_pcm;
_Static_assert(sizeof(s_rx_pcm) == 970U, "S3 PCM staging PSRAM budget changed");
EXT_RAM_BSS_ATTR static struct {
    int16_t samples[STARTER_PLAYOUT_PCM_SAMPLES];
    uint16_t fade_remaining;
} s_rx_render;
_Static_assert(sizeof(s_rx_render) == 642U, "S3 render scratch PSRAM budget changed");
/* AW88298 consumes interleaved 16-bit stereo PCM. */
EXT_RAM_BSS_ATTR static int16_t
    s_play_stereo[AUDIO_RX_BYTES * AUDIO_PCM8K_I2S_VALUES_PER_INPUT];

static void update_max_counter(atomic_uint_fast32_t *counter, uint32_t value)
{
    uint_fast32_t current = atomic_load_explicit(counter, memory_order_relaxed);
    while (current < value &&
           !atomic_compare_exchange_weak_explicit(counter,
                                                  &current,
                                                  value,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {
    }
}

static bool take_audio_output_lock(audio_output_owner_t owner, TickType_t timeout)
{
    uint32_t waiting_on = atomic_load(&s_playout_diag.owner);
    int64_t begin = esp_timer_get_time();
    bool locked = xSemaphoreTake(s_audio_output_mutex, timeout) == pdTRUE;
    if (owner == OUTPUT_OWNER_RX) {
        uint32_t wait_us = (uint32_t)(esp_timer_get_time() - begin);
        update_max_counter(&s_playout_diag.lock_max_us, wait_us);
        if (wait_us >= 20000U)
            atomic_store(&s_playout_diag.last_lock_owner, waiting_on);
        if (!locked) {
            atomic_fetch_add(&s_playout_diag.lock_timeouts, 1);
            atomic_store(&s_playout_diag.last_lock_owner, atomic_load(&s_playout_diag.owner));
            atomic_fetch_add(&s_audio_dropped, 1);
        }
    }
    if (locked) atomic_store(&s_playout_diag.owner, owner);
    return locked;
}

static void give_audio_output_lock(void)
{
    atomic_store(&s_playout_diag.owner, OUTPUT_OWNER_NONE);
    xSemaphoreGive(s_audio_output_mutex);
}

static bool same_session(starter_tirtc_mode_t mode, uint32_t generation)
{
    return atomic_load_explicit(&s_active, memory_order_acquire) &&
           atomic_load_explicit(&s_mode, memory_order_acquire) == (int)mode &&
           atomic_load_explicit(&s_generation, memory_order_acquire) == generation;
}

static uint32_t audio_samples_to_us(size_t samples)
{
    return (uint32_t)(((uint64_t)samples * 1000000U +
                       AUDIO_TRANSPORT_SAMPLE_RATE_HZ - 1U) /
                      AUDIO_TRANSPORT_SAMPLE_RATE_HZ);
}

static bool uplink_session_current(starter_tirtc_mode_t mode, uint32_t generation)
{
    /* EOS retains speaker ownership only, never microphone/network admission. */
    return same_session(mode, generation) && atomic_load(&s_drain_generation) == 0 &&
           (mode != STARTER_TIRTC_ROOM || atomic_load(&s_room_ptt_generation) == generation);
}

/* True while the released Room generation may still send frames captured at
 * or before *release_ms. Press, stop and generation changes end it; mute and
 * press also advance the epoch, so older frames can never cross either edge. */
static bool room_ptt_drain_open(starter_tirtc_mode_t mode, uint32_t generation,
                                uint32_t *release_ms)
{
    if (mode != STARTER_TIRTC_ROOM || generation == 0 ||
        atomic_load(&s_room_ptt_drain_generation) != generation ||
        !same_session(mode, generation) || atomic_load(&s_drain_generation) != 0) return false;
    uint32_t release = atomic_load(&s_room_ptt_release_ms);
    if ((uint32_t)(esp_timer_get_time() / 1000) - release > ROOM_PTT_DRAIN_MS) return false;
    if (release_ms != NULL) *release_ms = release;
    return true;
}

/* Admission by capture time: live sessions, or Room frames captured before
 * the PTT release while its bounded drain is open. */
static bool uplink_frame_admitted(starter_tirtc_mode_t mode, uint32_t generation,
                                  uint32_t captured_ms)
{
    uint32_t release_ms = 0;
    return uplink_session_current(mode, generation) ||
           (room_ptt_drain_open(mode, generation, &release_ms) &&
            (int32_t)(captured_ms - release_ms) <= 0);
}

bool starter_media_room_ptt(uint32_t generation, bool pressed)
{
    if (!pressed) {
        if (generation == 0) {
            /* Teardown/reset: no drain, invalidate everything in flight. */
            atomic_store(&s_room_ptt_drain_generation, 0);
            atomic_store(&s_room_ptt_generation, 0);
            atomic_fetch_add(&s_room_audio_diag.ptt_up, 1);
            atomic_fetch_add(&s_mute_epoch, 1);
            return true;
        }
        unsigned expected = generation;
        if (atomic_compare_exchange_strong(&s_room_ptt_generation, &expected, 0)) {
            /* Speech captured before release keeps flowing; later frames don't. */
            atomic_store(&s_room_ptt_release_ms, (uint32_t)(esp_timer_get_time() / 1000));
            atomic_store(&s_room_ptt_drain_generation, generation);
            atomic_fetch_add(&s_room_audio_diag.ptt_up, 1);
        }
        return true;
    }
    if (!generation || !same_session(STARTER_TIRTC_ROOM, generation) ||
        atomic_load(&s_microphone_muted)) return false;
    atomic_fetch_add(&s_room_audio_diag.ptt_down, 1);
    /* A new talk spurt ends any drain; old frames never cross into it. */
    atomic_store(&s_room_ptt_drain_generation, 0);
    atomic_fetch_add(&s_mute_epoch, 1);
    atomic_store(&s_room_ptt_generation, generation);
    return true;
}

static esp_err_t board_i2c_init(void)
{
    if (s_i2c_bus != NULL) {
        return ESP_OK;
    }
    const i2c_master_bus_config_t config = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&config, &s_i2c_bus);
    if (err != ESP_OK) {
        return err;
    }

    return starter_board_init(s_i2c_bus);
}

static esp_err_t audio_i2s_init(void)
{
    /* ES7210 puts the two microphones and electrical speaker reference on
     * one TDM-I2S output; the paired speaker remains standard I2S. */
    i2s_chan_config_t audio_channel =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_AUDIO_PORT, I2S_ROLE_MASTER);
    audio_channel.auto_clear_after_cb = true;
    audio_channel.dma_frame_num = STARTER_AUDIO_DMA_FRAME_COUNT;
    audio_channel.dma_desc_num = STARTER_AUDIO_DMA_DESCRIPTOR_COUNT;
    esp_err_t err = i2s_new_channel(&audio_channel, &s_i2s_tx, &s_i2s_rx);
    if (err != ESP_OK) {
        return err;
    }

    i2s_std_config_t tx_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_HW_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK,
            .bclk = I2S_BCLK,
            .ws = I2S_WS,
            .dout = I2S_DAC_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    tx_config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    err = i2s_channel_init_std_mode(s_i2s_tx, &tx_config);
    if (err != ESP_OK) {
        return err;
    }

    i2s_tdm_config_t rx_config = {
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(AUDIO_HW_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
            I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
        .gpio_cfg = {
            .mclk = I2S_MCLK,
            .bclk = I2S_BCLK,
            .ws = I2S_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = I2S_ADC_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    rx_config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    err = i2s_channel_init_tdm_mode(s_i2s_rx, &rx_config);
    if (err != ESP_OK) {
        return err;
    }
    return starter_board_audio_observe(s_i2s_rx);
}

static esp_err_t audio_i2s_start(void)
{
    /* esp_codec_dev opens both directions as one duplex transaction.  Calling
     * i2s_channel_enable here would bypass its paired-channel state machine. */
    return ESP_OK;
}

static esp_err_t audio_amplifier_clock_init(void)
{
    if (s_amp_codec_if == NULL || s_amp_codec_if->get_reg == NULL ||
        s_amp_codec_if->set_reg == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Four ADC slots share BCLK with two widened DAC slots: 64 bits per frame.
     * The codec's PCM-width setter selects 32fs, so change only I2SBCK after
     * both directions have opened. PCM samples remain 16-bit. */
    const int i2s_control = 0x06;
    const int bit_clock_mask = 0x30;
    const int bit_clock_64fs = 0x20;
    int before = 0;
    if (s_amp_codec_if->get_reg(s_amp_codec_if, i2s_control, &before) != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    int expected = (before & ~bit_clock_mask) | bit_clock_64fs;
    if (s_amp_codec_if->set_reg(s_amp_codec_if, i2s_control, expected) != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    int after = 0;
    if (s_amp_codec_if->get_reg(s_amp_codec_if, i2s_control, &after) != ESP_CODEC_DEV_OK ||
        after != expected) {
        ESP_LOGE(TAG, "amplifier shared clock readback failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "amplifier shared clock: BCLK=64fs reg06=%04x->%04x", before, after);
    return ESP_OK;
}

static esp_err_t audio_codecs_init(void)
{
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_AUDIO_PORT,
        .rx_handle = s_i2s_rx,
        .tx_handle = s_i2s_tx,
    };
    s_i2s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (s_i2s_data_if == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Codec addresses use the library's 8-bit wire representation. */
    audio_codec_i2c_cfg_t amp_i2c = {
        .port = BOARD_I2C_PORT,
        .addr = AW88298_CODEC_DEFAULT_ADDR,
        .bus_handle = s_i2c_bus,
        .clock_speed_hz = BOARD_I2C_HZ,
    };
    s_amp_ctrl_if = audio_codec_new_i2c_ctrl(&amp_i2c);
    if (s_amp_ctrl_if == NULL) {
        return ESP_ERR_NO_MEM;
    }
    aw88298_codec_cfg_t amp_cfg = {
        .ctrl_if = s_amp_ctrl_if,
        .reset_pin = -1,
    };
    s_amp_codec_if = aw88298_codec_new(&amp_cfg);
    if (s_amp_codec_if == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_codec_dev_cfg_t speaker_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = s_amp_codec_if,
        .data_if = s_i2s_data_if,
    };
    s_speaker_dev = esp_codec_dev_new(&speaker_cfg);
    if (s_speaker_dev == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_codec_dev_sample_info_t speaker_format = {
        .sample_rate = AUDIO_HW_SAMPLE_RATE_HZ,
        .bits_per_sample = 16,
        .channel = 2,
    };
    if (esp_codec_dev_open(s_speaker_dev, &speaker_format) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_set_out_vol(s_speaker_dev, 70) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_set_out_mute(s_speaker_dev, true) != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }

    audio_codec_i2c_cfg_t es7210_i2c = {
        .port = BOARD_I2C_PORT,
        .addr = ES7210_CODEC_DEFAULT_ADDR,
        .bus_handle = s_i2c_bus,
        .clock_speed_hz = BOARD_I2C_HZ,
    };
    s_es7210_ctrl_if = audio_codec_new_i2c_ctrl(&es7210_i2c);
    if (s_es7210_ctrl_if == NULL) {
        return ESP_ERR_NO_MEM;
    }
    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if = s_es7210_ctrl_if,
        .master_mode = false,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3,
        .mclk_div = AUDIO_MCLK_MULTIPLE,
    };
    s_es7210_codec_if = es7210_codec_new(&es7210_cfg);
    if (s_es7210_codec_if == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_codec_dev_cfg_t microphone_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = s_es7210_codec_if,
        .data_if = s_i2s_data_if,
    };
    s_microphone_dev = esp_codec_dev_new(&microphone_cfg);
    if (s_microphone_dev == NULL) {
        return ESP_ERR_NO_MEM;
    }
    /* Keep all wire slots; AFE repacks MIC1 + MIC3 reference to MR, MIC2 is measured only. */
    esp_codec_dev_sample_info_t microphone_format = {
        .sample_rate = AUDIO_HW_SAMPLE_RATE_HZ,
        .bits_per_sample = 16,
        .channel = AUDIO_MIC_CHANNELS,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) |
                        ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1) |
                        ESP_CODEC_DEV_MAKE_CHANNEL_MASK(2) |
                        ESP_CODEC_DEV_MAKE_CHANNEL_MASK(3),
    };
    if (esp_codec_dev_open(s_microphone_dev, &microphone_format) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_set_in_channel_gain(s_microphone_dev,
                                          ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
                                          STARTER_AEC_MIC_PGA_DB) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_set_in_channel_gain(s_microphone_dev,
                                          ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1),
                                          STARTER_AEC_MIC_PGA_DB) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_set_in_channel_gain(s_microphone_dev,
                                          ESP_CODEC_DEV_MAKE_CHANNEL_MASK(2),
                                          0.0f) != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    return audio_amplifier_clock_init();
}

static esp_err_t opus_init(void)
{
    size_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    esp_opus_enc_config_t encoder = ESP_OPUS_ENC_CONFIG_DEFAULT();
    encoder.sample_rate = ESP_AUDIO_SAMPLE_RATE_16K;
    encoder.channel = ESP_AUDIO_MONO;
    encoder.bits_per_sample = ESP_AUDIO_BIT16;
    encoder.bitrate = 24000;
    encoder.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_20_MS;
    /* Single-variable CoreS3 trial: keep the wire contract unchanged while
     * measuring the official low-delay encoder against the VOIP baseline. */
    encoder.application_mode = ESP_OPUS_ENC_APPLICATION_LOWDELAY;
    /* Complexity 0 is Espressif's measured low-CPU profile. The 24 kbps CBR
     * voice contract stays unchanged, while UI/audio contention is reduced. */
    encoder.complexity = AUDIO_OPUS_ENCODER_COMPLEXITY;
    encoder.enable_fec = false;
    encoder.enable_dtx = false;
    encoder.enable_vbr = false;
    if (esp_opus_enc_open(&encoder, sizeof(encoder), &s_opus_encoder) != ESP_AUDIO_ERR_OK) {
        return ESP_FAIL;
    }
    int input_bytes = 0, output_bytes = 0;
    if (esp_opus_enc_get_frame_size(s_opus_encoder, &input_bytes, &output_bytes) !=
            ESP_AUDIO_ERR_OK || input_bytes != (int)(AUDIO_PACKET_SAMPLES * sizeof(int16_t)) ||
        output_bytes <= 0 || output_bytes > AUDIO_ENCODED_MAX_BYTES) {
        esp_opus_enc_close(s_opus_encoder);
        s_opus_encoder = NULL;
        return ESP_ERR_NOT_SUPPORTED;
    }
    esp_opus_dec_cfg_t decoder = ESP_OPUS_DEC_CONFIG_DEFAULT();
    decoder.sample_rate = ESP_AUDIO_SAMPLE_RATE_16K;
    decoder.channel = ESP_AUDIO_MONO;
    decoder.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS;
    decoder.self_delimited = false;
    if (esp_opus_dec_open(&decoder, sizeof(decoder), &s_opus_decoder) != ESP_AUDIO_ERR_OK) {
        esp_opus_enc_close(s_opus_encoder);
        s_opus_encoder = NULL;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG,
             "Opus ready: 16k mono 20ms 24kbps CBR application=%u complexity=%u frame=%d/%d RAM/PSRAM=%u/%u B",
             (unsigned)encoder.application_mode,
             (unsigned)AUDIO_OPUS_ENCODER_COMPLEXITY,
             input_bytes, output_bytes,
             (unsigned)(internal_before - heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             (unsigned)(psram_before - heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    return ESP_OK;
}

esp_err_t starter_media_debug_hardware(void)
{
    if (!s_es7210_ctrl_if) return ESP_ERR_INVALID_STATE;
    starter_aec_log_kernels();
    esp_err_t err = starter_board_debug_power();
    if (err != ESP_OK) return err;
    const uint8_t amp_regs[] = {0x01, 0x04, 0x05, 0x06, 0x07, 0x0c, 0x12, 0x14, 0x61};
    for (size_t i = 0; i < sizeof(amp_regs); ++i) {
        int value = 0;
        if (s_amp_codec_if->get_reg(s_amp_codec_if, amp_regs[i], &value) != ESP_CODEC_DEV_OK)
            return ESP_FAIL;
        printf("+HW:AW88298,%02x=%04x\n", amp_regs[i], value);
    }
    const uint8_t regs[] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
        0x09,0x0a,0x0b,0x0d,0x10,0x11,0x12,0x13,0x14,0x15,0x3d,0x3e,0x3f,
        0x40,0x41,0x42,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,0x4b,0x4c};
    for (size_t i = 0; i < sizeof(regs); ++i) {
        uint8_t value = 0;
        int ret = s_es7210_ctrl_if->read_reg(s_es7210_ctrl_if, regs[i], 1, &value, 1);
        if (ret != ESP_CODEC_DEV_OK) return ESP_FAIL;
        printf("+HW:ES7210,%02x=%02x\n", regs[i], value);
    }
    const gpio_num_t pins[] = {I2S_MCLK, I2S_BCLK, I2S_WS, I2S_ADC_DIN, I2S_DAC_DOUT};
    for (size_t p = 0; p < sizeof(pins) / sizeof(pins[0]); ++p) {
        gpio_io_config_t config;
        err = gpio_get_io_config(pins[p], &config);
        if (err != ESP_OK) return err;
        unsigned highs = 0, changes = 0;
        int previous = gpio_get_level(pins[p]);
        for (unsigned n = 0; n < 1024; ++n) {
            int value = gpio_get_level(pins[p]);
            highs += value;
            changes += value != previous;
            previous = value;
        }
        printf("+HW:PIN,%d,high=%u/1024,changes=%u,ie=%u,oe=%u,periph=%u,out=%lu\n",
               pins[p], highs, changes, config.ie, config.oe,
               config.oe_ctrl_by_periph, (unsigned long)config.sig_out);
    }
    return gpio_dump_io_configuration(stdout, (1ULL << I2S_MCLK) |
        (1ULL << I2S_BCLK) | (1ULL << I2S_WS) | (1ULL << I2S_ADC_DIN) |
        (1ULL << I2S_DAC_DOUT));
}

static esp_err_t g711_init(void)
{
    esp_g711_enc_config_t encoder = ESP_G711_ENC_CONFIG_DEFAULT();
    encoder.sample_rate = ESP_AUDIO_SAMPLE_RATE_8K;
    encoder.channel = ESP_AUDIO_MONO;
    encoder.bits_per_sample = ESP_AUDIO_BIT16;
    encoder.frame_duration = STARTER_H5_AUDIO_PACKET_MS;
    if (esp_g711a_enc_open(&encoder, sizeof(encoder), &s_g711_encoder) !=
        ESP_AUDIO_ERR_OK) {
        return ESP_FAIL;
    }
    if (esp_g711_dec_open(NULL, 0, &s_g711_decoder) != ESP_AUDIO_ERR_OK) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "H5 PCMA ready: 8k mono 20ms");
    return ESP_OK;
}

/* 调用者必须持有 s_audio_output_mutex。 */
static esp_err_t audio_output_set_locked(bool enabled)
{
    if (enabled == s_amp_enabled) {
        return ESP_OK;
    }
    esp_err_t err = esp_codec_dev_set_out_mute(s_speaker_dev, !enabled) ==
                    ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
    if (err != ESP_OK) {
        return err;
    }
    s_amp_enabled = enabled;
    return ESP_OK;
}

static bool enqueue_uplink_pcm(starter_tirtc_mode_t mode,
                               uint32_t generation,
                               uint32_t timestamp_ms,
                               const int16_t *pcm, unsigned mute_epoch,
                               int64_t captured_ms)
{
    if (mute_epoch != atomic_load(&s_mute_epoch) || atomic_load(&s_microphone_muted)) return false;
    uint8_t slot = 0;
    if (pcm == NULL || s_audio_tx_free_queue == NULL ||
        xQueueReceive(s_audio_tx_free_queue, &slot, 0) != pdTRUE) {
        if (mode == STARTER_TIRTC_ROOM) atomic_fetch_add(&s_room_audio_diag.tx_full, 1);
        atomic_fetch_add_explicit(&s_audio_dropped, 1, memory_order_relaxed);
        return false;
    }
    audio_tx_item_t *item = &s_audio_tx_pool[slot];
    item->mode = mode;
    item->generation = generation;
    item->timestamp_ms = timestamp_ms;
    item->enqueued_us = (uint32_t)esp_timer_get_time();
    item->captured_ms = (uint32_t)captured_ms;
    item->mute_epoch = mute_epoch;
    memcpy(item->pcm, pcm, sizeof(item->pcm));
    if (xQueueSend(s_audio_tx_ready_queue, &slot, 0) != pdTRUE) {
        if (mode == STARTER_TIRTC_ROOM) atomic_fetch_add(&s_room_audio_diag.tx_full, 1);
        memset(item, 0, sizeof(*item));
        (void)xQueueSend(s_audio_tx_free_queue, &slot, 0);
        atomic_fetch_add_explicit(&s_audio_dropped, 1, memory_order_relaxed);
        return false;
    }
    update_max_counter(&s_pipeline_diag.tx_queue_peak, uxQueueMessagesWaiting(s_audio_tx_ready_queue));
    if (mode == STARTER_TIRTC_ROOM) atomic_fetch_add(&s_room_audio_diag.tx_enqueued, 1);
    return true;
}

/* Keep TiRTC calls outside the real-time I2S/AEC task, as in device-monitor's
 * rtc_audio_tx worker. The producer only hands off a bounded 20 ms PCM frame. */
static void audio_uplink_task(void *argument)
{
    (void)argument;
    uint32_t codec_generation = 0;
    starter_tirtc_mode_t codec_mode = STARTER_TIRTC_NONE;
    starter_audio_resampler_16k_to_8k_t h5_resampler = {0};
    for (;;) {
        uint8_t slot = 0;
        if (xQueueReceive(s_audio_tx_ready_queue, &slot, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        audio_tx_item_t *item = &s_audio_tx_pool[slot];
        update_max_counter(&s_pipeline_diag.tx_age_max_us,
                           (uint32_t)esp_timer_get_time() - item->enqueued_us);
        bool ready = uplink_frame_admitted(item->mode, item->generation, item->captured_ms) &&
                     item->mute_epoch == atomic_load(&s_mute_epoch) &&
                     starter_tirtc_audio_ready() &&
                     !atomic_load_explicit(&s_microphone_muted, memory_order_acquire);
        if (!ready && item->mode == STARTER_TIRTC_ROOM)
            atomic_fetch_add(&s_room_audio_diag.tx_stale, 1);
        if (ready && (codec_generation != item->generation ||
                      codec_mode != item->mode)) {
            if (STARTER_TIRTC_AUDIO_IS_ALAW(item->mode)) {
                starter_audio_resampler_16k_to_8k_reset(&h5_resampler);
            } else if (esp_opus_enc_reset(s_opus_encoder) != ESP_AUDIO_ERR_OK) {
                ESP_LOGE(TAG, "Opus encoder reset failed generation=%lu",
                         (unsigned long)item->generation);
                ready = false;
            }
            if (ready) {
                codec_generation = item->generation;
                codec_mode = item->mode;
            }
        }
#if CONFIG_XIAOTAI_CAPTURE_AGC
        if (ready) {
            /* The TX worker exclusively owns this slot. Both live and preroll
             * packets receive gain exactly once; wake retains the 16 kHz path. */
            int64_t gain_begin = esp_timer_get_time();
            esp_err_t gain_err = starter_agc_boost_uplink(item->pcm, AUDIO_PACKET_SAMPLES);
            update_max_counter(&s_pipeline_diag.tx_gain_max_us,
                               (uint32_t)(esp_timer_get_time() - gain_begin));
            if (gain_err != ESP_OK) {
                if (item->mode == STARTER_TIRTC_ROOM)
                    atomic_fetch_add(&s_room_audio_diag.tx_gain_error, 1);
                atomic_fetch_add(&s_audio_dropped, 1);
                ESP_LOGE(TAG, "TX gain failed: %s", esp_err_to_name(gain_err));
                ready = false;
            }
        }
#endif
        if (ready) {
            int64_t measure_begin = esp_timer_get_time();
            starter_signal_level_t level = starter_signal_measure(item->pcm, AUDIO_PACKET_SAMPLES);
            uint32_t clipped = 0;
            for (size_t i = 0; i < AUDIO_PACKET_SAMPLES; ++i)
                if (item->pcm[i] >= 32700 || item->pcm[i] <= -32700) ++clipped;
            atomic_store(&s_pipeline_diag.tx_peak, level.peak);
            atomic_store(&s_pipeline_diag.tx_rms, level.rms);
            atomic_fetch_add(&s_pipeline_diag.tx_clipped, clipped);
            atomic_fetch_add(&s_pipeline_diag.tx_frames, 1);
            update_max_counter(&s_pipeline_diag.tx_measure_max_us,
                               (uint32_t)(esp_timer_get_time() - measure_begin));
            esp_audio_enc_in_frame_t input = {0};
            esp_audio_enc_out_frame_t output = {0};
            int64_t encode_begin = esp_timer_get_time();
            esp_audio_err_t encoded;
            bool size_ok;
            const char *codec_name;
            if (STARTER_TIRTC_AUDIO_IS_ALAW(item->mode)) {
                size_t h5_samples = starter_audio_resampler_16k_to_8k_process(
                    &h5_resampler, item->pcm, AUDIO_PACKET_SAMPLES,
                    s_encode_h5_pcm, AUDIO_H5_PACKET_SAMPLES);
                if (h5_samples != AUDIO_H5_PACKET_SAMPLES) {
                    atomic_fetch_add(&s_audio_dropped, 1);
                    ESP_LOGE(TAG, "PCMA 16k-to-8k resample failed samples=%u",
                             (unsigned)h5_samples);
                    goto release_tx_slot;
                }
                input.buffer = (uint8_t *)s_encode_h5_pcm;
                input.len = h5_samples * sizeof(int16_t);
                output.buffer = s_encode_alaw;
                output.len = sizeof(s_encode_alaw);
                encoded = esp_g711_enc_process(s_g711_encoder, &input, &output);
                size_ok = output.encoded_bytes == AUDIO_H5_PACKET_SAMPLES;
                codec_name = STARTER_H5_AUDIO_CODEC_NAME;
            } else {
                input.buffer = (uint8_t *)item->pcm;
                input.len = sizeof(item->pcm);
                output.buffer = s_encode_opus;
                output.len = sizeof(s_encode_opus);
#if CONFIG_XIAOTAI_DEVELOPMENT_CONSOLE
                atomic_store_explicit(&s_codec_cpu.owner[0],
                    (uintptr_t)xTaskGetCurrentTaskHandle(), memory_order_relaxed);
#endif
                encoded = esp_opus_enc_process(s_opus_encoder, &input, &output);
#if CONFIG_XIAOTAI_DEVELOPMENT_CONSOLE
                atomic_store_explicit(&s_codec_cpu.owner[0], 0, memory_order_relaxed);
#endif
                size_ok = output.encoded_bytes > 0 &&
                          output.encoded_bytes <= sizeof(s_encode_opus);
                codec_name = STARTER_TIRTC_AUDIO_CODEC_NAME;
            }
            uint32_t encode_us = (uint32_t)(esp_timer_get_time() - encode_begin);
            update_max_counter(&s_pipeline_diag.encode_max_us, encode_us);
#if CONFIG_XIAOTAI_DEVELOPMENT_CONSOLE
            if (!STARTER_TIRTC_AUDIO_IS_ALAW(item->mode))
                record_codec_timing(0, (unsigned)xPortGetCoreID(), encode_us);
#endif
            if (encoded == ESP_AUDIO_ERR_OK && size_ok) {
                if (!uplink_frame_admitted(item->mode, item->generation, item->captured_ms) ||
                    item->mute_epoch != atomic_load(&s_mute_epoch)) {
                    if (item->mode == STARTER_TIRTC_ROOM)
                        atomic_fetch_add(&s_room_audio_diag.tx_stale, 1);
                    goto release_tx_slot;
                }
                int64_t send_begin = esp_timer_get_time();
                if (item->mode == STARTER_TIRTC_ROOM) {
                    if (s_room_audio_diag.tx_generation == item->generation &&
                        s_room_audio_diag.tx_epoch == item->mute_epoch) {
                        uint32_t gap = (uint32_t)send_begin - s_room_audio_diag.tx_last_us;
                        atomic_fetch_add(&s_room_audio_diag.tx_pairs, 1);
                        if (gap < 5000U) atomic_fetch_add(&s_room_audio_diag.tx_burst, 1);
                        if (gap > 60000U) atomic_fetch_add(&s_room_audio_diag.tx_gaps, 1);
                        update_max_counter(&s_room_audio_diag.tx_gap_max_us, gap);
                    }
                    s_room_audio_diag.tx_last_us = (uint32_t)send_begin;
                    s_room_audio_diag.tx_generation = item->generation;
                    s_room_audio_diag.tx_epoch = item->mute_epoch;
                }
                int ret = starter_tirtc_send_audio(item->timestamp_ms,
                                                   output.buffer,
                                                   output.encoded_bytes);
                update_max_counter(&s_pipeline_diag.send_max_us,
                                   (uint32_t)(esp_timer_get_time() - send_begin));
                if (ret >= 0) {
                    if (item->mode == STARTER_TIRTC_ROOM)
                        atomic_fetch_add(&s_room_audio_diag.tx_accepted, 1);
                    uint32_t sent = (uint32_t)atomic_fetch_add_explicit(
                        &s_audio_sent, 1, memory_order_relaxed) + 1U;
                    if (sent == 1U) {
                        ESP_LOGI(TAG,
                                 "uplink first packet mode=%d codec=%s bytes=%u timestamp=%lu encode_us=%lu stack_hwm=%u core=%d",
                                 (int)item->mode,
                                 codec_name,
                                 (unsigned)output.encoded_bytes,
                                 (unsigned long)item->timestamp_ms,
                                 (unsigned long)encode_us,
                                 (unsigned)uxTaskGetStackHighWaterMark(NULL), xPortGetCoreID());
                    }
                } else {
                    if (item->mode == STARTER_TIRTC_ROOM)
                        atomic_fetch_add(&s_room_audio_diag.tx_send_error, 1);
                    ESP_LOGW(TAG, "uplink send failed mode=%d bytes=%u ret=%d",
                             (int)item->mode, (unsigned)output.encoded_bytes, ret);
                }
            } else {
                if (item->mode == STARTER_TIRTC_ROOM)
                    atomic_fetch_add(&s_room_audio_diag.tx_encode_error, 1);
                ESP_LOGW(TAG, "uplink %s encode failed ret=%d bytes=%u",
                         codec_name, (int)encoded, (unsigned)output.encoded_bytes);
            }
        }
release_tx_slot:
        memset(item, 0, sizeof(*item));
        (void)xQueueSend(s_audio_tx_free_queue, &slot, 0);
        /* A queued burst must not let the unpinned codec worker continuously
         * outrank LVGL on CPU0. One millisecond per 20 ms packet preserves
         * audio headroom and gives the UI task a deterministic run chance. */
        vTaskDelay(pdMS_TO_TICKS(MEDIA_CPU_YIELD_MS));
    }
}

static void audio_input_task(void *argument)
{
    (void)argument;
    int64_t last_read_begin = 0;
    for (;;) {
        uint32_t capture_epoch = atomic_load(&s_capture_epoch);
        uint32_t mute_epoch = atomic_load(&s_mute_epoch);
        int16_t *capture = starter_aec_capture_buffer();
        size_t capture_bytes = starter_aec_capture_bytes();
        int64_t read_begin = esp_timer_get_time();
        if (last_read_begin != 0)
            update_max_counter(&s_pipeline_diag.read_gap_max_us, (uint32_t)(read_begin - last_read_begin));
        last_read_begin = read_begin;
        bool read_ok = s_microphone_dev &&
            starter_board_capture(capture, capture_bytes) == ESP_OK;
        int64_t read_end = esp_timer_get_time();
        update_max_counter(&s_pipeline_diag.read_max_us, (uint32_t)(read_end - read_begin));
        esp_err_t err = ESP_FAIL;
        if (read_ok) {
            atomic_fetch_add(&s_pipeline_diag.read_frames, 1);
            err = starter_aec_submit_capture(capture_bytes, read_end / 1000,
                                             capture_epoch, mute_epoch);
        } else {
            atomic_fetch_add(&s_pipeline_diag.read_errors, 1);
            starter_aec_report_capture_loss();
        }
        if (err != ESP_OK) {
            /* Lost input travels in-band with the next frame; the capture
             * task marks the preroll gap at that position, not here. */
            atomic_fetch_add_explicit(&s_aec_errors, 1, memory_order_relaxed);
            vTaskDelay(pdMS_TO_TICKS(MEDIA_CPU_YIELD_MS));
        }
    }
}

/* A capture gap ends the partial packet. Once the timeline has started, the
 * next packet timestamp moves past the dropped and lost samples, so receivers
 * see the discontinuity instead of a silent splice. */
static void uplink_timeline_skip(size_t *packet_samples, uint32_t *next_timestamp_ms,
                                 uint32_t lost_samples)
{
    if (lost_samples == 0) return;
    if (*next_timestamp_ms != 0U)
        *next_timestamp_ms += (uint32_t)(((uint64_t)*packet_samples + lost_samples) * 1000U /
                                         AUDIO_TRANSPORT_SAMPLE_RATE_HZ);
    *packet_samples = 0;
}

static void audio_capture_task(void *argument)
{
    (void)argument;
    int16_t packet_pcm[AUDIO_PACKET_SAMPLES];
    size_t packet_samples = 0;
    uint32_t packet_generation = 0;
    uint32_t next_timestamp_ms = 0;
    uint32_t smoothed_energy = 0;
    uint8_t candidate_level = 0;
    int64_t candidate_since_ms = 0;
    unsigned mute_epoch = 0;
    uint32_t uplink_gap_samples = 0; /* Lost or unsent since the last packetized frame. */

    for (;;) {
        starter_tirtc_mode_t mode = (starter_tirtc_mode_t)atomic_load_explicit(
            &s_mode, memory_order_acquire);
        uint32_t generation = (uint32_t)atomic_load_explicit(
            &s_generation, memory_order_acquire);
        unsigned current_mute_epoch = atomic_load(&s_mute_epoch);
        unsigned capture_epoch = atomic_load(&s_capture_epoch);
        if (current_mute_epoch != mute_epoch) {
            starter_agc_discard_pending();
            packet_samples = 0;
            next_timestamp_ms = 0;
            mute_epoch = current_mute_epoch;
        }
        /* A PTT drain keeps the timeline; its frames are gated by capture time. */
        bool session_ready = (uplink_session_current(mode, generation) ||
                              room_ptt_drain_open(mode, generation, NULL)) &&
                             starter_tirtc_audio_ready();
        if (!session_ready) {
            packet_samples = 0;
            packet_generation = 0;
            next_timestamp_ms = 0;
        }
        if (session_ready && packet_generation != generation) {
            starter_agc_discard_pending();
            packet_samples = 0;
            packet_generation = generation;
            next_timestamp_ms = 0;
        }
        /* No timeline yet: a gap has nothing to move. */
        if (next_timestamp_ms == 0U && packet_samples == 0) uplink_gap_samples = 0;

        starter_aec_output_t clean = {0};
        esp_err_t aec_err = starter_aec_fetch(&clean);
        update_max_counter(&s_pipeline_diag.fetch_wait_max_us, clean.fetch_wait_us);
        update_max_counter(&s_pipeline_diag.agc_max_us, clean.agc_us);
        const int64_t captured_ms = clean.captured_ms;
        /* feed works on a larger input chunk than fetch. Normalize the DSP
         * duration to one output frame, rather than counting the blocking
         * wait for fresh audio as CPU processing time. */
        uint32_t aec_elapsed_us = clean.feed_us * clean.samples_16k *
            STARTER_AEC_CAPTURE_DMA_CHANNELS * sizeof(int16_t) / starter_aec_capture_bytes();
        update_max_counter(&s_aec_max_process_us, aec_elapsed_us);
        uint32_t aec_deadline_us = (uint32_t)(
            starter_aec_frame_samples() * 1000000U / AUDIO_HW_SAMPLE_RATE_HZ);
        if (aec_elapsed_us > aec_deadline_us) {
            atomic_fetch_add_explicit(&s_aec_deadline_misses,
                                      1,
                                      memory_order_relaxed);
        }
        /* Even during backlog, leave a scheduling opportunity to other work. */
        vTaskDelay(pdMS_TO_TICKS(MEDIA_CPU_YIELD_MS));
        if (aec_err != ESP_OK) {
            atomic_fetch_add_explicit(&s_aec_errors, 1, memory_order_relaxed);
            atomic_fetch_or(&s_preroll_gap, PREROLL_GAP_FETCH);
            starter_agc_discard_pending();
            continue;
        }
        if (clean.lost_samples != 0) {
            /* Input was lost immediately before this frame: break the preroll
             * here, and move the uplink timeline past it below. */
            atomic_fetch_or(&s_preroll_gap, PREROLL_GAP_CAPTURE);
            uplink_gap_samples += clean.lost_samples;
            /* A listening edge resets the wake window; the per-frame call
             * below re-arms it, so the model never sees spliced audio. */
            starter_voice_set_listening(false);
        }
        update_audio_diagnostics(&clean, aec_elapsed_us, (uint32_t)captured_ms);
        int64_t age_ms = esp_timer_get_time() / 1000 - captured_ms;
        if (age_ms >= 0 && age_ms <= UINT32_MAX)
            update_max_counter(&s_pipeline_diag.afe_age_max_ms, (uint32_t)age_ms);
        atomic_store(&s_pipeline_diag.post_agc_peak, clean.uplink_level.peak);
        atomic_store(&s_pipeline_diag.post_agc_rms, clean.uplink_level.rms);
        if (mute_epoch != atomic_load(&s_mute_epoch) ||
            capture_epoch != atomic_load(&s_capture_epoch) ||
            clean.mute_epoch != mute_epoch || clean.capture_epoch != capture_epoch) {
            if (mode == STARTER_TIRTC_ROOM) atomic_fetch_add(&s_room_audio_diag.capture_stale, 1);
            starter_agc_discard_pending();
            continue;
        }
        atomic_fetch_add_explicit(&s_aec_processed, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&s_aec_mic_clipped,
                                  clean.mic_clipped,
                                  memory_order_relaxed);
        atomic_fetch_add_explicit(&s_aec_reference_clipped,
                                  clean.reference_clipped,
                                  memory_order_relaxed);

        /* A zero-wait take discarded valid PCM when a UI/runtime snapshot
         * briefly held this mutex. Bounded blocking lets priority inheritance
         * release that short owner without breaking the 32 ms audio timeline.
         * No codec, network, playback or logging is allowed under this mutex. */
        bool replay_interrupted = false;
        unsigned invalid_reason = 0;
        uint32_t invalid_token = 0;
        int64_t invalid_step_ms = 0;
        if (xSemaphoreTake(s_preroll_mutex, pdMS_TO_TICKS(PREROLL_CAPTURE_LOCK_WAIT_MS)) == pdTRUE) {
            if (capture_epoch != atomic_load(&s_capture_epoch) ||
                mute_epoch != atomic_load(&s_mute_epoch)) {
                xSemaphoreGive(s_preroll_mutex);
                continue;
            }
            if (s_expected_wake_token == 0 && s_preroll.token != 0 &&
                (s_preroll.failed || captured_ms > s_preroll.expires_ms)) {
                starter_preroll_clear(&s_preroll);
            }
            bool was_failed = s_preroll.failed;
            unsigned gap_reason = atomic_exchange(&s_preroll_gap, 0);
            int64_t previous_end = s_preroll.end_ms;
            if (gap_reason) {
                if (s_preroll.token != 0) s_preroll.failed = true;
                else starter_preroll_clear(&s_preroll);
            }
            bool muted = atomic_load(&s_microphone_muted);
            if (muted) {
                starter_preroll_clear(&s_preroll);
            } else if (s_preroll.token != 0 ||
                       (!atomic_load(&s_active) && atomic_load(&s_wake_allowed))) {
                if (previous_end != 0 && (captured_ms <= previous_end ||
                    captured_ms - previous_end >
                    (int64_t)clean.transport_samples / PREROLL_SAMPLES_PER_MS + 100))
                    gap_reason |= PREROLL_GAP_TIME;
                if (s_preroll.token != 0 &&
                    clean.transport_samples > s_preroll.capacity - s_preroll.count)
                    gap_reason |= PREROLL_GAP_CAPACITY;
                if (s_preroll.token != 0 && s_preroll.generation == 0 &&
                    captured_ms > s_preroll.expires_ms)
                    gap_reason |= PREROLL_GAP_DEADLINE;
                starter_preroll_append(&s_preroll, clean.transport_pcm,
                                       clean.transport_samples, captured_ms);
            }
            if (!was_failed && s_preroll.failed && s_preroll.token != 0) {
                invalid_reason = gap_reason;
                invalid_token = s_preroll.token;
                invalid_step_ms = captured_ms - previous_end;
            }
            if (s_preroll.failed && s_preroll.generation != 0) {
                /* Already admitted: lose damaged backlog, never the AI session.
                 * The current clean frame goes through the normal live path. */
                starter_preroll_clear(&s_preroll);
                s_expected_wake_token = 0;
                packet_samples = 0;
                next_timestamp_ms = 0;
                atomic_fetch_add(&s_audio_dropped, 1);
                replay_interrupted = true;
            }
            xSemaphoreGive(s_preroll_mutex);
        } else {
            atomic_fetch_or(&s_preroll_gap, PREROLL_GAP_LOCK);
        }
        if (invalid_token != 0) {
            ESP_LOGW(TAG, "wake PCM invalid: token=%lu cause=0x%x step=%lld age=%lld ms",
                     (unsigned long)invalid_token, invalid_reason,
                     (long long)invalid_step_ms,
                     (long long)(esp_timer_get_time() / 1000 - captured_ms));
        }
        if (replay_interrupted) {
            ESP_LOGW(TAG, "AI replay interrupted after admission; recovering live audio");
        }

        /* Only copy to the dedicated wake worker; never run TFLite in capture. */
        bool wake_listen = !atomic_load(&s_active) && atomic_load(&s_wake_allowed) &&
                           !atomic_load(&s_microphone_muted);
        starter_voice_set_listening(wake_listen);
        if (wake_listen && starter_voice_ready()) {
            /* The shared AGC preserves every sample with one fixed 10 ms
             * delay. Keep wake age/preroll timing tied to its actual input. */
            (void)starter_voice_feed_pcm16k(clean.wake_pcm_16k, clean.samples_16k,
                                            captured_ms - clean.wake_delay_ms);
        }

        /*
         * 产品 UI 只把 RMS 当作“有声音”提示，绝不据此推断情绪。用 IIR 平滑，
         * 并要求档位保持 300 ms 才发布，避免 20~40 ms 抖动让屏幕反复跳变。
         */
        uint64_t square_sum = 0;
        for (size_t i = 0; i < clean.transport_samples; ++i) {
            int32_t sample = clean.transport_pcm[i];
            square_sum += (uint64_t)((int64_t)sample * sample);
        }
        uint32_t energy = clean.transport_samples == 0U ? 0U :
                          (uint32_t)sqrtf((float)square_sum /
                                          (float)clean.transport_samples);
        smoothed_energy = (smoothed_energy * 3U + energy) / 4U;
        uint8_t measured_level = smoothed_energy < 100U ? 0U :
                                 smoothed_energy < 300U ? 1U :
                                 smoothed_energy < 850U ? 2U : 3U;
        int64_t activity_now_ms = esp_timer_get_time() / 1000;
        if (measured_level != candidate_level) {
            candidate_level = measured_level;
            candidate_since_ms = activity_now_ms;
        } else if (activity_now_ms - candidate_since_ms >= 300) {
            atomic_store_explicit(&s_audio_activity_level,
                                  candidate_level,
                                  memory_order_release);
            atomic_store_explicit(&s_voice_active,
                                  candidate_level >= 2U,
                                  memory_order_release);
        }

        mode = (starter_tirtc_mode_t)atomic_load_explicit(&s_mode,
                                                           memory_order_acquire);
        generation = (uint32_t)atomic_load_explicit(&s_generation,
                                                     memory_order_acquire);
        /* Room PTT release admits only frames captured up to the release. */
        bool live = uplink_session_current(mode, generation);
        session_ready = (live || uplink_frame_admitted(mode, generation, (uint32_t)captured_ms)) &&
                        starter_tirtc_audio_ready() &&
                        !atomic_load_explicit(&s_microphone_muted,
                                              memory_order_acquire);
        if (!session_ready) {
            uplink_gap_samples += (uint32_t)clean.transport_samples;
            continue;
        }
        if (!live) atomic_fetch_add(&s_room_audio_diag.ptt_drained, 1);

        bool replay = false;
        bool replay_completed = false;
        if (mode == STARTER_TIRTC_AI) {
            /* Catch up, then transfer the partial packet exactly once to live. */
            if (xSemaphoreTake(s_preroll_mutex, pdMS_TO_TICKS(PREROLL_CAPTURE_LOCK_WAIT_MS)) != pdTRUE) {
                uplink_gap_samples += (uint32_t)clean.transport_samples;
                continue;
            }
            replay = s_expected_wake_token != 0;
            if (replay) {
                /* The preroll owns continuity until it hands over to live. */
                uplink_gap_samples = 0;
                packet_samples = 0;
                for (unsigned packet = 0; packet < 3U; ++packet) {
                    uint32_t timestamp;
                    if (!starter_preroll_peek(&s_preroll, generation, packet_pcm,
                                              AUDIO_PACKET_SAMPLES, &timestamp) ||
                        !enqueue_uplink_pcm(mode, generation, timestamp, packet_pcm, mute_epoch,
                                            captured_ms)) break;
                    starter_preroll_consume(&s_preroll, AUDIO_PACKET_SAMPLES);
                }
                if (starter_preroll_finish_replay(&s_preroll, generation, packet_pcm,
                                                 AUDIO_PACKET_SAMPLES, &packet_samples,
                                                 &next_timestamp_ms)) {
                    s_expected_wake_token = 0;
                    /* Keep replay=true for this iteration: clean was already
                     * appended above and must not be transmitted a second time. */
                    replay_completed = true;
                }
            }
            xSemaphoreGive(s_preroll_mutex);
        }
        if (replay_completed) {
            ESP_LOGI(TAG, "AI preroll complete; live audio generation=%lu",
                     (unsigned long)generation);
        }
        if (replay) continue;

        uplink_timeline_skip(&packet_samples, &next_timestamp_ms, uplink_gap_samples);
        uplink_gap_samples = 0;
        for (size_t i = 0; i < clean.transport_samples; ++i) {
            packet_pcm[packet_samples++] = clean.transport_pcm[i];
            if (packet_samples != AUDIO_PACKET_SAMPLES) {
                continue;
            }
            packet_samples = 0;
            if (!uplink_frame_admitted(mode, generation, (uint32_t)captured_ms) ||
                !starter_tirtc_audio_ready()) {
                /* Keep the dropped packet's slot on the timeline, not a splice. */
                if (next_timestamp_ms != 0U) next_timestamp_ms += AUDIO_PACKET_MS;
                continue;
            }
            if (next_timestamp_ms == 0U) {
                next_timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
            }
            (void)enqueue_uplink_pcm(mode, generation, next_timestamp_ms, packet_pcm, mute_epoch,
                                     captured_ms);
            next_timestamp_ms += AUDIO_PACKET_MS;
        }
    }
}

typedef struct {
    uint32_t generation;
    uint32_t epoch;
    int16_t previous;
    bool initialized;
} h5_upsampler_t;

static size_t decode_audio_item(const audio_rx_item_t *item,
                                h5_upsampler_t *h5_upsampler)
{
    if (item == NULL || !same_session(item->mode, item->generation) ||
        item->epoch != atomic_load(&s_playout_diag.epoch)) {
        atomic_fetch_add(&s_playout_diag.stale, 1);
        return 0;
    }
    esp_audio_dec_in_raw_t input = {
        .buffer = (uint8_t *)item->payload,
        .len = item->frame.length,
    };
    int64_t begin = esp_timer_get_time();
    esp_audio_err_t result;
    size_t mono_samples = 0;
    const char *codec_name;
    if (STARTER_TIRTC_AUDIO_IS_ALAW(item->mode)) {
        esp_audio_dec_out_frame_t output = {
            .buffer = (uint8_t *)s_decode_h5_pcm,
            .len = sizeof(s_decode_h5_pcm),
        };
        esp_audio_dec_info_t info = {0};
        result = esp_g711a_dec_decode(s_g711_decoder, &input, &output, &info);
        size_t samples_8k = output.decoded_size / sizeof(int16_t);
        if (result != ESP_AUDIO_ERR_OK ||
            output.decoded_size != item->frame.length * sizeof(int16_t) ||
            samples_8k > AUDIO_DECODE_PCM_CAPACITY / 2U) {
            uint32_t failed = (uint32_t)atomic_fetch_add_explicit(
                                  &s_audio_decode_failed, 1,
                                  memory_order_relaxed) + 1U;
            if (failed <= 3U || failed % 100U == 0U) {
                ESP_LOGW(TAG,
                         "downlink PCMA decode failed ret=%d encoded=%u decoded=%u count=%lu",
                         (int)result,
                         (unsigned)item->frame.length,
                         (unsigned)output.decoded_size,
                         (unsigned long)failed);
            }
            return 0;
        }
        if (h5_upsampler == NULL ||
            h5_upsampler->generation != item->generation ||
            h5_upsampler->epoch != item->epoch) {
            if (h5_upsampler == NULL) {
                atomic_fetch_add_explicit(&s_audio_decode_failed, 1,
                                          memory_order_relaxed);
                return 0;
            }
            *h5_upsampler = (h5_upsampler_t){
                .generation = item->generation,
                .epoch = item->epoch,
            };
        }
        for (size_t i = 0; i < samples_8k; ++i) {
            int16_t current = s_decode_h5_pcm[i];
            int16_t previous = h5_upsampler->initialized
                                   ? h5_upsampler->previous
                                   : current;
            s_decode_pcm[i * 2U] =
                (int16_t)(((int32_t)previous + current) / 2);
            s_decode_pcm[i * 2U + 1U] = current;
            h5_upsampler->previous = current;
            h5_upsampler->initialized = true;
        }
        mono_samples = samples_8k * 2U;
        codec_name = STARTER_H5_AUDIO_CODEC_NAME;
    } else {
        esp_audio_dec_out_frame_t output = {
            .buffer = (uint8_t *)s_decode_pcm,
            .len = sizeof(s_decode_pcm),
        };
        esp_audio_dec_info_t info = {0};
#if CONFIG_XIAOTAI_DEVELOPMENT_CONSOLE
        atomic_store_explicit(&s_codec_cpu.owner[1],
            (uintptr_t)xTaskGetCurrentTaskHandle(), memory_order_relaxed);
#endif
        result = esp_opus_dec_decode(s_opus_decoder, &input, &output, &info);
#if CONFIG_XIAOTAI_DEVELOPMENT_CONSOLE
        atomic_store_explicit(&s_codec_cpu.owner[1], 0, memory_order_relaxed);
        record_codec_timing(1, (unsigned)xPortGetCoreID(),
                            (uint32_t)(esp_timer_get_time() - begin));
#endif
        if (result != ESP_AUDIO_ERR_OK ||
            output.decoded_size != AUDIO_PACKET_SAMPLES * sizeof(int16_t) ||
            info.sample_rate != AUDIO_TRANSPORT_SAMPLE_RATE_HZ ||
            info.channel != 1U || info.bits_per_sample != 16U) {
            uint32_t failed = (uint32_t)atomic_fetch_add_explicit(
                                  &s_audio_decode_failed, 1,
                                  memory_order_relaxed) + 1U;
            if (failed <= 3U || failed % 100U == 0U) {
                ESP_LOGW(TAG,
                         "downlink Opus decode failed ret=%d encoded=%u decoded=%u rate=%u channels=%u bits=%u count=%lu",
                         (int)result,
                         (unsigned)item->frame.length,
                         (unsigned)output.decoded_size,
                         (unsigned)info.sample_rate,
                         (unsigned)info.channel,
                         (unsigned)info.bits_per_sample,
                         (unsigned long)failed);
            }
            return 0;
        }
        mono_samples = output.decoded_size / sizeof(int16_t);
        codec_name = STARTER_TIRTC_AUDIO_CODEC_NAME;
    }
    update_max_counter(&s_playout_diag.decode_max_us, (uint32_t)(esp_timer_get_time() - begin));
    uint32_t decoded = (uint32_t)atomic_fetch_add_explicit(
                           &s_audio_decoded, 1, memory_order_relaxed) + 1U;
    atomic_fetch_add(&s_playout_diag.decoded_samples, mono_samples);
    atomic_store(&s_playout_diag.pcm_in_hash,
                 starter_playout_pcm_hash(atomic_load(&s_playout_diag.pcm_in_hash),
                                          s_decode_pcm, mono_samples));
    if (decoded == 1U) {
        ESP_LOGI(TAG, "downlink first %s frame samples=%u stack_hwm=%u core=%d",
                 codec_name,
                 (unsigned)mono_samples,
                 (unsigned)uxTaskGetStackHighWaterMark(NULL), xPortGetCoreID());
    }
    return mono_samples;
}

static bool play_audio_chunk(starter_tirtc_mode_t mode, uint32_t generation,
                              uint32_t epoch, const int16_t *pcm, size_t mono_samples,
                              bool restart)
{
    if (pcm == NULL || mono_samples == 0 || mono_samples > AUDIO_PACKET_SAMPLES ||
        !same_session(mode, generation) || epoch != atomic_load(&s_playout_diag.epoch)) {
        return false;
    }
    (void)restart;
    if (!take_audio_output_lock(OUTPUT_OWNER_RX, pdMS_TO_TICKS(AUDIO_OUTPUT_LOCK_WAIT_MS))) {
        return false;
    }
    bool played = false;
    if (same_session(mode, generation) &&
        epoch == atomic_load(&s_playout_diag.epoch) && s_amp_enabled &&
        !atomic_load_explicit(&s_speaker_muted, memory_order_acquire)) {
        /* This PCM buffer is also used by local prompts. Protect preparation,
         * not just I2S writes, so a cancelled prompt cannot race its first RTC
         * successor. Each write is at most 20 ms; never hold this mutex while
         * collecting the next network packet or waiting for a tail fragment. */
        for (size_t i = 0; i < mono_samples; ++i) {
            int16_t current = pcm[i];
            size_t output_index = i * AUDIO_RTC_I2S_VALUES_PER_INPUT;
            s_play_stereo[output_index] = current;
            s_play_stereo[output_index + 1U] = current;
        }
        size_t bytes = mono_samples *
                       AUDIO_RTC_I2S_VALUES_PER_INPUT * sizeof(int16_t);
        int64_t write_begin = esp_timer_get_time();
        played = s_speaker_dev != NULL &&
                 esp_codec_dev_write(s_speaker_dev, s_play_stereo, bytes) ==
                     ESP_CODEC_DEV_OK;
        uint32_t write_us = (uint32_t)(esp_timer_get_time() - write_begin);
        update_max_counter(&s_playout_diag.write_max_us, write_us);
        /* A blocking I2S write normally paces one frame; only excess time is
         * suspicious. This is not a measurement of actual DAC underruns. */
        if (write_us > audio_samples_to_us(mono_samples) + 20000U)
            atomic_fetch_add(&s_playout_diag.slow_writes, 1);
        if (!played) {
            atomic_fetch_add_explicit(&s_audio_write_failed, 1,
                                      memory_order_relaxed);
            /* Partial codec writes have unknown progress. Do not replay this
             * block, and do not interpolate from its unconfirmed last sample. */
        }
    } else {
        atomic_fetch_add_explicit(&s_audio_playback_blocked, 1,
                                  memory_order_relaxed);
    }
    give_audio_output_lock();
    return played;
}

static void release_audio_rx_slot(uint8_t slot, bool stale)
{
    if (slot >= AUDIO_RX_QUEUE_DEPTH) {
        atomic_fetch_add(&s_playout_diag.slot_errors, 1);
        return;
    }
    memset(&s_audio_rx_pool[slot], 0, sizeof(s_audio_rx_pool[slot]));
    if (stale) atomic_fetch_add(&s_playout_diag.stale, 1);
    if (xQueueSend(s_audio_rx_free_queue, &slot, 0) != pdTRUE) {
        atomic_fetch_add(&s_playout_diag.slot_errors, 1);
        ESP_LOGE(TAG, "audio RX slot %u could not be returned", (unsigned)slot);
    }
}

static void publish_playout(const starter_playout_t *q, uint32_t generation,
                             size_t pending_samples)
{
    uint32_t queued_ms = (q->queued_us + audio_samples_to_us(pending_samples) + 999U) / 1000U;
    atomic_store(&s_playout_diag.generation, generation);
    atomic_store(&s_playout_diag.target_ms, q->target_ms);
    atomic_store(&s_playout_diag.queued_ms, queued_ms);
    atomic_store(&s_playout_diag.pending_samples, pending_samples);
    update_max_counter(&s_playout_diag.queued_peak_ms, queued_ms);
    atomic_store(&s_playout_diag.buffering, q->buffering);
    atomic_store(&s_playout_diag.jitter_us, q->jitter_us);
    atomic_store(&s_playout_diag.gap_max_us, q->gap_max_us);
    atomic_store(&s_playout_diag.residence_max_us, q->residence_max_us);
    atomic_store(&s_playout_diag.late_bursts, q->late_bursts);
    atomic_store(&s_playout_diag.untimed_pairs, q->untimed_pairs);
    atomic_store(&s_playout_diag.timestamp_breaks, q->timestamp_breaks);
    atomic_store(&s_playout_diag.starts, q->starts);
    atomic_store(&s_playout_diag.rate_mode, q->rate_mode);
    atomic_store(&s_playout_diag.dma_estimate_us,
                 starter_playout_output_estimate(q, esp_timer_get_time()));
}

static void complete_audio_drain(const starter_playout_t *playout,
                                 size_t pending_samples, uint64_t now_us)
{
    uint32_t generation = atomic_load(&s_drain_generation);
    if (generation != 0 && same_session(STARTER_TIRTC_AI, generation) &&
        atomic_load(&s_audio_rx_writers) == 0 &&
        uxQueueMessagesWaiting(s_audio_rx_ready_queue) == 0 &&
        playout->count == 0 && pending_samples == 0 &&
        starter_playout_output_estimate(playout, now_us) == 0 &&
        (playout->last_write_us == 0 ||
         (now_us >= playout->last_write_us &&
          now_us - playout->last_write_us >= STARTER_PLAYOUT_DMA_ESTIMATE_US))) {
        atomic_store(&s_drained_generation, generation);
    }
}

static void audio_sink_task(void *argument)
{
    (void)argument;
    /* This task already has a PSRAM stack. It alone owns the local FIFO and
     * adaptive controller; SDK callbacks only copy to the PSRAM ingress pool. */
    starter_playout_t playout;
    starter_playout_init(&playout);
    starter_playout_pcm_init(&s_rx_pcm);
    atomic_store(&s_playout_diag.pcm_in_hash, STARTER_PLAYOUT_HASH_INITIAL);
    atomic_store(&s_playout_diag.pcm_out_hash, STARTER_PLAYOUT_HASH_INITIAL);
    uint32_t epoch = atomic_load(&s_playout_diag.epoch);
    atomic_store(&s_playout_diag.pcm_epoch, epoch);
    size_t decoded_count = 0, decoded_offset = 0;
    uint64_t partial_since_us = 0, last_service_us = 0;
    uint64_t last_log_us = 0;
    uint32_t last_faults = 0, last_target = 0, render_starts = 0;
    h5_upsampler_t h5_upsampler = {0};
    for (;;) {
        uint32_t current_epoch = atomic_load(&s_playout_diag.epoch);
        bool active = atomic_load(&s_active) && !atomic_load(&s_speaker_muted);
        uint32_t generation = atomic_load(&s_generation);
        starter_tirtc_mode_t mode = atomic_load(&s_mode);
        uint64_t service_us = esp_timer_get_time();
        if (active && last_service_us != 0 && service_us >= last_service_us)
            update_max_counter(&s_playout_diag.service_gap_max_us,
                               (uint32_t)(service_us - last_service_us));
        last_service_us = active ? service_us : 0;
        rx_trace_stage(active ? 1U : 0U, playout.count, playout.buffering);
        if (epoch != current_epoch || !active) {
            uint8_t old_slot;
            while (starter_playout_pop(&playout, 0, true, &old_slot))
                release_audio_rx_slot(old_slot, true);
            atomic_fetch_add(&s_playout_diag.discarded_samples,
                             decoded_count - decoded_offset + s_rx_pcm.count);
            decoded_count = decoded_offset = 0;
            starter_playout_pcm_init(&s_rx_pcm);
            starter_playout_init(&playout);
            s_rx_render.fade_remaining = 0;
            render_starts = 0;
            if (epoch != current_epoch) {
                if (esp_opus_dec_reset(s_opus_decoder) != ESP_AUDIO_ERR_OK)
                    ESP_LOGE(TAG, "Opus decoder reset failed epoch=%lu",
                             (unsigned long)current_epoch);
                memset(&h5_upsampler, 0, sizeof(h5_upsampler));
                atomic_store(&s_playout_diag.pcm_in_hash, STARTER_PLAYOUT_HASH_INITIAL);
                atomic_store(&s_playout_diag.pcm_out_hash, STARTER_PLAYOUT_HASH_INITIAL);
            }
            epoch = current_epoch;
            atomic_store(&s_playout_diag.pcm_epoch, epoch);
        }
        if (decoded_offset == decoded_count && s_rx_pcm.count == 0)
            starter_playout_idle(&playout, esp_timer_get_time());
        /* Drain only a bounded number of slots. Never wait for network input
         * while holding the output lock, and never sort by untrusted stamps. */
        uint8_t slot = 0;
        unsigned drained = 0;
        while (drained++ < AUDIO_RX_QUEUE_DEPTH && playout.count < STARTER_PLAYOUT_CAPACITY &&
               xQueueReceive(s_audio_rx_ready_queue, &slot, 0) == pdTRUE) {
            if (slot >= AUDIO_RX_QUEUE_DEPTH) {
                atomic_fetch_add(&s_playout_diag.slot_errors, 1);
                continue;
            }
            audio_rx_item_t *item = &s_audio_rx_pool[slot];
            if (!active || item->epoch != epoch || !same_session(item->mode, item->generation)) {
                release_audio_rx_slot(slot, true);
                continue;
            }
            uint64_t admitted_us = esp_timer_get_time();
            if (admitted_us >= item->received_us)
                update_max_counter(&s_playout_diag.ingress_max_us,
                                   (uint32_t)(admitted_us - item->received_us));
            /* Opus is fixed at 20 ms. PCMA carries one byte per 8 kHz sample,
             * so its encoded length is also the packet duration. */
            uint32_t duration_us = STARTER_TIRTC_AUDIO_IS_ALAW(item->mode)
                                       ? (uint32_t)(((uint64_t)item->frame.length *
                                                     1000000U) /
                                                    STARTER_H5_AUDIO_SAMPLE_RATE_HZ)
                                       : AUDIO_PACKET_MS * 1000U;
            if (!starter_playout_push(&playout, slot, duration_us,
                                       item->frame.timestamp_ms, item->frame.stream_id,
                                       item->received_us)) {
                atomic_fetch_add(&s_playout_diag.overflow, 1);
                atomic_fetch_add(&s_audio_dropped, 1);
                rx_trace_event(STARTER_RX_TRACE_PLAYOUT_FULL, item->received_us,
                               item->mode, item->generation, item->epoch, &item->frame);
                release_audio_rx_slot(slot, false);
            }
        }
        uint64_t now = esp_timer_get_time();
        update_max_counter(&s_playout_diag.queued_peak_ms,
                           (playout.queued_us +
                            audio_samples_to_us(decoded_count - decoded_offset + s_rx_pcm.count) +
                            999U) / 1000U);
        unsigned decoded_this_turn = 0;
        rx_trace_stage(2U, playout.count, playout.buffering);
        if (s_rx_pcm.count == 0)
            s_rx_pcm.limit = starter_playout_quantum(&playout, decoded_count - decoded_offset, now);
        while (s_rx_pcm.count < s_rx_pcm.limit) {
            if (decoded_offset < decoded_count) {
                if (s_rx_pcm.count == 0) partial_since_us = now;
                decoded_offset += starter_playout_pcm_append(
                    &s_rx_pcm, s_decode_pcm + decoded_offset, decoded_count - decoded_offset);
                continue;
            }
            if (decoded_this_turn++ >= AUDIO_RX_QUEUE_DEPTH ||
                !starter_playout_pop(&playout, now, false, &slot)) break;
            decoded_count = decode_audio_item(&s_audio_rx_pool[slot],
                                              &h5_upsampler);
            decoded_offset = 0;
            release_audio_rx_slot(slot, false);
        }
        now = esp_timer_get_time();
        /* After prebuffer admission, only a partial output quantum waits for a
         * following packet. A bounded 60 ms deadline writes a short tail at
         * its actual length: no padding per packet, repeated speech or drop.
         * round_end is deliberately not treated as proof of the last packet. */
        bool tail = s_rx_pcm.count != 0 && s_rx_pcm.count < s_rx_pcm.limit &&
                    starter_playout_tail_ready(partial_since_us, now);
        if (s_rx_pcm.count == s_rx_pcm.limit || tail) {
            size_t samples = s_rx_pcm.count;
            if (render_starts != playout.starts) {
                render_starts = playout.starts;
                s_rx_render.fade_remaining = STARTER_PLAYOUT_FADE_SAMPLES;
                atomic_fetch_add(&s_playout_diag.fade_starts, 1);
            }
            /* Only the private downlink scratch is transformed. Source PCM and
             * packet boundaries remain intact for conservation/order diagnostics. */
            bool restart = s_rx_render.fade_remaining == STARTER_PLAYOUT_FADE_SAMPLES;
            size_t output_samples = starter_playout_pcm_render(&s_rx_pcm, s_rx_render.samples,
                STARTER_PLAYOUT_PCM_SAMPLES, tail, &s_rx_render.fade_remaining);
            rx_trace_stage(3U, playout.count, playout.buffering);
            bool written = play_audio_chunk(mode, generation, epoch,
                                              s_rx_render.samples, output_samples, restart);
            uint64_t finished_us = esp_timer_get_time();
            if (written) {
                atomic_fetch_add(&s_playout_diag.written_samples, samples);
                atomic_fetch_add(&s_playout_diag.output_samples, output_samples);
                if (samples < output_samples) atomic_fetch_add(&s_playout_diag.slow_chunks, 1);
                if (samples > output_samples) atomic_fetch_add(&s_playout_diag.fast_chunks, 1);
                atomic_fetch_add(&s_playout_diag.chunks, 1);
                if (tail) atomic_fetch_add(&s_playout_diag.tails, 1);
                atomic_store(&s_playout_diag.pcm_out_hash,
                             starter_playout_pcm_hash(atomic_load(&s_playout_diag.pcm_out_hash),
                                                      s_rx_pcm.samples, samples));
                if (playout.last_write_us != 0 && finished_us >= playout.last_write_us)
                    update_max_counter(&s_playout_diag.write_gap_max_us,
                                       (uint32_t)(finished_us - playout.last_write_us));
                starter_playout_output_written(&playout, finished_us, output_samples);
            } else {
                atomic_fetch_add(&s_playout_diag.discarded_samples, samples);
                /* Unknown partial output invalidates the clock estimate. Do
                 * not replay the failed block; smooth the next confirmed start. */
                playout.output_estimate_us = 0;
                s_rx_render.fade_remaining = STARTER_PLAYOUT_FADE_SAMPLES;
            }
            atomic_fetch_add(&s_audio_played, starter_playout_pcm_commit(&s_rx_pcm, written));
            rx_trace_stage(4U, playout.count, playout.buffering);
            /* Give lower-priority volume/mute/UI owners a chance to acquire
             * the released mutex. I2S still supplies the playback clock;
             * never add a second 20 ms sleep after a blocking 20 ms write. */
            vTaskDelay(pdMS_TO_TICKS(MEDIA_CPU_YIELD_MS));
        } else {
            /* No new task and no busy spin. Stop/mute/session changes are seen
             * within this short wait even when the network sends no more data. */
            rx_trace_stage(4U, playout.count, playout.buffering);
            vTaskDelay(pdMS_TO_TICKS(active ? AUDIO_PLAYOUT_POLL_MS : 20U));
        }
        now = esp_timer_get_time();
        publish_playout(&playout, active ? generation : 0,
                        decoded_count - decoded_offset + s_rx_pcm.count);
        complete_audio_drain(&playout, decoded_count - decoded_offset + s_rx_pcm.count, now);
        uint32_t faults = (uint32_t)atomic_load(&s_playout_diag.overflow) +
            (uint32_t)atomic_load(&s_playout_diag.lock_timeouts) +
            (uint32_t)atomic_load(&s_playout_diag.slow_writes) +
            (uint32_t)atomic_load(&s_playout_diag.slot_errors) +
            (uint32_t)atomic_load(&s_audio_write_failed) + playout.late_bursts;
        if (active && (faults != last_faults || playout.target_ms != last_target) &&
            (last_log_us == 0 || now - last_log_us >= 5000000U)) {
            /* Retain the exact playback snapshot without making the I2S
             * owner wait for stdout/USB, even outside the playback lock. */
            starter_log_record(STARTER_LOG_PLAYOUT,
                (const uint32_t[STARTER_LOG_VALUES]){
                    playout.target_ms, atomic_load(&s_playout_diag.queued_ms),
                    playout.jitter_us / 1000U, playout.gap_max_us / 1000U,
                    playout.late_bursts, atomic_load(&s_playout_diag.overflow),
                    atomic_load(&s_playout_diag.lock_timeouts),
                    atomic_load(&s_playout_diag.lock_max_us) / 1000U,
                    atomic_load(&s_playout_diag.last_lock_owner),
                    atomic_load(&s_playout_diag.slow_writes),
                    atomic_load(&s_playout_diag.write_max_us) / 1000U,
                    atomic_load(&s_playout_diag.decoded_samples),
                    atomic_load(&s_playout_diag.written_samples),
                    atomic_load(&s_playout_diag.discarded_samples),
                    atomic_load(&s_playout_diag.pending_samples), generation, epoch});
            last_log_us = now;
            last_faults = faults;
            last_target = playout.target_ms;
        }
    }
}

static void drain_audio_rx_ready_queue(void)
{
    if (s_audio_rx_ready_queue == NULL || s_audio_rx_free_queue == NULL ||
        s_audio_rx_pool == NULL) {
        return;
    }
    uint8_t slot = 0;
    while (xQueueReceive(s_audio_rx_ready_queue, &slot, 0) == pdTRUE) {
        if (slot < AUDIO_RX_QUEUE_DEPTH) {
            release_audio_rx_slot(slot, true);
        }
    }
}

esp_err_t starter_media_init(void)
{
    if (atomic_load_explicit(&s_ready, memory_order_acquire)) {
        return ESP_OK;
    }
    s_audio_output_mutex = xSemaphoreCreateMutex();
    if (s_audio_output_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = board_i2c_init();
    if (err == ESP_OK) {
        err = audio_i2s_init();
    }
    if (err == ESP_OK) {
        err = opus_init();
    }
    if (err == ESP_OK) {
        err = g711_init();
    }
    if (err == ESP_OK) {
        err = starter_aec_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "board media initialization failed: %s", esp_err_to_name(err));
        return err;
    }

    s_audio_rx_pool = heap_caps_calloc(AUDIO_RX_QUEUE_DEPTH,
                                       sizeof(*s_audio_rx_pool),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_audio_tx_pool = heap_caps_calloc(AUDIO_TX_QUEUE_DEPTH,
                                       sizeof(*s_audio_tx_pool),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *preroll_pcm = heap_caps_calloc(PREROLL_CAPACITY_SAMPLES,
                                           sizeof(int16_t),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_preroll_mutex = xSemaphoreCreateMutex();
    if (preroll_pcm == NULL || s_preroll_mutex == NULL) return ESP_ERR_NO_MEM;
    starter_preroll_init(&s_preroll, preroll_pcm, PREROLL_CAPACITY_SAMPLES);
    s_audio_rx_ready_queue = xQueueCreateWithCaps(AUDIO_RX_QUEUE_DEPTH, sizeof(uint8_t),
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_audio_rx_free_queue = xQueueCreateWithCaps(AUDIO_RX_QUEUE_DEPTH, sizeof(uint8_t),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_audio_tx_ready_queue = xQueueCreate(AUDIO_TX_QUEUE_DEPTH, sizeof(uint8_t));
    s_audio_tx_free_queue = xQueueCreate(AUDIO_TX_QUEUE_DEPTH, sizeof(uint8_t));
    if (s_audio_rx_pool == NULL || s_audio_tx_pool == NULL ||
        !esp_ptr_external_ram(s_audio_rx_pool) ||
        !esp_ptr_external_ram(s_audio_tx_pool) ||
        !esp_ptr_external_ram(s_decode_h5_pcm) ||
        !esp_ptr_external_ram(s_decode_pcm) ||
        !esp_ptr_external_ram(s_encode_h5_pcm) ||
        !esp_ptr_external_ram(s_encode_alaw) ||
        !esp_ptr_external_ram(s_encode_opus) ||
        !esp_ptr_external_ram(&s_rx_pcm) ||
        !esp_ptr_external_ram(s_play_stereo) ||
        s_audio_rx_ready_queue == NULL || s_audio_rx_free_queue == NULL ||
        s_audio_tx_ready_queue == NULL || s_audio_tx_free_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    for (uint8_t slot = 0; slot < AUDIO_RX_QUEUE_DEPTH; ++slot) {
        if (xQueueSend(s_audio_rx_free_queue, &slot, 0) != pdTRUE) {
            return ESP_FAIL;
        }
    }
    for (uint8_t slot = 0; slot < AUDIO_TX_QUEUE_DEPTH; ++slot) {
        if (xQueueSend(s_audio_tx_free_queue, &slot, 0) != pdTRUE) {
            return ESP_FAIL;
        }
    }
    /* Opening the duplex codec starts RX DMA. Prepare AFE and pools
     * first, so their initialization cannot overflow an unread DMA queue. */
#if CONFIG_XIAOTAI_DEVELOPMENT_CONSOLE
    codec_cpu_tick_init();
#endif
    err = audio_codecs_init();
    if (err == ESP_OK) err = audio_i2s_start();
    if (err != ESP_OK) return err;
    /* Espressif's esp_audio_codec contract recommends about 40 KiB for an
     * encoder task and 20 KiB for a decoder task.  The wrapper, diagnostics
     * and TiRTC send call need their own margin, so use 48/24 KiB.  Both
     * stacks remain in PSRAM; internal/DMA SRAM is not consumed by this fix. */
    if (xTaskCreateWithCaps(audio_sink_task,
                            "board_audio_rx",
                            AUDIO_OPUS_DECODER_STACK_BYTES,
                            NULL,
                            8,
                            NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS ||
        xTaskCreateWithCaps(audio_uplink_task,
                            "rtc_audio_tx",
                            AUDIO_OPUS_ENCODER_STACK_BYTES,
                            NULL,
                            7,
                            NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS ||
        xTaskCreatePinnedToCoreWithCaps(audio_input_task,
                                        "board_audio_in",
                                        4096,
                                        NULL,
                                        9,
                                        NULL,
                                        MEDIA_REALTIME_CORE,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS ||
        xTaskCreatePinnedToCoreWithCaps(audio_capture_task,
                                        "board_audio_tx",
                                        6144,
                                        NULL,
                                        7,
                                        NULL,
                                        0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    atomic_store_explicit(&s_ready, true, memory_order_release);
    ESP_LOGI(TAG,
             "audio ready: PSRAM rx/tx=%u/%u B, H5/AI/Room=PCMA/8k call/VoIP=Opus/16k, "
             "full-duplex AEC=%u Hz->%u Hz chunk=%u media-core=%d yield=%u ms",
             (unsigned)(AUDIO_RX_QUEUE_DEPTH * sizeof(*s_audio_rx_pool)),
             (unsigned)(AUDIO_TX_QUEUE_DEPTH * sizeof(*s_audio_tx_pool)),
             AUDIO_HW_SAMPLE_RATE_HZ,
             AUDIO_TRANSPORT_SAMPLE_RATE_HZ,
             (unsigned)starter_aec_frame_samples(),
             MEDIA_REALTIME_CORE,
             MEDIA_CPU_YIELD_MS);
    return ESP_OK;
}

i2c_master_bus_handle_t starter_media_i2c_bus(void)
{
    return atomic_load_explicit(&s_ready, memory_order_acquire) ? s_i2c_bus : NULL;
}

esp_err_t starter_media_select_lcd(bool selected)
{
    if (!atomic_load_explicit(&s_ready, memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }
    (void)selected; /* CS is owned by the SPI panel driver. */
    return ESP_OK;
}

esp_err_t starter_media_camera_power(bool enabled)
{
    if (!atomic_load(&s_ready)) return ESP_ERR_INVALID_STATE;
    return starter_board_camera_power(enabled);
}

bool starter_media_h5_current(uint32_t generation)
{
    return generation != 0 && same_session(STARTER_TIRTC_H5, generation);
}

esp_err_t starter_media_set_speaker_volume(uint8_t volume)
{
    if (volume > 10U || s_speaker_dev == NULL || s_audio_output_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!take_audio_output_lock(OUTPUT_OWNER_VOLUME, pdMS_TO_TICKS(500))) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = esp_codec_dev_set_out_vol(s_speaker_dev, volume * 10U) ==
                            ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
    if (err == ESP_OK) {
        atomic_store_explicit(&s_speaker_volume, volume, memory_order_release);
    }
    give_audio_output_lock();
    return err;
}

esp_err_t starter_media_set_speaker_muted(bool muted)
{
    if (s_speaker_dev == NULL || s_audio_output_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!take_audio_output_lock(OUTPUT_OWNER_MUTE, pdMS_TO_TICKS(500))) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = esp_codec_dev_set_out_mute(s_speaker_dev, muted) ==
                            ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
    if (err == ESP_OK) {
        bool changed = atomic_exchange_explicit(&s_speaker_muted, muted, memory_order_acq_rel) != muted;
        /* Buffered audio must not cross either edge of a user mute change. */
        if (changed) atomic_fetch_add(&s_playout_diag.epoch, 1);
        err = audio_output_set_locked(!muted &&
            atomic_load_explicit(&s_active, memory_order_acquire));
    }
    give_audio_output_lock();
    return err;
}

void starter_media_set_microphone_muted(bool muted)
{
    bool previous = atomic_exchange_explicit(&s_microphone_muted, muted, memory_order_acq_rel);
    if (previous != muted) atomic_fetch_add(&s_mute_epoch, 1);
    if (muted) {
        starter_voice_set_listening(false);
        if (s_preroll_mutex != NULL && xSemaphoreTake(s_preroll_mutex, portMAX_DELAY) == pdTRUE) {
            bool was_bound = s_preroll.generation != 0;
            starter_preroll_clear(&s_preroll);
            if (was_bound) s_expected_wake_token = 0;
            /* Keep expected token so the pending AI start fails instead of
             * silently pretending it received the continuous utterance. */
            xSemaphoreGive(s_preroll_mutex);
        }
    }
}

void starter_media_set_wake_allowed(bool allowed)
{
    atomic_store(&s_wake_allowed, allowed);
    if (!allowed) starter_voice_set_listening(false);
}

uint32_t starter_media_prepare_ai_preroll(int64_t captured_ms)
{
    if (s_preroll_mutex == NULL || xSemaphoreTake(s_preroll_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        ESP_LOGW(TAG, "wake preroll unavailable: lock");
        return 0;
    }
    uint32_t token = 0;
    bool muted = atomic_load(&s_microphone_muted);
    bool active = atomic_load(&s_active);
    bool allowed = atomic_load(&s_wake_allowed);
    int64_t now = esp_timer_get_time() / 1000;
    if (!muted && !active && allowed) {
        token = starter_preroll_prepare(&s_preroll, captured_ms, now);
    }
    size_t buffered = s_preroll.count;
    uint32_t owned = s_preroll.token;
    bool failed = s_preroll.failed;
    int64_t end_age = now - s_preroll.end_ms;
    xSemaphoreGive(s_preroll_mutex);
    if (token == 0) {
        ESP_LOGW(TAG, "wake preroll rejected: muted=%d active=%d allowed=%d "
                 "wake-age=%lld end-age=%lld samples=%u owned=%lu failed=%d",
                 muted, active, allowed, (long long)(now - captured_ms),
                 (long long)end_age, (unsigned)buffered, (unsigned long)owned, failed);
    }
    return token;
}

void starter_media_cancel_ai_preroll(uint32_t token)
{
    if (token == 0 || s_preroll_mutex == NULL) return;
    xSemaphoreTake(s_preroll_mutex, portMAX_DELAY);
    if (s_preroll.token == token && s_preroll.generation == 0 &&
        s_expected_wake_token != token) starter_preroll_clear(&s_preroll);
    xSemaphoreGive(s_preroll_mutex);
}

bool starter_media_ai_preroll_failed(void)
{
    if (s_preroll_mutex == NULL || xSemaphoreTake(s_preroll_mutex, 0) != pdTRUE) return false;
    bool failed = starter_preroll_failure_requires_abort(&s_preroll,
        s_expected_wake_token, esp_timer_get_time() / 1000);
    xSemaphoreGive(s_preroll_mutex);
    return failed;
}

bool starter_media_preroll_status(starter_media_preroll_status_t *out)
{
    if (out == NULL || s_preroll_mutex == NULL ||
        xSemaphoreTake(s_preroll_mutex, 0) != pdTRUE) return false;
    *out = (starter_media_preroll_status_t){
        .buffered_ms = (uint32_t)(s_preroll.count / PREROLL_SAMPLES_PER_MS),
        .owned = s_preroll.token != 0,
        .bound = s_preroll.generation != 0,
        .failed = s_preroll.failed,
    };
    xSemaphoreGive(s_preroll_mutex);
    return true;
}

void starter_media_cancel_pcm8k_playback(void)
{
    atomic_fetch_add_explicit(&s_pcm8k_cancel_sequence, 1, memory_order_acq_rel);
}

esp_err_t starter_media_play_pcm8k(const int16_t *pcm, size_t sample_count)
{
    return starter_media_play_pcm8k_at_epoch(pcm, sample_count, starter_media_playback_epoch());
}

uint32_t starter_media_playback_epoch(void)
{
    return (uint32_t)atomic_load_explicit(&s_pcm8k_cancel_sequence, memory_order_acquire);
}

esp_err_t starter_media_play_pcm8k_at_epoch(const int16_t *pcm, size_t sample_count,
                                           uint32_t cancel_sequence)
{
    if (pcm == NULL || sample_count == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!atomic_load_explicit(&s_ready, memory_order_acquire) ||
        s_speaker_dev == NULL || s_audio_output_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!take_audio_output_lock(OUTPUT_OWNER_PROMPT, pdMS_TO_TICKS(500))) {
        return ESP_ERR_TIMEOUT;
    }
    if (atomic_load_explicit(&s_active, memory_order_acquire) ||
        atomic_load_explicit(&s_speaker_muted, memory_order_acquire) ||
        cancel_sequence != atomic_load_explicit(&s_pcm8k_cancel_sequence,
                                                 memory_order_acquire)) {
        give_audio_output_lock();
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = audio_output_set_locked(true);
    int16_t previous = pcm[0];
    size_t offset = 0;
    while (err == ESP_OK && offset < sample_count) {
        size_t chunk_samples = sample_count - offset;
        if (chunk_samples > AUDIO_RX_BYTES) {
            chunk_samples = AUDIO_RX_BYTES;
        }
        for (size_t i = 0; i < chunk_samples; ++i) {
            int16_t current = pcm[offset + i];
            int16_t midpoint = (int16_t)(((int32_t)previous + current) / 2);
            size_t output_index = i * AUDIO_PCM8K_I2S_VALUES_PER_INPUT;
            /* Linear 2x interpolation: the midpoint lies between previous
             * and current in time, as in the H5 downlink upsampler. */
            s_play_stereo[output_index] = midpoint;
            s_play_stereo[output_index + 1U] = midpoint;
            s_play_stereo[output_index + 2U] = current;
            s_play_stereo[output_index + 3U] = current;
            previous = current;
        }
        size_t bytes = chunk_samples * AUDIO_PCM8K_I2S_VALUES_PER_INPUT *
                       sizeof(int16_t);
        if (s_speaker_dev == NULL ||
            esp_codec_dev_write(s_speaker_dev, s_play_stereo, bytes) !=
                ESP_CODEC_DEV_OK) {
            err = ESP_FAIL;
        }
        offset += chunk_samples;
        if ((uint32_t)atomic_load_explicit(&s_pcm8k_cancel_sequence,
                                           memory_order_acquire) !=
            cancel_sequence) {
            /* 至多等待一个 PCM 块，实时通话随后可获得 I2S 输出锁。 */
            err = ESP_ERR_INVALID_STATE;
            ESP_LOGI(TAG, "PCM8k prompt interrupted for realtime media");
            break;
        }
    }
    if (err == ESP_OK) {
        /* 等待最后一小段 DMA 数据移出，避免关闭功放时切掉尾音。 */
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    esp_err_t disable_err = audio_output_set_locked(false);
    give_audio_output_lock();
    if (err == ESP_OK) {
        err = disable_err;
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "verification prompt played samples=%u rate=%u",
                 (unsigned)sample_count,
                 8000U);
    }
    return err;
}

esp_err_t starter_media_start(starter_tirtc_mode_t mode, uint32_t generation)
{
    if (!atomic_load_explicit(&s_ready, memory_order_acquire) ||
        generation == 0U ||
        (mode != STARTER_TIRTC_H5 && mode != STARTER_TIRTC_AI &&
         mode != STARTER_TIRTC_VOIP && mode != STARTER_TIRTC_CALL && mode != STARTER_TIRTC_ROOM)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (mode == STARTER_TIRTC_H5) {
        esp_err_t camera_err = starter_camera_prepare(generation);
        if (camera_err != ESP_OK) return camera_err;
    }
    xSemaphoreTake(s_preroll_mutex, portMAX_DELAY);
    bool preroll_ok = true;
    if (mode == STARTER_TIRTC_AI && s_expected_wake_token != 0) {
        preroll_ok = !atomic_load(&s_microphone_muted) &&
            starter_preroll_bind(&s_preroll, s_expected_wake_token, generation,
                                 esp_timer_get_time() / 1000);
    } else {
        starter_preroll_clear(&s_preroll);
        s_expected_wake_token = 0;
    }
    xSemaphoreGive(s_preroll_mutex);
    if (!preroll_ok) return ESP_ERR_INVALID_STATE;
    if (mode == STARTER_TIRTC_AI) {
        ESP_LOGI(TAG, "AI audio admitted generation=%lu", (unsigned long)generation);
    }
    /* 铃声/验证码提示音是可中断的；实时通话不能因其占用 I2S 而启动失败。 */
    starter_media_cancel_pcm8k_playback();
    atomic_fetch_add(&s_playout_diag.epoch, 1);
    drain_audio_rx_ready_queue();
    atomic_store_explicit(&s_audio_sent, 0, memory_order_release);
    atomic_store_explicit(&s_audio_received, 0, memory_order_release);
    atomic_store_explicit(&s_audio_dropped, 0, memory_order_release);
    atomic_store_explicit(&s_audio_decoded, 0, memory_order_release);
    atomic_store_explicit(&s_audio_played, 0, memory_order_release);
    atomic_store_explicit(&s_audio_decode_failed, 0, memory_order_release);
    atomic_store_explicit(&s_audio_playback_blocked, 0, memory_order_release);
    atomic_store_explicit(&s_audio_write_failed, 0, memory_order_release);
    atomic_store_explicit(&s_aec_processed, 0, memory_order_release);
    atomic_store_explicit(&s_aec_errors, 0, memory_order_release);
    atomic_store_explicit(&s_aec_mic_clipped, 0, memory_order_release);
    atomic_store_explicit(&s_aec_reference_clipped, 0, memory_order_release);
    atomic_store_explicit(&s_aec_max_process_us, 0, memory_order_release);
    atomic_store_explicit(&s_aec_deadline_misses, 0, memory_order_release);
    atomic_store_explicit(&s_mode, mode, memory_order_release);
    atomic_store_explicit(&s_generation, generation, memory_order_release);
    atomic_store(&s_drained_generation, 0);
    atomic_store(&s_drain_generation, 0);
    atomic_store_explicit(&s_active, true, memory_order_release);
    if (!take_audio_output_lock(OUTPUT_OWNER_START, pdMS_TO_TICKS(500))) {
        ESP_LOGE(TAG, "media start output lock timeout mode=%d generation=%lu",
                 (int)mode, (unsigned long)generation);
        atomic_store_explicit(&s_active, false, memory_order_release);
        atomic_store_explicit(&s_generation, 0, memory_order_release);
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = audio_output_set_locked(
        !atomic_load_explicit(&s_speaker_muted, memory_order_acquire));
    give_audio_output_lock();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "media start output enable failed mode=%d generation=%lu err=%s",
                 (int)mode, (unsigned long)generation, esp_err_to_name(err));
        atomic_store_explicit(&s_active, false, memory_order_release);
        atomic_store_explicit(&s_generation, 0, memory_order_release);
        drain_audio_rx_ready_queue();
        return err;
    }
    ESP_LOGI(TAG,
             "audio start mode=%d gen=%lu",
             (int)mode,
             (unsigned long)generation);
    starter_camera_set_session(mode == STARTER_TIRTC_H5 ? generation : 0);
    return ESP_OK;
}

static esp_err_t stop_media(uint32_t preserve_token)
{
    /* Revoke video before releasing RTC ownership; its sole worker performs
     * driver teardown after its borrowed raw frame/encode operation ends. */
    starter_camera_set_session(0);
    if (preserve_token == 0) atomic_fetch_add(&s_capture_epoch, 1);
    starter_media_cancel_pcm8k_playback();
    atomic_store_explicit(&s_active, false, memory_order_release);
    atomic_store_explicit(&s_generation, 0, memory_order_release);
    atomic_store(&s_drain_generation, 0);
    atomic_store(&s_drained_generation, 0);
    atomic_store(&s_room_ptt_drain_generation, 0);
    atomic_fetch_add(&s_playout_diag.epoch, 1);
    bool preserved = preserve_token == 0;
    if (s_preroll_mutex != NULL) {
        xSemaphoreTake(s_preroll_mutex, portMAX_DELAY);
        preserved = preserve_token == 0 ||
                    (!atomic_load(&s_microphone_muted) &&
                     starter_preroll_valid(&s_preroll, preserve_token, esp_timer_get_time() / 1000));
        if (preserve_token == 0 || !preserved) starter_preroll_clear(&s_preroll);
        s_expected_wake_token = preserved ? preserve_token : 0;
        xSemaphoreGive(s_preroll_mutex);
    }
    drain_audio_rx_ready_queue();
    if (s_audio_output_mutex != NULL &&
        take_audio_output_lock(OUTPUT_OWNER_STOP, pdMS_TO_TICKS(600))) {
        (void)audio_output_set_locked(false);
        give_audio_output_lock();
    }
    return preserved ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void starter_media_stop(void) { (void)stop_media(0); }
esp_err_t starter_media_stop_for_ai(uint32_t token) { return stop_media(token); }

bool starter_media_begin_audio_drain(uint32_t generation)
{
    if (generation == 0 || !same_session(STARTER_TIRTC_AI, generation)) return false;
    atomic_store(&s_drain_generation, generation);
    return true;
}

bool starter_media_audio_drained(uint32_t generation)
{
    return generation != 0 && same_session(STARTER_TIRTC_AI, generation) &&
           atomic_load(&s_drain_generation) == generation &&
           atomic_load(&s_drained_generation) == generation;
}

static void submit_audio(starter_tirtc_mode_t mode,
                                uint32_t generation,
                                const starter_tirtc_frame_t *frame,
                                const void *data)
{
    uint64_t received_us = esp_timer_get_time();
    uint32_t epoch = atomic_load(&s_playout_diag.epoch);
    if (s_audio_rx_ready_queue == NULL || s_audio_rx_free_queue == NULL ||
        s_audio_rx_pool == NULL || frame == NULL || data == NULL ||
        frame->length == 0U || frame->length > AUDIO_RX_BYTES) {
        atomic_fetch_add_explicit(&s_audio_dropped, 1, memory_order_relaxed);
        atomic_fetch_add(&s_playout_diag.invalid, 1);
        return;
    }
    if (!same_session(mode, generation) || atomic_load(&s_drain_generation) != 0) {
        atomic_fetch_add(&s_audio_dropped, 1);
        atomic_fetch_add(&s_playout_diag.stale, 1);
        return;
    }
    if (mode == STARTER_TIRTC_ROOM) {
        atomic_fetch_add(&s_room_audio_diag.rx_frames, 1);
        atomic_fetch_add(&s_room_audio_diag.rx_bytes, frame->length);
        /* Never serialize media on diagnostics if SDK callbacks overlap. Only
         * skip the timing observation; payload admission continues unchanged. */
        unsigned observer = 0;
        if (atomic_compare_exchange_strong(&s_room_audio_diag.rx_observer_busy, &observer, 1)) {
            bool same_generation = s_room_audio_diag.rx_generation == generation;
            uint32_t gap = (uint32_t)received_us - s_room_audio_diag.rx_last_us;
            /* Overlapping callbacks may reach this observer out of time order.
             * Do not turn that into a huge unsigned arrival gap. */
            bool ordered = !same_generation || gap <= INT32_MAX;
            if (same_generation && ordered) {
                uint32_t step = frame->timestamp_ms - s_room_audio_diag.rx_last_timestamp;
                atomic_fetch_add(&s_room_audio_diag.rx_pairs, 1);
                if (gap < 5000U) atomic_fetch_add(&s_room_audio_diag.rx_burst, 1);
                if (gap > 60000U) atomic_fetch_add(&s_room_audio_diag.rx_gaps, 1);
                update_max_counter(&s_room_audio_diag.rx_gap_max_us, gap);
                if (!step) atomic_fetch_add(&s_room_audio_diag.rx_repeat_ts, 1);
                else if (step > INT32_MAX) atomic_fetch_add(&s_room_audio_diag.rx_back_ts, 1);
                else if (step * (uint64_t)1000U > AUDIO_PACKET_MS * 1000ULL + 2000U)
                    atomic_fetch_add(&s_room_audio_diag.rx_forward_gap, 1);
            }
            if (ordered) {
                s_room_audio_diag.rx_last_us = (uint32_t)received_us;
                s_room_audio_diag.rx_last_timestamp = frame->timestamp_ms;
                s_room_audio_diag.rx_last_len = frame->length;
                s_room_audio_diag.rx_generation = generation;
            } else {
                atomic_fetch_add(&s_room_audio_diag.rx_observer_skipped, 1);
            }
            atomic_store(&s_room_audio_diag.rx_observer_busy, 0);
        } else {
            atomic_fetch_add(&s_room_audio_diag.rx_observer_skipped, 1);
        }
    }
    if (atomic_load(&s_speaker_muted)) {
        atomic_fetch_add(&s_audio_playback_blocked, 1);
        return;
    }
    uint8_t slot = 0;
    BaseType_t received_slot = xQueueReceive(s_audio_rx_free_queue, &slot, 0);
    if (received_slot != pdTRUE || slot >= AUDIO_RX_QUEUE_DEPTH) {
        atomic_fetch_add_explicit(&s_audio_dropped, 1, memory_order_relaxed);
        atomic_fetch_add(&s_playout_diag.overflow, 1);
        rx_trace_event(received_slot != pdTRUE ? STARTER_RX_TRACE_NO_FREE_SLOT :
                          STARTER_RX_TRACE_BAD_SLOT, received_us, mode, generation, epoch, frame);
        return;
    }
    audio_rx_item_t *item = &s_audio_rx_pool[slot];
    /* Decoders consume frame.length bytes; the unused PSRAM tail need not be
     * cleared on every packet, including small Opus silence packets. */
    item->received_us = received_us;
    item->mode = mode;
    item->generation = generation;
    item->epoch = epoch;
    item->frame = *frame;
    memcpy(item->payload, data, frame->length);
    if (xQueueSend(s_audio_rx_ready_queue, &slot, 0) == pdTRUE) {
        atomic_fetch_add_explicit(&s_audio_received, 1, memory_order_relaxed);
        rx_trace_event(STARTER_RX_TRACE_ACCEPTED, received_us, mode, generation, epoch, frame);
    } else {
        rx_trace_event(STARTER_RX_TRACE_READY_FULL, received_us, mode, generation, epoch, frame);
        release_audio_rx_slot(slot, false);
        atomic_fetch_add_explicit(&s_audio_dropped, 1, memory_order_relaxed);
        atomic_fetch_add(&s_playout_diag.overflow, 1);
    }
}

void starter_media_submit_audio(starter_tirtc_mode_t mode, uint32_t generation,
                                const starter_tirtc_frame_t *frame, const void *data)
{
    /* Count before checking EOS so the sink cannot acknowledge an empty queue
     * while a previously admitted callback is still copying into its slot. */
    atomic_fetch_add(&s_audio_rx_writers, 1);
    submit_audio(mode, generation, frame, data);
    atomic_fetch_sub(&s_audio_rx_writers, 1);
}

#if CONFIG_XIAOTAI_DEVELOPMENT_CONSOLE
esp_err_t starter_media_debug_rx_trace_arm(bool enabled)
{
    if (atomic_load(&s_active) || atomic_load(&s_audio_rx_writers)) return ESP_ERR_INVALID_STATE;
    return starter_rx_trace_arm(&s_rx_trace, enabled) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

starter_media_rx_trace_status_t starter_media_debug_rx_trace_status(void)
{
    return (starter_media_rx_trace_status_t) {
        .active = atomic_load(&s_rx_trace.active), .frozen = atomic_load(&s_rx_trace.frozen),
        .count = starter_rx_trace_count(&s_rx_trace), .skipped = atomic_load(&s_rx_trace.skipped),
        .missed_faults = atomic_load(&s_rx_trace.missed_faults), .max_us = atomic_load(&s_rx_trace.max_us),
    };
}

bool starter_media_debug_rx_trace_read(unsigned index, starter_rx_trace_entry_t *out)
{
    if (atomic_load(&s_active)) return false;
    return starter_rx_trace_read(&s_rx_trace, index, out);
}
#endif

void starter_media_log_room_audio(void)
{
    if (!same_session(STARTER_TIRTC_ROOM, atomic_load(&s_generation))) return;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (now - s_room_audio_diag.last_log_ms < 5000U) return;
    s_room_audio_diag.last_log_ms = now;
    starter_aec_stats_t afe;
    starter_aec_get_stats(&afe);
    /* Called by runtime, never the capture, SDK callback or speaker worker.
     * Gap/PTS counters are observations, not packet-loss or DAC-underrun rates. */
#define RA(field) ((unsigned long)atomic_load(&s_room_audio_diag.field))
#define PA(field) ((unsigned long)atomic_load(&s_pipeline_diag.field))
#define PB(field) ((unsigned long)atomic_load(&s_playout_diag.field))
    ESP_LOGI(TAG, "ROOM CAP t=%lu gen=%lu ptt=%d calls=%lu/%lu read=%lu err=%lu afe=%lu/%lu full=%lu timeout=%lu stale=%lu age=%lu ring=%lu drain=%lu",
             (unsigned long)now, (unsigned long)atomic_load(&s_generation),
             atomic_load(&s_room_ptt_generation) != 0, RA(ptt_down), RA(ptt_up), PA(read_frames), PA(read_errors),
             (unsigned long)afe.feed_frames, (unsigned long)afe.fetch_frames,
             (unsigned long)afe.input_full, (unsigned long)afe.fetch_timeouts, RA(capture_stale), PA(afe_age_max_ms),
             (unsigned long)afe.feed_ring_full, RA(ptt_drained));
    ESP_LOGI(TAG, "ROOM TX t=%lu in=%lu full=%lu stale=%lu sent=%lu err_gain/enc/send=%lu/%lu/%lu pairs=%lu burst5=%lu gap60=%lu gap_us=%lu q_us=%lu sdk_us=%lu rms=%lu",
             (unsigned long)now, RA(tx_enqueued), RA(tx_full), RA(tx_stale),
             RA(tx_accepted), RA(tx_gain_error), RA(tx_encode_error), RA(tx_send_error),
             RA(tx_pairs), RA(tx_burst), RA(tx_gaps), RA(tx_gap_max_us), PA(tx_age_max_us), PA(send_max_us), PA(tx_rms));
    ESP_LOGI(TAG, "ROOM RX t=%lu n=%lu bytes=%lu pairs=%lu burst5=%lu gap60=%lu gap_us=%lu pts_repeat/gap/back=%lu/%lu/%lu obs_skip=%lu ingress_us=%lu",
             (unsigned long)now, RA(rx_frames), RA(rx_bytes), RA(rx_pairs), RA(rx_burst), RA(rx_gaps),
             RA(rx_gap_max_us), RA(rx_repeat_ts), RA(rx_forward_gap), RA(rx_back_ts), RA(rx_observer_skipped), PB(ingress_max_us));
    ESP_LOGI(TAG, "ROOM PLAY t=%lu target=%lu q=%lu buffer=%lu starts=%lu late=%lu jitter_us=%lu untimed/break=%lu/%lu bad/full/stale=%lu/%lu/%lu mute=%lu decerr=%lu lock=%lu wrerr=%lu wrgap_us=%lu service_us=%lu pcm=%lu/%lu/%lu+%lu rate=%d slow/fast=%lu/%lu",
             (unsigned long)now, PB(target_ms), PB(queued_ms), PB(buffering), PB(starts), PB(late_bursts),
             PB(jitter_us), PB(untimed_pairs), PB(timestamp_breaks), PB(invalid), PB(overflow), PB(stale),
             (unsigned long)atomic_load(&s_audio_playback_blocked), (unsigned long)atomic_load(&s_audio_decode_failed),
             PB(lock_timeouts), (unsigned long)atomic_load(&s_audio_write_failed),
             PB(write_gap_max_us), PB(service_gap_max_us), PB(decoded_samples), PB(written_samples),
             PB(discarded_samples), PB(pending_samples), atomic_load(&s_playout_diag.rate_mode), PB(slow_chunks), PB(fast_chunks));
#undef PB
#undef PA
#undef RA
}

starter_media_playback_status_t starter_media_playback_status(void)
{
    return (starter_media_playback_status_t) {
        .generation = atomic_load(&s_playout_diag.generation),
        .target_ms = atomic_load(&s_playout_diag.target_ms),
        .queued_ms = atomic_load(&s_playout_diag.queued_ms),
        .queued_peak_ms = atomic_load(&s_playout_diag.queued_peak_ms),
        .buffering = atomic_load(&s_playout_diag.buffering),
        .jitter_us = atomic_load(&s_playout_diag.jitter_us),
        .gap_max_us = atomic_load(&s_playout_diag.gap_max_us),
        .residence_max_us = atomic_load(&s_playout_diag.residence_max_us),
        .late_bursts = atomic_load(&s_playout_diag.late_bursts),
        .untimed_pairs = atomic_load(&s_playout_diag.untimed_pairs),
        .timestamp_breaks = atomic_load(&s_playout_diag.timestamp_breaks),
        .starts = atomic_load(&s_playout_diag.starts),
        .rx_invalid = atomic_load(&s_playout_diag.invalid),
        .rx_overflow = atomic_load(&s_playout_diag.overflow),
        .rx_stale = atomic_load(&s_playout_diag.stale),
        .slot_errors = atomic_load(&s_playout_diag.slot_errors),
        .lock_timeouts = atomic_load(&s_playout_diag.lock_timeouts),
        .lock_max_us = atomic_load(&s_playout_diag.lock_max_us),
        .last_lock_owner = atomic_load(&s_playout_diag.last_lock_owner),
        .write_max_us = atomic_load(&s_playout_diag.write_max_us),
        .slow_writes = atomic_load(&s_playout_diag.slow_writes),
        .decoded_samples = atomic_load(&s_playout_diag.decoded_samples),
        .written_samples = atomic_load(&s_playout_diag.written_samples),
        .discarded_samples = atomic_load(&s_playout_diag.discarded_samples),
        .pending_samples = atomic_load(&s_playout_diag.pending_samples),
        .chunks = atomic_load(&s_playout_diag.chunks),
        .tails = atomic_load(&s_playout_diag.tails),
        .decode_max_us = atomic_load(&s_playout_diag.decode_max_us),
        .ingress_max_us = atomic_load(&s_playout_diag.ingress_max_us),
        .write_gap_max_us = atomic_load(&s_playout_diag.write_gap_max_us),
        .service_gap_max_us = atomic_load(&s_playout_diag.service_gap_max_us),
        .pcm_in_hash = atomic_load(&s_playout_diag.pcm_in_hash),
        .pcm_out_hash = atomic_load(&s_playout_diag.pcm_out_hash),
        .pcm_epoch = atomic_load(&s_playout_diag.pcm_epoch),
        .output_samples = atomic_load(&s_playout_diag.output_samples),
        .slow_chunks = atomic_load(&s_playout_diag.slow_chunks),
        .fast_chunks = atomic_load(&s_playout_diag.fast_chunks),
        .fade_starts = atomic_load(&s_playout_diag.fade_starts),
        .dma_estimate_us = atomic_load(&s_playout_diag.dma_estimate_us),
        .rate_mode = atomic_load(&s_playout_diag.rate_mode),
    };
}

starter_media_pipeline_status_t starter_media_pipeline_status(void)
{
    starter_aec_stats_t afe;
    starter_aec_get_stats(&afe);
    starter_media_pipeline_status_t status = {
        .afe_feed_frames = afe.feed_frames, .afe_fetch_frames = afe.fetch_frames,
        .afe_input_full = afe.input_full, .afe_feed_avg_us = afe.feed_avg_us,
        .afe_feed_max_us = afe.max_feed_us, .afe_detail_enabled = afe.detail_enabled,
        .aec_calls = afe.aec_calls, .aec_avg_us = afe.aec_avg_us, .aec_max_us = afe.aec_max_us,
        .nlp_calls = afe.nlp_calls, .nlp_avg_us = afe.nlp_avg_us, .nlp_max_us = afe.nlp_max_us,
        .se_calls = afe.se_calls, .se_avg_us = afe.se_avg_us, .se_max_us = afe.se_max_us,
        .aec_run_ticks = afe.aec_run_ticks, .aec_preempt_ticks = afe.aec_preempt_ticks,
        .nlp_run_ticks = afe.nlp_run_ticks, .nlp_preempt_ticks = afe.nlp_preempt_ticks,
        .se_run_ticks = afe.se_run_ticks, .se_preempt_ticks = afe.se_preempt_ticks,
        .dsp_cycles = {afe.dsp_cycles[0], afe.dsp_cycles[1], afe.dsp_cycles[2]},
        .dsp_perf_errors = afe.dsp_perf_errors,
        .read_frames = atomic_load(&s_pipeline_diag.read_frames),
        .read_errors = atomic_load(&s_pipeline_diag.read_errors),
        .read_gap_max_us = atomic_load(&s_pipeline_diag.read_gap_max_us),
        .read_max_us = atomic_load(&s_pipeline_diag.read_max_us),
        .afe_age_max_ms = atomic_load(&s_pipeline_diag.afe_age_max_ms),
        .fetch_wait_max_us = atomic_load(&s_pipeline_diag.fetch_wait_max_us),
        .agc_max_us = atomic_load(&s_pipeline_diag.agc_max_us),
        .tx_queue_peak = atomic_load(&s_pipeline_diag.tx_queue_peak),
        .tx_age_max_us = atomic_load(&s_pipeline_diag.tx_age_max_us),
        .tx_gain_max_us = atomic_load(&s_pipeline_diag.tx_gain_max_us),
        .encode_max_us = atomic_load(&s_pipeline_diag.encode_max_us),
        .send_max_us = atomic_load(&s_pipeline_diag.send_max_us),
        .post_agc_peak = atomic_load(&s_pipeline_diag.post_agc_peak),
        .post_agc_rms = atomic_load(&s_pipeline_diag.post_agc_rms),
        .tx_peak = atomic_load(&s_pipeline_diag.tx_peak),
        .tx_rms = atomic_load(&s_pipeline_diag.tx_rms),
        .tx_clipped = atomic_load(&s_pipeline_diag.tx_clipped),
        .tx_frames = atomic_load(&s_pipeline_diag.tx_frames),
        .tx_measure_max_us = atomic_load(&s_pipeline_diag.tx_measure_max_us),
        .afe_feed_ring_full = afe.feed_ring_full, .afe_stamp_timeouts = afe.stamp_timeouts,
        .afe_failed = afe.failed,
    };
    memcpy(status.dsp_stall_pm, afe.dsp_stall_pm, sizeof(status.dsp_stall_pm));
    return status;
}

#if CONFIG_XIAOTAI_DEVELOPMENT_CONSOLE
esp_err_t starter_media_debug_afe_detail(bool enabled)
{
    return starter_aec_debug_detail(enabled);
}

void starter_media_debug_codec_timing(starter_media_codec_timing_t *out)
{
    if (out == NULL) return;
    out->tick_ready = atomic_load(&s_codec_tick_ready);
    out->tick_hz = configTICK_RATE_HZ;
    for (unsigned role = 0; role < 2U; ++role) {
        for (unsigned core = 0; core < 2U; ++core) {
            out->frames[role][core] = atomic_load_explicit(
                &s_codec_diag[role][core].frames, memory_order_acquire);
            out->avg_us[role][core] = atomic_load_explicit(
                &s_codec_diag[role][core].avg_us, memory_order_relaxed);
            out->running_ticks[role][core] = atomic_load_explicit(
                &s_codec_cpu.running[role][core], memory_order_relaxed);
        }
    }
}
#endif

starter_media_status_t starter_media_status(void)
{
    return (starter_media_status_t) {
        .active = atomic_load_explicit(&s_active, memory_order_acquire),
        .mode = (starter_tirtc_mode_t)atomic_load_explicit(&s_mode,
                                                            memory_order_acquire),
        .generation = (uint32_t)atomic_load_explicit(&s_generation,
                                                      memory_order_acquire),
        .audio_sent = (uint32_t)atomic_load_explicit(&s_audio_sent,
                                                      memory_order_acquire),
        .audio_received = (uint32_t)atomic_load_explicit(&s_audio_received,
                                                          memory_order_acquire),
        .audio_dropped = (uint32_t)atomic_load_explicit(&s_audio_dropped,
                                                         memory_order_acquire),
        .audio_decoded = (uint32_t)atomic_load_explicit(&s_audio_decoded,
                                                         memory_order_acquire),
        .audio_played = (uint32_t)atomic_load_explicit(&s_audio_played,
                                                        memory_order_acquire),
        .audio_decode_failed = (uint32_t)atomic_load_explicit(
            &s_audio_decode_failed, memory_order_acquire),
        .audio_playback_blocked = (uint32_t)atomic_load_explicit(
            &s_audio_playback_blocked, memory_order_acquire),
        .audio_write_failed = (uint32_t)atomic_load_explicit(
            &s_audio_write_failed, memory_order_acquire),
        .aec_processed = (uint32_t)atomic_load_explicit(&s_aec_processed,
                                                         memory_order_acquire),
        .aec_errors = (uint32_t)atomic_load_explicit(&s_aec_errors,
                                                      memory_order_acquire),
        .aec_mic_clipped = (uint32_t)atomic_load_explicit(&s_aec_mic_clipped,
                                                           memory_order_acquire),
        .aec_reference_clipped = (uint32_t)atomic_load_explicit(
            &s_aec_reference_clipped, memory_order_acquire),
        .aec_max_process_us = (uint32_t)atomic_load_explicit(
            &s_aec_max_process_us, memory_order_acquire),
        .aec_deadline_misses = (uint32_t)atomic_load_explicit(
            &s_aec_deadline_misses, memory_order_acquire),
        .audio_activity_level = atomic_load_explicit(
            &s_audio_activity_level, memory_order_acquire),
        .voice_active = atomic_load_explicit(&s_voice_active,
                                              memory_order_acquire),
        .speaker_volume = atomic_load_explicit(&s_speaker_volume,
                                                memory_order_acquire),
        .speaker_muted = atomic_load_explicit(&s_speaker_muted,
                                               memory_order_acquire),
        .microphone_muted = atomic_load_explicit(&s_microphone_muted,
                                                  memory_order_acquire),
    };
}
