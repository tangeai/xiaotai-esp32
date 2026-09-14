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
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "platform_client.h"
#include "runtime_config.h"
#include "starter_media.h"
#include "starter_product.h"
#include "starter_runtime.h"
#include "starter_tirtc.h"
#include "starter_voice.h"
#include "wifi_manager.h"

static esp_console_repl_t *s_repl;

static void print_firmware_identity(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    char elf_sha256[65];
    (void)esp_app_get_elf_sha256(elf_sha256, sizeof(elf_sha256));
    printf("Firmware: version=%s built=%s %s elf-sha256=%s\n",
           app->version,
           app->date,
           app->time,
           elf_sha256);
}

static int command_version(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        printf("usage: version\n");
        return 1;
    }
    print_firmware_identity();
    return 0;
}

/* [DEBUG-wake49] Console-only output: never print from the capture/model hot path. */
static void print_wake_diagnostics(void)
{
    starter_media_audio_diagnostics_t audio;
    starter_media_audio_diagnostics(&audio);
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    starter_voice_status_t v = starter_voice_status();
    printf("Voicute: ready=%d init-error=%d listening=%d threshold=%u/1000 frontend=%s "
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
    printf("[DEBUG-wake49] level mic/ref/clean rms-avg=%lu/%lu/%lu rms-max=%lu/%lu/%lu "
           "peak=%lu/%lu/%lu dc=%ld/%ld/%ld\n",
           (unsigned long)audio.rms_avg[0], (unsigned long)audio.rms_avg[1], (unsigned long)audio.rms_avg[2],
           (unsigned long)audio.rms_max[0], (unsigned long)audio.rms_max[1], (unsigned long)audio.rms_max[2],
           (unsigned long)audio.peak[0], (unsigned long)audio.peak[1], (unsigned long)audio.peak[2],
           (long)audio.dc_avg[0], (long)audio.dc_avg[1], (long)audio.dc_avg[2]);
    printf("[DEBUG-wake49] window=%lu frames=%lu at-ms=%lu age-ms=%lu "
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
        printf("Usage: voice-diag [seconds:5..30]\n");
        return 1;
    }
    print_firmware_identity();
    printf("Voicute observation only: say 你好小钛; no configuration changes.\n");
    for (unsigned long i = 0; i <= seconds; ++i) {
        print_wake_diagnostics();
        if (i != seconds) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    printf("[DEBUG-wake49] Observation complete; no configuration or model state changed.\n");
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
    printf("Wi-Fi: %s", wifi_manager_connected() ? "connected" : "disconnected");
    if (wifi_manager_provisioning()) {
        printf(" (SoftAP %s)", wifi_manager_provisioning_ssid());
    }
    printf("\nPlatform: api=%s mqtt=%s\n",
           platform_client_ready() ? "ready" : "offline",
           platform_client_mqtt_connected() ? "connected" : "disconnected");
    printf("Local voice: wake=voicute-tflite phrase=你好小钛 ready=%d listening=%d\n",
           starter_voice_ready(), starter_voice_status().listening);
    printf("TiRTC: started=%s connection=%s mode=%d send-buffer=%u\n",
           starter_tirtc_started() ? "yes" : "no",
           starter_tirtc_connected() ? "active" : "none",
           (int)starter_tirtc_mode(),
           (unsigned)starter_tirtc_send_buffer_used());
    printf("Session: %s generation=%lu connection=%lu last-error=%d stack-min-free=%lu\n",
           starter_runtime_state_name(runtime.state),
           (unsigned long)runtime.session_generation,
           (unsigned long)runtime.connection_generation,
           runtime.last_error,
           (unsigned long)runtime.stack_high_water_bytes);
    printf("Media: active=%s audio-tx=%lu audio-rx=%lu dropped=%lu\n",
           media.active ? "yes" : "no",
           (unsigned long)media.audio_sent,
           (unsigned long)media.audio_received,
           (unsigned long)media.audio_dropped);
    printf("Downlink: decoded=%lu played=%lu decode-fail=%lu gate=%lu i2s-fail=%lu\n",
           (unsigned long)media.audio_decoded,
           (unsigned long)media.audio_played,
           (unsigned long)media.audio_decode_failed,
           (unsigned long)media.audio_playback_blocked,
           (unsigned long)media.audio_write_failed);
#if CONFIG_IDF_TARGET_ESP32S3
    starter_media_playback_status_t playback = starter_media_playback_status();
    printf("JB: gen=%lu target=%lu q/peak=%lu/%lu ms buffer=%d jitter/gap/hold=%lu/%lu/%lu us starts=%lu late=%lu ts-est/break=%lu/%lu\n",
           (unsigned long)playback.generation, (unsigned long)playback.target_ms,
           (unsigned long)playback.queued_ms, (unsigned long)playback.queued_peak_ms,
           playback.buffering, (unsigned long)playback.jitter_us,
           (unsigned long)playback.gap_max_us, (unsigned long)playback.residence_max_us,
           (unsigned long)playback.starts, (unsigned long)playback.late_bursts,
           (unsigned long)playback.untimed_pairs, (unsigned long)playback.timestamp_breaks);
    printf("RX: invalid/full/stale/slot=%lu/%lu/%lu/%lu; OUT: lock-timeout=%lu wait-max=%lu us owner=%lu write-max=%lu us slow=%lu\n",
           (unsigned long)playback.rx_invalid, (unsigned long)playback.rx_overflow,
           (unsigned long)playback.rx_stale, (unsigned long)playback.slot_errors,
           (unsigned long)playback.lock_timeouts, (unsigned long)playback.lock_max_us,
           (unsigned long)playback.last_lock_owner, (unsigned long)playback.write_max_us,
           (unsigned long)playback.slow_writes);
    printf("PCM source: epoch=%lu in/consumed/drop/pending=%lu/%lu/%lu/%lu samples chunks/tails=%lu/%lu hash=%08lx/%08lx\n",
           (unsigned long)playback.pcm_epoch,
           (unsigned long)playback.decoded_samples, (unsigned long)playback.written_samples,
           (unsigned long)playback.discarded_samples, (unsigned long)playback.pending_samples,
           (unsigned long)playback.chunks, (unsigned long)playback.tails,
           (unsigned long)playback.pcm_in_hash, (unsigned long)playback.pcm_out_hash);
    printf("PCM timing: ingress/decode/service/write-gap-max=%lu/%lu/%lu/%lu us\n",
           (unsigned long)playback.ingress_max_us, (unsigned long)playback.decode_max_us,
           (unsigned long)playback.service_gap_max_us, (unsigned long)playback.write_gap_max_us);
    printf("RATE: mode=%d slow/fast=%lu/%lu render=%lu samples fade=%lu DMA-est=%lu us\n",
           playback.rate_mode, (unsigned long)playback.slow_chunks, (unsigned long)playback.fast_chunks,
           (unsigned long)playback.output_samples, (unsigned long)playback.fade_starts,
           (unsigned long)playback.dma_estimate_us);
    starter_media_pipeline_status_t pipeline = starter_media_pipeline_status();
    printf("IN: frames/err=%lu/%lu read/gap-max=%lu/%lu us AFE-age-max=%lu ms\n",
           (unsigned long)pipeline.read_frames, (unsigned long)pipeline.read_errors,
           (unsigned long)pipeline.read_max_us, (unsigned long)pipeline.read_gap_max_us,
           (unsigned long)pipeline.afe_age_max_ms);
    printf("DSP: fetch-wait/AGC/resample-max=%lu/%lu/%lu us post8k-peak/rms=%lu/%lu\n",
           (unsigned long)pipeline.fetch_wait_max_us, (unsigned long)pipeline.agc_max_us,
           (unsigned long)pipeline.resample_max_us, (unsigned long)pipeline.post_agc_peak,
           (unsigned long)pipeline.post_agc_rms);
    printf("TX: q-peak=%lu age/gain/encode/send-max=%lu/%lu/%lu/%lu us peak/rms=%lu/%lu clip=%lu frames=%lu measure-max=%lu us\n",
           (unsigned long)pipeline.tx_queue_peak, (unsigned long)pipeline.tx_age_max_us,
           (unsigned long)pipeline.tx_gain_max_us, (unsigned long)pipeline.encode_max_us,
           (unsigned long)pipeline.send_max_us, (unsigned long)pipeline.tx_peak,
           (unsigned long)pipeline.tx_rms, (unsigned long)pipeline.tx_clipped,
           (unsigned long)pipeline.tx_frames, (unsigned long)pipeline.tx_measure_max_us);
#endif
    printf("AEC: processed=%lu errors=%lu clipped-mic/ref=%lu/%lu\n",
           (unsigned long)media.aec_processed,
           (unsigned long)media.aec_errors,
           (unsigned long)media.aec_mic_clipped,
           (unsigned long)media.aec_reference_clipped);
#if CONFIG_IDF_TARGET_ESP32S3
    printf("Media timing: max-feed-normalized=%lu us feed-budget-miss=%lu\n",
#else
    printf("Media timing: max-aec=%lu us deadline-miss=%lu\n",
#endif
           (unsigned long)media.aec_max_process_us,
           (unsigned long)media.aec_deadline_misses);
#if CONFIG_IDF_TARGET_ESP32P4
    printf("Capture: i2s-queue-overflow=%lu\n", (unsigned long)media.capture_queue_overflows);
#endif
    printf("Heap: internal-free=%u internal-largest=%u psram-free=%u psram-largest=%u\n",
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
        printf("usage: ai-start\n");
        return 1;
    }
    esp_err_t err = starter_runtime_ai_start();
    if (err != ESP_OK) {
        printf("AI start rejected: %s\n", esp_err_to_name(err));
        return 1;
    }
    return 0;
}

static int command_voice_config(int argc, char **argv)
{
    char *end = NULL;
    unsigned long threshold = argc == 2 ? strtoul(argv[1], &end, 10) : 0;
    if (argc != 2 || end == argv[1] || *end != '\0' || threshold < 50 || threshold > 990) {
        printf("usage: voice-config <threshold-milli:50..990>; volatile, default 700\n");
        return 1;
    }
    esp_err_t err = starter_voice_configure((unsigned)threshold, true);
    printf("Voicute configuration: %s\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int command_ai_stop(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        printf("usage: ai-stop\n");
        return 1;
    }
    esp_err_t err = starter_runtime_ai_stop();
    if (err != ESP_OK) {
        printf("AI stop rejected: %s\n", esp_err_to_name(err));
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
                printf("voice parser self-test failed at case %u: err=%s intent=%s\n",
                       (unsigned)i, esp_err_to_name(err),
                       starter_voice_intent_name(parsed.intent));
                return 1;
            }
        }
        printf("voice parser self-test passed: %u wake suffixes map to AI\n",
               (unsigned)(sizeof(cases) / sizeof(cases[0])));
        return 0;
    }

    starter_voice_result_t result = {0};
    if (argc == 2) {
        /* 中文输入在支持 UTF-8 argv 的控制台实现上仍可直接使用。 */
        (void)starter_voice_parse(argv[1], &result);
    }
    if (result.intent == STARTER_VOICE_INTENT_NONE) {
        printf("usage: voice-test self-test|你好小钛[，任意后续内容]\n");
        return 1;
    }
    esp_err_t err = starter_product_submit_voice(&result);
    if (err != ESP_OK) {
        printf("voice intent submission failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("voice intent queued: %s\n", starter_voice_intent_name(result.intent));
    return 0;
}

static int command_wifi_set(int argc, char **argv)
{
    /* 保存后重启，让 wifi_manager 从单一的正常启动路径应用新配置。 */
    if (argc != 3) {
        printf("usage: wifi-set <ssid> <password>; use \"\" for an open network\n");
        return 1;
    }
    char error[80];
    if (!wifi_manager_credentials_valid(argv[1], argv[2], error, sizeof(error))) {
        printf("invalid Wi-Fi config: %s\n", error);
        return 1;
    }
    esp_err_t err = wifi_manager_save_credentials(argv[1], argv[2]);
    if (err != ESP_OK) {
        printf("save failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("Wi-Fi config saved; restarting...\n");
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
        printf("clear failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("Wi-Fi config cleared; restarting into SoftAP mode...\n");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return 0;
}

static int command_tirtc_set(int argc, char **argv)
{
    /* 仅用于受控联调；正常设备通过验证码绑定获得并保存凭证。 */
    if (argc < 3 || argc > 4) {
        printf("usage: tirtc-set <device_id> <device_secret> [client_id]\n");
        return 1;
    }
    runtime_tirtc_config_t config = {0};
    if (strlen(argv[1]) >= sizeof(config.device_id) ||
        strlen(argv[2]) >= sizeof(config.device_secret) ||
        (argc == 4 && strlen(argv[3]) >= sizeof(config.client_id))) {
        printf("one or more arguments are too long\n");
        return 1;
    }
    (void)snprintf(config.device_id, sizeof(config.device_id), "%s", argv[1]);
    (void)snprintf(config.device_secret, sizeof(config.device_secret), "%s", argv[2]);
    if (argc == 4) {
        (void)snprintf(config.client_id, sizeof(config.client_id), "%s", argv[3]);
    }
    esp_err_t err = runtime_config_save_tirtc(&config);
    if (err != ESP_OK) {
        printf("save failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("device credentials saved (secret is not displayed); restarting...\n");
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
        printf("clear failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("device credentials cleared; restarting into binding flow...\n");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return 0;
}

static int command_restart(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("restarting...\n");
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

esp_err_t starter_console_start(void)
{
    if (s_repl != NULL) {
        return ESP_OK;
    }
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

    /* sdkconfig.defaults 默认走 USB Serial/JTAG，UART 分支便于其他板型复用。 */
    esp_console_repl_config_t repl = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl.prompt = "starter> ";
    repl.max_cmdline_length = 512;
#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    esp_console_dev_usb_serial_jtag_config_t device =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_console_new_repl_usb_serial_jtag(&device, &repl, &s_repl),
                        "starter_console", "create USB Serial/JTAG REPL");
#elif defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
    esp_console_dev_uart_config_t device = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_console_new_repl_uart(&device, &repl, &s_repl),
                        "starter_console", "create UART REPL");
#else
#error Unsupported console transport
#endif
    return esp_console_start_repl(s_repl);
}
