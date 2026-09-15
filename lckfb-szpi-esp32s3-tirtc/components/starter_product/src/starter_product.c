/*
 * 小钛 S3 产品界面，使用 320x240 屏幕坐标。
 *
 * LVGL 回调只写本地状态或投递 runtime 意图；网络与 TiRTC 状态仍由各自任务
 * 持有。屏幕休眠使用单调时间，触摸和持续声音活动均可唤醒。
 */
#include "starter_product.h"

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs.h"
#include "nvs_worker.h"
#include "platform_client.h"
#include "starter_media.h"
#include "starter_runtime.h"
#include "wifi_manager.h"

LV_FONT_DECLARE(ui_font_cn_18);
#define PRODUCT_TEXT_FONT ui_font_cn_18

#define LCD_HOST SPI3_HOST
#define LCD_H_RES 320
#define LCD_V_RES 240
#define LCD_NATIVE_H_RES 240
#define LCD_NATIVE_V_RES 320
#define LCD_PIN_MOSI GPIO_NUM_40
#define LCD_PIN_SCLK GPIO_NUM_41
#define LCD_PIN_DC GPIO_NUM_39
#define LCD_PIN_BL GPIO_NUM_42
#define LCD_BACKLIGHT_ON_LEVEL 0
#define LCD_BACKLIGHT_OFF_LEVEL 1
#define LCD_DRAW_LINES 10
#define LCD_PIXEL_CLOCK_HZ (80U * 1000U * 1000U)
#define TOUCH_I2C_HZ 100000U

#define PRODUCT_NVS "product"
#define PRODUCT_AMBIENT_VISIBLE_MS 4500
#define PRODUCT_IDLE_DAYDREAM_MS 45000
#define PRODUCT_IDLE_SLEEPY_MS 180000
#define PRODUCT_VOICE_FEEDBACK_MS 3200
#define PRODUCT_WIFI_CONNECTED_TEXT "Wi-Fi 连接成功"
#define PRODUCT_WIFI_FAILED_TEXT "Wi-Fi 连接失败，请重新配网"
#define PRODUCT_REST_AUDIO_GUARD_MS 2500
#define PRODUCT_VOICE_QUEUE_LENGTH 8
#define WAV_PCM_OFFSET 44U
/* CPU1 is reserved for the capture/AFE path (priority 7/8).  Keeping
 * LVGL on CPU0 prevents a full UI refresh or touch burst from stealing an
 * audio deadline, while priority 6 keeps it ahead of ordinary controls. */
#define PRODUCT_UI_TASK_PRIORITY 6
#define PRODUCT_UI_TASK_CORE 0
#define PRODUCT_UI_TASK_STACK_BYTES (16 * 1024)
#define PRODUCT_UI_REFRESH_SLOW_US (20LL * 1000LL)
#define PRODUCT_UI_REFRESH_LOG_INTERVAL_US (5LL * 1000LL * 1000LL)
#define FACE_CANVAS_W 220
#define FACE_CANVAS_H 108
#define HOME_BACKGROUND_COLOR 0x141719

extern const uint8_t kids_watch_outgoing_tiny_wav_start[]
    asm("_binary_kids_watch_outgoing_tiny_wav_start");
extern const uint8_t kids_watch_outgoing_tiny_wav_end[]
    asm("_binary_kids_watch_outgoing_tiny_wav_end");
extern const uint8_t kids_watch_incoming_tiny_wav_start[]
    asm("_binary_kids_watch_incoming_tiny_wav_start");
extern const uint8_t kids_watch_incoming_tiny_wav_end[]
    asm("_binary_kids_watch_incoming_tiny_wav_end");
extern const uint8_t ai_ack_female_8k_wav_start[]
    asm("_binary_ai_ack_female_8k_wav_start");
extern const uint8_t ai_ack_female_8k_wav_end[]
    asm("_binary_ai_ack_female_8k_wav_end");
extern const uint8_t ai_ack_male_8k_wav_start[]
    asm("_binary_ai_ack_male_8k_wav_start");
extern const uint8_t ai_ack_male_8k_wav_end[]
    asm("_binary_ai_ack_male_8k_wav_end");

typedef enum {
    PAGE_HOME_FACE = 0,
    PAGE_HOME_CLOCK,
    PAGE_MENU = 3,
    PAGE_CONTACTS,
    PAGE_CONTACT_DETAIL,
    PAGE_EMOJIS,
    PAGE_EMOJI_PREVIEW,
    PAGE_SETTINGS,
    PAGE_NETWORK,
    PAGE_BINDING,
    PAGE_CALL,
    PAGE_CALL_RESULT,
    PAGE_DIAGNOSTICS,
    PAGE_ROOM,
} product_page_t;

typedef enum {
    ACTION_AI = 1,
    ACTION_MENU,
    ACTION_HOME,
    ACTION_CONTACTS,
    ACTION_EMOJIS,
    ACTION_SETTINGS,
    ACTION_NETWORK,
    ACTION_VOLUME_DOWN,
    ACTION_VOLUME_UP,
    ACTION_SPEAKER_MUTE,
    ACTION_MIC_MUTE,
    ACTION_SLEEP,
    ACTION_ACK_VOICE,
    ACTION_EMOJI_APPLY,
    ACTION_NETWORK_BACK,
    ACTION_CALL_ACCEPT,
    ACTION_CALL_REJECT,
    ACTION_CALL_HANGUP,
    ACTION_CALL_MUTE,
    ACTION_CONTACTS_BACK = 22,
    ACTION_VOICE_CALL,
    ACTION_DIAGNOSTICS = 25,
    ACTION_DIAG_SYSTEM,
    ACTION_DIAG_AUDIO,
    ACTION_DIAG_EVENTS,
    ACTION_CONTACT_BASE = 100,
    ACTION_CONTACT_CALL_BASE = 120,
    ACTION_EMOJI_BASE = 200,
} product_action_t;

typedef struct {
    uint8_t volume;
    bool speaker_muted;
    bool microphone_muted;
    bool acknowledgement_male;
    uint8_t sleep_index;
    uint8_t emoji_index;
} product_preferences_t;

static const char *TAG = "starter_product";
static EXT_RAM_BSS_ATTR atomic_int s_binding_ui_state;
static bool s_started;
static QueueHandle_t s_voice_queue;
static esp_lcd_panel_io_handle_t s_lcd_io;
static esp_lcd_panel_handle_t s_lcd_panel;
static esp_lcd_touch_handle_t s_touch;
static lv_disp_t *s_display;
static product_page_t s_page = PAGE_HOME_FACE;
static product_preferences_t s_preferences = {.volume = 7, .sleep_index = 1};
static int64_t s_last_interaction_ms;
static int64_t s_ambient_hide_ms;
typedef enum {
    CALL_RING_NONE = 0,
    CALL_RING_OUTGOING,
    CALL_RING_INCOMING,
    /* One-shot local acknowledgement before the remote AI session is ready. */
    CALL_RING_AI_ACK,
} call_ring_kind_t;
typedef enum {
    WIFI_NOTICE_NONE = 0,
    WIFI_NOTICE_CONNECTED,
    WIFI_NOTICE_FAILED,
} wifi_notice_t;
static atomic_uint s_call_ring_kind;
typedef struct {
    int64_t expires_ms;
    bool male;
    uint32_t expected_session;
    uint32_t playback_epoch;
} ai_ack_t;
static QueueHandle_t s_ai_ack_queue;
static ai_ack_t s_pending_ai_ack; /* UI task only; queue publishes to audio task */
static lv_obj_t *s_wake_debug_label; /* Top-layer object: survives page replacement. */
static int64_t s_wake_debug_hide_ms;
static uint32_t s_wake_debug_session;
static uint32_t s_previous_ai_generation;
static TaskHandle_t s_call_ring_task;
static uint32_t s_wifi_signal_revision = UINT32_MAX;
static int s_wifi_signal_style = -1;
static bool s_previous_wifi_connected;
static bool s_previous_wifi_failed;
static wifi_notice_t s_pending_wifi_notice;
static int64_t s_voice_feedback_hide_ms;
static int64_t s_audio_wake_guard_until_ms;
static bool s_display_awake = true;
static bool s_ambient_visible;
static uint8_t s_previous_audio_level;
static uint8_t s_expression_audio_level;
static starter_runtime_state_t s_previous_runtime_state = STARTER_RUNTIME_WAITING;
static starter_ai_ui_phase_t s_previous_ai_phase = STARTER_AI_UI_IDLE;
static char s_rendered_expression[16];
static char s_call_result[33];
static char s_previous_verification_code[17];
static char s_voice_feedback[65];
static uint8_t s_selected_contact;
static uint8_t s_preview_emoji;
static product_page_t s_network_back_page = PAGE_MENU;
static uint8_t s_previous_contact_count;
static EXT_RAM_BSS_ATTR starter_product_contact_t
    s_previous_contacts[STARTER_PRODUCT_CONTACTS_MAX];
