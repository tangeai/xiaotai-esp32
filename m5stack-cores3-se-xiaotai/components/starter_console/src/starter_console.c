/*
 * 开发期串口控制台。
 *
 * 这个模块是人工操作的 adapter：只解析参数、调用公开接口并打印结果。
 * 它不拥有 Wi-Fi、凭证或会话状态。量产按键/UI 应调用相同接口，而不是复用
 * ESP console 的命令处理函数。
 */
#include "starter_console.h"
#include "sdkconfig.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_rom_crc.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "platform_client.h"
#include "runtime_config.h"
#include "starter_media.h"
#include "starter_product.h"
#include "starter_runtime.h"
#include "starter_tirtc.h"
#include "starter_voice.h"
#include "starter_board.h"
#include "wifi_manager.h"
#if CONFIG_IDF_TARGET_ESP32S3
#include "soc/soc.h"
#include "soc/extmem_reg.h"
#endif

static TaskHandle_t s_console_task;
static EXT_RAM_BSS_ATTR esp_err_t s_console_output_error;

static esp_err_t console_write_record(const char *record, size_t length)
{
    if (!record || !length) return ESP_ERR_INVALID_SIZE;
    const TickType_t timeout = pdMS_TO_TICKS(500);
    flockfile(stdout);
    esp_err_t err = fflush(stdout) == 0 ? ESP_OK : ESP_FAIL;
    if (err == ESP_OK) err = usb_serial_jtag_wait_tx_done(timeout);
    /* Keep the whole record ordered while draining chunks smaller than the
     * USB ring. The VFS character writer otherwise hides its timeout loss. */
    for (size_t offset = 0; err == ESP_OK && offset < length;) {
        size_t count = length - offset;
        if (count > 224U) count = 224U;
        if (usb_serial_jtag_write_bytes(record + offset, count, timeout) != (int)count)
            err = ESP_ERR_TIMEOUT;
        if (err == ESP_OK) err = usb_serial_jtag_wait_tx_done(timeout);
        offset += count;
    }
    funlockfile(stdout);
    return err;
}

static int console_printf(const char *format, ...)
{
    /* This buffer lives on the existing PSRAM console stack, not the heap. */
    char record[1024];
    va_list args;
    va_start(args, format);
    int formatted = vsnprintf(record, sizeof(record), format, args);
    va_end(args);
    if (formatted < 0 || (size_t)formatted >= sizeof(record)) {
        s_console_output_error = ESP_ERR_INVALID_SIZE;
        return -1;
    }
    if (!formatted) return 0;
    size_t length = (size_t)formatted, extra = 0;
    for (size_t i = 0; i < length; ++i)
        if (record[i] == '\n' && (i == 0 || record[i - 1] != '\r')) ++extra;
    if (length + extra >= sizeof(record)) {
        s_console_output_error = ESP_ERR_INVALID_SIZE;
        return -1;
    }
    size_t wire_length = length + extra, destination = wire_length;
    for (size_t source = length; source;) {
        char ch = record[--source];
        record[--destination] = ch;
        if (ch == '\n' && (source == 0 || record[source - 1] != '\r'))
            record[--destination] = '\r';
    }
    esp_err_t err = console_write_record(record, wire_length);
    if (err != ESP_OK) {
        s_console_output_error = err;
        return -1;
    }
    return formatted;
}

static void print_firmware_identity(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    char elf_sha256[65];
    (void)esp_app_get_elf_sha256(elf_sha256, sizeof(elf_sha256));
    console_printf("Firmware: version=%s built=%s %s elf-sha256=%s\n",
           app->version,
           app->date,
           app->time,
           elf_sha256);
}

static int command_version(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        console_printf("usage: version\n");
        return 1;
    }
    print_firmware_identity();
    return 0;
}

static int at_result(esp_err_t error);

static int command_cache(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) return at_result(ESP_ERR_INVALID_ARG);
#if CONFIG_IDF_TARGET_ESP32S3
    /* Read-only shared counters; snapshots are sequential, not per-task or
     * atomic. Do not reset counters or change cache/filter configuration. */
    const int64_t started = esp_timer_get_time();
    const uint32_t data = REG_READ(EXTMEM_DBUS_ACS_CNT_REG);
    const uint32_t psram_miss = REG_READ(EXTMEM_DBUS_ACS_SPIRAM_MISS_CNT_REG);
    const uint32_t flash_miss = REG_READ(EXTMEM_DBUS_ACS_FLASH_MISS_CNT_REG);
    const uint32_t instruction = REG_READ(EXTMEM_IBUS_ACS_CNT_REG);
    const uint32_t instruction_miss = REG_READ(EXTMEM_IBUS_ACS_MISS_CNT_REG);
    const uint32_t flags = REG_READ(EXTMEM_CACHE_ILG_INT_ST_REG);
    const uint32_t data_start = REG_READ(EXTMEM_DBUS_TO_FLASH_START_VADDR_REG);
    const uint32_t data_end = REG_READ(EXTMEM_DBUS_TO_FLASH_END_VADDR_REG);
    const uint32_t instruction_start = REG_READ(EXTMEM_IBUS_TO_FLASH_START_VADDR_REG);
    const uint32_t instruction_end = REG_READ(EXTMEM_IBUS_TO_FLASH_END_VADDR_REG);
    const int64_t ended = esp_timer_get_time();
    int written = console_printf("+CACHE:t-us=%lld,read-us=%lld,data=%lu,psram-miss=%lu,"
                          "flash-miss=%lu,instruction=%lu,instruction-miss=%lu,"
                          "flags=%08lx,data-filter=%08lx-%08lx,"
                          "instruction-filter=%08lx-%08lx\n",
                          (long long)started, (long long)(ended - started),
                          (unsigned long)data, (unsigned long)psram_miss,
                          (unsigned long)flash_miss, (unsigned long)instruction,
                          (unsigned long)instruction_miss, (unsigned long)flags,
                          (unsigned long)data_start, (unsigned long)data_end,
                          (unsigned long)instruction_start,
                          (unsigned long)instruction_end);
    return at_result(written < 0 ? ESP_FAIL : ESP_OK);
