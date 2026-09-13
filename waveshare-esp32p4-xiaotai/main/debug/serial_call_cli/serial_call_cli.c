#include "serial_call_cli.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "app.h"
#include "app_memory_policy.h"
#include "app_task_affinity.h"
#include "audio_device.h"
#include "call_video_renderer.h"
#include "camera_pipeline.h"
#include "device_call.h"
#include "display.h"
#include "media_sink.h"
#include "rtc_transport.h"
#include "wifi.h"
#include "wechat_voip_service.h"

static const char *TAG = "serial_call_cli";

#define SERIAL_CALL_CLI_RX_BUFFER_SIZE 256U
#define SERIAL_CALL_CLI_LINE_SIZE      160U
#define SERIAL_CALL_CLI_IO_SIZE        64U
#define SERIAL_CALL_CLI_TASK_STACK     (8U * 1024U)
#define SERIAL_CALL_CLI_TASK_PRIORITY  4U

static TaskHandle_t s_serial_call_cli_task;

static void serial_call_cli_writef(const char *format, ...)
{
    char line[256];
    va_list args;

    va_start(args, format);
    int length = vsnprintf(line, sizeof(line) - 2U, format, args);
    va_end(args);
    if (length < 0) {
        return;
    }
    size_t used = (size_t)length;
    if (used > sizeof(line) - 3U) {
        used = sizeof(line) - 3U;
    }
    line[used++] = '\r';
    line[used++] = '\n';

    /* ESP_LOG uses the console stdio stream. Keep AT replies on the same
     * serialization path so a concurrent log cannot split one response. */
    flockfile(stdout);
    (void)fwrite(line, 1U, used, stdout);
    (void)fflush(stdout);
    funlockfile(stdout);
}

static void serial_call_cli_print_help(void)
{
    serial_call_cli_writef("+HELP:AT+APP=IPC|CALL|WECHAT|AI|SYSTEM|HOME");
    serial_call_cli_writef("+HELP:AT+AI=START|STOP");
    serial_call_cli_writef("+HELP:AT+CALL=<id>|AT+CALLVIDEO=<id>|AT+CALLVIDEO=FIRST|AT+CALL?");
    serial_call_cli_writef("+HELP:AT+WXCALL=FIRST|AT+WX?|AT+WXANSWER|AT+WXHANGUP");
    serial_call_cli_writef("+HELP:AT+ANSWER|AT+REJECT|AT+HANGUP");
    serial_call_cli_writef("+HELP:AT+VIDEO=ON|OFF|AT+MEDIA?");
    serial_call_cli_writef("+HELP:AT+WIFI=<ssid>,<password>");
    serial_call_cli_writef("+HELP:AT+MEM?|AT+AUDIO?");
    serial_call_cli_writef("OK");
}

static void serial_call_cli_print_wechat(void)
{
    wechat_voip_contacts_snapshot_t contacts = {0};

    wechat_voip_service_get_contacts(&contacts);
    serial_call_cli_writef("+WX:state=%u,pending=%u,contacts=%u,ready=%u,synced=%u,error=%s",
                           (unsigned)wechat_voip_service_get_call_state(),
                           wechat_voip_service_has_incoming_call() ? 1U : 0U,
                           (unsigned)contacts.count,
                           contacts.ready ? 1U : 0U,
                           contacts.server_synced ? 1U : 0U,
                           esp_err_to_name(contacts.last_error));
    serial_call_cli_writef("OK");
}