static int64_t s_ui_refresh_slow_last_us;

/* 当前页面对象只在 LVGL 任务内创建和访问。 */
static lv_obj_t *s_clock;
static lv_obj_t *s_wifi_signal;
static lv_obj_t *s_wifi_signal_bars[4];
static lv_obj_t *s_date;
static lv_obj_t *s_state;
static lv_obj_t *s_subtitle;
static lv_obj_t *s_face;
/* One persistent RGB565 scene is deliberately used for a face.  Independent
 * LVGL objects had valid geometry but their separate dirty rectangles could be
 * dropped by this panel's flush path.  A canvas gives the display one atomic
 * face region to commit, matching the image-asset pattern in device-monitor. */
static lv_color_t *s_face_canvas_buffer;
static lv_obj_t *s_header_title;
static lv_obj_t *s_call_peer_label;
static lv_obj_t *s_call_status_label;
static lv_obj_t *s_call_accept_button;
static lv_obj_t *s_call_reject_button;
static lv_obj_t *s_call_mute_button;
static lv_obj_t *s_call_hangup_button;
/* Settings owns a small persistent set of labels.  These pointers are only
 * dereferenced from the LVGL task and are cleared before a page is rebuilt. */
static lv_obj_t *s_settings_volume;
static lv_obj_t *s_settings_speaker;
static lv_obj_t *s_settings_microphone;
static lv_obj_t *s_settings_sleep;
static lv_obj_t *s_settings_acknowledgement;

static const uint16_t s_sleep_minutes[] = {1, 5, 10, 30, 0};
static const char *const s_sleep_names[] = {
    "1 分钟", "5 分钟", "10 分钟", "30 分钟", "永不",
};
static const char *const s_emoji_names[] = {
    "中性", "开心", "大笑", "逗趣", "难过", "生气", "哭泣",
    "喜爱", "害羞", "惊讶", "震惊", "思考", "眨眼", "酷",
    "放松", "美味", "亲亲", "自信", "困倦", "搞怪", "困惑",
    "聆听", "悠闲",
};
static const char *const s_emoji_keys[] = {
    "neutral", "happy", "laughing", "funny", "sad", "angry", "crying",
    "loving", "embarrassed", "surprised", "shocked", "thinking",
    "winking", "cool", "relaxed", "delicious", "kissy", "confident",
    "sleepy", "silly", "confused",
    "listening", "ambient",
};

void starter_product_set_binding_state(starter_product_binding_state_t state)
{
    atomic_store_explicit(&s_binding_ui_state, state, memory_order_release);
}

static int64_t monotonic_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void s3_face_visibility_changed(bool awake);

static void backlight_set(bool awake)
{
    int level =
        awake ? LCD_BACKLIGHT_ON_LEVEL : LCD_BACKLIGHT_OFF_LEVEL;
    (void)gpio_set_level(LCD_PIN_BL, level);
    s_display_awake = awake;
    s3_face_visibility_changed(awake);
}

static void note_interaction(void)
{
    s_last_interaction_ms = monotonic_ms();
    if (!s_display_awake) {
        backlight_set(true);
    }
}

static esp_err_t preferences_write(const product_preferences_t *snapshot)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(PRODUCT_NVS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    const struct {
        const char *key;
        uint8_t value;
    } values[] = {
        {"volume", snapshot->volume},
        {"spk_mute", snapshot->speaker_muted ? 1U : 0U},
        {"mic_mute", snapshot->microphone_muted ? 1U : 0U},
        {"ack_voice", snapshot->acknowledgement_male ? 1U : 0U},
        {"sleep", snapshot->sleep_index},
        {"emoji", snapshot->emoji_index},
    };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        err = nvs_set_u8(nvs, values[i].key, values[i].value);
        if (err != ESP_OK) {
            break;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static esp_err_t preferences_write_job(void *data, size_t size)
{
    if (size != sizeof(product_preferences_t)) return ESP_ERR_INVALID_SIZE;
    return preferences_write(data);
}

static void preferences_save(void)
{
    /* Only the LVGL owner submits preferences. Copy all fields before return;
     * newer edits replace only the queued snapshot, never the in-flight write.
     * UI/media changes stay immediate; persistence never blocks that owner. */
    esp_err_t err = nvs_worker_submit_latest(preferences_write_job,
                                              &s_preferences, sizeof(s_preferences));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "preferences save not queued: %s", esp_err_to_name(err));
    }
}

static esp_err_t preferences_load_job(void *data, size_t size)
{
    if (size != sizeof(product_preferences_t)) return ESP_ERR_INVALID_SIZE;
    product_preferences_t *snapshot = data;
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(PRODUCT_NVS, NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        uint8_t value = 0;
        if (nvs_get_u8(nvs, "volume", &value) == ESP_OK && value <= 10U) {
            snapshot->volume = value;
        }
        if (nvs_get_u8(nvs, "spk_mute", &value) == ESP_OK) {
            snapshot->speaker_muted = value != 0U;
        }
        if (nvs_get_u8(nvs, "mic_mute", &value) == ESP_OK) {
            snapshot->microphone_muted = value != 0U;
        }
        if (nvs_get_u8(nvs, "ack_voice", &value) == ESP_OK) {
            snapshot->acknowledgement_male = value != 0U;
        }
        if (nvs_get_u8(nvs, "sleep", &value) == ESP_OK &&
            value < sizeof(s_sleep_minutes) / sizeof(s_sleep_minutes[0])) {
            snapshot->sleep_index = value;
        }
        if (nvs_get_u8(nvs, "emoji", &value) == ESP_OK &&
            value < sizeof(s_emoji_names) / sizeof(s_emoji_names[0])) {
            snapshot->emoji_index = value;
        }
        nvs_close(nvs);
    }
    return err;
}

static void preferences_load(void)
{
    esp_err_t err = nvs_worker_call(preferences_load_job, &s_preferences,
                                     sizeof(s_preferences), NVS_WORKER_WAIT_MS);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
        ESP_LOGE(TAG, "preferences load failed: %s", esp_err_to_name(err));
    (void)starter_media_set_speaker_volume(s_preferences.volume);
    (void)starter_media_set_speaker_muted(s_preferences.speaker_muted);
    starter_media_set_microphone_muted(s_preferences.microphone_muted);
}

static void set_bg(lv_obj_t *object, lv_color_t color)
{
    lv_obj_set_style_bg_color(object, color, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(object, 0, 0);
}

static lv_obj_t *make_label(lv_obj_t *parent,
                            const char *text,
                            lv_coord_t x,
                            lv_coord_t y,
                            lv_coord_t width,
                            lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, width);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_font(label, &PRODUCT_TEXT_FONT, 0);
    return label;
}

/* LVGL invalidates and lays out a label even when callers write the same text.
 * Periodic product ticks therefore compare first, especially for the clock and
 * the normally-empty activity label. */
static void label_set_text_if_changed(lv_obj_t *label, const char *text)
{
    if (label == NULL || text == NULL) {
        return;
    }
    const char *current = lv_label_get_text(label);
    if (current == NULL || strcmp(current, text) != 0) {
        lv_label_set_text(label, text);
    }
}

static lv_obj_t *make_button(lv_obj_t *parent,
                             const char *text,
                             lv_coord_t x,
                             lv_coord_t y,
                             lv_coord_t width,
                             lv_coord_t height,
                             product_action_t action);
static void render_page(void);
static void refresh_settings_controls(void);
static void refresh_call_controls(starter_runtime_status_t runtime,
                                  const starter_runtime_product_snapshot_t *product);
static const char *phase_text(starter_ai_ui_phase_t phase);
static void apply_expression(const char *emotion,
                             starter_ai_ui_phase_t phase,
                             uint8_t activity_level);

/* 普通功能页退出 AI；菜单、返回和诊断页保持会话。
 * 人际通话由更高优先级的状态机管理，绝不在页面导航中挂断。 */
static bool page_ends_ai(product_page_t page)
{
    return page == PAGE_CONTACTS || page == PAGE_CONTACT_DETAIL ||
           page == PAGE_EMOJIS || page == PAGE_EMOJI_PREVIEW ||
           page == PAGE_SETTINGS || page == PAGE_NETWORK || page == PAGE_ROOM;
}

static bool enter_page(product_page_t page)
{
    if (page_ends_ai(page)) {
        /* Queue even while the public snapshot says WAITING: a previous AI
         * start may already be queued. Runtime revalidates STOP, never ends calls. */
        esp_err_t err = starter_runtime_ai_stop();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "page navigation rejected: AI stop queue full page=%u", (unsigned)page);
            return false;
        }
    }
    s_page = page;
    return true;
}