#else
    return at_result(ESP_ERR_NOT_SUPPORTED);
#endif
}

/* [DEBUG-wake49] Console-only output: never print from the capture/model hot path. */
static void print_wake_diagnostics(void)
{
    starter_media_audio_diagnostics_t audio;
    starter_media_audio_diagnostics(&audio);
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    starter_voice_status_t v = starter_voice_status();
    console_printf("Voicute: ready=%d init-error=%d listening=%d threshold=%u/1000 frontend=%s "
           "fed=%lu dropped=%lu resets=%lu inference=%lu hits=%lu probability=%lu/1000 "
           "avg/max-us=%lu/%lu over-hop=%lu reject-epoch/age/error=%lu/%lu/%lu\n",
           v.ready, v.init_error, v.listening, v.threshold_milli,
#if CONFIG_XIAOTAI_WAKE_CAPTURE_AGC
           "aec-clean-agc",
#else
           "aec-clean",
#endif
           (unsigned long)v.fed,
           (unsigned long)v.dropped, (unsigned long)v.resets, (unsigned long)v.model_frames,
           (unsigned long)v.detections, (unsigned long)v.last_raw_probability_milli,
           (unsigned long)v.detect_avg_us, (unsigned long)v.max_detect_us,
           (unsigned long)v.detect_over_budget, (unsigned long)v.reject_epoch,
           (unsigned long)v.reject_age, (unsigned long)v.reject_id);
    console_printf("Wake phases: mel/invoke/head-max=%lu/%lu/%lu us reuse/new=%lu/%lu\n",
           (unsigned long)v.mel_max_us, (unsigned long)v.invoke_max_us,
           (unsigned long)v.head_max_us,
           (unsigned long)v.mel_reused_frames, (unsigned long)v.mel_computed_frames);
    console_printf("[DEBUG-wake49] level mic/ref/clean rms-avg=%lu/%lu/%lu rms-max=%lu/%lu/%lu "
           "peak=%lu/%lu/%lu dc=%ld/%ld/%ld\n",
           (unsigned long)audio.rms_avg[0], (unsigned long)audio.rms_avg[1], (unsigned long)audio.rms_avg[2],
           (unsigned long)audio.rms_max[0], (unsigned long)audio.rms_max[1], (unsigned long)audio.rms_max[2],
           (unsigned long)audio.peak[0], (unsigned long)audio.peak[1], (unsigned long)audio.peak[2],
           (long)audio.dc_avg[0], (long)audio.dc_avg[1], (long)audio.dc_avg[2]);
    console_printf("[DEBUG-wake49] window=%lu frames=%lu at-ms=%lu age-ms=%lu "
           "aec-us-avg=%lu measure-us-avg/max=%lu/%lu\n",
           (unsigned long)audio.window, (unsigned long)audio.frames,
           (unsigned long)audio.at_ms, (unsigned long)(now - audio.at_ms),
           (unsigned long)audio.aec_avg_us, (unsigned long)audio.measurement_avg_us,
           (unsigned long)audio.measurement_max_us);
}

