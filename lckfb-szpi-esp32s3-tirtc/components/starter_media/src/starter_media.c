/*
 * 立创·实战派 ESP32-S3 V1.0.1 媒体适配器。
 *
 * 上行：ES7210 TDM MIC1/MIC2 + MIC3 硬件回采 -> ESP-SR AFE 16 kHz ->
 *       PCM 8 kHz mono -> G.711 A-law。
 * 下行：G.711 A-law -> PCM 8 kHz mono -> 16 kHz -> ES8311 -> NS4150B。
 *
 * ES7210 的四槽 TDM 与 ES8311 的标准 I2S 共用 MCLK/BCLK/WS。两路一次性
 * 注册为 I2S0 配对通道，由同一个控制器输出共享时钟，采集和播放可同时运行。
 * 板上的 ES8311 差分输出经电阻网络接入 ES7210 MIC3，作为同步 AEC 参考。
 */
#include "starter_media.h"
#include "starter_camera.h"
#include "starter_aec.h"
#include "starter_agc.h"
#include "starter_voice.h"
#include "starter_preroll.h"
#include "starter_playout.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "decoder/impl/esp_g711_dec.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "encoder/impl/esp_g711_enc.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es7210_adc.h"
#include "es8311_codec.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define BOARD_I2C_PORT I2C_NUM_0
#define BOARD_I2C_SDA GPIO_NUM_1
#define BOARD_I2C_SCL GPIO_NUM_2
#define BOARD_I2C_HZ 100000U

#define PCA9557_ADDRESS 0x19U
#define PCA9557_INPUT_REG 0x00U
#define PCA9557_OUTPUT_REG 0x01U
#define PCA9557_CONFIG_REG 0x03U
#define PCA9557_LCD_CS_MASK BIT(0)
#define PCA9557_PA_EN_MASK BIT(1)
#define PCA9557_DVP_PWDN_MASK BIT(2)

#define AUDIO_TRANSPORT_SAMPLE_RATE_HZ 8000U
#define AUDIO_HW_SAMPLE_RATE_HZ 16000U
#define AUDIO_PACKET_MS 20U
#define AUDIO_PACKET_SAMPLES \
    ((AUDIO_TRANSPORT_SAMPLE_RATE_HZ * AUDIO_PACKET_MS) / 1000U)
#define AUDIO_TDM_SLOTS 4U
#define AUDIO_MCLK_MULTIPLE 256U
#define AUDIO_MCLK_HZ (AUDIO_HW_SAMPLE_RATE_HZ * AUDIO_MCLK_MULTIPLE)
#define AUDIO_RX_BYTES 1500U
#define AUDIO_RX_QUEUE_DEPTH STARTER_PLAYOUT_CAPACITY
#define AUDIO_TX_QUEUE_DEPTH 12U
#define AUDIO_PLAYBACK_UPSAMPLE 2U
#define AUDIO_PLAYBACK_I2S_VALUES_PER_INPUT (AUDIO_PLAYBACK_UPSAMPLE * 2U)
#define AUDIO_OUTPUT_LOCK_WAIT_MS 500U
#define AUDIO_PLAYOUT_POLL_MS 5U
#define NS4150B_STARTUP_MS 120U
#define MEDIA_CPU_YIELD_MS 1U
#define MEDIA_REALTIME_CORE 1
#define PREROLL_CAPTURE_LOCK_WAIT_MS 2U

_Static_assert(AUDIO_HW_SAMPLE_RATE_HZ == STARTER_AEC_SAMPLE_RATE_HZ,
               "I2S and AEC sample rates must match");
_Static_assert(AUDIO_TRANSPORT_SAMPLE_RATE_HZ ==
                   STARTER_AEC_TRANSPORT_RATE_HZ,
               "AEC output and TiRTC sample rates must match");
_Static_assert(AUDIO_PACKET_SAMPLES == STARTER_PLAYOUT_PCM_SAMPLES,
               "playout quantum must be 20 ms at 8 kHz");
_Static_assert(AUDIO_TDM_SLOTS == 4U,
               "ES7210 physical TDM wire layout must remain four-slot");
_Static_assert(STARTER_AEC_CAPTURE_DMA_CHANNELS == 4U,
               "full ES7210 slot mask must retain both microphones and reference");