static void enter_settings_page(void)
{
    (void)enter_page(PAGE_SETTINGS);
}

static lv_obj_t *s_diagnostics_label;
static lv_obj_t *s_diagnostics_panel;
static unsigned s_diagnostics_tab;
static int64_t s_diagnostics_due_ms;
static EXT_RAM_BSS_ATTR char s_diagnostics_text[1400];
static void refresh_diagnostics(int64_t now);

/* All S3 pages share this renderer and asynchronous action owner. */
static bool s3_handle_action(product_action_t action);
static void s3_style_button(lv_obj_t *button);
static void s3_reset_page(void);
static void s3_render_call(lv_obj_t *screen);
static bool s3_render_page(lv_obj_t *screen);
static void s3_refresh_settings(void);
static void s3_refresh_ui(int64_t now, starter_runtime_status_t runtime,
                          const starter_runtime_product_snapshot_t *product);
static bool s3_ai_request_pending(void);
static void s3_note_ai_request(starter_runtime_state_t from);

static void set_voice_feedback_for(const char *text, int64_t now,
                                   int64_t duration_ms)
{
    (void)snprintf(s_voice_feedback, sizeof(s_voice_feedback), "%s",
                   text == NULL ? "" : text);
    s_voice_feedback_hide_ms = now + duration_ms;
}

static void set_voice_feedback(const char *text, int64_t now)
{
    set_voice_feedback_for(text, now, PRODUCT_VOICE_FEEDBACK_MS);
}

static void set_wifi_feedback(wifi_notice_t notice, int64_t now)
{
    const char *text = notice == WIFI_NOTICE_CONNECTED
                           ? PRODUCT_WIFI_CONNECTED_TEXT
                           : PRODUCT_WIFI_FAILED_TEXT;
    set_voice_feedback(text, now);
}

static void format_wake_debug(char *text, size_t size, const starter_voice_result_t *result)
{
    if (result == NULL || !result->acoustic_score_valid) {
        (void)snprintf(text, size, "手动启动");
        return;
    }
    unsigned score = result->probability_milli;
    unsigned threshold = result->threshold_milli;
    (void)snprintf(text, size, "语音唤醒 %u.%03u / 阈值 %u.%03u",
                   score / 1000, score % 1000, threshold / 1000, threshold % 1000);
}

