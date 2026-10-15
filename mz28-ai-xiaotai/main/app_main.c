#include "app_config.h"
#include "boot_button.h"
#include "demo_platform.h"
#include "wechat_voip.h"
#include "memory_probe.h"
#include "product_audio.h"
#include "status_led.h"
#include "wifi_provisioning.h"
#include "cJSON.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
static bool s_started;
static char s_line[256];
static atomic_uint s_alloc_failure_state, s_alloc_failures;
static atomic_uint s_binding_prompt_id;
static atomic_bool s_local_binding_reset_requested;
static atomic_bool s_wifi_reset_requested;
static atomic_bool s_platform_start_failed;
static atomic_bool s_call_ring_stop_requested;
static wechat_voip_state_t s_last_voip_state = WECHAT_VOIP_IDLE;
static bool s_call_feedback_active;
static bool s_call_ring_suppressed;
static product_audio_ring_t s_preview_ring = PRODUCT_AUDIO_RING_NONE;
static bool s_provisioned_wifi_pending;
static wifi_provisioning_credentials_t s_provisioned_wifi;
static uint32_t s_provisioned_wifi_retry_at_ms;
static uint32_t s_last_status_update_ms;
static size_t s_failed_size;
static uint32_t s_failed_caps;
static const char *s_failed_function;

static void play_cue(product_audio_cue_t cue, const char *name);

static esp_err_t binding_prompt_begin(void *user_data)
{
    (void)user_data;
    uint32_t prompt_id = product_audio_prompt_begin();
    atomic_store_explicit(&s_binding_prompt_id, prompt_id,
                          memory_order_release);
    return ESP_OK;
}

static esp_err_t binding_prompt_play_code(const char *code, void *user_data)
{
    (void)user_data;
    uint32_t prompt_id = product_audio_prompt_begin();
    atomic_store_explicit(&s_binding_prompt_id, prompt_id,
                          memory_order_release);
    esp_err_t err = product_audio_prompt_play_code(prompt_id, code);
    if (err == ESP_OK) {
        err = product_audio_prompt_end(prompt_id, 20000U);
    } else {
        product_audio_prompt_cancel(prompt_id);
    }
    unsigned expected = prompt_id;
    (void)atomic_compare_exchange_strong(&s_binding_prompt_id, &expected, 0U);
    return err;
}

static esp_err_t binding_prompt_write(const int16_t *samples,
                                      size_t sample_count, void *user_data)
{
    (void)user_data;
    uint32_t prompt_id = atomic_load_explicit(&s_binding_prompt_id,
                                               memory_order_acquire);
    return prompt_id == 0U
               ? ESP_ERR_INVALID_STATE
               : product_audio_prompt_write_pcm8(prompt_id, samples,
                                                  sample_count);
}

static esp_err_t binding_prompt_finish(void *user_data)
{
    (void)user_data;
    uint32_t prompt_id = atomic_load_explicit(&s_binding_prompt_id,
                                               memory_order_acquire);
    if (prompt_id == 0U) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = product_audio_prompt_end(prompt_id, 20000U);
    if (err == ESP_OK) {
        unsigned expected = prompt_id;
        (void)atomic_compare_exchange_strong(&s_binding_prompt_id, &expected,
                                             0U);
    }
    return err;
}

static void binding_prompt_cancel(void *user_data)
{
    (void)user_data;
    uint32_t prompt_id = atomic_exchange_explicit(&s_binding_prompt_id, 0U,
                                                   memory_order_acq_rel);
    if (prompt_id != 0U) {
        product_audio_prompt_cancel(prompt_id);
    }
}

static void voice_command_received(const char *command, void *user_data)
{
    (void)user_data;
    ESP_LOGI("app", "voice command received: %s", command);
}

static bool voip_requires_double_click(wechat_voip_state_t state)
{
    return state == WECHAT_VOIP_DIALING ||
           state == WECHAT_VOIP_CONNECTING ||
           state == WECHAT_VOIP_WAITING_PEER ||
           state == WECHAT_VOIP_ACTIVE;
}