#define I2S_MCLK GPIO_NUM_38
#define I2S_BCLK GPIO_NUM_14
#define I2S_WS GPIO_NUM_13
#define I2S_ADC_DIN GPIO_NUM_12
#define I2S_DAC_DOUT GPIO_NUM_45
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
 * 20 ms、8 kHz、16-bit 单声道 PCM 包；池放在 PSRAM，队列只传递索引。
 */
typedef struct {
    starter_tirtc_mode_t mode;
    uint32_t generation;
    uint32_t timestamp_ms;
    uint32_t enqueued_us; /* Monotonic low 32 bits; queue budget is far below its wrap. */
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
static atomic_bool s_preroll_gap;
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
    atomic_uint_fast32_t afe_age_max_ms, fetch_wait_max_us, agc_max_us, resample_max_us;
    atomic_uint_fast32_t tx_queue_peak, tx_age_max_us, tx_gain_max_us, encode_max_us, send_max_us;
    atomic_uint_fast32_t post_agc_peak, post_agc_rms, tx_peak, tx_rms, tx_clipped, tx_frames, tx_measure_max_us;
} s_pipeline_diag;
_Static_assert(sizeof(s_pipeline_diag) == 80U, "S3 pipeline diagnostics PSRAM budget changed");

/* Room-only observation, no pacing/gain/gate changes. Counters are lifetime
 * values. TX/RX each own their last-* fields; runtime owns last_log_ms. No
 * allocation, payload logging, SDK query or serial output on the hot paths. */
EXT_RAM_BSS_ATTR static struct {
    atomic_uint_fast32_t tx_enqueued, tx_full, tx_stale, tx_encode_error, tx_send_error;
    atomic_uint_fast32_t tx_pairs, tx_burst, tx_gaps, tx_gap_max_us, tx_accepted, tx_gain_error;
    atomic_uint_fast32_t ptt_down, ptt_up, capture_stale;
    atomic_uint_fast32_t rx_frames, rx_bytes, rx_pairs, rx_gap_max_us, rx_burst, rx_gaps;
    atomic_uint_fast32_t rx_repeat_ts, rx_forward_gap, rx_back_ts;
    atomic_uint rx_observer_busy;
    atomic_uint_fast32_t rx_observer_skipped;
    uint32_t tx_last_us, tx_generation, tx_epoch;
    uint32_t rx_last_us, rx_last_timestamp, rx_last_len, rx_generation, last_log_ms;
} s_room_audio_diag;
_Static_assert(sizeof(s_room_audio_diag) == 132U, "Room diagnostics PSRAM budget changed");

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
/* 单调序号避免“清除取消标志”与新铃声启动之间的竞态。 */
static atomic_uint_fast32_t s_pcm8k_cancel_sequence;

static audio_rx_item_t *s_audio_rx_pool;
static audio_tx_item_t *s_audio_tx_pool;
static QueueHandle_t s_audio_rx_ready_queue;
static QueueHandle_t s_audio_rx_free_queue;
static QueueHandle_t s_audio_tx_ready_queue;
static QueueHandle_t s_audio_tx_free_queue;
static SemaphoreHandle_t s_audio_output_mutex;
static i2s_chan_handle_t s_i2s_rx;
static i2s_chan_handle_t s_i2s_tx;
static bool s_amp_enabled;
/* The codec devices own the shared I2S configuration.  ES8311 playback and
 * ES7210 TDM capture must never reconfigure the paired channels independently. */
static const audio_codec_data_if_t *s_i2s_data_if;
static const audio_codec_ctrl_if_t *s_es7210_ctrl_if;
static const audio_codec_ctrl_if_t *s_es8311_ctrl_if;
static const audio_codec_gpio_if_t *s_es8311_gpio_if;
static const audio_codec_if_t *s_es7210_codec_if;
static const audio_codec_if_t *s_es8311_codec_if;
static esp_codec_dev_handle_t s_microphone_dev;
static esp_codec_dev_handle_t s_speaker_dev;
static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_pca9557_i2c_dev;
static void *s_g711_encoder;
static void *s_g711_decoder;
static uint8_t s_pca9557_output;