static esp_err_t serial_call_cli_call_first_wechat_contact(void)
{
    wechat_voip_contacts_snapshot_t contacts = {0};

    wechat_voip_service_get_contacts(&contacts);
    if (!contacts.ready || !contacts.server_synced) {
        return ESP_ERR_INVALID_STATE;
    }
    if (contacts.count == 0U || contacts.contacts[0].open_id[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }
    return app_wechat_call_contact_with_type(contacts.contacts[0].open_id,
                                              APP_CALL_TYPE_VIDEO);
}

static void serial_call_cli_print_call(void)
{
    device_call_snapshot_t snapshot = {0};

    device_call_get_snapshot(&snapshot);
    serial_call_cli_writef("+CALL:state=%u,pending=%u,peer=%s,room=%s,error=%s",
                           (unsigned)snapshot.state,
                           snapshot.pending_incoming ? 1U : 0U,
                           snapshot.peer_device_id[0] != '\0' ? snapshot.peer_device_id : "-",
                           snapshot.room_id[0] != '\0' ? snapshot.room_id : "-",
                           esp_err_to_name(snapshot.last_error));
    serial_call_cli_writef("OK");
}

static esp_err_t serial_call_cli_call_first_device_contact(void)
{
    app_call_contacts_snapshot_t contacts = {0};

    app_get_call_contacts(&contacts);
    if (!contacts.ready || contacts.refreshing) {
        return ESP_ERR_INVALID_STATE;
    }
    for (uint8_t index = 0U; index < contacts.count; ++index) {
        if (contacts.contacts[index].online &&
            contacts.contacts[index].device_id[0] != '\0') {
            return app_call_contact(contacts.contacts[index].device_id,
                                    APP_CALL_TYPE_VIDEO);
        }
    }
    return ESP_ERR_NOT_FOUND;
}

static void serial_call_cli_print_memory(void)
{
    app_memory_snapshot_t memory = {0};

    app_memory_get_snapshot(&memory);
    serial_call_cli_writef(
        "+MEM:int=%u/%u/%u,dma=%u/%u/%u,ps=%u/%u/%u,tasks=%u,fail=%u,integrity=%u",
        (unsigned)memory.internal_free,
        (unsigned)memory.internal_largest,
        (unsigned)memory.internal_min_free,
        (unsigned)memory.dma_free,
        (unsigned)memory.dma_largest,
        (unsigned)memory.dma_min_free,
        (unsigned)memory.psram_free,
        (unsigned)memory.psram_largest,
        (unsigned)memory.psram_min_free,
        (unsigned)uxTaskGetNumberOfTasks(),
        (unsigned)memory.psram_alloc_failures,
        heap_caps_check_integrity_all(false) ? 1U : 0U);
    serial_call_cli_writef("OK");
}

static void serial_call_cli_print_audio(void)
{
    audio_stats_t audio = {0};
    media_sink_stats_t sink = {0};

    audio_device_get_stats(&audio);
    media_sink_get_stats(&sink);
    serial_call_cli_writef(
        "+AUDIO:ready=%u,cap=%u,spk=%u,vol=%u,send=%u,codec=%u,upload=%u,auto=%u/%u,aec=%u/%u/%u,peak=%u/%u/%u,suppress=%u,frames=%lu/%lu",
        audio.ready ? 1U : 0U,
        audio.capture_enabled ? 1U : 0U,
        audio.speaker_enabled ? 1U : 0U,
        (unsigned)audio.speaker_volume_percent,
        (unsigned)audio.capture_gain_percent,
        (unsigned)audio.capture_codec_gain_percent,
        (unsigned)audio.capture_upload_gain_percent,
        (unsigned)audio.capture_effective_auto_gain_max_percent,
        (unsigned)audio.capture_auto_gain_max_percent,
        audio.aec_active ? 1U : 0U,
        audio.aec_reference_active ? 1U : 0U,
        audio.aec_near_end_detected ? 1U : 0U,
        (unsigned)audio.aec_ref_peak,
        (unsigned)audio.aec_mic_peak,
        (unsigned)audio.aec_out_peak,
        (unsigned)audio.aec_suppress_percent,
        (unsigned long)audio.capture_frames,
        (unsigned long)audio.aec_process_frames);
    serial_call_cli_writef(
        "+PLAY:profile=%u,q=%u/%u,buffer=%ums,target=%ums,pre=%ums,jitter=%ums,peak=%ums,condition=%u,pcm=%u/%u",
        (unsigned)sink.audio_profile,
        (unsigned)sink.audio_queue_len,
        (unsigned)sink.audio_queue_capacity,
        (unsigned)sink.audio_buffered_ms,
        (unsigned)sink.audio_target_delay_ms,
        (unsigned)sink.audio_prebuffer_ms,
        (unsigned)sink.audio_arrival_jitter_ms,
        (unsigned)sink.audio_peak_timing_error_ms,
        (unsigned)sink.audio_condition,
        (unsigned)sink.audio_pcm_used_bytes,
        (unsigned)sink.audio_pcm_capacity);
    serial_call_cli_writef("OK");
}

static void serial_call_cli_print_media(void)
{
    camera_pipeline_metrics_t camera = {0};
    call_video_renderer_stats_t renderer = {0};
    rtc_transport_stats_t rtc = {0};

    camera_pipeline_get_metrics(&camera);
    call_video_renderer_get_stats(&renderer);
    rtc_transport_get_stats(&rtc);

    serial_call_cli_writef(
        "+MEDIA:call=%u,send=%u,camera=%u,%ux%u@%u,measured=%u.%u/%uk,drop=%u,enc_fail=%u",
        rtc.call_active ? 1U : 0U,
        rtc.local_video_send_enabled ? 1U : 0U,
        camera.running ? 1U : 0U,
        (unsigned)camera.width,
        (unsigned)camera.height,
        (unsigned)camera.target_fps,
        (unsigned)(camera.measured_fps_x10 / 10U),
        (unsigned)(camera.measured_fps_x10 % 10U),
        (unsigned)camera.measured_bitrate_kbps,
        (unsigned)camera.dropped_frames,
        (unsigned)camera.encode_failures);
    serial_call_cli_writef(
        "+MEDIA_UP:frames=%u,bytes=%u,q=%u,free=%u,fail=%u,sendbuf=%u/%u",
        (unsigned)rtc.tx_video_frames,
        (unsigned)rtc.tx_video_bytes,
        (unsigned)rtc.local_video_tx_queue_len,
        (unsigned)rtc.local_video_tx_free_slots,
        (unsigned)rtc.tx_failures,
        (unsigned)rtc.send_buffer_used,
        (unsigned)rtc.send_buffer_limit);
    serial_call_cli_writef(
        "+MEDIA_DOWN:run=%u,wait_key=%u,rx=%u,decode=%u,convert=%u,show=%u,q=%u/%u,drop=%u/%u,fail=%u/%u,disc=%u,overflow=%u",
        renderer.running ? 1U : 0U,
        renderer.waiting_for_key_frame ? 1U : 0U,
        (unsigned)renderer.received_frames,
        (unsigned)renderer.decoded_frames,
        (unsigned)renderer.converted_frames,
        (unsigned)renderer.presented_frames,
        (unsigned)renderer.queue_depth,
        (unsigned)renderer.conversion_queue_depth,
        (unsigned)renderer.dropped_frames,
        (unsigned)renderer.conversion_dropped_frames,
        (unsigned)renderer.decode_failures,
        (unsigned)renderer.conversion_failures,
        (unsigned)renderer.discontinuities,
        (unsigned)renderer.input_overflows);
    serial_call_cli_writef("OK");
}

static void serial_call_cli_process_line(const char *line)
{
    esp_err_t ret = ESP_OK;

    if (strcmp(line, "AT+HELP") == 0) {
        serial_call_cli_print_help();
    } else if (strcmp(line, "AT+APP=IPC") == 0) {
        ret = app_request_enter_app(APP_ID_DEVICE);
        serial_call_cli_writef("+APP:target=IPC,ret=%s", esp_err_to_name(ret));
    } else if (strcmp(line, "AT+APP=CALL") == 0) {
        ret = app_request_enter_app(APP_ID_CALL);
        serial_call_cli_writef("+APP:target=CALL,ret=%s", esp_err_to_name(ret));
    } else if (strcmp(line, "AT+APP=WECHAT") == 0) {
        ret = app_request_enter_app(APP_ID_WECHAT);
        serial_call_cli_writef("+APP:target=WECHAT,ret=%s", esp_err_to_name(ret));
    } else if (strcmp(line, "AT+APP=AI") == 0) {
        ret = app_request_enter_app(APP_ID_AI_CHAT);
        serial_call_cli_writef("+APP:target=AI,ret=%s", esp_err_to_name(ret));
    } else if (strcmp(line, "AT+APP=SYSTEM") == 0) {
        ret = app_request_enter_app(APP_ID_SYSTEM);
        serial_call_cli_writef("+APP:target=SYSTEM,ret=%s", esp_err_to_name(ret));
    } else if (strcmp(line, "AT+APP=HOME") == 0) {
        ret = app_request_return_home();
        serial_call_cli_writef("+APP:target=HOME,ret=%s", esp_err_to_name(ret));
    } else if (strcmp(line, "AT+AI=START") == 0) {
        ret = app_request_start_ai_chat();
        serial_call_cli_writef("+AI:action=START,ret=%s", esp_err_to_name(ret));
    } else if (strcmp(line, "AT+AI=STOP") == 0) {
        ret = app_close_ai_chat();
        serial_call_cli_writef("+AI:action=STOP,ret=%s", esp_err_to_name(ret));
    } else if (strcmp(line, "AT+CALL?") == 0) {
        serial_call_cli_print_call();
    } else if (strcmp(line, "AT+WX?") == 0) {
        serial_call_cli_print_wechat();
    } else if (strcmp(line, "AT+MEM?") == 0) {
        serial_call_cli_print_memory();
    } else if (strcmp(line, "AT+AUDIO?") == 0) {
        serial_call_cli_print_audio();
    } else if (strcmp(line, "AT+MEDIA?") == 0) {
        serial_call_cli_print_media();
    } else if (strcmp(line, "AT+VIDEO=ON") == 0) {
        ret = app_set_local_video_enabled(true);
        serial_call_cli_writef("+VIDEO:enabled=1,ret=%s", esp_err_to_name(ret));
    } else if (strcmp(line, "AT+VIDEO=OFF") == 0) {
        ret = app_set_local_video_enabled(false);
        serial_call_cli_writef("+VIDEO:enabled=0,ret=%s", esp_err_to_name(ret));
    } else if (strcmp(line, "AT+CALLVIDEO=FIRST") == 0) {
        ret = serial_call_cli_call_first_device_contact();
        serial_call_cli_writef("+CALL:target=FIRST,type=video,ret=%s",
                               esp_err_to_name(ret));
    } else if (strncmp(line, "AT+CALLVIDEO=", strlen("AT+CALLVIDEO=")) == 0) {
        const char *device_id = line + strlen("AT+CALLVIDEO=");
        ret = app_call_contact(device_id, APP_CALL_TYPE_VIDEO);
        serial_call_cli_writef("+CALL:target=%s,type=video,ret=%s",
                               device_id,
                               esp_err_to_name(ret));
    } else if (strncmp(line, "AT+CALL=", strlen("AT+CALL=")) == 0) {
        const char *device_id = line + strlen("AT+CALL=");
        ret = app_call_contact(device_id, APP_CALL_TYPE_AUDIO);
        serial_call_cli_writef("+CALL:target=%s,ret=%s", device_id, esp_err_to_name(ret));
    } else if (strcmp(line, "AT+WXCALL=FIRST") == 0) {
        ret = serial_call_cli_call_first_wechat_contact();
        serial_call_cli_writef("+WXCALL:target=FIRST,type=video,ret=%s",
                               esp_err_to_name(ret));
    } else if (strcmp(line, "AT+WXANSWER") == 0) {
        ret = app_wechat_accept_call();
        serial_call_cli_writef("+WXANSWER:ret=%s", esp_err_to_name(ret));
    } else if (strcmp(line, "AT+WXHANGUP") == 0) {
        ret = app_wechat_hangup_call();
        serial_call_cli_writef("+WXHANGUP:ret=%s", esp_err_to_name(ret));
    } else if (strcmp(line, "AT+ANSWER") == 0) {
        ret = app_request_accept_call();
        serial_call_cli_writef("+ANSWER:ret=%s", esp_err_to_name(ret));
    } else if (strcmp(line, "AT+REJECT") == 0) {
        ret = app_reject_call();
        serial_call_cli_writef("+REJECT:ret=%s", esp_err_to_name(ret));
    } else if (strcmp(line, "AT+HANGUP") == 0) {
        ret = app_hangup_call_async();
        serial_call_cli_writef("+HANGUP:ret=%s", esp_err_to_name(ret));
    } else if (strncmp(line, "AT+WIFI=", strlen("AT+WIFI=")) == 0) {
        char credentials[SERIAL_CALL_CLI_LINE_SIZE] = {0};
        strlcpy(credentials, line + strlen("AT+WIFI="), sizeof(credentials));
        char *password = strchr(credentials, ',');
        if (password == NULL) {
            serial_call_cli_writef("+WIFI:ret=%s", esp_err_to_name(ESP_ERR_INVALID_ARG));
        } else {
            *password++ = '\0';
            size_t ssid_len = strlen(credentials);
            size_t password_len = strlen(password);
            if (ssid_len == 0U || ssid_len > 32U || password_len > WIFI_PASSWORD_MAX_LEN) {
                serial_call_cli_writef("+WIFI:ret=%s", esp_err_to_name(ESP_ERR_INVALID_ARG));
            } else {
                ret = app_connect_wifi(credentials, password);
                serial_call_cli_writef("+WIFI:ssid=%s,ret=%s", credentials, esp_err_to_name(ret));
            }
        }
    } else {
        serial_call_cli_writef("ERROR,UNKNOWN_COMMAND");
    }
}

static void serial_call_cli_task(void *arg)
{
    (void)arg;
    char line[SERIAL_CALL_CLI_LINE_SIZE] = {0};
    uint8_t input[SERIAL_CALL_CLI_IO_SIZE] = {0};
    size_t line_length = 0U;
    bool discard_line = false;

    serial_call_cli_writef("+READY:serial_call_cli,transport=uart,baud=%u",
                           (unsigned)CONFIG_ESP_CONSOLE_UART_BAUDRATE);
    while (true) {
        int read_length = uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM,
                                          input,
                                          sizeof(input),
                                          pdMS_TO_TICKS(50));
        for (int index = 0; index < read_length; ++index) {
            uint8_t value = input[index];
            if (value == '\r' || value == '\n') {
                if (discard_line) {
                    serial_call_cli_writef("ERROR,LINE_TOO_LONG");
                } else if (line_length > 0U) {
                    line[line_length] = '\0';
                    serial_call_cli_process_line(line);
                }
                memset(line, 0, sizeof(line));
                line_length = 0U;
                discard_line = false;
            } else if (value == 0x08U || value == 0x7fU) {
                if (!discard_line && line_length > 0U) {
                    line[--line_length] = '\0';
                }
            } else if (!discard_line) {
                if (line_length + 1U < sizeof(line)) {
                    line[line_length++] = (char)value;
                } else {
                    line_length = 0U;
                    discard_line = true;
                }
            }
        }
    }
}