static void boot_button_single_clicked(void)
{
    wechat_voip_state_t state = wechat_voip_state();
    status_led_feedback(voip_requires_double_click(state)
                            ? STATUS_LED_FEEDBACK_WARNING
                            : STATUS_LED_FEEDBACK_TAP);
    esp_err_t err = wechat_voip_command(VOIP_PRIMARY_ACTION, 0);
    if (err == ESP_OK && state == WECHAT_VOIP_RINGING) {
        atomic_store_explicit(&s_call_ring_stop_requested, true,
                              memory_order_release);
    } else if (err != ESP_OK) {
        ESP_LOGW("app", "BOOT action not queued: %s", esp_err_to_name(err));
    }
}

static void boot_button_double_clicked(void)
{
    status_led_feedback(STATUS_LED_FEEDBACK_DOUBLE_TAP);
    esp_err_t err = wechat_voip_command(VOIP_DOUBLE_ACTION, 0);
    if (err == ESP_OK && voip_requires_double_click(wechat_voip_state())) {
        atomic_store_explicit(&s_call_ring_stop_requested, true,
                              memory_order_release);
    } else if (err != ESP_OK) {
        ESP_LOGW("app", "BOOT double-click action not queued: %s",
                 esp_err_to_name(err));
    }
}

static void boot_button_quadruple_clicked(void)
{
    /* Timer callbacks must stay nonblocking. NVS and restart run in app_main. */
    atomic_store_explicit(&s_wifi_reset_requested, true,
                          memory_order_release);
}

static void process_call_ring_stop(void)
{
    if (!atomic_exchange_explicit(&s_call_ring_stop_requested, false,
                                  memory_order_acq_rel)) {
        return;
    }
    s_preview_ring = PRODUCT_AUDIO_RING_NONE;
    s_call_ring_suppressed = true;
    esp_err_t err = product_audio_set_ring(PRODUCT_AUDIO_RING_NONE);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW("app", "immediate call ringtone stop failed: %s",
                 esp_err_to_name(err));
    }
}

static void boot_button_long_pressed(void)
{
    /* Timer callbacks must stay nonblocking. NVS and playback run in app_main. */
    atomic_store_explicit(&s_local_binding_reset_requested, true,
                          memory_order_release);
}