/* 下行会话和启动期绑定播报复用这些缓冲，不放到任务栈上。 */
EXT_RAM_BSS_ATTR static int16_t s_decode_pcm[AUDIO_RX_BYTES];
EXT_RAM_BSS_ATTR static starter_playout_pcm_t s_rx_pcm;
_Static_assert(sizeof(s_rx_pcm) == 490U, "S3 PCM staging PSRAM budget changed");
EXT_RAM_BSS_ATTR static struct {
    int16_t samples[STARTER_PLAYOUT_PCM_SAMPLES];
    uint16_t fade_remaining;
} s_rx_render;
_Static_assert(sizeof(s_rx_render) == 322U, "S3 render scratch PSRAM budget changed");
/* ES8311 consumes ordinary interleaved 16-bit stereo PCM.  Keep this exactly
 * aligned with the validated device-monitor board adapter. */
EXT_RAM_BSS_ATTR static int16_t
    s_play_stereo[AUDIO_RX_BYTES * AUDIO_PLAYBACK_I2S_VALUES_PER_INPUT];
static uint32_t s_playback_resampler_generation;
EXT_RAM_BSS_ATTR static uint32_t s_playback_resampler_epoch;
static int16_t s_playback_previous;

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

static bool uplink_session_current(starter_tirtc_mode_t mode, uint32_t generation)
{
    /* EOS retains speaker ownership only, never microphone/network admission. */
    return same_session(mode, generation) && atomic_load(&s_drain_generation) == 0 &&
           (mode != STARTER_TIRTC_ROOM || atomic_load(&s_room_ptt_generation) == generation);
}

bool starter_media_room_ptt(uint32_t generation, bool pressed)
{
    if (!pressed) {
        unsigned expected = generation;
        if (generation == 0 || atomic_compare_exchange_strong(&s_room_ptt_generation, &expected, 0)) {
            atomic_fetch_add(&s_room_audio_diag.ptt_up, 1);
            if (generation == 0) atomic_store(&s_room_ptt_generation, 0);
            atomic_fetch_add(&s_mute_epoch, 1);
        }
        return true;
    }
    if (!generation || !same_session(STARTER_TIRTC_ROOM, generation) ||
        atomic_load(&s_microphone_muted)) return false;
    atomic_fetch_add(&s_room_audio_diag.ptt_down, 1);
    atomic_fetch_add(&s_mute_epoch, 1);
    atomic_store(&s_room_ptt_generation, generation);
    return true;
}

static esp_err_t pca9557_write_register(uint8_t reg, uint8_t value)
{
    uint8_t bytes[2] = {reg, value};
    if (s_pca9557_i2c_dev == NULL) {
        i2c_device_config_t device = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = PCA9557_ADDRESS,
            .scl_speed_hz = BOARD_I2C_HZ,
        };
        esp_err_t err = i2c_master_bus_add_device(s_i2c_bus,
                                                   &device,
                                                   &s_pca9557_i2c_dev);
        if (err != ESP_OK) {
            return err;
        }
    }
    return i2c_master_transmit(s_pca9557_i2c_dev,
                               bytes,
                               sizeof(bytes),
                               100);
}

static esp_err_t pca9557_set_output(uint8_t mask, bool high)
{
    /* All runtime callers hold the output mutex. Commit the shadow only
     * after I2C succeeds; a camera PWDN write must preserve LCD/PA bits. */
    uint8_t value = high ? s_pca9557_output | mask : s_pca9557_output & (uint8_t)~mask;
    esp_err_t err = pca9557_write_register(PCA9557_OUTPUT_REG, value);
    if (err == ESP_OK) s_pca9557_output = value;
    return err;
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

    /* LCD_CS=1、PA_EN=0、DVP_PWDN=1；I/O0..2 为输出，其余保持输入。 */
    s_pca9557_output = PCA9557_LCD_CS_MASK | PCA9557_DVP_PWDN_MASK;
    err = pca9557_write_register(PCA9557_OUTPUT_REG, s_pca9557_output);
    if (err != ESP_OK) {
        return err;
    }
    return pca9557_write_register(PCA9557_CONFIG_REG, 0xF8U);
}

static esp_err_t audio_i2s_init(void)
{
    /*
     * ESP32-S3 I2S v2 支持在一次 i2s_new_channel() 中注册配对 TX/RX，硬件
     * 会共享 BCLK/WS。ES7210 RX 使用四槽 TDM，而 ES8311 TX 保持板级
     * 参考实现验证过的 16-bit stereo standard-I2S；两路共享 256 Fs MCLK。
     */
    i2s_chan_config_t audio_channel =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_AUDIO_PORT, I2S_ROLE_MASTER);
    audio_channel.auto_clear = true;
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
            I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_STEREO,
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
    return ESP_OK;
}