esp_err_t serial_call_cli_start(void)
{
    if (s_serial_call_cli_task != NULL) {
        return ESP_OK;
    }
    if (!uart_is_driver_installed(CONFIG_ESP_CONSOLE_UART_NUM)) {
        const uart_config_t uart_config = {
            .baud_rate = CONFIG_ESP_CONSOLE_UART_BAUDRATE,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        esp_err_t ret = uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM,
                                            SERIAL_CALL_CLI_RX_BUFFER_SIZE,
                                            0,
                                            0,
                                            NULL,
                                            0);
        if (ret != ESP_OK) {
            return ret;
        }
        ret = uart_param_config(CONFIG_ESP_CONSOLE_UART_NUM, &uart_config);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    /* The bootloader and flashing stub share the console UART. Discard any
     * bytes left in RX before framing the first AT command after reset. */
    esp_err_t flush_ret = uart_flush_input(CONFIG_ESP_CONSOLE_UART_NUM);
    if (flush_ret != ESP_OK) {
        ESP_LOGW(TAG, "serial call CLI RX flush failed: %s", esp_err_to_name(flush_ret));
    }

    BaseType_t task_ret = xTaskCreatePinnedToCoreWithCaps(serial_call_cli_task,
                                                          "serial_call_cli",
                                                          SERIAL_CALL_CLI_TASK_STACK,
                                                          NULL,
                                                          SERIAL_CALL_CLI_TASK_PRIORITY,
                                                          &s_serial_call_cli_task,
                                                          APP_TASK_CORE_BACKGROUND,
                                                          APP_TASK_STACK_CAPS_BACKGROUND);
    if (task_ret != pdPASS) {
        s_serial_call_cli_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "serial call CLI ready: transport=uart stack=psram");
    return ESP_OK;
}