static void process_local_binding_reset(void)
{
    if (!atomic_exchange_explicit(&s_local_binding_reset_requested, false,
                                  memory_order_acq_rel)) {
        return;
    }
    ESP_LOGW("app", "BOOT held for %ums: clearing local binding identity",
             (unsigned)APP_BOOT_LONG_PRESS_MS);
    status_led_feedback(STATUS_LED_FEEDBACK_SUCCESS);
    binding_prompt_cancel(NULL);
    esp_err_t err = platform_identity_clear_local();
    if (err != ESP_OK) {
        ESP_LOGE("app", "local binding identity clear failed: %s; restart cancelled",
                 esp_err_to_name(err));
        return;
    }
    esp_err_t sound = product_audio_play_confirmation();
    if (sound != ESP_OK) {
        ESP_LOGW("app", "binding reset confirmation sound failed: %s",
                 esp_err_to_name(sound));
    }
    ESP_LOGW("app",
             "local binding identity cleared; Wi-Fi retained; cloud account unlink remains a mini-program action");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

static void process_wifi_reset(void)
{
    if (!atomic_exchange_explicit(&s_wifi_reset_requested, false,
                                  memory_order_acq_rel)) {
        return;
    }
    ESP_LOGW("app",
             "BOOT pressed 4 times: clearing saved Wi-Fi and entering SoftAP provisioning");
    binding_prompt_cancel(NULL);
    s_preview_ring = PRODUCT_AUDIO_RING_NONE;
    s_call_ring_suppressed = true;
    (void)product_audio_set_ring(PRODUCT_AUDIO_RING_NONE);
    esp_err_t err = platform_request_wifi_provisioning();
    if (err != ESP_OK) {
        status_led_feedback(STATUS_LED_FEEDBACK_WARNING);
        play_cue(PRODUCT_AUDIO_CUE_WARNING, "wifi-reset-failed");
        ESP_LOGE("app", "saved Wi-Fi clear failed: %s; restart cancelled",
                 esp_err_to_name(err));
        return;
    }
    status_led_feedback(STATUS_LED_FEEDBACK_SUCCESS);
    esp_err_t sound = product_audio_play_confirmation();
    if (sound != ESP_OK) {
        ESP_LOGW("app", "Wi-Fi reset confirmation sound failed: %s",
                 esp_err_to_name(sound));
    }
    ESP_LOGW("app",
             "saved Wi-Fi cleared; binding identity retained; restarting into SoftAP provisioning");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

static void process_wifi_provisioning(void)
{
    if (!wifi_provisioning_active())
        return;

    if (!s_provisioned_wifi_pending) {
        s_provisioned_wifi_pending =
            wifi_provisioning_take_credentials(&s_provisioned_wifi);
        if (!s_provisioned_wifi_pending)
            return;
        s_provisioned_wifi_retry_at_ms = 0U;
    }

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_provisioned_wifi_retry_at_ms != 0U &&
        (int32_t)(now_ms - s_provisioned_wifi_retry_at_ms) < 0) {
        return;
    }

    esp_err_t err = platform_save_wifi(s_provisioned_wifi.ssid,
                                       s_provisioned_wifi.password);
    if (err != ESP_OK) {
        s_provisioned_wifi_retry_at_ms = now_ms + 2000U;
        status_led_feedback(STATUS_LED_FEEDBACK_WARNING);
        ESP_LOGE("app", "verified Wi-Fi persistence failed: %s; retrying",
                 esp_err_to_name(err));
        return;
    }

    memset(&s_provisioned_wifi, 0, sizeof(s_provisioned_wifi));
    s_provisioned_wifi_pending = false;
    status_led_feedback(STATUS_LED_FEEDBACK_SUCCESS);
    (void)product_audio_play_confirmation();
    ESP_LOGI("app", "SoftAP provisioning complete; restarting normal runtime");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

static void play_cue(product_audio_cue_t cue, const char *name)
{
    esp_err_t err = product_audio_play_cue(cue);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW("app", "interaction cue '%s' not queued: %s", name,
                 esp_err_to_name(err));
    }
}

static product_audio_ring_t call_ring_for(
    wechat_voip_state_t state, wechat_voip_direction_t direction)
{
    if (state == WECHAT_VOIP_RINGING) {
        return PRODUCT_AUDIO_RING_INCOMING;
    }
    if (direction == WECHAT_VOIP_DIRECTION_OUTGOING &&
        (state == WECHAT_VOIP_DIALING ||
         state == WECHAT_VOIP_CONNECTING ||
         state == WECHAT_VOIP_WAITING_PEER)) {
        return PRODUCT_AUDIO_RING_OUTGOING;
    }
    return PRODUCT_AUDIO_RING_NONE;
}

static status_led_state_t product_led_state(wechat_voip_state_t state)
{
    if (wifi_provisioning_active())
        return STATUS_LED_WIFI_PROVISIONING;

    switch (state) {
    case WECHAT_VOIP_DIALING:
        return STATUS_LED_OUTGOING;
    case WECHAT_VOIP_RINGING:
        return STATUS_LED_INCOMING;
    case WECHAT_VOIP_CONNECTING:
    case WECHAT_VOIP_WAITING_PEER:
        return STATUS_LED_CALL_CONNECTING;
    case WECHAT_VOIP_ACTIVE:
        return STATUS_LED_ACTIVE;
    case WECHAT_VOIP_CLOSING:
        return STATUS_LED_CLOSING;
    case WECHAT_VOIP_IDLE:
    default:
        break;
    }

    platform_identity_t identity;
    platform_identity_snapshot(&identity);
    bool bound = identity.device_id[0] != '\0' && identity.device_key[0] != '\0';
    memset(&identity, 0, sizeof(identity));
    if (!s_started) {
        return atomic_load_explicit(&s_platform_start_failed,
                                    memory_order_acquire)
                   ? STATUS_LED_ERROR
                   : STATUS_LED_BOOT;
    }
    if (!bound) {
        return STATUS_LED_BINDING;
    }
    return platform_is_online() ? STATUS_LED_READY : STATUS_LED_CONNECTING;
}

static void update_product_feedback(void)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if ((uint32_t)(now_ms - s_last_status_update_ms) < 40U) {
        status_led_tick(now_ms);
        return;
    }
    s_last_status_update_ms = now_ms;
    wechat_voip_state_t current = wechat_voip_state();
    wechat_voip_direction_t direction = wechat_voip_direction();
    if (s_last_voip_state == WECHAT_VOIP_IDLE &&
        (current == WECHAT_VOIP_DIALING ||
         current == WECHAT_VOIP_RINGING)) {
        s_call_ring_suppressed = false;
    }
    if (current != WECHAT_VOIP_IDLE) {
        s_call_feedback_active = true;
        s_preview_ring = PRODUCT_AUDIO_RING_NONE;
    }
    product_audio_ring_t ring =
        current == WECHAT_VOIP_IDLE
            ? s_preview_ring
            : (s_call_ring_suppressed
                   ? PRODUCT_AUDIO_RING_NONE
                   : call_ring_for(current, direction));
    esp_err_t ring_err = product_audio_set_ring(ring);
    if (ring_err != ESP_OK && ring_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW("app", "call ringtone state update failed: %s",
                 esp_err_to_name(ring_err));
    }
    if (current != s_last_voip_state) {
        if (current == WECHAT_VOIP_ACTIVE) {
            status_led_feedback(STATUS_LED_FEEDBACK_SUCCESS);
        } else if (current == WECHAT_VOIP_IDLE && s_call_feedback_active) {
            if (wechat_voip_last_end_failed()) {
                play_cue(PRODUCT_AUDIO_CUE_WARNING, "call-failed");
            } else {
                play_cue(PRODUCT_AUDIO_CUE_CALL_ENDED, "call-ended");
            }
            s_call_feedback_active = false;
            s_call_ring_suppressed = false;
        }
        s_last_voip_state = current;
    }
    status_led_set_state(product_led_state(current));
    status_led_tick(now_ms);
}