static void show_wake_debug(const starter_voice_result_t *result, int64_t now)
{
    char text[80];
    format_wake_debug(text, sizeof(text), result);
    /* Acoustic scores remain available in diagnostics, not over conversation. */
    if (s_page != PAGE_DIAGNOSTICS) return;
    if (s_wake_debug_label == NULL) {
        s_wake_debug_label = make_label(lv_layer_top(), "", 8, 36, 304,
                                       lv_color_hex(0xFFFFFF));
        set_bg(s_wake_debug_label, lv_color_hex(0x163440));
        lv_obj_set_style_text_align(s_wake_debug_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_ver(s_wake_debug_label, 4, 0);
        lv_obj_clear_flag(s_wake_debug_label, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_label_set_text(s_wake_debug_label, text);
    lv_obj_clear_flag(s_wake_debug_label, LV_OBJ_FLAG_HIDDEN);
    s_wake_debug_hide_ms = now + 5000;
}

static void handle_voice_result(const starter_voice_result_t *result,
                                int64_t now)
{
    if (result == NULL || result->intent == STARTER_VOICE_INTENT_NONE) {
        return;
    }
    if (!wifi_manager_connected()) {
        /* Offline onboarding owns the UI. Release any wake preroll without
         * acknowledging or starting an unreachable cloud conversation. */
        starter_media_cancel_ai_preroll(result->wake_token);
        return;
    }
    note_interaction();
    switch (result->intent) {
    case STARTER_VOICE_INTENT_WAKE_ONLY:
        {
            if (starter_media_status().microphone_muted ||
                (result->captured_ms != 0 && now - result->captured_ms > 1000)) {
                starter_media_cancel_ai_preroll(result->wake_token);
                ESP_LOGI(TAG, "wake rejected by product: muted or stale");
                break;
            }
            /*
             * “小钛” is deliberately a single, predictable action: open
             * the remote AI conversation.  Local commands remain available to
             * the developer console, but are no longer run from the live wake
             * path.  A foreground call owns the microphone and is never
             * interrupted by a wake detection.
             */
            starter_runtime_status_t status = starter_runtime_status();
            if (s3_ai_request_pending()) {
                /* The LVGL owner already accepted a tap/wake. Its runtime
                 * transition may still be queued; do not acknowledge twice. */
                starter_media_cancel_ai_preroll(result->wake_token);
                break;
            }
            if (status.state == STARTER_RUNTIME_WAITING ||
                status.state == STARTER_RUNTIME_H5_ACTIVE) {
                esp_err_t err = result->wake_token != 0 ?
                    starter_runtime_ai_start_from_wake(result->wake_token) :
                    starter_runtime_ai_start();
                if (err == ESP_OK) {
                    s3_note_ai_request(status.state);
                    ESP_LOGI(TAG, "wake request queued token=%lu", (unsigned long)result->wake_token);
                    s_pending_ai_ack = (ai_ack_t){
                        .expires_ms = now + 1000,
                        .male = s_preferences.acknowledgement_male,
                        .expected_session = status.session_generation + 1U,
                    };
                    if (s_pending_ai_ack.expected_session == 0) s_pending_ai_ack.expected_session = 1;
                    s_wake_debug_session = s_pending_ai_ack.expected_session;
                    show_wake_debug(result, now);
                    /* The acknowledgement assets are 1.2--1.6 seconds.
                     * Do not keep the UI in a fake-success state after the
                     * clip has completed: the runtime state below owns the
                     * subsequent \"connecting\" / \"active\" status. */
                    set_voice_feedback_for("在呢。", now, 1700);
                } else {
                    starter_media_cancel_ai_preroll(result->wake_token);
                    set_voice_feedback("现在暂时不能开始对话", now);
                }
                backlight_set(true);
                if (s_page != PAGE_HOME_FACE && s_page != PAGE_HOME_CLOCK) {
                    s_page = PAGE_HOME_FACE;
                    render_page();
                }
            } else {
                starter_media_cancel_ai_preroll(result->wake_token);
                ESP_LOGI(TAG, "wake ignored while foreground state=%s",
                         starter_runtime_state_name(status.state));
                /* 已建立 AI 时，重复说唤醒词不应看起来像设备失灵：会话已经
                 * 占有麦克风，用户可以直接说“呼叫爸爸”。通话/H5 仍严格不
                 * 响应，防止前台媒体被误抢占。 */
                if (status.state == STARTER_RUNTIME_AI_CONNECTING ||
                    status.state == STARTER_RUNTIME_AI_ACTIVE) {
                    set_voice_feedback("我在听，请直接说。", now);
                    if (s_page != PAGE_HOME_FACE && s_page != PAGE_HOME_CLOCK) {
                        s_page = PAGE_HOME_FACE;
                        render_page();
                    }
                }
            }
        }
        break;
    case STARTER_VOICE_INTENT_OPEN_SETTINGS:
        enter_settings_page();
        render_page();
        break;
    case STARTER_VOICE_INTENT_VOLUME_UP:
        if (s_preferences.volume < 10U) {
            ++s_preferences.volume;
            (void)starter_media_set_speaker_volume(s_preferences.volume);
            preferences_save();
        }
        ESP_LOGI(TAG, "speaker volume=%u/10 (voice up)",
                 (unsigned)s_preferences.volume);
        {
            char feedback[32];
            (void)snprintf(feedback, sizeof(feedback), "音量已调至 %u",
                           s_preferences.volume);
            set_voice_feedback(feedback, now);
        }
        refresh_settings_controls();
        break;
    case STARTER_VOICE_INTENT_VOLUME_DOWN:
        if (s_preferences.volume > 0U) {
            --s_preferences.volume;
            (void)starter_media_set_speaker_volume(s_preferences.volume);
            preferences_save();
        }
        ESP_LOGI(TAG, "speaker volume=%u/10 (voice down)",
                 (unsigned)s_preferences.volume);
        {
            char feedback[32];
            (void)snprintf(feedback, sizeof(feedback), "音量已调至 %u",
                           s_preferences.volume);
            set_voice_feedback(feedback, now);
        }
        refresh_settings_controls();
        break;
    case STARTER_VOICE_INTENT_MUTE_UPLINK:
        /* 全局闭麦停止上行及本地唤醒；底层 AEC 采集仍继续。 */
        s_preferences.microphone_muted = true;
        starter_media_set_microphone_muted(true);
        (void)starter_runtime_call_set_microphone_muted(true);
        preferences_save();
        set_voice_feedback("已闭麦 · 本地唤醒暂停", now);
        s_page = PAGE_HOME_FACE;
        render_page();
        break;
    case STARTER_VOICE_INTENT_UNMUTE_UPLINK:
        s_preferences.microphone_muted = false;
        starter_media_set_microphone_muted(false);
        (void)starter_runtime_call_set_microphone_muted(false);
        preferences_save();
        set_voice_feedback("远程麦克风已开启", now);
        s_page = PAGE_HOME_FACE;
        render_page();
        break;
    case STARTER_VOICE_INTENT_OPEN_CONTACTS:
        (void)s3_handle_action(ACTION_CONTACTS);
        break;
    case STARTER_VOICE_INTENT_REST:
        /* 休息不关网络、不重启，也不误挂正在进行的人际通话。 */
        {
            starter_runtime_state_t state = starter_runtime_status().state;
            if (state == STARTER_RUNTIME_AI_CONNECTING ||
                state == STARTER_RUNTIME_AI_ACTIVE) {
                (void)starter_runtime_ai_stop();
            }
        }
        s_page = PAGE_HOME_FACE;
        s_voice_feedback[0] = '\0';
        render_page();
        s_audio_wake_guard_until_ms = now + PRODUCT_REST_AUDIO_GUARD_MS;
        backlight_set(false);
        break;
    case STARTER_VOICE_INTENT_START_AI_CHAT:
    case STARTER_VOICE_INTENT_CALL_CONTACT_REMOTE:
        {
            (void)s3_handle_action(ACTION_AI);
            if (result->intent == STARTER_VOICE_INTENT_CALL_CONTACT_REMOTE) {
                ESP_LOGI(TAG, "remote contact recognition requested: target=%s",
                         result->call_target);
            }
        }
        break;
    default:
        break;
    }
    ESP_LOGI(TAG, "voice intent handled: %s",
             starter_voice_intent_name(result->intent));
}

static void on_action(lv_event_t *event)
{
    product_action_t action = (product_action_t)(uintptr_t)lv_event_get_user_data(event);
    product_page_t previous_page = s_page;
    note_interaction();
    if (s3_handle_action(action)) return;
    if (action == ACTION_MENU) {
        s_page = PAGE_MENU;
    } else if (action == ACTION_HOME) {
        s_page = PAGE_HOME_FACE;
    } else if (action == ACTION_CONTACTS) {
        if (enter_page(PAGE_CONTACTS)) (void)starter_runtime_contacts_refresh();
    } else if (action == ACTION_EMOJIS) {
        (void)enter_page(PAGE_EMOJIS);
    } else if (action == ACTION_SETTINGS) {
        enter_settings_page();
    } else if (action == ACTION_NETWORK) {
        s_network_back_page = s_page == PAGE_SETTINGS ? PAGE_SETTINGS : PAGE_MENU;
        (void)enter_page(PAGE_NETWORK);
    } else if (action == ACTION_DIAGNOSTICS) {
        (void)enter_page(PAGE_DIAGNOSTICS);
    } else if (action >= ACTION_DIAG_SYSTEM && action <= ACTION_DIAG_EVENTS) {
        s_diagnostics_tab = (unsigned)(action - ACTION_DIAG_SYSTEM);
        s_diagnostics_due_ms = 0;
        refresh_diagnostics(esp_timer_get_time() / 1000);
        if (s_diagnostics_panel != NULL) lv_obj_scroll_to_y(s_diagnostics_panel, 0, LV_ANIM_OFF);
    } else if (action == ACTION_VOLUME_DOWN && s_preferences.volume > 0U) {
        s_preferences.volume--;
        (void)starter_media_set_speaker_volume(s_preferences.volume);
        preferences_save();
    } else if (action == ACTION_VOLUME_UP && s_preferences.volume < 10U) {
        s_preferences.volume++;
        (void)starter_media_set_speaker_volume(s_preferences.volume);
        preferences_save();
    } else if (action == ACTION_SPEAKER_MUTE) {
        s_preferences.speaker_muted = !s_preferences.speaker_muted;
        (void)starter_media_set_speaker_muted(s_preferences.speaker_muted);
        preferences_save();
    } else if (action == ACTION_MIC_MUTE) {
        s_preferences.microphone_muted = !s_preferences.microphone_muted;
        starter_media_set_microphone_muted(s_preferences.microphone_muted);
        preferences_save();
    } else if (action == ACTION_SLEEP) {
        s_preferences.sleep_index = (uint8_t)((s_preferences.sleep_index + 1U) %
            (sizeof(s_sleep_minutes) / sizeof(s_sleep_minutes[0])));
        preferences_save();
    } else if (action == ACTION_ACK_VOICE) {
        s_preferences.acknowledgement_male = !s_preferences.acknowledgement_male;
        preferences_save();
    } else if (action == ACTION_EMOJI_APPLY) {
        s_preferences.emoji_index = s_preview_emoji;
        preferences_save();
        s_page = PAGE_HOME_FACE;
    } else if (action == ACTION_NETWORK_BACK) {
        s_page = s_network_back_page;
    } else if (action == ACTION_CALL_MUTE) {
        s_preferences.microphone_muted = !s_preferences.microphone_muted;
        starter_media_set_microphone_muted(s_preferences.microphone_muted);
        (void)starter_runtime_call_set_microphone_muted(
            s_preferences.microphone_muted);
        preferences_save();
    } else if (action == ACTION_CONTACTS_BACK) {
        (void)enter_page(PAGE_CONTACTS);
    } else if (action >= ACTION_CONTACT_CALL_BASE &&
               action < ACTION_CONTACT_CALL_BASE + STARTER_PRODUCT_CONTACTS_MAX) {
        s_selected_contact = (uint8_t)(action - ACTION_CONTACT_CALL_BASE);
        (void)enter_page(PAGE_CONTACT_DETAIL);
    } else if (action >= ACTION_CONTACT_BASE &&
               action < ACTION_CONTACT_BASE + STARTER_PRODUCT_CONTACTS_MAX) {
        s_selected_contact = (uint8_t)(action - ACTION_CONTACT_BASE);
        (void)enter_page(PAGE_CONTACT_DETAIL);
    } else if (action >= ACTION_EMOJI_BASE &&
               action < ACTION_EMOJI_BASE +
                            sizeof(s_emoji_names) / sizeof(s_emoji_names[0])) {
        s_preview_emoji = (uint8_t)(action - ACTION_EMOJI_BASE);
        (void)enter_page(PAGE_EMOJI_PREVIEW);
    }
    /* A settings adjustment is a local label mutation, not a new screen.
     * Recreating the complete object tree here made repeated +/- taps visibly
     * jittery and competed with the audio path. Only navigation rebuilds the
     * page; same-page settings and call state update their existing controls. */
    if (s_page != previous_page) {
        render_page();
    } else if (s_page == PAGE_SETTINGS) {
        refresh_settings_controls();
    } else if (s_page == PAGE_CALL) {
        starter_runtime_product_snapshot_t product =
            starter_runtime_product_snapshot();
        refresh_call_controls(starter_runtime_status(), &product);
    }
}

static lv_obj_t *make_button(lv_obj_t *parent,
                             const char *text,
                             lv_coord_t x,
                             lv_coord_t y,
                             lv_coord_t width,
                             lv_coord_t height,
                             product_action_t action)
{
    lv_obj_t *button = lv_btn_create(parent);
    s3_style_button(button);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, height);
    lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(button, on_action, LV_EVENT_CLICKED, (void *)(uintptr_t)action);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &PRODUCT_TEXT_FONT, 0);
    lv_obj_set_width(label, width - 8);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    return button;
}

static void set_button_text(lv_obj_t *button, const char *text)
{
    if (button == NULL) {
        return;
    }
    lv_obj_t *label = lv_obj_get_child(button, 0);
    if (label != NULL) {
        label_set_text_if_changed(label, text);
    }
}

static void set_object_visible(lv_obj_t *object, bool visible)
{
    if (object == NULL) {
        return;
    }
    if (visible) {
        lv_obj_clear_flag(object, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh_settings_controls(void)
{
    s3_refresh_settings();
}

static void on_screen_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        note_interaction();
    }
}

static void on_home_tap(lv_event_t *event)
{
    (void)event;
    note_interaction();
    (void)s3_handle_action(ACTION_AI);
}

static void call_ring_task(void *argument)
{
    (void)argument;
    call_ring_kind_t logged_kind = CALL_RING_NONE;
    for (;;) {
        const uint8_t *start = NULL;
        const uint8_t *end = NULL;
        call_ring_kind_t ring_kind = s_call_ring_kind;
        ai_ack_t ack = {0};
        if (ring_kind == CALL_RING_NONE && s_ai_ack_queue != NULL &&
            xQueueReceive(s_ai_ack_queue, &ack, 0) == pdTRUE) {
            starter_runtime_status_t status = starter_runtime_status();
            if (esp_timer_get_time() / 1000 <= ack.expires_ms &&
                status.state == STARTER_RUNTIME_AI_CONNECTING &&
                status.session_generation == ack.expected_session) {
                ring_kind = CALL_RING_AI_ACK;
            }
        }
        if (ring_kind != logged_kind) {
            ESP_LOGI(TAG, "call ringtone %s",
                     ring_kind == CALL_RING_OUTGOING ? "outgoing" :
                     ring_kind == CALL_RING_INCOMING ? "incoming" : "stopped");
            logged_kind = ring_kind;
        }
        if (ring_kind == CALL_RING_OUTGOING) {
            start = kids_watch_outgoing_tiny_wav_start;
            end = kids_watch_outgoing_tiny_wav_end;
        } else if (ring_kind == CALL_RING_INCOMING) {
            start = kids_watch_incoming_tiny_wav_start;
            end = kids_watch_incoming_tiny_wav_end;
        } else if (ring_kind == CALL_RING_AI_ACK) {
            if (ack.male) {
                start = ai_ack_male_8k_wav_start;
                end = ai_ack_male_8k_wav_end;
            } else {
                start = ai_ack_female_8k_wav_start;
                end = ai_ack_female_8k_wav_end;
            }
            ESP_LOGI(TAG, "AI acknowledgement voice=%s",
                     ack.male ? "male" : "female");
        } else {
            vTaskDelay(pdMS_TO_TICKS(80));
            continue;
        }
        size_t bytes = (size_t)(end - start);
        if (bytes <= WAV_PCM_OFFSET || ((bytes - WAV_PCM_OFFSET) & 1U) != 0U) {
            ESP_LOGE(TAG, "invalid call ringtone asset");
            s_call_ring_kind = CALL_RING_NONE;
            continue;
        }
        /* tiny WAV assets are PCM 8 kHz / 16-bit / mono. */
        esp_err_t err = ring_kind == CALL_RING_AI_ACK ?
            starter_media_play_pcm8k_at_epoch(
                (const int16_t *)(start + WAV_PCM_OFFSET),
                (bytes - WAV_PCM_OFFSET) / sizeof(int16_t), ack.playback_epoch) :
            starter_media_play_pcm8k(
            (const int16_t *)(start + WAV_PCM_OFFSET),
            (bytes - WAV_PCM_OFFSET) / sizeof(int16_t));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "call ringtone playback failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        /* One-shot acknowledgement was consumed before playback. Cancellation,
         * silence or a busy output never leaves it pending for another session. */
    }
}

static void render_header_status(lv_obj_t *screen)
{
    /*
     * HOME_WIFI_REAL_RSSI / HOME_WIFI_SIGNAL_BARS /
     * HOME_WIFI_ICON_ONLY_RIGHT: signal quality without a noisy numeric value.
     */
    s_wifi_signal = lv_obj_create(screen);
    lv_obj_set_pos(s_wifi_signal, 292, 5);
    lv_obj_set_size(s_wifi_signal, 22, 24);
    lv_obj_set_style_bg_opa(s_wifi_signal, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_wifi_signal, 0, 0);
    lv_obj_set_style_pad_all(s_wifi_signal, 0, 0);
    lv_obj_clear_flag(s_wifi_signal,
                      LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    /* Use LVGL's native Wi-Fi glyph so its orientation and proportions are
     * consistent with the rest of the symbol font (the 🛜 visual). */
    lv_obj_t *wifi_glyph = lv_label_create(s_wifi_signal);
    lv_label_set_text(wifi_glyph, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(wifi_glyph, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(wifi_glyph, lv_color_hex(0x78F0CF), 0);
    lv_obj_align(wifi_glyph, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(wifi_glyph, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    s_wifi_signal_bars[0] = wifi_glyph;
    for (uint8_t i = 1; i < 4U; ++i) {
        s_wifi_signal_bars[i] = NULL;
    }

    s_clock = make_label(screen, "--:--", 224, 8, 58,
                         lv_color_hex(0x91B0BC));
    lv_obj_set_style_text_align(s_clock, LV_TEXT_ALIGN_RIGHT, 0);
}

static void render_header(lv_obj_t *screen, const char *title)
{
    lv_coord_t title_x = 14;
    lv_coord_t title_width = 190;
    if (strcmp(title, "小钛") == 0) {
        /* HOME_BRAND_ROBOT_MARK: code-native product mark, no bitmap asset. */
        lv_obj_t *mark = lv_obj_create(screen);
        lv_obj_set_pos(mark, 14, 7);
        lv_obj_set_size(mark, 20, 20);
        set_bg(mark, lv_color_hex(0x72DEF8));
        lv_obj_set_style_radius(mark, 6, 0);
        lv_obj_set_style_pad_all(mark, 0, 0);
        lv_obj_clear_flag(mark, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        for (uint8_t i = 0; i < 2U; ++i) {
            lv_obj_t *eye = lv_obj_create(mark);
            lv_obj_set_pos(eye, (lv_coord_t)(4 + i * 9), 7);
            lv_obj_set_size(eye, 3, 5);
            set_bg(eye, lv_color_hex(0x07151C));
            lv_obj_set_style_radius(eye, LV_RADIUS_CIRCLE, 0);
            lv_obj_clear_flag(eye, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        }
        title_x = 40;
        title_width = 164;
    }
    s_header_title = make_label(screen, title, title_x, 8, title_width,
                                lv_color_hex(0xBFE9F3));
    lv_label_set_long_mode(s_header_title, LV_LABEL_LONG_DOT);
    lv_obj_set_height(s_header_title, 20);

    render_header_status(screen);
}

static lv_color_t product_face_background_color(void)
{
    return lv_color_hex(HOME_BACKGROUND_COLOR);
}

#include "starter_product_s3_face.inc"

static void create_expression_face(lv_obj_t *parent,
                                   lv_coord_t x,
                                   lv_coord_t y)
{
    if (s_face_canvas_buffer == NULL) {
        s_face_canvas_buffer = heap_caps_calloc(
            FACE_CANVAS_W * FACE_CANVAS_H, sizeof(*s_face_canvas_buffer),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_face_canvas_buffer == NULL) {
        ESP_LOGE(TAG, "face canvas allocation failed");
        return;
    }
    s_face = lv_canvas_create(parent);
    lv_canvas_set_buffer(s_face, s_face_canvas_buffer,
                         FACE_CANVAS_W, FACE_CANVAS_H,
                         LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(s_face, x, y);
    lv_obj_clear_flag(s_face, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_canvas_fill_bg(s_face, product_face_background_color(), LV_OPA_COVER);
    s3_face_attach();
}

static void refresh_call_controls(starter_runtime_status_t runtime,
                                  const starter_runtime_product_snapshot_t *product)
{
    if (product != NULL) s3_refresh_ui(monotonic_ms(), runtime, product);
}

static void refresh_diagnostics(int64_t now)
{
    if (s_page != PAGE_DIAGNOSTICS || s_diagnostics_label == NULL) return;
    /* UI-task-owned scratch; no large stack frame or background sampling task. */
    static EXT_RAM_BSS_ATTR char text[1400];
    static starter_runtime_diagnostics_t events;
    static uint32_t previous_sent, previous_received, previous_dropped;
    static int64_t sampled_ms;
    starter_runtime_status_t runtime = starter_runtime_status();
    starter_media_status_t media = starter_media_status();
    starter_voice_status_t voice = starter_voice_status();
    text[0] = '\0';
    if (s_diagnostics_tab == 0) {
        const uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        int8_t rssi = 0;
        bool has_rssi = wifi_manager_signal_dbm(&rssi);
        char signal[24];
        if (has_rssi) (void)snprintf(signal, sizeof(signal), "%d dBm", (int)rssi);
        else (void)snprintf(signal, sizeof(signal), "不可用");
        (void)snprintf(text, sizeof(text),
            "版本 %s\n运行 %llu s  会话 %lu / %lu\n状态 %s\n"
            "内存 KiB: 可用 / 最低 / 最大块\n"
            "RAM   %u / %u / %u\nPSRAM %u / %u / %u\n"
            "会话栈最低剩余 %lu B\n"
            "WiFi %s  RSSI %s\nAPI %s  MQTT %s\n"
            "TiRTC 发送缓冲 %u B\n最后错误 %d",
            esp_app_get_description()->version, (unsigned long long)(now / 1000),
            (unsigned long)runtime.session_generation, (unsigned long)runtime.connection_generation,
            starter_runtime_state_name(runtime.state),
            (unsigned)(heap_caps_get_free_size(internal) / 1024),
            (unsigned)(heap_caps_get_minimum_free_size(internal) / 1024),
            (unsigned)(heap_caps_get_largest_free_block(internal) / 1024),
            (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
            (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) / 1024),
            (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024),
            (unsigned long)runtime.stack_high_water_bytes,
            wifi_manager_connected() ? "在线" : "离线", signal,
            platform_client_ready() ? "在线" : "离线",
            platform_client_mqtt_connected() ? "在线" : "离线",
            (unsigned)starter_tirtc_send_buffer_used(), runtime.last_error);
    } else if (s_diagnostics_tab == 1) {
        const char *reason = !voice.ready ? "唤醒未就绪" :
            media.microphone_muted ? "已闭麦" :
            runtime.state != STARTER_RUNTIME_WAITING ? "会话占用" :
            !voice.listening ? "启动或切换中" : "监听中";
        starter_media_preroll_status_t cache = {0};
        bool cache_valid = starter_media_preroll_status(&cache);
        char cache_text[128];
        if (cache_valid) {
            (void)snprintf(cache_text, sizeof(cache_text),
                "缓存 %s %lu ms\n所有权 %u 已绑定 %u 失效 %u",
                cache.owned ? "待发送" : "待机预录", (unsigned long)cache.buffered_ms,
                cache.owned, cache.bound, cache.failed);
        } else {
            (void)snprintf(cache_text, sizeof(cache_text), "缓存采样忙，等待下次刷新");
        }
        uint32_t dt = sampled_ms == 0 || s_diagnostics_due_ms == 0 ? 0 : (uint32_t)(now - sampled_ms);
        (void)snprintf(text, sizeof(text),
            "唤醒 %s\n阈值 %u.%03u  %s\n"
            "检测 %lu 送帧 %lu\n丢帧 %lu 过期 %lu 重置 %lu\n"
            "结果时龄 %lu ms 检测最高 %lu us\n"
            "音频 TX/RX/丢弃 累计\n%lu / %lu / %lu\n"
            "最近 %lu ms 增量 %lu / %lu / %lu\n"
            "播放 %lu 解码错 %lu 写错 %lu\n"
            "AEC 最高 %lu us 超时 %lu 错误 %lu\n"
            "%s\n"
            "麦克风静音 %u 扬声器静音 %u",
            reason, voice.threshold_milli / 1000, voice.threshold_milli % 1000,
            "AEC clean",
            (unsigned long)voice.detections, (unsigned long)voice.fed,
            (unsigned long)voice.dropped, (unsigned long)voice.stale, (unsigned long)voice.resets,
            (unsigned long)voice.max_age_ms, (unsigned long)voice.max_detect_us,
            (unsigned long)media.audio_sent, (unsigned long)media.audio_received,
            (unsigned long)media.audio_dropped, (unsigned long)dt,
            (unsigned long)(dt ? media.audio_sent - previous_sent : 0),
            (unsigned long)(dt ? media.audio_received - previous_received : 0),
            (unsigned long)(dt ? media.audio_dropped - previous_dropped : 0),
            (unsigned long)media.audio_played, (unsigned long)media.audio_decode_failed,
            (unsigned long)media.audio_write_failed, (unsigned long)media.aec_max_process_us,
            (unsigned long)media.aec_deadline_misses, (unsigned long)media.aec_errors,
            cache_text,
            media.microphone_muted, media.speaker_muted);
    } else {
        starter_runtime_diagnostics(&events);
        size_t used = (size_t)snprintf(text, sizeof(text), "最近事件（最多 8 条）\n时间 / 会话 / 事件 / 数值\n");
        for (unsigned i = 0; i < events.count && used < sizeof(text); ++i) {
            const starter_diagnostic_event_t *event = &events.events[i];
            int written = snprintf(text + used, sizeof(text) - used,
                "%lu.%03lu s / #%lu\n%s : %d\n",
                (unsigned long)(event->at_ms / 1000), (unsigned long)(event->at_ms % 1000),
                (unsigned long)event->session, event->label, event->value);
            if (written < 0 || (size_t)written >= sizeof(text) - used) break;
            used += (size_t)written;
        }
        if (events.count == 0) (void)snprintf(text + used, sizeof(text) - used, "暂无事件");
    }
    previous_sent = media.audio_sent;
    previous_received = media.audio_received;
    previous_dropped = media.audio_dropped;
    sampled_ms = now;
    if (strcmp(text, s_diagnostics_text) != 0) {
        (void)snprintf(s_diagnostics_text, sizeof(s_diagnostics_text), "%s", text);
        lv_label_set_text(s_diagnostics_label, s_diagnostics_text);
    }
    s_diagnostics_due_ms = now + 1000;
}

#include "starter_product_s3_ui.inc"

static void render_page(void)
{
    lv_obj_t *screen = lv_scr_act();
    /* Deleting a scroller may finish its animation and invoke a callback. */
    s3_reset_page();
    s3_face_detach();
    lv_obj_clean(screen);
    set_bg(screen, lv_color_hex(HOME_BACKGROUND_COLOR));
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    s_clock = NULL;
    s_wifi_signal = NULL;
    memset(s_wifi_signal_bars, 0, sizeof(s_wifi_signal_bars));
    s_wifi_signal_revision = UINT32_MAX;
    s_wifi_signal_style = -1;
    s_date = NULL;
    s_state = NULL;
    s_subtitle = NULL;
    s_face = NULL;
    s_rendered_expression[0] = '\0';
    s_header_title = NULL;
    s_call_peer_label = NULL;
    s_call_status_label = NULL;
    s_call_accept_button = NULL;
    s_call_reject_button = NULL;
    s_call_mute_button = NULL;
    s_call_hangup_button = NULL;
    s_settings_volume = NULL;
    s_settings_speaker = NULL;
    s_settings_microphone = NULL;
    s_settings_sleep = NULL;
    s_settings_acknowledgement = NULL;
    s_diagnostics_label = NULL;
    s_diagnostics_panel = NULL;
    s_diagnostics_due_ms = 0;
    s_diagnostics_text[0] = '\0';
    if (!s3_render_page(screen)) {
        ESP_LOGE(TAG, "unknown UI page=%u", (unsigned)s_page);
        s_page = PAGE_HOME_FACE;
        (void)s3_render_page(screen);
    }
}

static const char *phase_text(starter_ai_ui_phase_t phase)
{
    switch (phase) {
    case STARTER_AI_UI_LISTENING: return "正在聆听";
    case STARTER_AI_UI_THINKING: return "AI 正在思考";
    case STARTER_AI_UI_SPEAKING: return "AI 回复中";
    default: return "小钛在这儿";
    }
}

static void apply_expression(const char *emotion,
                             starter_ai_ui_phase_t phase,
                             uint8_t activity_level)
{
    s3_face_set_target(emotion, phase, activity_level);
}

static void product_tick(lv_timer_t *timer)
{
    const int64_t refresh_started_us = esp_timer_get_time();
    (void)timer;
    int64_t expression_elapsed_us = 0;
    int64_t now = monotonic_ms();
    starter_voice_result_t voice_result;
    for (uint8_t i = 0; i < PRODUCT_VOICE_QUEUE_LENGTH &&
                        xQueueReceive(s_voice_queue, &voice_result, 0) == pdTRUE;
         ++i) {
        handle_voice_result(&voice_result, now);
    }
    if (s_voice_feedback[0] != '\0' && now >= s_voice_feedback_hide_ms) {
        s_voice_feedback[0] = '\0';
    }
    starter_runtime_status_t runtime = starter_runtime_status();
    starter_runtime_product_snapshot_t product = starter_runtime_product_snapshot();
    starter_media_status_t media = starter_media_status();
    bool runtime_changed = runtime.state != s_previous_runtime_state;
    bool call_now = runtime.state == STARTER_RUNTIME_CALL_INCOMING ||
                    runtime.state == STARTER_RUNTIME_CALL_CONNECTING ||
                    runtime.state == STARTER_RUNTIME_CALL_ACTIVE;
    bool call_before = s_previous_runtime_state == STARTER_RUNTIME_CALL_INCOMING ||
                        s_previous_runtime_state == STARTER_RUNTIME_CALL_CONNECTING ||
                        s_previous_runtime_state == STARTER_RUNTIME_CALL_ACTIVE;
    bool session_idle = runtime.state == STARTER_RUNTIME_WAITING;
    bool ai_now = runtime.state == STARTER_RUNTIME_AI_CONNECTING ||
                  runtime.state == STARTER_RUNTIME_AI_ACTIVE;
    bool home = s_page == PAGE_HOME_FACE || s_page == PAGE_HOME_CLOCK;
    bool wifi_connected = wifi_manager_connected();
    bool wifi_failed = wifi_manager_connection_failed();
    if (wifi_connected && !s_previous_wifi_connected) {
        s_pending_wifi_notice = WIFI_NOTICE_CONNECTED;
    }
    if (wifi_failed && !s_previous_wifi_failed) {
        s_pending_wifi_notice = WIFI_NOTICE_FAILED;
    }
    if (s_pending_wifi_notice == WIFI_NOTICE_CONNECTED &&
        wifi_connected && home && session_idle) {
        set_wifi_feedback(WIFI_NOTICE_CONNECTED, now);
        s_pending_wifi_notice = WIFI_NOTICE_NONE;
        note_interaction();
    }
    if (s_pending_wifi_notice == WIFI_NOTICE_FAILED &&
        wifi_failed && session_idle && !call_now) {
        s_pending_wifi_notice = WIFI_NOTICE_NONE;
        note_interaction();
    }
    if (ai_now && runtime.session_generation != s_previous_ai_generation) {
        /* BOOT, screen and console bypass the acoustic-result queue. */
        if (runtime.session_generation != s_wake_debug_session) show_wake_debug(NULL, now);
        s_previous_ai_generation = runtime.session_generation;
    }
    if (s_wake_debug_label != NULL && (now >= s_wake_debug_hide_ms || call_now)) {
        lv_obj_add_flag(s_wake_debug_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_pending_ai_ack.expires_ms != 0) {
        if (now > s_pending_ai_ack.expires_ms || call_now ||
            runtime.state == STARTER_RUNTIME_AI_ACTIVE) {
            s_pending_ai_ack.expires_ms = 0;
        } else if (runtime.state == STARTER_RUNTIME_AI_CONNECTING &&
                   runtime.session_generation == s_pending_ai_ack.expected_session) {
            s_pending_ai_ack.playback_epoch = starter_media_playback_epoch();
            (void)xQueueOverwrite(s_ai_ack_queue, &s_pending_ai_ack);
            s_pending_ai_ack.expires_ms = 0;
        }
    }

    /* starter_runtime_ai_start() is asynchronous: an accepted queue item is
     * not an accepted remote session.  Surface an AI setup failure explicitly
     * instead of leaving the acknowledgement as the last user-visible state.
     * A normal server idle close starts from AI_ACTIVE, so it is intentionally
     * not reported as a failed connection here. */
    if (s_previous_runtime_state == STARTER_RUNTIME_AI_CONNECTING &&
        session_idle && runtime.last_error != 0) {
        ESP_LOGW(TAG, "AI connection did not become active: error=%d",
                 runtime.last_error);
        set_voice_feedback("连接失败，请重新开始", now);
    }

    if (runtime_changed && session_idle &&
        s_previous_runtime_state != STARTER_RUNTIME_WAITING) {
        /* 会话/通话刚结束，从“刚安静下来”重新计时。 */
        note_interaction();
    }

    bool show_call = call_now;
    show_call = call_now && wifi_connected;
    if (show_call) {
        if (s_ai_ack_queue != NULL) {
            (void)xQueueReset(s_ai_ack_queue);
        }
        note_interaction();
        if (s_page != PAGE_CALL) {
            s_page = PAGE_CALL;
            starter_media_set_microphone_muted(s_preferences.microphone_muted);
            (void)starter_runtime_call_set_microphone_muted(
                s_preferences.microphone_muted);
            render_page();
        } else if (runtime_changed) {
            refresh_call_controls(runtime, &product);
        }
        /*
         * CALL_CONNECTING 同时涵盖“主动拨号”和“被叫已点接听后的建连”。
         * 铃声由阶段和角色共同决定：接通后无论主被叫都必须停止，不能让
         * 回铃任务与通话下行 I2S 竞争。
         */
        if (runtime.state == STARTER_RUNTIME_CALL_INCOMING) {
            s_call_ring_kind = CALL_RING_INCOMING;
        } else if (runtime.state == STARTER_RUNTIME_CALL_CONNECTING &&
                   !product.call_incoming) {
            s_call_ring_kind = CALL_RING_OUTGOING;
        } else {
            s_call_ring_kind = CALL_RING_NONE;
        }
    } else if (!call_now && call_before && s_page == PAGE_CALL) {
        (void)snprintf(s_call_result, sizeof(s_call_result), "%s",
            product.call_result[0] ? product.call_result :
            runtime.last_error ? "通话未完成" : "通话已结束");
        s_page = PAGE_CALL_RESULT;
        render_page();
    }
    if (!show_call) {
        s_call_ring_kind = CALL_RING_NONE;
    }
    /* S3 keeps the result until explicit navigation; a new incoming call still
     * takes priority above. Do not turn a short reading delay into a lost result. */

    /* Wi-Fi 已连但尚未绑定时，验证码页优先于所有空闲产品页。 */
    bool binding_now = platform_client_provisioning();
    const char *verification_code = platform_client_verification_code();
    s3_update_setup(wifi_connected, session_idle, binding_now, verification_code);

    /* 上面的通话/绑定跳转可能重建页面，后续刷新以当前页面为准。 */
    home = s_page == PAGE_HOME_FACE || s_page == PAGE_HOME_CLOCK;

    bool contacts_changed = product.contact_count != s_previous_contact_count ||
                            memcmp(product.contacts, s_previous_contacts,
                                   sizeof(s_previous_contacts)) != 0;
    if ((s_page == PAGE_CONTACTS || s_page == PAGE_CONTACT_DETAIL) &&
        contacts_changed) {
        render_page();
    }

    if (now >= s_audio_wake_guard_until_ms &&
        media.audio_activity_level >= 2U && s_previous_audio_level < 2U) {
        note_interaction();
        if (runtime.state == STARTER_RUNTIME_WAITING &&
            (s_page == PAGE_HOME_FACE || s_page == PAGE_HOME_CLOCK)) {
            s_ambient_visible = true;
            s_ambient_hide_ms = now + PRODUCT_AMBIENT_VISIBLE_MS;
        }
    }
    s_previous_audio_level = media.audio_activity_level;

    uint16_t sleep_minutes = s_sleep_minutes[s_preferences.sleep_index];
    if (sleep_minutes != 0U && home && session_idle && s_display_awake &&
        now - s_last_interaction_ms >= (int64_t)sleep_minutes * 60000) {
        backlight_set(false);
    }

    if (s_ambient_visible && now >= s_ambient_hide_ms) {
        s_ambient_visible = false;
    }

    uint8_t idle_stage = 0U;
    if (session_idle) {
        int64_t idle_ms = now - s_last_interaction_ms;
        if (idle_ms >= PRODUCT_IDLE_SLEEPY_MS) {
            idle_stage = 2U;
        } else if (idle_ms >= PRODUCT_IDLE_DAYDREAM_MS) {
            idle_stage = 1U;
        }
    }

    uint32_t signal_revision = wifi_manager_signal_revision();
    if (s_wifi_signal != NULL && signal_revision != s_wifi_signal_revision) {
        int8_t rssi = 0;
        lv_color_t signal_color = lv_color_hex(0xF28A8A);
        int signal_style = 0;
        if (wifi_manager_signal_dbm(&rssi)) {
            signal_style = rssi >= -60 ? 3 : rssi >= -72 ? 2 : 1;
            signal_color = rssi >= -60 ? lv_color_hex(0x78F0CF)
                           : rssi >= -72 ? lv_color_hex(0xFFD67A)
                                         : lv_color_hex(0xF28A8A);
        }
        if (signal_style != s_wifi_signal_style) {
            for (uint8_t i = 0; i < 4U; ++i) {
                if (s_wifi_signal_bars[i] != NULL) {
                    lv_obj_set_style_text_color(s_wifi_signal_bars[i], signal_color, 0);
                }
            }
        }
        s_wifi_signal_style = signal_style;
        s_wifi_signal_revision = signal_revision;
    }

    if (s_clock != NULL || (s_page == PAGE_HOME_CLOCK && s_face != NULL)) {
        time_t current = time(NULL);
        struct tm local = {0};
        localtime_r(&current, &local);
        char clock_text[8];
        (void)snprintf(clock_text, sizeof(clock_text), "%02d:%02d",
                       local.tm_hour, local.tm_min);
        if (s_clock != NULL) {
            label_set_text_if_changed(s_clock, clock_text);
        }
        if (s_page == PAGE_HOME_CLOCK && s_face != NULL) {
            label_set_text_if_changed(s_face, clock_text);
            if (s_date != NULL) {
                char date_text[40];
                (void)strftime(date_text, sizeof(date_text), "%Y-%m-%d", &local);
                static const char *const weekdays[] = {
                    "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"
                };
                size_t used = strlen(date_text);
                (void)snprintf(date_text + used, sizeof(date_text) - used,
                               "  %s", weekdays[local.tm_wday]);
                label_set_text_if_changed(s_date, date_text);
            }
        }
    }

    /* Home state and full captions have one writer: s3_refresh_ui below. */
    const char *emotion = product.emotion;
    if (session_idle) {
        emotion = s_emoji_keys[s_preferences.emoji_index];
        if (s_ambient_visible) {
            emotion = media.voice_active ? "listening" : "ambient";
        } else if (idle_stage >= 2U) {
            emotion = "sleepy";
        } else if (idle_stage == 1U) {
            emotion = "ambient";
        }
    }
    if (home && s_page == PAGE_HOME_FACE &&
        (strcmp(emotion, s_rendered_expression) != 0 ||
         product.ai_phase != s_previous_ai_phase ||
         media.audio_activity_level != s_expression_audio_level)) {
        const int64_t expression_started_us = esp_timer_get_time();
        apply_expression(emotion, product.ai_phase, media.audio_activity_level);
        expression_elapsed_us = esp_timer_get_time() - expression_started_us;
        s_expression_audio_level = media.audio_activity_level;
        (void)snprintf(s_rendered_expression,
                       sizeof(s_rendered_expression), "%s", emotion);
    }

    /* 无网络时产品页优先给出可执行的 AP 配网指引。 */
    s3_refresh_ui(now, runtime, &product);
    s_previous_wifi_connected = wifi_connected;
    s_previous_wifi_failed = wifi_failed;
    s_previous_runtime_state = runtime.state;
    if (s_page == PAGE_DIAGNOSTICS && now >= s_diagnostics_due_ms) {
        refresh_diagnostics(now);
    }
    s_previous_ai_phase = product.ai_phase;
    s_previous_contact_count = product.contact_count;
    memcpy(s_previous_contacts, product.contacts, sizeof(s_previous_contacts));

    const int64_t refresh_elapsed_us = esp_timer_get_time() - refresh_started_us;
    if (refresh_elapsed_us > PRODUCT_UI_REFRESH_SLOW_US &&
        refresh_started_us - s_ui_refresh_slow_last_us >
            PRODUCT_UI_REFRESH_LOG_INTERVAL_US) {
        s_ui_refresh_slow_last_us = refresh_started_us;
        ESP_LOGW(TAG,
                 "UI refresh slow: total=%lld us expression=%lld us page=%u "
                 "runtime=%u",
                 (long long)refresh_elapsed_us,
                 (long long)expression_elapsed_us,
                 (unsigned)s_page,
                 (unsigned)runtime.state);
    }
}

static esp_err_t display_hardware_init(void)
{
    const gpio_config_t backlight = {
        .pin_bit_mask = 1ULL << LCD_PIN_BL,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&backlight), TAG, "backlight GPIO");
    ESP_RETURN_ON_ERROR(gpio_set_level(LCD_PIN_BL, LCD_BACKLIGHT_OFF_LEVEL), TAG,
                        "backlight off");

    const spi_bus_config_t bus = {
        .sclk_io_num = LCD_PIN_SCLK,
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_H_RES * LCD_DRAW_LINES * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO),
                        TAG, "LCD SPI bus");
    const esp_lcd_panel_io_spi_config_t io = {
        .dc_gpio_num = LCD_PIN_DC,
        .cs_gpio_num = GPIO_NUM_NC,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 2,
        .trans_queue_depth = 4,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(
                            (esp_lcd_spi_bus_handle_t)LCD_HOST, &io, &s_lcd_io),
                        TAG, "LCD panel IO");
    const esp_lcd_panel_dev_config_t panel = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_lcd_io, &panel, &s_lcd_panel),
                        TAG, "ST7789 panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_lcd_panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(starter_media_select_lcd(true), TAG, "cannot select LCD");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_lcd_panel), TAG, "panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_lcd_panel, true), TAG,
                        "panel invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_lcd_panel, true), TAG, "panel swap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_lcd_panel, true, false), TAG, "panel mirror");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_lcd_panel, true), TAG, "panel on");

    esp_lcd_panel_io_handle_t touch_io = NULL;
    esp_lcd_panel_io_i2c_config_t touch_io_config =
        ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    /* Reuse the board's sole driver_ng I2C0 master.  Touch must never create
     * the deprecated panel-IO v1 path because it conflicts before app_main. */
    touch_io_config.scl_speed_hz = TOUCH_I2C_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(
                            starter_media_i2c_bus(), &touch_io_config, &touch_io),
                        TAG, "touch I2C IO");
    const esp_lcd_touch_config_t touch = {
        .x_max = LCD_NATIVE_H_RES,
        .y_max = LCD_NATIVE_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = 1, .mirror_x = 1, .mirror_y = 0},
    };
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_ft5x06(touch_io, &touch, &s_touch),
                        TAG, "FT6336 touch");
    return ESP_OK;
}

static esp_err_t lvgl_init(void)
{
    const lvgl_port_cfg_t port = {
        .task_priority = PRODUCT_UI_TASK_PRIORITY,
        .task_stack = PRODUCT_UI_TASK_STACK_BYTES,
        .task_affinity = PRODUCT_UI_TASK_CORE,
        .task_max_sleep_ms = 100,
        /* Rendering owns no DMA buffers and runs independently of the flash
         * persistence path, so its long-lived stack belongs in PSRAM. */
        .task_stack_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
        .timer_period_ms = 5,
    };
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port), TAG, "LVGL port");
    /*
     * The working device-monitor BSP for this exact ST7789 + FT5x06 display
     * uses a complete canvas in PSRAM and sends it in 10-line SPI chunks.
     * A two-buffer, 10-line render canvas caused intermittent incomplete
     * invalidated areas here: LVGL's objects were valid, but one eye's dirty
     * rectangle was not consistently committed to the panel.  Keep the full
     * scene coherent in one PSRAM buffer; SPI DMA still transfers only ten
     * lines at a time, so this costs memory rather than frame latency.
     */
    const lvgl_port_display_cfg_t display = {
        .io_handle = s_lcd_io,
        .panel_handle = s_lcd_panel,
        .buffer_size = LCD_H_RES * LCD_V_RES,
        .double_buffer = false,
        .trans_size = LCD_H_RES * LCD_DRAW_LINES,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .rotation = {.swap_xy = true, .mirror_x = true, .mirror_y = false},
        .flags = {.buff_dma = false, .buff_spiram = true},
    };
    s_display = lvgl_port_add_disp(&display);
    if (s_display == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const lvgl_port_touch_cfg_t touch = {.disp = s_display, .handle = s_touch};
    if (lvgl_port_add_touch(&touch) == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (!lvgl_port_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    /* The screen survives page rebuilds; lv_obj_clean removes children only.
     * Re-registering here on every render wraps LVGL's 6-bit event count. */
    lv_obj_add_event_cb(lv_scr_act(), on_screen_event, LV_EVENT_ALL, NULL);
    render_page();
    (void)lv_timer_create(product_tick, 100, NULL);
    if (s_call_ring_task == NULL &&
        xTaskCreate(call_ring_task, "call_ring", 4096, NULL, 4,
                    &s_call_ring_task) != pdPASS) {
        lvgl_port_unlock();
        return ESP_ERR_NO_MEM;
    }
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t starter_product_start(void)
{
    if (s_started) {
        return ESP_OK;
    }
    if (s_ai_ack_queue == NULL) {
        s_ai_ack_queue = xQueueCreate(1, sizeof(ai_ack_t));
        if (s_ai_ack_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_voice_queue == NULL) {
        s_voice_queue = xQueueCreate(PRODUCT_VOICE_QUEUE_LENGTH,
                                     sizeof(starter_voice_result_t));
        ESP_RETURN_ON_FALSE(s_voice_queue != NULL, ESP_ERR_NO_MEM, TAG,
                            "voice intent queue");
    }
    ESP_RETURN_ON_ERROR(display_hardware_init(), TAG, "display hardware");
    preferences_load();
    ESP_RETURN_ON_ERROR(lvgl_init(), TAG, "product UI");
    s_last_interaction_ms = monotonic_ms();
    lvgl_port_lock(0);
    backlight_set(true);
    lvgl_port_unlock();
    s_started = true;
    ESP_LOGI(TAG,
             "AIROBOT S3 UI ready: 320x240 touch, voice-only contacts, sleep=%s",
             s_sleep_names[s_preferences.sleep_index]);
    return ESP_OK;
}

esp_err_t starter_product_submit_voice(const starter_voice_result_t *result)
{
    if (result == NULL || result->intent <= STARTER_VOICE_INTENT_NONE ||
        result->intent > STARTER_VOICE_INTENT_CALL_CONTACT_REMOTE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_started || s_voice_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return xQueueSend(s_voice_queue, result, 0) == pdTRUE
               ? ESP_OK : ESP_ERR_TIMEOUT;
}