static int command_voice_diag(int argc, char **argv)
{
    char *end = NULL;
    unsigned long seconds = argc == 1 ? 20 : argc == 2 ? strtoul(argv[1], &end, 10) : 0;
    if (seconds < 5 || seconds > 30 || (argc == 2 && (end == argv[1] || *end != '\0'))) {
        console_printf("Usage: voice-diag [seconds:5..30]\n");
        return 1;
    }
    print_firmware_identity();
    console_printf("Voicute observation only: say 你好小钛; no configuration changes.\n");
    for (unsigned long i = 0; i <= seconds; ++i) {
        print_wake_diagnostics();
        if (i != seconds) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    console_printf("[DEBUG-wake49] Observation complete; no configuration or model state changed.\n");
    return 0;
}

/* 汇总各深模块的只读快照，不读取或打印 device_secret。 */
static int command_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    starter_runtime_status_t runtime = starter_runtime_status();
    starter_media_status_t media = starter_media_status();
    print_firmware_identity();
    console_printf("Wi-Fi: %s", wifi_manager_connected() ? "connected" : "disconnected");
    if (wifi_manager_provisioning()) {
        console_printf(" (SoftAP %s)", wifi_manager_provisioning_ssid());
    }
    console_printf("\nPlatform: api=%s mqtt=%s\n",
           platform_client_ready() ? "ready" : "offline",
           platform_client_mqtt_connected() ? "connected" : "disconnected");
    console_printf("Local voice: wake=voicute-tflite phrase=你好小钛 ready=%d listening=%d\n",
           starter_voice_ready(), starter_voice_status().listening);
    console_printf("TiRTC: started=%s connection=%s mode=%d send-buffer=%u\n",
           starter_tirtc_started() ? "yes" : "no",
           starter_tirtc_connected() ? "active" : "none",
           (int)starter_tirtc_mode(),
           (unsigned)starter_tirtc_send_buffer_used());
    console_printf("Session: %s generation=%lu connection=%lu last-error=%d stack-min-free=%lu\n",
           starter_runtime_state_name(runtime.state),
           (unsigned long)runtime.session_generation,
           (unsigned long)runtime.connection_generation,
           runtime.last_error,
           (unsigned long)runtime.stack_high_water_bytes);
    console_printf("Media: active=%s audio-tx=%lu audio-rx=%lu dropped=%lu\n",
           media.active ? "yes" : "no",
           (unsigned long)media.audio_sent,
           (unsigned long)media.audio_received,
           (unsigned long)media.audio_dropped);
    console_printf("Downlink: decoded=%lu played=%lu decode-fail=%lu gate=%lu i2s-fail=%lu\n",
           (unsigned long)media.audio_decoded,
           (unsigned long)media.audio_played,
           (unsigned long)media.audio_decode_failed,
           (unsigned long)media.audio_playback_blocked,
           (unsigned long)media.audio_write_failed);
#if CONFIG_IDF_TARGET_ESP32S3
    starter_media_playback_status_t playback = starter_media_playback_status();
    console_printf("JB: gen=%lu target=%lu q/peak=%lu/%lu ms buffer=%d jitter/gap/hold=%lu/%lu/%lu us starts=%lu late=%lu ts-est/break=%lu/%lu\n",
           (unsigned long)playback.generation, (unsigned long)playback.target_ms,
           (unsigned long)playback.queued_ms, (unsigned long)playback.queued_peak_ms,
           playback.buffering, (unsigned long)playback.jitter_us,
           (unsigned long)playback.gap_max_us, (unsigned long)playback.residence_max_us,
           (unsigned long)playback.starts, (unsigned long)playback.late_bursts,
           (unsigned long)playback.untimed_pairs, (unsigned long)playback.timestamp_breaks);
    console_printf("RX: invalid/full/stale/slot=%lu/%lu/%lu/%lu; OUT: lock-timeout=%lu wait-max=%lu us owner=%lu write-max=%lu us slow=%lu\n",
           (unsigned long)playback.rx_invalid, (unsigned long)playback.rx_overflow,
           (unsigned long)playback.rx_stale, (unsigned long)playback.slot_errors,
           (unsigned long)playback.lock_timeouts, (unsigned long)playback.lock_max_us,
           (unsigned long)playback.last_lock_owner, (unsigned long)playback.write_max_us,
           (unsigned long)playback.slow_writes);
    console_printf("PCM source: epoch=%lu in/consumed/drop/pending=%lu/%lu/%lu/%lu samples chunks/tails=%lu/%lu hash=%08lx/%08lx\n",
           (unsigned long)playback.pcm_epoch,
           (unsigned long)playback.decoded_samples, (unsigned long)playback.written_samples,
           (unsigned long)playback.discarded_samples, (unsigned long)playback.pending_samples,
           (unsigned long)playback.chunks, (unsigned long)playback.tails,
           (unsigned long)playback.pcm_in_hash, (unsigned long)playback.pcm_out_hash);
    console_printf("PCM timing: ingress/decode/service/write-gap-max=%lu/%lu/%lu/%lu us\n",
           (unsigned long)playback.ingress_max_us, (unsigned long)playback.decode_max_us,
           (unsigned long)playback.service_gap_max_us, (unsigned long)playback.write_gap_max_us);
    console_printf("RATE: mode=%d slow/fast=%lu/%lu render=%lu samples fade=%lu DMA-est=%lu us\n",
           playback.rate_mode, (unsigned long)playback.slow_chunks, (unsigned long)playback.fast_chunks,
           (unsigned long)playback.output_samples, (unsigned long)playback.fade_starts,
           (unsigned long)playback.dma_estimate_us);
    starter_media_pipeline_status_t pipeline = starter_media_pipeline_status();
    console_printf("AFE feed: detail=%d frames/fetch/full=%lu/%lu/%lu avg/max=%lu/%lu us\n",
           pipeline.afe_detail_enabled, (unsigned long)pipeline.afe_feed_frames,
           (unsigned long)pipeline.afe_fetch_frames, (unsigned long)pipeline.afe_input_full,
           (unsigned long)pipeline.afe_feed_avg_us, (unsigned long)pipeline.afe_feed_max_us);
    console_printf("AFE drop: ring-full=%lu stamp-timeout=%lu failed=%d\n",
           (unsigned long)pipeline.afe_feed_ring_full, (unsigned long)pipeline.afe_stamp_timeouts,
           pipeline.afe_failed);
    console_printf("IN: frames/err=%lu/%lu read/gap-max=%lu/%lu us AFE-age-max=%lu ms\n",
           (unsigned long)pipeline.read_frames, (unsigned long)pipeline.read_errors,
           (unsigned long)pipeline.read_max_us, (unsigned long)pipeline.read_gap_max_us,
           (unsigned long)pipeline.afe_age_max_ms);
    console_printf("DSP: fetch-wait/AGC-max=%lu/%lu us post16k-peak/rms=%lu/%lu\n",
           (unsigned long)pipeline.fetch_wait_max_us, (unsigned long)pipeline.agc_max_us,
           (unsigned long)pipeline.post_agc_peak,
           (unsigned long)pipeline.post_agc_rms);
    console_printf("AEC linear: calls=%lu avg/max=%lu/%lu us\n", (unsigned long)pipeline.aec_calls,
           (unsigned long)pipeline.aec_avg_us, (unsigned long)pipeline.aec_max_us);
    console_printf("AEC NLP: calls=%lu avg/max=%lu/%lu us\n", (unsigned long)pipeline.nlp_calls,
           (unsigned long)pipeline.nlp_avg_us, (unsigned long)pipeline.nlp_max_us);
    console_printf("SE BSS: calls=%lu avg/max=%lu/%lu us\n", (unsigned long)pipeline.se_calls,
           (unsigned long)pipeline.se_avg_us, (unsigned long)pipeline.se_max_us);
    console_printf("DSP CPU1 ticks: linear=%lu/%lu NLP=%lu/%lu BSS=%lu/%lu run/preempt @%u Hz\n",
           (unsigned long)pipeline.aec_run_ticks, (unsigned long)pipeline.aec_preempt_ticks,
           (unsigned long)pipeline.nlp_run_ticks, (unsigned long)pipeline.nlp_preempt_ticks,
           (unsigned long)pipeline.se_run_ticks, (unsigned long)pipeline.se_preempt_ticks,
           (unsigned)configTICK_RATE_HZ);
    console_printf("DSP PM: cycles=%lu/%lu/%lu permille errors=%lu\n",
           (unsigned long)pipeline.dsp_cycles[0], (unsigned long)pipeline.dsp_cycles[1],
           (unsigned long)pipeline.dsp_cycles[2], (unsigned long)pipeline.dsp_perf_errors);
    static const char *const causes[] = {"all", "busy", "PIF", "store", "lookup", "uncached", "bank", "instruction"};
    for (unsigned cause = 0; cause < 8U; ++cause)
        console_printf("DSP stall %s: %lu/%lu/%lu\n", causes[cause],
               (unsigned long)pipeline.dsp_stall_pm[0][cause],
               (unsigned long)pipeline.dsp_stall_pm[1][cause],
               (unsigned long)pipeline.dsp_stall_pm[2][cause]);
    console_printf("TX: q-peak=%lu age/gain/encode/send-max=%lu/%lu/%lu/%lu us peak/rms=%lu/%lu clip=%lu frames=%lu measure-max=%lu us\n",
           (unsigned long)pipeline.tx_queue_peak, (unsigned long)pipeline.tx_age_max_us,
           (unsigned long)pipeline.tx_gain_max_us, (unsigned long)pipeline.encode_max_us,
           (unsigned long)pipeline.send_max_us, (unsigned long)pipeline.tx_peak,
           (unsigned long)pipeline.tx_rms, (unsigned long)pipeline.tx_clipped,
           (unsigned long)pipeline.tx_frames, (unsigned long)pipeline.tx_measure_max_us);
#if CONFIG_XIAOTAI_DEVELOPMENT_CONSOLE
    starter_media_codec_timing_t codec;
    starter_media_debug_codec_timing(&codec);
    console_printf("OPUS ticks: ready=%d hz=%lu (running, includes memory stalls)\n",
           codec.tick_ready, (unsigned long)codec.tick_hz);
    for (unsigned core = 0; core < 2U; ++core)
        console_printf("OPUS CPU%u: enc/dec-n=%lu/%lu avg-us=%lu/%lu run-ticks=%lu/%lu\n", core,
               (unsigned long)codec.frames[0][core], (unsigned long)codec.frames[1][core],
               (unsigned long)codec.avg_us[0][core], (unsigned long)codec.avg_us[1][core],
               (unsigned long)codec.running_ticks[0][core],
               (unsigned long)codec.running_ticks[1][core]);
#endif
#endif
    console_printf("AEC: processed=%lu errors=%lu clipped-mic/ref=%lu/%lu\n",
           (unsigned long)media.aec_processed,
           (unsigned long)media.aec_errors,
           (unsigned long)media.aec_mic_clipped,
           (unsigned long)media.aec_reference_clipped);
#if CONFIG_IDF_TARGET_ESP32S3
    console_printf("Media timing: max-feed-normalized=%lu us feed-budget-miss=%lu\n",
#else
    console_printf("Media timing: max-aec=%lu us deadline-miss=%lu\n",
#endif
           (unsigned long)media.aec_max_process_us,
           (unsigned long)media.aec_deadline_misses);
#if CONFIG_IDF_TARGET_ESP32P4
    console_printf("Capture: i2s-queue-overflow=%lu\n", (unsigned long)media.capture_queue_overflows);
#endif
    console_printf("Heap: internal-free=%u internal-largest=%u psram-free=%u psram-largest=%u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    print_wake_diagnostics();
    return 0;
}

static int command_ai_start(int argc, char **argv)
{
    /* 串口命令和板载 BOOT 按键复用同一产品控制接口。 */
    (void)argv;
    if (argc != 1) {
        console_printf("usage: ai-start\n");
        return 1;
    }
    esp_err_t err = starter_runtime_ai_start();
    if (err != ESP_OK) {
        console_printf("AI start rejected: %s\n", esp_err_to_name(err));
        return 1;
    }
    return 0;
}

static int command_voice_config(int argc, char **argv)
{
    char *end = NULL;
    unsigned long threshold = argc == 2 ? strtoul(argv[1], &end, 10) : 0;
    if (argc != 2 || end == argv[1] || *end != '\0' || threshold < 50 || threshold > 990) {
        console_printf("usage: voice-config <threshold-milli:50..990>; volatile, default %u\n",
               (unsigned)CONFIG_XIAOTAI_WAKE_THRESHOLD_MILLI);
        return 1;
    }
    esp_err_t err = starter_voice_configure((unsigned)threshold, true);
    console_printf("Voicute configuration: %s\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int command_ai_stop(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        console_printf("usage: ai-stop\n");
        return 1;
    }
    esp_err_t err = starter_runtime_ai_stop();
    if (err != ESP_OK) {
        console_printf("AI stop rejected: %s\n", esp_err_to_name(err));
        return 1;
    }
    return 0;
}

static int command_voice_test(int argc, char **argv)
{
    static const struct {
        const char *phrase;
    } cases[] = {
        {"你好小钛"},
        {"小钛小钛，今天天气怎么样"},
        {"小钛同学，给我讲个故事"},
        {"你好小钛，帮我联系家人"},
    };
    if (argc == 2 && strcmp(argv[1], "self-test") == 0) {
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            starter_voice_result_t parsed = {0};
            esp_err_t err = starter_voice_parse(cases[i].phrase, &parsed);
            if (err != ESP_OK || parsed.intent != STARTER_VOICE_INTENT_WAKE_ONLY) {
                console_printf("voice parser self-test failed at case %u: err=%s intent=%s\n",
                       (unsigned)i, esp_err_to_name(err),
                       starter_voice_intent_name(parsed.intent));
                return 1;
            }
        }
        console_printf("voice parser self-test passed: %u wake suffixes map to AI\n",
               (unsigned)(sizeof(cases) / sizeof(cases[0])));
        return 0;
    }

    starter_voice_result_t result = {0};
    if (argc == 2) {
        /* 中文输入在支持 UTF-8 argv 的控制台实现上仍可直接使用。 */
        (void)starter_voice_parse(argv[1], &result);
    }
    if (result.intent == STARTER_VOICE_INTENT_NONE) {
        console_printf("usage: voice-test self-test|你好小钛[，任意后续内容]\n");
        return 1;
    }
    esp_err_t err = starter_product_submit_voice(&result);
    if (err != ESP_OK) {
        console_printf("voice intent submission failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    console_printf("voice intent queued: %s\n", starter_voice_intent_name(result.intent));
    return 0;
}

static int command_wifi_set(int argc, char **argv)
{
    /* 保存后重启，让 wifi_manager 从单一的正常启动路径应用新配置。 */
    if (argc != 3) {
        console_printf("usage: wifi-set <ssid> <password>; use \"\" for an open network\n");
        return 1;
    }
    char error[80];
    if (!wifi_manager_credentials_valid(argv[1], argv[2], error, sizeof(error))) {
        console_printf("invalid Wi-Fi config: %s\n", error);
        return 1;
    }
    esp_err_t err = wifi_manager_save_credentials(argv[1], argv[2]);
    if (err != ESP_OK) {
        console_printf("save failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    console_printf("Wi-Fi config saved; restarting...\n");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return 0;
}

static int command_wifi_clear(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_err_t err = wifi_manager_forget_credentials();
    if (err != ESP_OK) {
        console_printf("clear failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    console_printf("Wi-Fi config cleared; restarting into SoftAP mode...\n");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return 0;
}

static int command_tirtc_set(int argc, char **argv)
{
    /* 仅用于受控联调；正常设备通过验证码绑定获得并保存凭证。 */
    if (argc < 3 || argc > 4) {
        console_printf("usage: tirtc-set <device_id> <device_secret> [client_id]\n");
        return 1;
    }
    runtime_tirtc_config_t config = {0};
    if (strlen(argv[1]) >= sizeof(config.device_id) ||
        strlen(argv[2]) >= sizeof(config.device_secret) ||
        (argc == 4 && strlen(argv[3]) >= sizeof(config.client_id))) {
        console_printf("one or more arguments are too long\n");
        return 1;
    }
    (void)snprintf(config.device_id, sizeof(config.device_id), "%s", argv[1]);
    (void)snprintf(config.device_secret, sizeof(config.device_secret), "%s", argv[2]);
    if (argc == 4) {
        (void)snprintf(config.client_id, sizeof(config.client_id), "%s", argv[3]);
    }
    esp_err_t err = runtime_config_save_tirtc(&config);
    if (err != ESP_OK) {
        console_printf("save failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    console_printf("device credentials saved (secret is not displayed); restarting...\n");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return 0;
}

static int command_tirtc_clear(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_err_t err = runtime_config_clear_tirtc();
    if (err != ESP_OK) {
        console_printf("clear failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    console_printf("device credentials cleared; restarting into binding flow...\n");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return 0;
}

static int command_restart(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    console_printf("restarting...\n");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return 0;
}

static esp_err_t register_command(const char *name,
                                  const char *help,
                                  esp_console_cmd_func_t function)
{
    const esp_console_cmd_t command = {
        .command = name,
        .help = help,
        .func = function,
    };
    return esp_console_cmd_register(&command);
}

static bool numeric(const char *text, unsigned max, unsigned *value)
{
    if (!text || !*text) return false;
    for (const char *p = text; *p; ++p) if (*p < '0' || *p > '9') return false;
    errno = 0;
    char *end;
    unsigned long parsed = strtoul(text, &end, 10);
    if (errno || *end || parsed > max) return false;
    *value = (unsigned)parsed;
    return true;
}

static int at_result(esp_err_t error)
{
    if (error == ESP_OK && s_console_output_error != ESP_OK)
        error = s_console_output_error;
    if (error == ESP_OK) console_printf("OK\n");
    else console_printf("ERROR:%s\n", esp_err_to_name(error));
    return error == ESP_OK && s_console_output_error == ESP_OK ? 0 : 1;
}

static int command_at(int argc, char **argv)
{
    esp_err_t err = ESP_ERR_INVALID_ARG;
    unsigned value, y, end_x, end_y, duration;
    if (argc == 1 && !strcmp(argv[0], "AT")) return at_result(ESP_OK);
    if (!strcmp(argv[0], "AT+AI") && argc == 2) {
        if (!strcmp(argv[1], "START")) err = starter_runtime_ai_start();
        else if (!strcmp(argv[1], "STOP")) err = starter_runtime_ai_stop();
    } else if (!strcmp(argv[0], "AT+CONTACTS")) {
        if (argc == 2 && !strcmp(argv[1], "REFRESH")) err = starter_runtime_contacts_refresh();
        else if (argc == 1) {
            starter_runtime_product_snapshot_t view = starter_runtime_product_snapshot();
            console_printf("+CONTACTS:%u\n", view.contact_count);
            for (unsigned i = 0; i < view.contact_count; ++i)
                console_printf("+CONTACT:%u,%d,%d,%s,%s\n", i, view.contacts[i].source,
                       view.contacts[i].online, view.contacts[i].id, view.contacts[i].name);
            err = ESP_OK;
        }
    } else if (!strcmp(argv[0], "AT+CALL") && argc == 2 &&
               numeric(argv[1], STARTER_PRODUCT_CONTACTS_MAX-1, &value)) {
        err = starter_runtime_call_contact(value);
    } else if (!strcmp(argv[0], "AT+ANSWER") && argc == 1) err = starter_runtime_call_accept();
    else if (!strcmp(argv[0], "AT+REJECT") && argc == 1) err = starter_runtime_call_reject();
    else if (!strcmp(argv[0], "AT+HANGUP") && argc == 1) err = starter_runtime_call_hangup();
    else if (!strcmp(argv[0], "AT+VOL") && argc == 2 && numeric(argv[1], 10, &value))
        err = starter_media_set_speaker_volume(value);
    else if (!strcmp(argv[0], "AT+MIC") && argc == 2 && numeric(argv[1], 1, &value)) {
        starter_media_set_microphone_muted(!value);
        err = ESP_OK;
    } else if (!strcmp(argv[0], "AT+ROOM") && argc >= 2) {
        if (argc == 2 && (!strcmp(argv[1], "OPEN") || !strcmp(argv[1], "CLOSE"))) {
            starter_runtime_room_set_open(!strcmp(argv[1], "OPEN"));
            err = ESP_OK;
        } else if (argc == 2 && !strcmp(argv[1], "REFRESH")) err = starter_runtime_room_refresh();
        else if (argc == 2 && !strcmp(argv[1], "LEAVE")) err = starter_runtime_room_leave();
        else if (argc <= 3 && !strcmp(argv[1], "CREATE"))
            err = starter_runtime_room_create(argc == 3 ? argv[2] : "");
        else if ((argc == 3 || argc == 4) && !strcmp(argv[1], "JOIN"))
            err = starter_runtime_room_join(argv[2], argc == 4 ? argv[3] : "");
        else if (argc == 2 && !strcmp(argv[1], "STATUS")) {
            starter_room_view_t view = {0};
            if (starter_runtime_room_read(0, &view)) {
                console_printf("+ROOM:%s,phase=%d,gen=%lu,open=%d,assigned=%d,count=%u,error=%d\n",
                       view.room_code, view.phase, (unsigned long)view.generation,
                       view.app_open, view.assigned, view.member_count, view.error);
                err = ESP_OK;
            } else err = ESP_ERR_INVALID_STATE;
        }
    } else if (!strcmp(argv[0], "AT+PTT") && argc == 2 && numeric(argv[1], 1, &value)) {
        starter_room_view_t view = {0};
        if (starter_runtime_room_read(0, &view)) err = starter_runtime_room_ptt(view.generation, value);
        else err = ESP_ERR_INVALID_STATE;
    } else if (!strcmp(argv[0], "AT+DISCONNECT") && argc == 1) err = wifi_manager_disconnect();
    else if (!strcmp(argv[0], "AT+UI") && argc == 2 && !strcmp(argv[1], "PERF")) {
        starter_product_display_stats_t stats;
        err = starter_product_debug_display(&stats);
        if (err == ESP_OK) console_printf("+UI-PERF:refresh=%lu,pixels=%llu,ms=%llu,flush=%lu,bytes=%llu,"
                                 "wait=%lu,us=%llu,face=%lu,face-us=%llu,clear-us=%llu,raster-us=%llu,"
                                 "blit=%lu,blit-us=%llu,blit-pixels=%llu,blit-flags=%lu,submit-us=%llu,"
                                 "rows=%llu,byte-rows=%llu,byte-pixels=%llu\n",
                                 (unsigned long)stats.refreshes, (unsigned long long)stats.pixels,
                                 (unsigned long long)stats.refresh_ms, (unsigned long)stats.flushes,
                                 (unsigned long long)stats.flush_bytes, (unsigned long)stats.waits,
                                 (unsigned long long)stats.wait_us, (unsigned long)stats.face_frames,
                                 (unsigned long long)stats.face_us,
                                 (unsigned long long)stats.face_clear_us,
                                 (unsigned long long)stats.face_raster_us,
                                 (unsigned long)stats.face_blits,
                                 (unsigned long long)stats.face_blit_us,
                                 (unsigned long long)stats.face_blit_pixels,
                                 (unsigned long)stats.face_blit_flags,
                                 (unsigned long long)stats.flush_submit_us,
                                 (unsigned long long)stats.face_blit_rows,
                                 (unsigned long long)stats.face_byte_rows,
                                 (unsigned long long)stats.face_byte_pixels);
    } else if (!strcmp(argv[0], "AT+UI") && argc == 1) {
        unsigned page; bool awake;
        err = starter_product_debug_state(&page, &awake);
        if (err == ESP_OK) console_printf("+UI:page=%u,awake=%d\n", page, awake);
    } else if (!strcmp(argv[0], "AT+TAP") && argc == 3 &&
               numeric(argv[1], 319, &value) && numeric(argv[2], 239, &y))
        err = starter_product_debug_tap(value, y);
    else if (!strcmp(argv[0], "AT+SWIPE") && argc == 6 &&
             numeric(argv[1], 319, &value) && numeric(argv[2], 239, &y) &&
             numeric(argv[3], 319, &end_x) && numeric(argv[4], 239, &end_y) &&
             numeric(argv[5], 2000, &duration))
        err = starter_product_debug_swipe(value, y, end_x, end_y, duration);
    else if (!strcmp(argv[0], "AT+HW") && argc == 1) err = starter_media_debug_hardware();
    else if (!strcmp(argv[0], "AT+AUDIO") && argc == 3 && !strcmp(argv[1], "RXTRACE") &&
             numeric(argv[2], 1, &value)) {
        err = starter_media_debug_rx_trace_arm(value);
    } else if (!strcmp(argv[0], "AT+AUDIO") && argc == 2 && !strcmp(argv[1], "RXTRACE")) {
        starter_media_rx_trace_status_t trace = starter_media_debug_rx_trace_status();
        console_printf("+RXTRACE:active=%d,frozen=%d,count=%u,skipped=%lu,missed=%lu,max-us=%lu\n",
                       trace.active, trace.frozen, trace.count, (unsigned long)trace.skipped,
                       (unsigned long)trace.missed_faults, (unsigned long)trace.max_us);
        err = ESP_OK;
        for (unsigned i = 0; i < trace.count; ++i) {
            starter_rx_trace_entry_t e;
            if (!starter_media_debug_rx_trace_read(i, &e)) {
                err = ESP_ERR_INVALID_STATE;
                break;
            }
            console_printf("+RXE:%u,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu\n",
                i, (unsigned long)e.at_us, (unsigned long)e.timestamp_ms,
                (unsigned long)e.generation, (unsigned long)e.epoch, (unsigned long)e.result,
                (unsigned long)e.format, (unsigned long)e.length, (unsigned long)e.queues,
                (unsigned long)e.queued_ms, (unsigned long)e.pending_samples,
                (unsigned long)e.decoded, (unsigned long)e.played, (unsigned long)e.sink_stage,
                (unsigned long)e.sink_at_us, (unsigned long)e.owner, (unsigned long)e.buffering_count);
        }
    } else if (!strcmp(argv[0], "AT+AUDIO") && argc == 3 && !strcmp(argv[1], "DETAIL") &&
             numeric(argv[2], 1, &value)) {
        err = starter_media_debug_afe_detail(value);
        if (err == ESP_OK) console_printf("+AUDIO:detail=%u\n", value);
    } else if (!strcmp(argv[0], "AT+AUDIO") && argc == 1) {
        starter_board_audio_stats_t stats = starter_board_audio_stats();
        console_printf("+AUDIO:blocks=%lu,overflow=%lu,dma-size-error=%lu\n",
               (unsigned long)stats.captured, (unsigned long)stats.overflow,
               (unsigned long)stats.invalid_dma_size);
        for (unsigned i = 0; i < 4; ++i)
            console_printf("+AUDIO:slot=%u,peak=%lu,nonzero=%lu\n", i,
                   (unsigned long)stats.peak[i], (unsigned long)stats.nonzero[i]);
        err = ESP_OK;
    }
    return at_result(err);
}

static esp_err_t screen_write_line(const char *line)
{
    size_t length = strlen(line);
    if (length == 0 || length >= 256) return ESP_ERR_INVALID_SIZE;
    return console_write_record(line, length);
}

static int command_screen(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) return at_result(ESP_ERR_INVALID_ARG);
    uint8_t *pixels = heap_caps_malloc(320U*240U*2U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pixels) return at_result(ESP_ERR_NO_MEM);
    size_t bytes = 0;
    unsigned page = 0;
    esp_err_t err = starter_product_debug_snapshot(pixels, 320U*240U*2U, &bytes, &page);
    if (err == ESP_OK) {
        char line[160];
        snprintf(line, sizeof(line), "+SCREEN:320,240,rgb565-swap,page=%u,bytes=%u,crc=%08lx\r\n",
                 page, (unsigned)bytes, (unsigned long)esp_rom_crc32_le(0, pixels, bytes));
        err = screen_write_line(line);
        const char hex[] = "0123456789abcdef";
        for (size_t offset = 0; err == ESP_OK && offset < bytes; offset += 64) {
            size_t n = bytes-offset < 64 ? bytes-offset : 64;
            size_t prefix = (size_t)snprintf(line, sizeof(line), "+PIX:%u:", (unsigned)offset);
            for (size_t i = 0; i < n; ++i) {
                line[prefix+i*2] = hex[pixels[offset+i] >> 4];
                line[prefix+i*2+1] = hex[pixels[offset+i] & 15];
            }
            line[prefix+n*2] = '\r';
            line[prefix+n*2+1] = '\n';
            line[prefix+n*2+2] = 0;
            err = screen_write_line(line);
        }
        if (err == ESP_OK) err = screen_write_line("+SCREEN:END\r\n");
    }
    heap_caps_free(pixels);
    return at_result(err);
}

static void console_task(void *arg)
{
    (void)arg;
    char line[512];
    size_t length = 0;
    bool overflow = false;
    console_printf("+AT:READY\n");
    for (;;) {
        int ch = fgetc(stdin);
        if (ch == EOF) { clearerr(stdin); vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (ch == '\r' || ch == '\n') {
            s_console_output_error = ESP_OK;
            if (overflow) at_result(ESP_ERR_INVALID_SIZE);
            else if (length) {
                line[length] = 0;
                int result;
                esp_err_t err = esp_console_run(line, &result);
                if (err != ESP_OK) at_result(err);
                else if (s_console_output_error != ESP_OK) at_result(s_console_output_error);
            }
            length = 0; overflow = false;
        } else if (ch == 8 || ch == 127) { if (length) --length; }
        else if (ch >= 32 && !overflow) {
            if (length+1 < sizeof(line)) line[length++] = ch;
            else overflow = true;
        }
    }
}

esp_err_t starter_console_start(void)
{
    if (s_console_task != NULL) {
        return ESP_OK;
    }
    esp_console_config_t config = ESP_CONSOLE_CONFIG_DEFAULT();
    config.max_cmdline_length = 512;
    config.max_cmdline_args = 8;
    config.heap_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    ESP_RETURN_ON_ERROR(esp_console_init(&config), "starter_console", "console init");
    ESP_RETURN_ON_ERROR(esp_console_register_help_command(),
                        "starter_console",
                        "register help");
    ESP_RETURN_ON_ERROR(register_command("version", "Show firmware identity", command_version),
                        "starter_console", "register version");
    ESP_RETURN_ON_ERROR(register_command("status", "Show starter status", command_status),
                        "starter_console", "register status");
    ESP_RETURN_ON_ERROR(register_command("voice-config", "Set volatile Voicute threshold (50..990)", command_voice_config),
                        "starter_console", "register voice-config");
    ESP_RETURN_ON_ERROR(register_command("voice-diag", "Observe audio levels for 5..30 seconds", command_voice_diag),
                        "starter_console", "register voice-diag");
    ESP_RETURN_ON_ERROR(register_command("ai-start", "Start AI talk", command_ai_start),
                        "starter_console", "register ai-start");
    ESP_RETURN_ON_ERROR(register_command("ai-stop", "Stop AI talk", command_ai_stop),
                        "starter_console", "register ai-stop");
    ESP_RETURN_ON_ERROR(register_command(
                            "voice-test",
                            "Manual text injection only; not acoustic recognition",
                            command_voice_test),
                        "starter_console", "register voice-test");
    ESP_RETURN_ON_ERROR(register_command("wifi-set", "wifi-set <ssid> <password>", command_wifi_set),
                        "starter_console", "register wifi-set");
    ESP_RETURN_ON_ERROR(register_command("wifi-clear", "Clear Wi-Fi and use SoftAP", command_wifi_clear),
                        "starter_console", "register wifi-clear");
    ESP_RETURN_ON_ERROR(register_command("tirtc-set", "Pre-load device credentials", command_tirtc_set),
                        "starter_console", "register tirtc-set");
    ESP_RETURN_ON_ERROR(register_command("tirtc-clear", "Clear device credentials", command_tirtc_clear),
                        "starter_console", "register tirtc-clear");
    ESP_RETURN_ON_ERROR(register_command("restart", "Restart the device", command_restart),
                        "starter_console", "register restart");

    static const char *commands[] = {"AT", "AT+AI", "AT+CONTACTS", "AT+CALL", "AT+ANSWER",
        "AT+REJECT", "AT+HANGUP", "AT+VOL", "AT+MIC", "AT+ROOM", "AT+PTT",
        "AT+DISCONNECT", "AT+UI", "AT+TAP", "AT+SWIPE", "AT+AUDIO", "AT+HW"};
    for (size_t i = 0; i < sizeof(commands)/sizeof(commands[0]); ++i)
        ESP_RETURN_ON_ERROR(register_command(commands[i], "CoreS3-SE control; see AT guide", command_at),
                            "starter_console", "register AT");
    ESP_RETURN_ON_ERROR(register_command("AT+SCREEN", "Capture current LVGL RGB565 scene", command_screen),
                        "starter_console", "register screen");
    ESP_RETURN_ON_ERROR(register_command("AT+STATUS", "Status and memory", command_status),
                        "starter_console", "register status alias");
    ESP_RETURN_ON_ERROR(register_command("AT+CACHE", "Read-only shared cache counters", command_cache),
                        "starter_console", "register cache counters");
    ESP_RETURN_ON_ERROR(register_command("AT+VERSION", "Firmware identity", command_version),
                        "starter_console", "register version alias");
    ESP_RETURN_ON_ERROR(register_command("AT+WIFI", "Save network and restart", command_wifi_set),
                        "starter_console", "register Wi-Fi alias");
    ESP_RETURN_ON_ERROR(register_command("AT+RESTART", "Restart", command_restart),
                        "starter_console", "register restart alias");

    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);
    usb_serial_jtag_driver_config_t usb = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(usb_serial_jtag_driver_install(&usb), "starter_console", "USB driver");
    usb_serial_jtag_vfs_use_driver();
    fcntl(fileno(stdin), F_SETFL, 0);
    if (xTaskCreateWithCaps(console_task, "core_at", 12288, NULL, 1, &s_console_task,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}