static void allocation_failed(size_t size, uint32_t caps, const char *function)
{
    /* The allocator callback only records evidence; logging may allocate too. */
    atomic_fetch_add(&s_alloc_failures, 1);
    unsigned empty = 0;
    if (atomic_compare_exchange_strong(&s_alloc_failure_state, &empty, 1)) {
        s_failed_size = size;
        s_failed_caps = caps;
        s_failed_function = function;
        atomic_store(&s_alloc_failure_state, 2);
    }
}
static void report_allocation_failure(void)
{
    static int64_t last_report;
    if (atomic_load(&s_alloc_failure_state) != 2)
        return;
    int64_t now = esp_timer_get_time();
    if (last_report && now - last_report < 1000000)
        return;
    multi_heap_info_t pool;
    heap_caps_get_info(&pool, s_failed_caps);
    ESP_LOGE("memory",
             "allocation failed: requested=%uB caps=0x%lx function=%s total=%u; "
             "sample=report-time internal_free=%uB internal_largest=%uB "
             "requested_pool_free=%uB pool_largest=%uB pool_min=%uB",
             (unsigned)s_failed_size, (unsigned long)s_failed_caps, s_failed_function,
             (unsigned)atomic_load(&s_alloc_failures),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)pool.total_free_bytes, (unsigned)pool.largest_free_block,
             (unsigned)pool.minimum_free_bytes);
    atomic_store(&s_alloc_failure_state, 0);
    last_report = now;
}
static void help(void)
{
    puts("\nMZ28 Xiaotai: ESP32-C3 / TiRTC 2.5.0 / VB6824 live Opus");
    puts("wifi {\"ssid\":\"YOUR_SSID\",\"password\":\"YOUR_PASSWORD\"}");
    puts("status | memory | id | contacts | call 1 | answer | reject | hangup | prompt 123456 | "
         "ring incoming|outgoing|stop | cue ended|warning|tap|wifi | rx-record | rx-dump | retry "
         "| help");
    puts("BOOT single: idle=call contact 1 | incoming=answer");
    puts("BOOT double: calling/connected=hangup");
    puts("BOOT 4 clicks: clear saved Wi-Fi, keep binding identity, restart into XiaoTai SoftAP");
    puts("BOOT hold 7s: clear local binding identity, keep Wi-Fi, then restart");
    puts("First boot/no Wi-Fi: connect XiaoTai-XXXX, then open http://192.168.6.1; serial wifi JSON remains a fallback.");
    puts("After Wi-Fi connects, the six-digit BIND CODE repeats until authorization.");
    printf(
        "Then open WeChat mini-program and authorize this device. Mini-program version=%u (%s).\n",
        (unsigned)APP_WECHAT_VERSION,
        APP_WECHAT_VERSION == WECHAT_VERSION_RELEASE       ? "release"
        : APP_WECHAT_VERSION == WECHAT_VERSION_DEVELOPMENT ? "development"
                                                           : "trial");
}