static esp_err_t audio_i2s_start(void)
{
    /* esp_codec_dev opens both directions as one duplex transaction.  Calling
     * i2s_channel_enable here would bypass its paired-channel state machine. */
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

    /* The PCA9557 and codecs share the driver_ng I2C family.  Codec
     * addresses remain in the library's 8-bit wire representation. */
    audio_codec_i2c_cfg_t es8311_i2c = {
        .port = BOARD_I2C_PORT,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = s_i2c_bus,
        .clock_speed_hz = BOARD_I2C_HZ,
    };
    s_es8311_ctrl_if = audio_codec_new_i2c_ctrl(&es8311_i2c);
    s_es8311_gpio_if = audio_codec_new_gpio();
    if (s_es8311_ctrl_if == NULL || s_es8311_gpio_if == NULL) {
        return ESP_ERR_NO_MEM;
    }
    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = s_es8311_ctrl_if,
        .gpio_if = s_es8311_gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = GPIO_NUM_NC,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .mclk_div = AUDIO_MCLK_MULTIPLE,
    };
    s_es8311_codec_if = es8311_codec_new(&es8311_cfg);
    if (s_es8311_codec_if == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_codec_dev_cfg_t speaker_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = s_es8311_codec_if,
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
        esp_codec_dev_set_out_mute(s_speaker_dev, false) != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }

    audio_codec_i2c_cfg_t es7210_i2c = {
        .port = BOARD_I2C_PORT,
        .addr = ES7210_CODEC_DEFAULT_ADDR | 0x02U,
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
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 |
                        ES7210_SEL_MIC3 | ES7210_SEL_MIC4,
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
    /* Capture both people microphones without changing the shared BCLK/WS.
     * The physical order is MIC1/MIC3(reference)/MIC2/MIC4(unused). */
    esp_codec_dev_sample_info_t microphone_format = {
        .sample_rate = AUDIO_HW_SAMPLE_RATE_HZ,
        .bits_per_sample = 16,
        .channel = AUDIO_TDM_SLOTS,
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
    return ESP_OK;
}

static esp_err_t g711_init(void)
{
    esp_g711_enc_config_t encoder = ESP_G711_ENC_CONFIG_DEFAULT();
    encoder.sample_rate = ESP_AUDIO_SAMPLE_RATE_8K;
    encoder.channel = ESP_AUDIO_MONO;
    encoder.bits_per_sample = ESP_AUDIO_BIT16;
    encoder.frame_duration = AUDIO_PACKET_MS;
    if (esp_g711a_enc_open(&encoder, sizeof(encoder), &s_g711_encoder) !=
        ESP_AUDIO_ERR_OK) {
        return ESP_FAIL;
    }
    if (esp_g711_dec_open(NULL, 0, &s_g711_decoder) != ESP_AUDIO_ERR_OK) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* 调用者必须持有 s_audio_output_mutex。 */
static esp_err_t audio_output_set_locked(bool enabled)
{
    if (enabled == s_amp_enabled) {
        return ESP_OK;
    }
    esp_err_t err = pca9557_set_output(PCA9557_PA_EN_MASK, enabled);
    if (err != ESP_OK) {
        return err;
    }
    s_amp_enabled = enabled;
    if (enabled) {
        /* NS4150B V1.1 给出的典型启动时间为 120 ms。 */
        vTaskDelay(pdMS_TO_TICKS(NS4150B_STARTUP_MS));
    }
    return ESP_OK;
}

static bool enqueue_uplink_pcm(starter_tirtc_mode_t mode,
                               uint32_t generation,
                               uint32_t timestamp_ms,
                               const int16_t *pcm, unsigned mute_epoch)
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
    uint8_t alaw[AUDIO_PACKET_SAMPLES];
    for (;;) {
        uint8_t slot = 0;
        if (xQueueReceive(s_audio_tx_ready_queue, &slot, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        audio_tx_item_t *item = &s_audio_tx_pool[slot];
        update_max_counter(&s_pipeline_diag.tx_age_max_us,
                           (uint32_t)esp_timer_get_time() - item->enqueued_us);
        bool ready = uplink_session_current(item->mode, item->generation) &&
                     item->mute_epoch == atomic_load(&s_mute_epoch) &&
                     starter_tirtc_audio_ready() &&
                     !atomic_load_explicit(&s_microphone_muted, memory_order_acquire);
        if (!ready && item->mode == STARTER_TIRTC_ROOM)
            atomic_fetch_add(&s_room_audio_diag.tx_stale, 1);
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
            esp_audio_enc_in_frame_t input = {
                .buffer = (uint8_t *)item->pcm,
                .len = sizeof(item->pcm),
            };
            esp_audio_enc_out_frame_t output = {
                .buffer = alaw,
                .len = sizeof(alaw),
            };
            int64_t encode_begin = esp_timer_get_time();
            esp_err_t encoded = esp_g711_enc_process(s_g711_encoder, &input, &output);
            update_max_counter(&s_pipeline_diag.encode_max_us,
                               (uint32_t)(esp_timer_get_time() - encode_begin));
            if (encoded == ESP_AUDIO_ERR_OK && output.encoded_bytes == AUDIO_PACKET_SAMPLES) {
                if (!uplink_session_current(item->mode, item->generation) ||
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
                int ret = starter_tirtc_send_alaw(item->timestamp_ms,
                                                   alaw,
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
                                 "uplink first packet mode=%d bytes=%u timestamp=%lu",
                                 (int)item->mode,
                                 (unsigned)output.encoded_bytes,
                                 (unsigned long)item->timestamp_ms);
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
                ESP_LOGW(TAG, "uplink A-law encode failed ret=%s bytes=%u",
                         esp_err_to_name(encoded), (unsigned)output.encoded_bytes);
            }
        }
release_tx_slot:
        memset(item, 0, sizeof(*item));
        (void)xQueueSend(s_audio_tx_free_queue, &slot, 0);
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
            esp_codec_dev_read(s_microphone_dev, capture, capture_bytes) == ESP_CODEC_DEV_OK;
        int64_t read_end = esp_timer_get_time();
        update_max_counter(&s_pipeline_diag.read_max_us, (uint32_t)(read_end - read_begin));
        esp_err_t err = ESP_FAIL;
        if (read_ok) {
            atomic_fetch_add(&s_pipeline_diag.read_frames, 1);
            err = starter_aec_submit_capture(capture_bytes, read_end / 1000,
                                             capture_epoch, mute_epoch);
        } else {
            atomic_fetch_add(&s_pipeline_diag.read_errors, 1);
        }
        if (err != ESP_OK) {
            atomic_fetch_add_explicit(&s_aec_errors, 1, memory_order_relaxed);
            atomic_store(&s_preroll_gap, true);
            vTaskDelay(pdMS_TO_TICKS(MEDIA_CPU_YIELD_MS));
        }
    }
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
        bool session_ready = uplink_session_current(mode, generation) &&
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

        starter_aec_output_t clean = {0};
        esp_err_t aec_err = starter_aec_fetch(&clean);
        update_max_counter(&s_pipeline_diag.fetch_wait_max_us, clean.fetch_wait_us);
        update_max_counter(&s_pipeline_diag.agc_max_us, clean.agc_us);
        update_max_counter(&s_pipeline_diag.resample_max_us, clean.resample_us);
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
            atomic_store(&s_preroll_gap, true);
            starter_agc_discard_pending();
            continue;
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
            if (atomic_exchange(&s_preroll_gap, false)) {
                if (s_preroll.token != 0) s_preroll.failed = true;
                else starter_preroll_clear(&s_preroll);
            }
            bool muted = atomic_load(&s_microphone_muted);
            if (muted) {
                starter_preroll_clear(&s_preroll);
            } else if (s_preroll.token != 0 ||
                       (!atomic_load(&s_active) && atomic_load(&s_wake_allowed))) {
                starter_preroll_append(&s_preroll, clean.pcm_8k, clean.samples, captured_ms);
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
            atomic_store(&s_preroll_gap, true);
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
        for (size_t i = 0; i < clean.samples; ++i) {
            int32_t sample = clean.pcm_8k[i];
            square_sum += (uint64_t)((int64_t)sample * sample);
        }
        uint32_t energy = clean.samples == 0U ? 0U :
                          (uint32_t)sqrtf((float)square_sum /
                                          (float)clean.samples);
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
        session_ready = uplink_session_current(mode, generation) &&
                        starter_tirtc_audio_ready() &&
                        !atomic_load_explicit(&s_microphone_muted,
                                              memory_order_acquire);
        if (!session_ready) {
            continue;
        }

        bool replay = false;
        bool replay_completed = false;
        if (mode == STARTER_TIRTC_AI) {
            /* Catch up, then transfer the partial packet exactly once to live. */
            if (xSemaphoreTake(s_preroll_mutex, pdMS_TO_TICKS(PREROLL_CAPTURE_LOCK_WAIT_MS)) != pdTRUE) continue;
            replay = s_expected_wake_token != 0;
            if (replay) {
                packet_samples = 0;
                for (unsigned packet = 0; packet < 3U; ++packet) {
                    uint32_t timestamp;
                    if (!starter_preroll_peek(&s_preroll, generation, packet_pcm,
                                              AUDIO_PACKET_SAMPLES, &timestamp) ||
                        !enqueue_uplink_pcm(mode, generation, timestamp, packet_pcm, mute_epoch)) break;
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

        for (size_t i = 0; i < clean.samples; ++i) {
            packet_pcm[packet_samples++] = clean.pcm_8k[i];
            if (packet_samples != AUDIO_PACKET_SAMPLES) {
                continue;
            }
            packet_samples = 0;
            if (!uplink_session_current(mode, generation) || !starter_tirtc_audio_ready()) {
                continue;
            }
            if (next_timestamp_ms == 0U) {
                next_timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
            }
            (void)enqueue_uplink_pcm(mode, generation, next_timestamp_ms, packet_pcm, mute_epoch);
            next_timestamp_ms += AUDIO_PACKET_MS;
        }
    }
}

static size_t decode_audio_item(const audio_rx_item_t *item)
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
    esp_audio_dec_out_frame_t output = {
        .buffer = (uint8_t *)s_decode_pcm,
        .len = sizeof(s_decode_pcm),
    };
    esp_audio_dec_info_t info = {0};
    int64_t begin = esp_timer_get_time();
    esp_audio_err_t result = esp_g711a_dec_decode(s_g711_decoder, &input, &output, &info);
    update_max_counter(&s_playout_diag.decode_max_us, (uint32_t)(esp_timer_get_time() - begin));
    if (result != ESP_AUDIO_ERR_OK || output.decoded_size > sizeof(s_decode_pcm) ||
        output.decoded_size != item->frame.length * sizeof(int16_t)) {
        atomic_fetch_add_explicit(&s_audio_decode_failed, 1, memory_order_relaxed);
        return 0;
    }
    atomic_fetch_add_explicit(&s_audio_decoded, 1, memory_order_relaxed);
    size_t mono_samples = output.decoded_size / sizeof(int16_t);
    atomic_fetch_add(&s_playout_diag.decoded_samples, mono_samples);
    atomic_store(&s_playout_diag.pcm_in_hash,
                 starter_playout_pcm_hash(atomic_load(&s_playout_diag.pcm_in_hash),
                                          s_decode_pcm, mono_samples));
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
        /* A restart must not interpolate from the last sample before silence:
         * that stale midpoint would bypass the new 5 ms fade-in. */
        if (restart || s_playback_resampler_generation != generation ||
            s_playback_resampler_epoch != epoch) {
            s_playback_resampler_generation = generation;
            s_playback_resampler_epoch = epoch;
            s_playback_previous = pcm[0];
        }
        for (size_t i = 0; i < mono_samples; ++i) {
            int16_t current = pcm[i];
            int16_t midpoint = (int16_t)(((int32_t)s_playback_previous + current) / 2);
            size_t output_index = i * AUDIO_PLAYBACK_I2S_VALUES_PER_INPUT;
            /* Keep the existing chronological interpolation and gain. */
            s_play_stereo[output_index] = midpoint;
            s_play_stereo[output_index + 1U] = midpoint;
            s_play_stereo[output_index + 2U] = current;
            s_play_stereo[output_index + 3U] = current;
            s_playback_previous = current;
        }
        size_t bytes = mono_samples *
                       AUDIO_PLAYBACK_I2S_VALUES_PER_INPUT * sizeof(int16_t);
        int64_t write_begin = esp_timer_get_time();
        played = s_speaker_dev != NULL &&
                 esp_codec_dev_write(s_speaker_dev, s_play_stereo, bytes) ==
                     ESP_CODEC_DEV_OK;
        uint32_t write_us = (uint32_t)(esp_timer_get_time() - write_begin);
        update_max_counter(&s_playout_diag.write_max_us, write_us);
        /* A blocking I2S write normally paces one frame; only excess time is
         * suspicious. This is not a measurement of actual DAC underruns. */
        if (write_us > mono_samples * 125U + 20000U)
            atomic_fetch_add(&s_playout_diag.slow_writes, 1);
        if (!played) {
            atomic_fetch_add_explicit(&s_audio_write_failed, 1,
                                      memory_order_relaxed);
            /* Partial codec writes have unknown progress. Do not replay this
             * block, and do not interpolate from its unconfirmed last sample. */
            s_playback_resampler_generation = 0;
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
    uint32_t queued_ms = (q->queued_us + pending_samples * 125U + 999U) / 1000U;
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
            /* A-law 8 kHz mono: one encoded byte represents 125 us of audio.
             * This works for 20/40/80 ms packets, not just a fixed packet count. */
            if (!starter_playout_push(&playout, slot, item->frame.length * 125U,
                                      item->frame.timestamp_ms, item->frame.stream_id,
                                      item->received_us)) {
                atomic_fetch_add(&s_playout_diag.overflow, 1);
                atomic_fetch_add(&s_audio_dropped, 1);
                release_audio_rx_slot(slot, false);
            }
        }
        uint64_t now = esp_timer_get_time();
        update_max_counter(&s_playout_diag.queued_peak_ms,
                           (playout.queued_us +
                            (decoded_count - decoded_offset + s_rx_pcm.count) * 125U + 999U) / 1000U);
        unsigned decoded_this_turn = 0;
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
            decoded_count = decode_audio_item(&s_audio_rx_pool[slot]);
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
            /* Give lower-priority volume/mute/UI owners a chance to acquire
             * the released mutex. I2S still supplies the playback clock;
             * never add a second 20 ms sleep after a blocking 20 ms write. */
            vTaskDelay(pdMS_TO_TICKS(MEDIA_CPU_YIELD_MS));
        } else {
            /* No new task and no busy spin. Stop/mute/session changes are seen
             * within this short wait even when the network sends no more data. */
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
            /* Refill I2S first. Log outside the mutex, never per frame. */
            ESP_LOGI(TAG, "JB target=%lu q=%lu jitter=%lu gap=%lu late=%lu ovf=%lu lock=%lu/%lu owner=%lu wr=%lu/%lu pcm=%lu/%lu/%lu+%lu",
                     (unsigned long)playout.target_ms, (unsigned long)atomic_load(&s_playout_diag.queued_ms),
                     (unsigned long)(playout.jitter_us / 1000U), (unsigned long)(playout.gap_max_us / 1000U),
                     (unsigned long)playout.late_bursts, (unsigned long)atomic_load(&s_playout_diag.overflow),
                     (unsigned long)atomic_load(&s_playout_diag.lock_timeouts),
                     (unsigned long)(atomic_load(&s_playout_diag.lock_max_us) / 1000U),
                     (unsigned long)atomic_load(&s_playout_diag.last_lock_owner),
                     (unsigned long)atomic_load(&s_playout_diag.slow_writes),
                     (unsigned long)(atomic_load(&s_playout_diag.write_max_us) / 1000U),
                     (unsigned long)atomic_load(&s_playout_diag.decoded_samples),
                     (unsigned long)atomic_load(&s_playout_diag.written_samples),
                     (unsigned long)atomic_load(&s_playout_diag.discarded_samples),
                     (unsigned long)atomic_load(&s_playout_diag.pending_samples));
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
        !esp_ptr_external_ram(s_decode_pcm) ||
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
    err = audio_codecs_init();
    if (err == ESP_OK) err = audio_i2s_start();
    if (err != ESP_OK) return err;
    /* These long-running workers use only PSRAM-backed frame pools and board
     * drivers. Keeping their stacks in PSRAM preserves internal/DMA SRAM for
     * Wi-Fi and TiRTC bootstrap allocations. */
    if (xTaskCreateWithCaps(audio_sink_task,
                            "board_audio_rx",
                            6144,
                            NULL,
                            8,
                            NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS ||
        xTaskCreateWithCaps(audio_uplink_task,
                            "rtc_audio_tx",
                            6144,
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
             "audio ready: PSRAM rx/tx=%u/%u B, ES7210/ES8311 A-law, "
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
    if (!take_audio_output_lock(OUTPUT_OWNER_LCD, pdMS_TO_TICKS(500))) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = pca9557_set_output(PCA9557_LCD_CS_MASK, !selected);
    give_audio_output_lock();
    return err;
}

esp_err_t starter_media_camera_power(bool enabled)
{
    if (!atomic_load(&s_ready)) return ESP_ERR_INVALID_STATE;
    /* Do not hold this mutex while probing SCCB, waiting for DMA or encoding. */
    if (!take_audio_output_lock(8, pdMS_TO_TICKS(100))) return ESP_ERR_TIMEOUT;
    esp_err_t err = pca9557_set_output(PCA9557_DVP_PWDN_MASK, !enabled);
    give_audio_output_lock();
    return err;
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
    if (s_preroll_mutex == NULL || xSemaphoreTake(s_preroll_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return 0;
    uint32_t token = 0;
    if (!atomic_load(&s_microphone_muted) && !atomic_load(&s_active) &&
        atomic_load(&s_wake_allowed)) {
        token = starter_preroll_prepare(&s_preroll, captured_ms, esp_timer_get_time() / 1000);
    }
    xSemaphoreGive(s_preroll_mutex);
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
        .buffered_ms = (uint32_t)(s_preroll.count / 8U),
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
            size_t output_index = i * AUDIO_PLAYBACK_I2S_VALUES_PER_INPUT;
            s_play_stereo[output_index] = current;
            s_play_stereo[output_index + 1U] = current;
            s_play_stereo[output_index + 2U] = midpoint;
            s_play_stereo[output_index + 3U] = midpoint;
            previous = current;
        }
        size_t bytes = chunk_samples * AUDIO_PLAYBACK_I2S_VALUES_PER_INPUT *
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
                 AUDIO_TRANSPORT_SAMPLE_RATE_HZ);
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
        /* Video failure is visible in camera status; preserve H5 audio. */
        if (camera_err != ESP_OK)
            ESP_LOGE(TAG, "H5 camera worker unavailable: %s", esp_err_to_name(camera_err));
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
                else if (step * (uint64_t)1000U > s_room_audio_diag.rx_last_len * 125ULL + 2000U)
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
    if (xQueueReceive(s_audio_rx_free_queue, &slot, 0) != pdTRUE ||
        slot >= AUDIO_RX_QUEUE_DEPTH) {
        atomic_fetch_add_explicit(&s_audio_dropped, 1, memory_order_relaxed);
        atomic_fetch_add(&s_playout_diag.overflow, 1);
        return;
    }
    audio_rx_item_t *item = &s_audio_rx_pool[slot];
    *item = (audio_rx_item_t) {
        .received_us = received_us,
        .mode = mode,
        .generation = generation,
        .epoch = epoch,
        .frame = *frame,
    };
    memcpy(item->payload, data, frame->length);
    if (xQueueSend(s_audio_rx_ready_queue, &slot, 0) == pdTRUE) {
        atomic_fetch_add_explicit(&s_audio_received, 1, memory_order_relaxed);
    } else {
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
    ESP_LOGI(TAG, "ROOM CAP t=%lu gen=%lu ptt=%d calls=%lu/%lu read=%lu err=%lu afe=%lu/%lu full=%lu timeout=%lu stale=%lu age=%lu",
             (unsigned long)now, (unsigned long)atomic_load(&s_generation),
             atomic_load(&s_room_ptt_generation) != 0, RA(ptt_down), RA(ptt_up), PA(read_frames), PA(read_errors),
             (unsigned long)afe.feed_frames, (unsigned long)afe.fetch_frames,
             (unsigned long)afe.input_full, (unsigned long)afe.fetch_timeouts, RA(capture_stale), PA(afe_age_max_ms));
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
    return (starter_media_pipeline_status_t) {
        .read_frames = atomic_load(&s_pipeline_diag.read_frames),
        .read_errors = atomic_load(&s_pipeline_diag.read_errors),
        .read_gap_max_us = atomic_load(&s_pipeline_diag.read_gap_max_us),
        .read_max_us = atomic_load(&s_pipeline_diag.read_max_us),
        .afe_age_max_ms = atomic_load(&s_pipeline_diag.afe_age_max_ms),
        .fetch_wait_max_us = atomic_load(&s_pipeline_diag.fetch_wait_max_us),
        .agc_max_us = atomic_load(&s_pipeline_diag.agc_max_us),
        .resample_max_us = atomic_load(&s_pipeline_diag.resample_max_us),
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
    };
}

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
