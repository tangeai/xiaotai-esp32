#include "starter_at.h"
#include "coprocessor_update.h"
#include "regression_controls.h"

#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "starter_product.h"
#include "starter_runtime.h"
#include "starter_media.h"
#include "starter_tirtc.h"
#include "starter_voice.h"
#include "platform_client.h"
#include "hardware_board.h"
#include "p4_video.h"
#include "display_driver.h"
#include "wifi_manager.h"
#include "wifi_path_trace.h"

#define AT_RX_BYTES 512
#define AT_LINE_BYTES 320
#define AT_STACK_BYTES 6144

static const char *TAG = "starter_at";
static TaskHandle_t s_task;
static bool s_commands_enabled = true;
static atomic_bool s_companion_ready;

void starter_at_companion_ready(void)
{
    atomic_store(&s_companion_ready, true);
}

static void at_reply(const char *reply)
{
    flockfile(stdout);
    printf("%s\r\n", reply);
    fflush(stdout);
    funlockfile(stdout);
}

static void at_status(void)
{
    const starter_runtime_status_t runtime = starter_runtime_status();
    const starter_media_status_t media = starter_media_status();
    starter_media_audio_diagnostics_t audio = {0};
    starter_media_audio_diagnostics(&audio);
    flockfile(stdout);
    printf("+STATUS:wifi=%u,platform=%u,mqtt=%u,rtc=%u,connected=%u,state=%s,session=%lu,connection=%lu,error=%d\r\n",
           (unsigned)wifi_manager_connected(), (unsigned)platform_client_ready(),
           (unsigned)platform_client_mqtt_connected(), (unsigned)starter_tirtc_started(),
           (unsigned)starter_tirtc_connected(), starter_runtime_state_name(runtime.state),
           (unsigned long)runtime.session_generation, (unsigned long)runtime.connection_generation,
           runtime.last_error);
    printf("+MEDIA:active=%u,tx=%lu,rx=%lu,decoded=%lu,played=%lu,drop=%lu,decode_fail=%lu,write_fail=%lu,aec=%lu,aec_err=%lu\r\n",
           (unsigned)media.active, (unsigned long)media.audio_sent,
           (unsigned long)media.audio_received, (unsigned long)media.audio_decoded,
           (unsigned long)media.audio_played, (unsigned long)media.audio_dropped,
           (unsigned long)media.audio_decode_failed, (unsigned long)media.audio_write_failed,
           (unsigned long)media.aec_processed, (unsigned long)media.aec_errors);
    printf("+TX:pool_full=%lu,ready_full=%lu,queue_age_max_ms=%lu,encode_max_us=%lu,send_max_us=%lu,rx_drop=%lu\r\n",
           (unsigned long)media.audio_tx_pool_full, (unsigned long)media.audio_tx_ready_full,
           (unsigned long)media.audio_tx_queue_age_max_ms, (unsigned long)media.audio_tx_encode_max_us,
           (unsigned long)media.audio_tx_send_max_us, (unsigned long)media.audio_rx_dropped);
    printf("+RX:pool_full=%lu,pcm_wait=%lu\r\n",
           (unsigned long)media.audio_rx_pool_full, (unsigned long)media.audio_rx_pcm_wait);
    printf("+AUDIO:window=%lu,at_ms=%lu,rms_mic_ref_clean=%lu/%lu/%lu,mute_mic_spk=%u/%u,spk_error=%s,capovf=%lu,aec_late=%lu,aec_max_us=%lu\r\n",
           (unsigned long)audio.window, (unsigned long)audio.at_ms,
           (unsigned long)audio.rms_avg[0], (unsigned long)audio.rms_avg[1],
           (unsigned long)audio.rms_avg[2], (unsigned)media.microphone_muted,
           (unsigned)media.speaker_muted, esp_err_to_name(media.speaker_control_error),
           (unsigned long)media.capture_queue_overflows,
           (unsigned long)media.aec_deadline_misses,
           (unsigned long)media.aec_max_process_us);
    printf("+HEAP:internal=%u,largest=%u,minimum=%u,psram=%u,psram_largest=%u\r\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    fflush(stdout);
    funlockfile(stdout);
    at_reply("OK");
}

static void at_wake_status(void)
{
    const starter_voice_status_t wake = starter_voice_status();
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t age = wake.last_raw_at_ms ? now - wake.last_raw_at_ms : UINT32_MAX;
    flockfile(stdout);
    printf("+WAKE:ready=%u,listening=%u,init_error=%d,threshold=%u,frontend=%s,fed=%lu,dropped=%lu,resets=%lu,stale=%lu,inference=%lu,raw_hits=%lu,accepted=%lu\r\n",
           (unsigned)wake.ready, (unsigned)wake.listening, wake.init_error, wake.threshold_milli,
#if CONFIG_XIAOTAI_WAKE_CAPTURE_AGC
           "aec-clean-agc",
#else
           "aec-clean",
#endif
           (unsigned long)wake.fed, (unsigned long)wake.dropped,
           (unsigned long)wake.resets, (unsigned long)wake.stale,
           (unsigned long)wake.model_frames, (unsigned long)wake.model_detected,
           (unsigned long)wake.detections);
    printf("+WAKE_SCORE:last=%lu,at_ms=%lu,age_ms=%lu,avg_us=%lu,max_us=%lu,over_hop=%lu,reject_epoch_age_error=%lu/%lu/%lu,unarmed=%lu,cooldown=%lu\r\n",
           (unsigned long)wake.last_raw_probability_milli, (unsigned long)wake.last_raw_at_ms,
           (unsigned long)age, (unsigned long)wake.detect_avg_us, (unsigned long)wake.max_detect_us,
           (unsigned long)wake.detect_over_budget, (unsigned long)wake.reject_epoch,
           (unsigned long)wake.reject_age, (unsigned long)wake.reject_id,
           (unsigned long)wake.skipped_unarmed, (unsigned long)wake.skipped_cooldown);
    fflush(stdout);
    funlockfile(stdout);
    at_reply("OK");
}

static void at_command(char *line)
{
    if (strcmp(line, "AT") == 0) {
        at_reply("OK");
        return;
    }
    if (strcmp(line, "AT+CLI?") == 0) {
        at_reply(s_commands_enabled ? "+CLI:ON" : "+CLI:OFF");
        at_reply("OK");
        return;
    }
    if (strcmp(line, "AT+CLI=ON") == 0 || strcmp(line, "AT+CLI=OFF") == 0) {
        s_commands_enabled = strcmp(line, "AT+CLI=ON") == 0;
        at_reply("OK");
        return;
    }
    if (!s_commands_enabled) {
        at_reply("ERROR,CLI_DISABLED");
        return;
    }
    if (starter_at_regression_command(line)) return;
    if (strcmp(line, "AT+HELP") == 0) {
        at_reply("+HELP:AT|AT+CLI?|AT+CLI=ON|OFF|AT+STATUS?|AT+WAKE?|AT+NETDIAG?|AT+AUDIOTRACE?|AT+AI=START|STOP|AT+HW?|AT+CAMTEST|AT+CAMINFO?|AT+LCD?|AT+WIFI?|AT+WIFI=<ssid>,<password>|AT+COMPANIONRETRY");
        at_reply("OK");
        return;
    }
    if (strcmp(line, "AT+WAKE?") == 0) {
        at_wake_status();
        return;
    }
    if (strcmp(line, "AT+AUDIOTRACE?") == 0) {
        const esp_err_t err = starter_media_dump_ai_audio_trace();
        bool transport_ok = err == ESP_OK && starter_tirtc_dump_ai_transport_trace();
        at_reply(transport_ok ? "OK" : "ERROR,AUDIO_TRACE_BUSY");
        return;
    }
    if (strcmp(line, "AT+PTRACE=START") == 0 || strcmp(line, "AT+PTRACE=STOP") == 0 ||
        strcmp(line, "AT+PTRACE?") == 0) {
        bool start = strcmp(line, "AT+PTRACE=START") == 0;
        bool dump = strcmp(line, "AT+PTRACE?") == 0;
        bool idle = !starter_tirtc_connected() && !starter_media_status().active &&
            starter_runtime_status().state == STARTER_RUNTIME_WAITING;
        esp_err_t err = ESP_OK;
        if ((start || dump) && !idle) err = ESP_ERR_INVALID_STATE;
        else if (start) err = wifi_path_trace_start();
        else if (dump) err = wifi_path_trace_dump();
        else wifi_path_trace_stop();
        if (err == ESP_OK) starter_tirtc_refresh_transport_trace();
        if (err == ESP_OK) at_reply("OK");
        else { printf("ERROR,PTRACE,%s\r\n", esp_err_to_name(err)); fflush(stdout); }
        return;
    }
    if (strcmp(line, "AT+NETDIAG?") == 0) {
        at_reply(wifi_manager_request_link_diagnostics() == ESP_OK ? "+NETDIAG:QUEUED" : "ERROR,NETWORK_NOT_READY");
        return;
    }
    if (strcmp(line, "AT+NETRX=START") == 0) {
        at_reply(wifi_manager_start_udp_probe() == ESP_OK ? "OK" : "ERROR,UDP_PROBE_BUSY");
        return;
    }
    if (strcmp(line, "AT+CPINFO?") == 0 || strncmp(line, "AT+C61=", 7) == 0) {
        esp_err_t err = line[5] == 'I' ? coprocessor_log_identity()
            : coprocessor_diagnostics_command(line + 7);
        at_reply(err == ESP_OK ? "OK" : "ERROR,CP_DIAGNOSTIC_FAILED");
        return;
    }
    if (strncmp(line, "AT+CPOTA=", 9) == 0) {
        char *hash = strrchr(line + 9, ',');
        if (!hash) { at_reply("ERROR,CPOTA_FORMAT"); return; }
        *hash++ = '\0';
        esp_err_t err = coprocessor_update_start(line + 9, hash);
        at_reply(err == ESP_OK ? "+CPOTA:QUEUED" : "ERROR,CPOTA_REJECTED");
        return;
    }
    if (strcmp(line, "AT+STATUS?") == 0) {
        at_status();
        return;
    }
    if (strcmp(line, "AT+AI=START") == 0 || strcmp(line, "AT+AI=STOP") == 0) {
        /* Same queued intents and busy/auth gates as the button and UI. */
        const esp_err_t err = strcmp(line, "AT+AI=START") == 0
            ? starter_runtime_ai_start() : starter_runtime_ai_stop();
        if (err == ESP_OK) at_reply("OK");
        else {
            flockfile(stdout);
            printf("ERROR,AI,%s\r\n", esp_err_to_name(err));
            fflush(stdout);
            funlockfile(stdout);
        }
        return;
    }
    if (strcmp(line, "AT+COMPANIONRETRY") == 0) {
        if (!starter_product_request_companion_retry()) {
            at_reply("ERROR,COMPANION_RETRY_UNAVAILABLE");
            return;
        }
        at_reply("OK");
        return;
    }
    if (strcmp(line, "AT+HW?") == 0) {
        const esp_err_t err = hardware_board_log_i2c_diagnostics();
        at_reply(err == ESP_OK ? "+HW:OBSERVATIONS_CAPTURED" : "ERROR,HARDWARE_NOT_READY");
        return;
    }
    if (strcmp(line, "AT+CAMTEST") == 0) {
        const esp_err_t err = p4_video_request_camera_probe();
        at_reply(err == ESP_OK ? "+CAMTEST:QUEUED" : "ERROR,CAMERA_PROBE_PENDING");
        return;
    }
    if (strcmp(line, "AT+LCD?") == 0) {
        display_driver_log_ppa_diagnostics();
        at_reply("+LCD:OBSERVATIONS_CAPTURED");
        return;
    }
    if (strcmp(line, "AT+CAMINFO?") == 0) {
        const esp_err_t err = p4_video_log_camera_quality();
        at_reply(err == ESP_OK ? "+CAMINFO:OBSERVATIONS_CAPTURED" : "ERROR,CAMERA_QUALITY_UNAVAILABLE");
        return;
    }
    if (strcmp(line, "AT+WIFI?") == 0) {
        wifi_manager_credentials_t saved = {0};
        esp_err_t err = wifi_manager_load_credentials(&saved);
        if (err == ESP_OK) {
            flockfile(stdout);
            printf("+WIFI:ssid=%s,connected=%u\r\n", saved.ssid,
                   (unsigned)wifi_manager_connected());
            fflush(stdout);
            funlockfile(stdout);
            at_reply("OK");
        } else {
            at_reply("ERROR,WIFI_READ_FAILED");
        }
        return;
    }
    const char *prefix = "AT+WIFI=";
    if (strncmp(line, prefix, strlen(prefix)) == 0) {
        if (!atomic_load(&s_companion_ready)) {
            at_reply("ERROR,COMPANION_NOT_READY");
            return;
        }
        char *ssid = line + strlen(prefix);
        char *password = strchr(ssid, ',');
        if (password == NULL) {
            at_reply("ERROR,WIFI_FORMAT");
            return;
        }
        *password++ = '\0';
        if (!wifi_manager_credentials_valid(ssid, password, NULL, 0)) {
            at_reply("ERROR,WIFI_INVALID");
            return;
        }
        esp_err_t err = wifi_manager_save_credentials(ssid, password);
        if (err != ESP_OK) {
            at_reply("ERROR,WIFI_SAVE_FAILED");
            return;
        }
        at_reply("+WIFI:SAVED,RESTARTING");
        at_reply("OK");
        vTaskDelay(pdMS_TO_TICKS(150));
        esp_restart();
        return;
    }
    at_reply("ERROR,UNKNOWN_COMMAND");
}

static void at_task(void *argument)
{
    (void)argument;
    char line[AT_LINE_BYTES] = {0};
    uint8_t bytes[64];
    size_t length = 0;
    bool overflow = false;
    at_reply("+READY:AT,UART0,115200");
    for (;;) {
        int count = uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, bytes,
                                    sizeof(bytes), pdMS_TO_TICKS(50));
        for (int i = 0; i < count; ++i) {
            uint8_t value = bytes[i];
            if (value == '\r' || value == '\n') {
                if (overflow) at_reply("ERROR,LINE_TOO_LONG");
                else if (length != 0) {
                    line[length] = '\0';
                    at_command(line);
                }
                memset(line, 0, sizeof(line));
                length = 0;
                overflow = false;
            } else if (value == 0x08 || value == 0x7f) {
                if (!overflow && length != 0) line[--length] = '\0';
            } else if (!overflow) {
                if (value < 0x20 || length + 1 >= sizeof(line)) {
                    overflow = true;
                    length = 0;
                } else {
                    line[length++] = (char)value;
                }
            }
        }
    }
}

esp_err_t starter_at_start(void)
{
    if (s_task != NULL) return ESP_OK;
    if (!uart_is_driver_installed(CONFIG_ESP_CONSOLE_UART_NUM)) {
        esp_err_t err = uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM,
                                            AT_RX_BYTES, 0, 0, NULL, 0);
        if (err != ESP_OK) return err;
        const uart_config_t config = {
            .baud_rate = CONFIG_ESP_CONSOLE_UART_BAUDRATE,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        err = uart_param_config(CONFIG_ESP_CONSOLE_UART_NUM, &config);
        if (err != ESP_OK) return err;
    }
    ESP_RETURN_ON_ERROR(uart_flush_input(CONFIG_ESP_CONSOLE_UART_NUM), TAG,
                        "flush UART input");
    if (xTaskCreateWithCaps(at_task, "starter_at", AT_STACK_BYTES, NULL, 3,
                            &s_task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}