static void configure_nonblocking_console_input(void)
{
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags < 0 || fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) < 0) {
        ESP_LOGW("app", "console input remains blocking; product feedback may stall");
        return;
    }
    ESP_LOGI("app", "console input ready: nonblocking");
}

static void start_network(const char *ssid, const char *password)
{
    if (s_started) {
        puts("Wi-Fi saved. Restart device to use the new network.");
        return;
    }
    esp_err_t err = platform_start(ssid, password, APP_DISCOVERY_URL, wechat_voip_platform_event);
    if (err == ESP_OK) {
        s_started = true;
        atomic_store_explicit(&s_platform_start_failed, false,
                              memory_order_release);
    } else {
        atomic_store_explicit(&s_platform_start_failed, true,
                              memory_order_release);
        status_led_feedback(STATUS_LED_FEEDBACK_WARNING);
        play_cue(PRODUCT_AUDIO_CUE_WARNING, "platform-start-failed");
        ESP_LOGE("app", "platform start: %s", esp_err_to_name(err));
    }
}
static void command(char *line)
{
    if (strncmp(line, "wifi ", 5) == 0) {
        cJSON *root = cJSON_Parse(line + 5);
        cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
        cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");
        if (cJSON_IsString(ssid) && cJSON_IsString(password)) {
            esp_err_t err = platform_save_wifi(ssid->valuestring, password->valuestring);
            if (err == ESP_OK) {
                if (wifi_provisioning_active()) {
                    ESP_LOGI("app", "serial Wi-Fi saved during provisioning; restarting");
                    memset(password->valuestring, 0,
                           strlen(password->valuestring));
                    cJSON_Delete(root);
                    vTaskDelay(pdMS_TO_TICKS(100));
                    esp_restart();
                    return;
                }
                start_network(ssid->valuestring, password->valuestring);
            } else {
                ESP_LOGE("app", "save Wi-Fi: %s", esp_err_to_name(err));
            }
            memset(password->valuestring, 0, strlen(password->valuestring));
        } else
            puts("Invalid wifi JSON");
        cJSON_Delete(root);
        return;
    }
    if (strcmp(line, "help") == 0) {
        help();
        return;
    }
    if (strcmp(line, "memory") == 0) {
        memory_probe_snapshot();
        return;
    }
    if (strcmp(line, "id") == 0) {
        platform_identity_t id;
        platform_identity_snapshot(&id);
        printf("device_id=%s client_id=%s\n", id.device_id, platform_client_id());
        memset(&id, 0, sizeof(id));
        return;
    }
    if (strncmp(line, "prompt ", 7) == 0) {
        const char *value = line + 7;
        s_preview_ring = PRODUCT_AUDIO_RING_NONE;
        (void)product_audio_set_ring(PRODUCT_AUDIO_RING_NONE);
        esp_err_t err = strcmp(value, "success") == 0
                            ? product_audio_play_confirmation()
                            : binding_prompt_play_code(value, NULL);
        if (err != ESP_OK) {
            ESP_LOGE("app", "prompt playback failed: %s",
                     esp_err_to_name(err));
        }
        return;
    }
    if (strncmp(line, "ring ", 5) == 0) {
        const char *value = line + 5;
        if (strcmp(value, "incoming") == 0) {
            s_preview_ring = PRODUCT_AUDIO_RING_INCOMING;
        } else if (strcmp(value, "outgoing") == 0) {
            s_preview_ring = PRODUCT_AUDIO_RING_OUTGOING;
        } else if (strcmp(value, "stop") == 0) {
            s_preview_ring = PRODUCT_AUDIO_RING_NONE;
        } else {
            puts("Use ring incoming|outgoing|stop");
            return;
        }
        esp_err_t err = product_audio_set_ring(s_preview_ring);
        if (err != ESP_OK) {
            ESP_LOGE("app", "ring preview failed: %s", esp_err_to_name(err));
        }
        return;
    }
    if (strncmp(line, "cue ", 4) == 0) {
        const char *value = line + 4;
        product_audio_cue_t cue;
        if (strcmp(value, "ended") == 0) {
            cue = PRODUCT_AUDIO_CUE_CALL_ENDED;
        } else if (strcmp(value, "warning") == 0) {
            cue = PRODUCT_AUDIO_CUE_WARNING;
        } else if (strcmp(value, "tap") == 0) {
            cue = PRODUCT_AUDIO_CUE_TAP;
        } else if (strcmp(value, "wifi") == 0) {
            cue = PRODUCT_AUDIO_CUE_WIFI_PROVISIONING;
        } else {
            puts("Use cue ended|warning|tap|wifi");
            return;
        }
        s_preview_ring = PRODUCT_AUDIO_RING_NONE;
        (void)product_audio_set_ring(PRODUCT_AUDIO_RING_NONE);
        play_cue(cue, value);
        return;
    }
    voip_command_t action;
    unsigned index = 0;
    if (strcmp(line, "status") == 0)
        action = VOIP_STATUS;
    else if (strcmp(line, "contacts") == 0)
        action = VOIP_CONTACTS;
    else if (strcmp(line, "answer") == 0)
        action = VOIP_ANSWER;
    else if (strcmp(line, "reject") == 0)
        action = VOIP_REJECT;
    else if (strcmp(line, "hangup") == 0 || strcmp(line, "cancel") == 0)
        action = VOIP_HANGUP;
    else if (strcmp(line, "rx-dump") == 0)
        action = VOIP_RX_DUMP;
    else if (strcmp(line, "rx-record") == 0)
        action = VOIP_RX_RECORD;
    else if (strcmp(line, "retry") == 0)
        action = VOIP_RETRY;
    else if (strncmp(line, "call ", 5) == 0) {
        char *end;
        unsigned long value = strtoul(line + 5, &end, 10);
        if (end == line + 5 || *end || value == 0 || value > 8) {
            puts("Use call 1..8");
            return;
        }
        index = value;
        action = VOIP_CALL;
    } else {
        puts("Unknown command; enter help");
        return;
    }
    esp_err_t err = wechat_voip_command(action, index);
    if (err == ESP_OK &&
        (action == VOIP_ANSWER || action == VOIP_REJECT ||
         action == VOIP_HANGUP)) {
        s_preview_ring = PRODUCT_AUDIO_RING_NONE;
        s_call_ring_suppressed = true;
        (void)product_audio_set_ring(PRODUCT_AUDIO_RING_NONE);
    } else if (err != ESP_OK) {
        ESP_LOGE("app", "command not queued: %s", esp_err_to_name(err));
    }
}
void app_main(void)
{
    ESP_ERROR_CHECK(heap_caps_register_failed_alloc_callback(allocation_failed));
    memory_probe_init();
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE("app", "NVS init failed: %s; identity not erased", esp_err_to_name(err));
        return;
    }
    const product_audio_config_t audio = {
        .uart_port = (uart_port_t)APP_VB6824_UART_NUM,
        .tx_gpio = (gpio_num_t)APP_VB6824_TX_GPIO,
        .rx_gpio = (gpio_num_t)APP_VB6824_RX_GPIO,
        .reset_gpio = (gpio_num_t)APP_VB6824_RESET_GPIO,
        .baud_rate = APP_VB6824_BAUD_RATE,
        .speaker_volume = APP_SPEAKER_VOLUME,
    };
    ESP_ERROR_CHECK(product_audio_init(&audio));
    const status_led_config_t status_led = {
        .data_gpio = (gpio_num_t)APP_STATUS_LED_GPIO,
        .pixel_count = APP_STATUS_LED_COUNT,
        .brightness = APP_STATUS_LED_BRIGHTNESS,
    };
    ESP_ERROR_CHECK(status_led_init(&status_led));
    product_audio_set_voice_command_callback(voice_command_received, NULL);
    const platform_binding_prompt_t prompt = {
        .play_code = binding_prompt_play_code,
        .begin = binding_prompt_begin,
        .write_pcm8 = binding_prompt_write,
        .finish = binding_prompt_finish,
        .cancel = binding_prompt_cancel,
    };
    ESP_ERROR_CHECK(platform_set_binding_prompt(&prompt));
    ESP_ERROR_CHECK(boot_button_start((gpio_num_t)APP_BOOT_BUTTON_GPIO,
                                      boot_button_single_clicked,
                                      boot_button_double_clicked,
                                      boot_button_quadruple_clicked,
                                      boot_button_long_pressed,
                                      APP_BOOT_LONG_PRESS_MS,
                                      APP_BOOT_DOUBLE_CLICK_MS));
    ESP_LOGI("app",
              "BOOT ready: gpio=%d single=call/answer double=hangup four-click=reset-Wi-Fi window=%ums hold=%ums=clear-local-binding",
              APP_BOOT_BUTTON_GPIO, (unsigned)APP_BOOT_DOUBLE_CLICK_MS,
              (unsigned)APP_BOOT_LONG_PRESS_MS);
    char ssid[33] = APP_WIFI_SSID, password[65] = APP_WIFI_PASSWORD;
    bool force_provisioning = platform_wifi_provisioning_requested();
    if (!force_provisioning && !ssid[0])
        platform_load_wifi(ssid, sizeof(ssid), password, sizeof(password));
    help();
    if (force_provisioning || !ssid[0]) {
        memset(ssid, 0, sizeof(ssid));
        memset(password, 0, sizeof(password));
        err = wifi_provisioning_start();
        if (err != ESP_OK) {
            atomic_store_explicit(&s_platform_start_failed, true,
                                  memory_order_release);
            status_led_feedback(STATUS_LED_FEEDBACK_WARNING);
            ESP_LOGE("app", "SoftAP provisioning start failed: %s",
                     esp_err_to_name(err));
        } else {
            ESP_LOGI("app",
                     "Wi-Fi provisioning mode active: SSID=XiaoTai-XXXX portal=http://192.168.6.1 forced=%d",
                     force_provisioning ? 1 : 0);
            play_cue(PRODUCT_AUDIO_CUE_WIFI_PROVISIONING,
                     "wifi-provisioning");
        }
    } else {
        ESP_ERROR_CHECK(wechat_voip_start(APP_WECHAT_VERSION));
        start_network(ssid, password);
    }
    memset(password, 0, sizeof(password));
    size_t used = 0;
    bool overflow = false;
    setvbuf(stdin, NULL, _IONBF, 0);
    configure_nonblocking_console_input();
    for (;;) {
        memory_probe_sample();
        memory_probe_poll();
        report_allocation_failure();
        process_wifi_reset();
        process_local_binding_reset();
        process_wifi_provisioning();
        process_call_ring_stop();
        update_product_feedback();
        int ch = getchar();
        if (ch == EOF) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            if (overflow)
                puts("Command too long; discarded");
            else if (used) {
                s_line[used] = 0;
                command(s_line);
            }
            memset(s_line, 0, sizeof(s_line));
            used = 0;
            overflow = false;
        } else if (ch == 8 || ch == 127) {
            if (used)
                --used;
        } else if (ch >= 32 && !overflow) {
            if (used + 1 < sizeof(s_line))
                s_line[used++] = (char)ch;
            else
                overflow = true;
        }
    }
}
