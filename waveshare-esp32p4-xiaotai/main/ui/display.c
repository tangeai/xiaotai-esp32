#include "display.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "device.h"
#include "display_driver.h"
#include "display_layout.h"
#include "display_page.h"
#include "display_theme.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "sdkconfig.h"
#include "extra/libs/qrcode/qrcodegen.h"

#include "app_version.h"
#include "app_memory_policy.h"
#include "app_task_affinity.h"
#include "app_config.h"
#include "ai_chat_assets.h"
#include "ai_chat_avatar_assets.h"
#include "ai_chat_font.h"
#include "call_video_renderer.h"
#include "home_assets.h"
#include "media_tuning.h"
#include "video_frame_converter.h"
#include "text_assets.h"

static const char *TAG = "display";
static const char *CALL_FLOW_TAG = "CALL_FLOW";

static void *display_calloc_psram(size_t count, size_t size)
{
    return app_memory_calloc_psram(count, size);
}

#define DISPLAY_DEVICE_QR_PAYLOAD_MAX       DISPLAY_TIRTC_CONFIG_TEXT_MAX
#define DISPLAY_CONTACT_QR_PAYLOAD_MAX      (128U + DISPLAY_TIRTC_CONFIG_TEXT_MAX * 4U)
#define DISPLAY_BINDING_PLATFORM_URL        APP_CONFIG_DEVICE_BINDING_PORTAL_URL
#define DISPLAY_BINDING_CODE_PLACEHOLDER    "------"
#define DISPLAY_HOME_PORTRAIT_HEADER_HEIGHT 28
#define DISPLAY_AI_APP_TITLE                 "AI 对讲"
#define DISPLAY_AI_SETTINGS_TITLE            "AI 对讲设置"
#define DISPLAY_AI_AVATAR_NAME               "小钛"
#define DISPLAY_HOME_LANDSCAPE_HEADER_HEIGHT DISPLAY_UI_HEADER_HEIGHT
#define DISPLAY_HOME_LANDSCAPE_MIN_WIDTH    440
#define DISPLAY_HOME_LANDSCAPE_MIN_HEIGHT   300
#define DISPLAY_CAPTURE_LOCK_TIMEOUT_MS     15000
#define DISPLAY_SNAPSHOT_TASK_PERIOD_MS     250
#define DISPLAY_SNAPSHOT_TASK_STACK_SIZE    8192
#define DISPLAY_REFRESH_TASK_PERIOD_MS      250
/* Poll faster than the media cadence so presentation is not quantized into
 * uneven display gaps. The timer is paused outside an active video call, and
 * ticks without a new frame are intentionally cheap. */
#define DISPLAY_VIDEO_REFRESH_TASK_PERIOD_MS 10
#define DISPLAY_CALL_PAGE_TRANSITION_GRACE_US (750LL * 1000LL)
#define DISPLAY_CALL_VIDEO_CONTROLS_VISIBLE_US (5LL * 1000LL * 1000LL)
#define DISPLAY_CALL_VIDEO_FRAME_INTERVAL_US    \
    ((1000000U + APP_MEDIA_CALL_VIDEO_FPS - 1U) / APP_MEDIA_CALL_VIDEO_FPS)
#define DISPLAY_CALL_VIDEO_STALL_GAP_US         \
    (DISPLAY_CALL_VIDEO_FRAME_INTERVAL_US * 3U)
#define DISPLAY_CALL_VIDEO_STALL_LOG_US         (5LL * 1000LL * 1000LL)
#define DISPLAY_CALL_VIDEO_SCREEN_WIDTH       DISPLAY_NATIVE_WIDTH
#define DISPLAY_CALL_VIDEO_SCREEN_HEIGHT      DISPLAY_NATIVE_HEIGHT
#define DISPLAY_CALL_VIDEO_X                  0U
#define DISPLAY_CALL_VIDEO_Y                  0U
#define DISPLAY_CALL_VIDEO_OVERLAY_MARGIN     8U
#define DISPLAY_CALL_VIDEO_TOP_X              DISPLAY_CALL_VIDEO_OVERLAY_MARGIN
#define DISPLAY_CALL_VIDEO_TOP_Y              DISPLAY_CALL_VIDEO_OVERLAY_MARGIN
#define DISPLAY_CALL_VIDEO_TOP_WIDTH          \
    (DISPLAY_CALL_VIDEO_SCREEN_WIDTH - (2U * DISPLAY_CALL_VIDEO_OVERLAY_MARGIN))
#define DISPLAY_CALL_VIDEO_TOP_HEIGHT         64U
#define DISPLAY_CALL_VIDEO_CONTROLS_X         DISPLAY_CALL_VIDEO_OVERLAY_MARGIN
#define DISPLAY_CALL_VIDEO_CONTROLS_Y         260U
#define DISPLAY_CALL_VIDEO_CONTROLS_WIDTH     330U
#define DISPLAY_CALL_VIDEO_CONTROLS_HEIGHT    52U
#define DISPLAY_CALL_VIDEO_HANGUP_X           350U
#define DISPLAY_CALL_VIDEO_HANGUP_Y           DISPLAY_CALL_VIDEO_CONTROLS_Y
#define DISPLAY_CALL_VIDEO_HANGUP_WIDTH       122U
#define DISPLAY_CALL_VIDEO_HANGUP_HEIGHT      DISPLAY_CALL_VIDEO_CONTROLS_HEIGHT
#define DISPLAY_UI_WATCHDOG_TASK_STACK_SIZE 3072
#define DISPLAY_UI_WATCHDOG_PERIOD_MS       1000
#define DISPLAY_UI_WATCHDOG_STALL_US        (2LL * 1000LL * 1000LL)
#define DISPLAY_UI_WATCHDOG_LOG_INTERVAL_US (3LL * 1000LL * 1000LL)
#define DISPLAY_LVGL_STACK_LOW_WATERMARK    2048U
#define DISPLAY_LVGL_STACK_LOG_INTERVAL_US  (5LL * 1000LL * 1000LL)
#define DISPLAY_SCAN_PREVIEW_BUFFER_COUNT 2U
#define DISPLAY_SCAN_PREVIEW_ALIGNMENT    64U
#if CONFIG_CACHE_L2_CACHE_LINE_SIZE > CONFIG_CACHE_L1_CACHE_LINE_SIZE
#define DISPLAY_VIDEO_CACHE_LINE_SIZE CONFIG_CACHE_L2_CACHE_LINE_SIZE
#else
#define DISPLAY_VIDEO_CACHE_LINE_SIZE CONFIG_CACHE_L1_CACHE_LINE_SIZE
#endif
#define DISPLAY_CALL_VIDEO_FRAME_BYTES \
    ((size_t)DISPLAY_CALL_VIDEO_SCREEN_WIDTH * \
     DISPLAY_CALL_VIDEO_SCREEN_HEIGHT * sizeof(uint16_t))

_Static_assert(DISPLAY_CALL_VIDEO_SCREEN_WIDTH == 480U &&
                   DISPLAY_CALL_VIDEO_SCREEN_HEIGHT == 320U,
               "call video layout is designed for the native landscape panel");
_Static_assert(CALL_VIDEO_RENDER_WIDTH == DISPLAY_CALL_VIDEO_SCREEN_WIDTH &&
                   CALL_VIDEO_RENDER_HEIGHT == DISPLAY_CALL_VIDEO_SCREEN_HEIGHT,
               "call video must fill the native landscape panel");
_Static_assert(DISPLAY_CALL_VIDEO_TOP_X + DISPLAY_CALL_VIDEO_TOP_WIDTH +
                       DISPLAY_CALL_VIDEO_OVERLAY_MARGIN ==
                   DISPLAY_CALL_VIDEO_SCREEN_WIDTH,
               "top call overlay must preserve equal side margins");
_Static_assert(DISPLAY_CALL_VIDEO_CONTROLS_Y +
                       DISPLAY_CALL_VIDEO_CONTROLS_HEIGHT +
                       DISPLAY_CALL_VIDEO_OVERLAY_MARGIN ==
                   DISPLAY_CALL_VIDEO_SCREEN_HEIGHT,
               "bottom call overlays must preserve the lower margin");
_Static_assert(DISPLAY_CALL_VIDEO_HANGUP_X + DISPLAY_CALL_VIDEO_HANGUP_WIDTH +
                       DISPLAY_CALL_VIDEO_OVERLAY_MARGIN ==
                   DISPLAY_CALL_VIDEO_SCREEN_WIDTH,
               "hangup control must fit the landscape panel");
_Static_assert((DISPLAY_CALL_VIDEO_FRAME_BYTES % DISPLAY_VIDEO_CACHE_LINE_SIZE) == 0U,
               "call video composition frame must be cache-line aligned");

static lv_disp_t *s_display;
static lv_indev_t *s_touch_indev;
static bool s_display_initialized;

static bool display_utf8_is_continuation(unsigned char ch)
{
    return ch >= 0x80U && ch <= 0xBFU;
}

static size_t display_utf8_sequence_length(const unsigned char *src)
{
    unsigned char lead = 0;

    if (src == NULL || src[0] == '\0') {
        return 0;
    }

    lead = src[0];
    if (lead >= 0xC2U && lead <= 0xDFU &&
        display_utf8_is_continuation(src[1])) {
        return 2;
    }
    if (lead == 0xE0U &&
        src[1] >= 0xA0U && src[1] <= 0xBFU &&
        display_utf8_is_continuation(src[2])) {
        return 3;
    }
    if (((lead >= 0xE1U && lead <= 0xECU) ||
         (lead >= 0xEEU && lead <= 0xEFU)) &&
        display_utf8_is_continuation(src[1]) &&
        display_utf8_is_continuation(src[2])) {
        return 3;
    }
    if (lead == 0xEDU &&
        src[1] >= 0x80U && src[1] <= 0x9FU &&
        display_utf8_is_continuation(src[2])) {
        return 3;
    }
    if (lead == 0xF0U &&
        src[1] >= 0x90U && src[1] <= 0xBFU &&
        display_utf8_is_continuation(src[2]) &&
        display_utf8_is_continuation(src[3])) {
        return 4;
    }
    if (lead >= 0xF1U && lead <= 0xF3U &&
        display_utf8_is_continuation(src[1]) &&
        display_utf8_is_continuation(src[2]) &&
        display_utf8_is_continuation(src[3])) {
        return 4;
    }
    if (lead == 0xF4U &&
        src[1] >= 0x80U && src[1] <= 0x8FU &&
        display_utf8_is_continuation(src[2]) &&
        display_utf8_is_continuation(src[3])) {
        return 4;
    }

    return 0;
}

static void display_copy_safe_utf8(char *dst, size_t dst_size, const char *src, const char *fallback)
{
    size_t out = 0;
    const unsigned char *cursor = (const unsigned char *)src;

    if (dst == NULL || dst_size == 0) {
        return;
    }

    dst[0] = '\0';
    if (cursor == NULL || cursor[0] == '\0') {
        if (fallback != NULL) {
            strlcpy(dst, fallback, dst_size);
        }
        return;
    }

    while (*cursor != '\0' && out + 1 < dst_size) {
        if (*cursor >= 0x20U && *cursor <= 0x7EU) {
            dst[out++] = (char)*cursor++;
            continue;
        }

        size_t sequence_length = display_utf8_sequence_length(cursor);
        if (sequence_length > 0) {
            if (out + sequence_length >= dst_size) {
                break;
            }
            memcpy(dst + out, cursor, sequence_length);
            out += sequence_length;
            cursor += sequence_length;
            continue;
        }

        if (out == 0 || dst[out - 1] != '?') {
            dst[out++] = '?';
        }
        ++cursor;
    }

    dst[out] = '\0';
    if (out == 0 && fallback != NULL) {
        strlcpy(dst, fallback, dst_size);
    }
}

static void display_format_ssid(char *dst, size_t dst_size, const char *ssid)
{
    display_copy_safe_utf8(dst, dst_size, ssid, "Hidden SSID");
}

static display_actions_t s_actions;
static display_snapshot_cb_t s_snapshot_provider;
static void *s_snapshot_ctx;
static display_status_t *s_last_status_ptr;
static display_status_t *s_refresh_status_ptr;
static display_status_t *s_refresh_previous_status_ptr;
static display_status_t *s_snapshot_status_ptr;
static display_status_t *s_snapshot_scratch_status_ptr;
static SemaphoreHandle_t s_snapshot_mutex;
static TaskHandle_t s_snapshot_task;
static lv_timer_t *s_refresh_timer;
static lv_timer_t *s_video_refresh_timer;
static bool s_video_refresh_enabled;
static TaskHandle_t s_ui_watchdog_task;
static bool s_snapshot_valid;
static portMUX_TYPE s_lvgl_watchdog_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_lvgl_last_heartbeat_us;
static int64_t s_lvgl_last_refresh_enter_us;
static int64_t s_lvgl_last_refresh_exit_us;
static int64_t s_lvgl_stack_last_log_us;
#if CONFIG_ESP_TASK_WDT_EN
static bool s_lvgl_task_wdt_added;
#endif
#define s_last_status (*s_last_status_ptr)
#define s_refresh_status (*s_refresh_status_ptr)
#define s_refresh_previous_status (*s_refresh_previous_status_ptr)

static esp_err_t display_allocate_status_buffers(void)
{
    if (s_last_status_ptr != NULL &&
        s_refresh_status_ptr != NULL &&
        s_refresh_previous_status_ptr != NULL &&
        s_snapshot_status_ptr != NULL &&
        s_snapshot_scratch_status_ptr != NULL &&
        s_snapshot_mutex != NULL) {
        return ESP_OK;
    }

    s_last_status_ptr = (display_status_t *)display_calloc_psram(1, sizeof(*s_last_status_ptr));
    s_refresh_status_ptr = (display_status_t *)display_calloc_psram(1, sizeof(*s_refresh_status_ptr));
    s_refresh_previous_status_ptr =
        (display_status_t *)display_calloc_psram(1, sizeof(*s_refresh_previous_status_ptr));
    s_snapshot_status_ptr = (display_status_t *)display_calloc_psram(1, sizeof(*s_snapshot_status_ptr));
    s_snapshot_scratch_status_ptr =
        (display_status_t *)display_calloc_psram(1, sizeof(*s_snapshot_scratch_status_ptr));
    if (s_snapshot_mutex == NULL) {
        s_snapshot_mutex = xSemaphoreCreateMutexWithCaps(APP_SYNC_CAPS_CONTROL);
    }
    if (s_last_status_ptr == NULL ||
        s_refresh_status_ptr == NULL ||
        s_refresh_previous_status_ptr == NULL ||
        s_snapshot_status_ptr == NULL ||
        s_snapshot_scratch_status_ptr == NULL ||
        s_snapshot_mutex == NULL) {
        heap_caps_free(s_last_status_ptr);
        heap_caps_free(s_refresh_status_ptr);
        heap_caps_free(s_refresh_previous_status_ptr);
        heap_caps_free(s_snapshot_status_ptr);
        heap_caps_free(s_snapshot_scratch_status_ptr);
        if (s_snapshot_mutex != NULL) {
            vSemaphoreDeleteWithCaps(s_snapshot_mutex);
        }
        s_last_status_ptr = NULL;
        s_refresh_status_ptr = NULL;
        s_refresh_previous_status_ptr = NULL;
        s_snapshot_status_ptr = NULL;
        s_snapshot_scratch_status_ptr = NULL;
        s_snapshot_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
static TaskHandle_t s_wechat_hangup_task;
static int64_t s_last_wifi_scan_request_us;
static int64_t s_wifi_connect_request_us;
static char s_selected_ssid[33];
static char s_wifi_connect_target_ssid[33];
static char s_main_hint_text[96] = "Ready";
static bool s_wifi_connect_pending;
static lv_obj_t *s_wifi_alert_box;
static lv_obj_t *s_binding_prompt_overlay;
static lv_obj_t *s_binding_nowifi_dialog;
static lv_obj_t *s_binding_code_dialog;
static lv_obj_t *s_binding_code_label;
static lv_obj_t *s_binding_platform_qrcode;
static bool s_binding_prompt_visible;
static bool s_binding_prompt_code_dialog_visible;
static char s_binding_prompt_code_text[16];
static lv_obj_t *s_call_alert_box;
static lv_obj_t *s_call_contact_request_box;
static lv_obj_t *s_call_delete_confirm_box;
static lv_obj_t *s_wechat_delete_confirm_box;
static bool s_call_alert_wechat;
static char s_call_contact_request_peer[DISPLAY_CALL_CONTACT_DEVICE_ID_MAX];
static char s_call_contact_request_suppressed_peer[DISPLAY_CALL_CONTACT_DEVICE_ID_MAX];
static bool s_call_contact_request_dismissed;
static uint8_t s_call_delete_pending_index = UINT8_MAX;
static char s_call_delete_pending_device_id[DISPLAY_CALL_CONTACT_DEVICE_ID_MAX];
static uint8_t s_wechat_delete_pending_index = UINT8_MAX;
static char s_wechat_delete_pending_open_id[DISPLAY_WECHAT_OPEN_ID_MAX];
static char s_wechat_remark_edit_open_id[DISPLAY_WECHAT_OPEN_ID_MAX];
static bool s_ai_dialog_external_font_applied;
static int64_t s_refresh_slow_last_log_us;

#define DISPLAY_TEXT_IMAGE_MAGIC 0x54455854U
#define DISPLAY_DEVICE_QR_SIZE        198
#define DISPLAY_CALL_QR_IMAGE_SIZE    216
#define DISPLAY_CONTACT_QR_IMAGE_SIZE 232
#define DISPLAY_AI_CHAT_CAPTION_BOLD_LAYER_COUNT 3U
#define DISPLAY_REFRESH_SLOW_LOG_US (40LL * 1000LL)
#define DISPLAY_REFRESH_SLOW_LOG_INTERVAL_US (5LL * 1000LL * 1000LL)
#define DISPLAY_AI_HEADER_SETTINGS_X 239
#define DISPLAY_AI_HEADER_SETTINGS_Y 1
#define DISPLAY_AI_HEADER_SETTINGS_TEXT_Y 7
#define DISPLAY_AI_HEADER_SETTINGS_TEXT_WIDTH 48
#define DISPLAY_AI_HEADER_SETTINGS_HIT_X 226
#define DISPLAY_AI_HEADER_SETTINGS_HIT_Y 0
#define DISPLAY_AI_HEADER_SETTINGS_HIT_WIDTH 66
#define DISPLAY_AI_HEADER_SETTINGS_HIT_HEIGHT 28
#define DISPLAY_AI_HEADER_SETTINGS_ZOOM 307U
#define DISPLAY_AI_CHAT_CARD_WIDTH 304
#define DISPLAY_AI_CHAT_BUBBLE_LEFT_X 11
#define DISPLAY_AI_CHAT_BUBBLE_TEXT_X 9
#define DISPLAY_AI_CHAT_BUBBLE_TEXT_Y 7
#define DISPLAY_AI_CHAT_BUBBLE_PAD_RIGHT 9
#define DISPLAY_AI_CHAT_BUBBLE_PAD_BOTTOM 5
#define DISPLAY_AI_CHAT_BUBBLE_TOP_Y 12
#define DISPLAY_AI_CHAT_BUBBLE_GAP_Y 10
#define DISPLAY_AI_CHAT_BUBBLE_RADIUS 8
#define DISPLAY_AI_CHAT_TEXT_LINE_SPACE 4
#define DISPLAY_AI_CHAT_CJK_CHAR_WIDTH 14
#define DISPLAY_AI_CHAT_MIN_TEXT_WIDTH 28
#define DISPLAY_AI_CHAT_TEXT_SAFE_WIDTH 3
#define DISPLAY_AI_CHAT_TEXT_SAFE_HEIGHT 1
#define DISPLAY_AI_CHAT_MESSAGE_VISIBLE_MAX 14U
#define DISPLAY_AI_CHAT_SCROLLBAR_GUTTER 8
#define DISPLAY_AI_CHAT_CONTENT_WIDTH (DISPLAY_AI_CHAT_CARD_WIDTH - DISPLAY_AI_CHAT_SCROLLBAR_GUTTER)
#define DISPLAY_AI_CHAT_NEW_BUTTON_WIDTH 178
#define DISPLAY_AI_CHAT_NEW_BUTTON_HEIGHT 40
#define DISPLAY_AI_CHAT_VIEWPORT_HEIGHT 196
#define DISPLAY_AI_CHAT_VIRTUAL_OVERSCAN 52
#define DISPLAY_AI_AVATAR_COUNT 2U
#define DISPLAY_AI_AVATAR_BUDDY 0U
#define DISPLAY_AI_AVATAR_SPROUT 1U
#define DISPLAY_AI_AVATAR_CARD_X 10
#define DISPLAY_AI_AVATAR_CARD_Y 54
#define DISPLAY_AI_AVATAR_CARD_WIDTH 132
#define DISPLAY_AI_AVATAR_CARD_HEIGHT 250
#define DISPLAY_AI_AVATAR_IMG_X 18
#define DISPLAY_AI_AVATAR_IMG_Y 20
#define DISPLAY_AI_CAPTION_CARD_X 152
#define DISPLAY_AI_CAPTION_CARD_Y 54
#define DISPLAY_AI_CAPTION_CARD_WIDTH 318
#define DISPLAY_AI_CAPTION_CARD_HEIGHT 250
#define DISPLAY_AI_CHAT_TEXT_MAX_WIDTH \
    ((DISPLAY_AI_CHAT_CONTENT_WIDTH - 2 * (DISPLAY_AI_CHAT_BUBBLE_LEFT_X + DISPLAY_AI_CHAT_BUBBLE_TEXT_X)) - \
     (4 * DISPLAY_AI_CHAT_CJK_CHAR_WIDTH))
#define DISPLAY_WECHAT_HANGUP_TASK_STACK (32U * 1024U)
#define DISPLAY_WECHAT_HANGUP_TASK_PRIORITY 4
#define DISPLAY_WIFI_INDICATOR_MAX 32U
#define DISPLAY_WIFI_INDICATOR_BAR_COUNT 3U

typedef struct {
    uint32_t magic;
    lv_coord_t x;
    lv_coord_t y;
    lv_coord_t width;
    const ui_text_asset_t *current_asset;
    uint8_t font_size;
    lv_text_align_t align;
    lv_color_t color;
    bool layout_dirty;
} display_text_image_ctx_t;

typedef struct {
    lv_obj_t *bars[DISPLAY_WIFI_INDICATOR_BAR_COUNT];
    lv_obj_t *x_lines[2];
    lv_color_t active_color;
    lv_color_t inactive_color;
    bool active;
    bool status_valid;
    bool connected;
    uint8_t level;
} display_wifi_indicator_t;

typedef struct {
    uint32_t text_hash;
    int64_t utterance_id;
    lv_coord_t x;
    lv_coord_t y;
    lv_coord_t text_width;
    lv_coord_t text_height;
    lv_coord_t bubble_width;
    lv_coord_t bubble_height;
    uint8_t caption_type;
    uint8_t message_index;
    bool align_right;
} display_ai_message_layout_t;

static display_wifi_indicator_t s_wifi_indicators[DISPLAY_WIFI_INDICATOR_MAX];
static size_t s_wifi_indicator_count;

typedef enum {
    DISPLAY_WIFI_CONNECT_STATE_IDLE = 0,
    DISPLAY_WIFI_CONNECT_STATE_SELECT_FIRST,
    DISPLAY_WIFI_CONNECT_STATE_UNAVAILABLE,
    DISPLAY_WIFI_CONNECT_STATE_SHORT_PASSWORD,
    DISPLAY_WIFI_CONNECT_STATE_CONNECTING,
    DISPLAY_WIFI_CONNECT_STATE_FAILED,
    DISPLAY_WIFI_CONNECT_STATE_TIMEOUT,
    DISPLAY_WIFI_CONNECT_STATE_CONNECTED,
} display_wifi_connect_state_t;

typedef enum {
    DISPLAY_PAGE_HOME = 0,
    DISPLAY_PAGE_DEVICE,
    DISPLAY_PAGE_UUID_EDIT,
    DISPLAY_PAGE_SYSTEM,
    DISPLAY_PAGE_WIFI,
    DISPLAY_PAGE_WIFI_CONNECT,
    DISPLAY_PAGE_CALL_ADD,
    DISPLAY_PAGE_CALL_SCAN,
    DISPLAY_PAGE_CALL_LIST,
    DISPLAY_PAGE_CALL_REMARK,
    DISPLAY_PAGE_CALL_ACTIVE,
    DISPLAY_PAGE_WECHAT,
    DISPLAY_PAGE_WECHAT_ADD,
    DISPLAY_PAGE_WECHAT_ADD_EDIT,
    DISPLAY_PAGE_WECHAT_LIST,
    DISPLAY_PAGE_WECHAT_ACTIVE,
    DISPLAY_PAGE_AI_CHAT,
    DISPLAY_PAGE_AI_CHAT_SETTINGS,
    DISPLAY_PAGE_NETWORK_TEST,
    DISPLAY_PAGE_TIRTC_CONFIG,
    DISPLAY_PAGE_TIRTC_SCAN,
    DISPLAY_PAGE_DRIVER_STATUS,
    DISPLAY_PAGE_DEVICE_INFO,
} display_page_id_t;

typedef enum {
    DISPLAY_SCAN_OWNER_CALL = 0,
    DISPLAY_SCAN_OWNER_TIRTC_CONFIG,
    DISPLAY_SCAN_OWNER_WECHAT_CONTACT,
} display_scan_owner_t;

typedef enum {
    DISPLAY_CALL_ADD_FIELD_DEVICE_ID = 0,
    DISPLAY_CALL_ADD_FIELD_COUNT,
} display_call_add_field_t;

typedef enum {
    DISPLAY_DEVICE_VOLUME_RECEIVE_DOWN = 0,
    DISPLAY_DEVICE_VOLUME_RECEIVE_UP,
    DISPLAY_DEVICE_VOLUME_RECEIVE_MUTE,
    DISPLAY_DEVICE_VOLUME_SEND_DOWN,
    DISPLAY_DEVICE_VOLUME_SEND_UP,
    DISPLAY_DEVICE_VOLUME_SEND_MUTE,
} display_device_volume_action_t;

typedef enum {
    DISPLAY_CALL_VOLUME_MIC_DOWN = 0,
    DISPLAY_CALL_VOLUME_MIC_UP,
    DISPLAY_CALL_VOLUME_SPEAKER_DOWN,
    DISPLAY_CALL_VOLUME_SPEAKER_UP,
} display_call_volume_action_t;

typedef enum {
    DISPLAY_AI_SETTING_MIC_DOWN = 0,
    DISPLAY_AI_SETTING_MIC_UP,
    DISPLAY_AI_SETTING_SPEAKER_DOWN,
    DISPLAY_AI_SETTING_SPEAKER_UP,
    DISPLAY_AI_SETTING_AVATAR_BUDDY,
    DISPLAY_AI_SETTING_AVATAR_SPROUT,
} display_ai_setting_action_t;

typedef enum {
    DISPLAY_DEBUG_ACTION_TAP = 0,
    DISPLAY_DEBUG_ACTION_SCROLL,
} display_debug_action_type_t;

typedef struct {
    display_debug_action_type_t type;
    uint16_t x;
    uint16_t y;
    int16_t dx;
    int16_t dy;
    esp_err_t ret;
    SemaphoreHandle_t done;
    bool cancelled;
} display_debug_action_request_t;

typedef struct {
    uint8_t **bmp_data;
    size_t *bmp_size;
    esp_err_t ret;
    SemaphoreHandle_t done;
    bool cancelled;
} display_debug_capture_request_t;

typedef struct {
    lv_img_dsc_t dsc;
    lv_color_t *pixels;
    lv_coord_t size;
} display_qr_image_t;

static display_wifi_connect_state_t s_wifi_connect_state;
static display_qr_image_t s_binding_platform_qr_image;
static display_page_id_t s_wifi_parent_page = DISPLAY_PAGE_DEVICE;
static display_page_id_t s_uuid_parent_page = DISPLAY_PAGE_DEVICE;
static portMUX_TYPE s_debug_capture_lock = portMUX_INITIALIZER_UNLOCKED;
static display_debug_action_request_t *s_debug_action_request;
static display_debug_capture_request_t *s_debug_capture_request;

typedef enum {
    DISPLAY_VIDEO_SURFACE_DEVICE_CALL = 0,
    DISPLAY_VIDEO_SURFACE_WECHAT,
} display_video_surface_t;

typedef struct {
    uint8_t *buffer;
    size_t capacity;
    lv_img_dsc_t image;
    lv_coord_t x;
    lv_coord_t y;
    bool valid;
#if CONFIG_APP_MEDIA_PERIODIC_DIAGNOSTICS
    uint32_t diagnostic_checksum;
#endif
} display_call_video_overlay_snapshot_t;

typedef struct {
    lv_obj_t *top;
    lv_obj_t *controls;
    lv_obj_t *hangup;
    display_call_video_overlay_snapshot_t snapshots[3];
    uint8_t *snapshot_storage;
    size_t snapshot_storage_capacity;
    int64_t hide_at_us;
    int64_t snapshot_at_us;
    bool hidden;
    bool snapshot_dirty;
    bool refresh_trace_pending;
} display_call_video_overlays_t;

static lv_obj_t *s_home_page;
static lv_obj_t *s_home_carousel;
static lv_obj_t *s_home_pages[2];
static lv_obj_t *s_home_indicator_dots[2];
static lv_obj_t *s_home_time_label;
static lv_obj_t *s_home_wifi_bars[3];
static lv_obj_t *s_home_wifi_x_lines[2];
static time_t s_home_clock_last_second = (time_t)-1;
static bool s_home_wifi_status_valid;
static bool s_home_wifi_connected;
static uint8_t s_home_wifi_level;
static bool s_home_indicator_valid;
static bool s_home_indicator_second_page;
static lv_obj_t *s_main_page;
static lv_obj_t *s_call_page;
static lv_obj_t *s_call_add_page;
static lv_obj_t *s_call_add_edit_page;
static lv_obj_t *s_call_scan_page;
static lv_obj_t *s_call_list_page;
static lv_obj_t *s_call_remark_page;
static lv_obj_t *s_call_active_page;
static lv_obj_t *s_wechat_page;
static lv_obj_t *s_wechat_add_page;
static lv_obj_t *s_wechat_add_edit_page;
static lv_obj_t *s_wechat_list_page;
static lv_obj_t *s_wechat_remark_page;
static lv_obj_t *s_wechat_active_page;
static lv_obj_t *s_uuid_edit_page;
static lv_obj_t *s_system_page;
static lv_obj_t *s_system_memory_free_label;
static lv_obj_t *s_system_memory_largest_label;
static lv_obj_t *s_wifi_page;
static lv_obj_t *s_wifi_connect_page;
static lv_obj_t *s_network_test_page;
static lv_obj_t *s_tirtc_config_page;
static lv_obj_t *s_tirtc_config_edit_page;
static lv_obj_t *s_ota_page;
static lv_obj_t *s_ai_chat_page;
static lv_obj_t *s_ai_chat_settings_page;
static display_page_registry_t s_page_registry;
static const display_page_entry_t s_page_entries[] = {
    {"home", &s_home_page},
    {"device", &s_main_page},
    {"uuid_edit", &s_uuid_edit_page},
    {"system", &s_system_page},
    {"wifi", &s_wifi_page},
    {"wifi_connect", &s_wifi_connect_page},
    {"call", &s_call_page},
    {"call_add", &s_call_add_page},
    {"call_add_edit", &s_call_add_edit_page},
    {"call_scan", &s_call_scan_page},
    {"call_list", &s_call_list_page},
    {"call_remark", &s_call_remark_page},
    {"call_active", &s_call_active_page},
    {"wechat", &s_wechat_page},
    {"wechat_add", &s_wechat_add_page},
    {"wechat_add_edit", &s_wechat_add_edit_page},
    {"wechat_list", &s_wechat_list_page},
    {"wechat_remark", &s_wechat_remark_page},
    {"wechat_active", &s_wechat_active_page},
    {"network_test", &s_network_test_page},
    {"tirtc_config", &s_tirtc_config_page},
    {"tirtc_config_edit", &s_tirtc_config_edit_page},
    {"ota", &s_ota_page},
    {"ai_chat", &s_ai_chat_page},
    {"ai_chat_settings", &s_ai_chat_settings_page},
};
static lv_obj_t *s_keyboard;
static lv_obj_t *s_uuid_keyboard;
static lv_obj_t *s_uuid_label;
static lv_obj_t *s_device_qrcode;
static display_qr_image_t s_device_qr_image;
static lv_obj_t *s_device_connection_dot;
static lv_obj_t *s_device_door_dot;
static lv_obj_t *s_device_connection_value_label;
static lv_obj_t *s_device_door_value_label;
static lv_obj_t *s_device_receive_volume_label;
static lv_obj_t *s_device_send_volume_label;
static lv_obj_t *s_device_receive_mute_label;
static lv_obj_t *s_device_send_mute_label;
static lv_obj_t *s_device_qr_view;
static lv_obj_t *s_device_media_bitrate_label;
static lv_obj_t *s_device_media_fps_label;
static lv_obj_t *s_device_media_resolution_label;
static uint8_t s_device_receive_restore_volume = 50U;
static uint8_t s_device_send_restore_volume = 50U;
static bool s_device_receive_restore_valid;
static bool s_device_send_restore_valid;
static lv_obj_t *s_uuid_ta;
static lv_obj_t *s_uuid_edit_hint_label;
static lv_obj_t *s_uuid_edit_length_label;
static lv_obj_t *s_uuid_edit_status_label;
static lv_obj_t *s_main_hint_label;
static lv_obj_t *s_wifi_connection_state_label;
static lv_obj_t *s_wifi_scan_state_label;
static lv_obj_t *s_wifi_scan_count_label;
static lv_obj_t *s_wifi_list;
static lv_obj_t *s_wifi_list_buttons[DISPLAY_WIFI_SCAN_MAX];
static lv_obj_t *s_wifi_list_ssid_labels[DISPLAY_WIFI_SCAN_MAX];
static lv_obj_t *s_wifi_list_rssi_labels[DISPLAY_WIFI_SCAN_MAX];
static lv_obj_t *s_wifi_connect_hint_label;
static lv_obj_t *s_wifi_connect_rssi_label;
static lv_obj_t *s_wifi_connect_details_label;
static lv_obj_t *s_network_summary_wifi_label;
static lv_obj_t *s_network_summary_ip_label;
static lv_obj_t *s_network_gateway_value_label;
static lv_obj_t *s_network_dns_value_label;
static lv_obj_t *s_network_wan_value_label;
static lv_obj_t *s_network_service_row;
static lv_obj_t *s_network_service_value_label;
static lv_obj_t *s_network_jitter_value_label;
static lv_obj_t *s_network_loss_value_label;
static lv_obj_t *s_network_result_box;
static lv_obj_t *s_network_result_label;
static lv_obj_t *s_network_result_detail_label;
static lv_obj_t *s_tirtc_config_value_labels[DISPLAY_TIRTC_CONFIG_FIELD_COUNT];
static lv_obj_t *s_tirtc_edit_ta;
static lv_obj_t *s_tirtc_edit_keyboard;
static lv_obj_t *s_tirtc_edit_hint_label;
static lv_obj_t *s_tirtc_edit_length_label;
static lv_obj_t *s_tirtc_edit_status_label;
static display_tirtc_config_field_t s_tirtc_edit_field;
static lv_obj_t *s_call_qrcode;
static lv_obj_t *s_call_device_id_label;
static display_qr_image_t s_call_qr_image;
static lv_obj_t *s_call_duration_label;
static lv_obj_t *s_call_mic_value_label;
static lv_obj_t *s_call_speaker_value_label;
static lv_obj_t *s_call_audio_panel;
static lv_obj_t *s_call_video_panel;
static lv_obj_t *s_call_audio_state_label;
static lv_obj_t *s_call_audio_peer_label;
static lv_obj_t *s_call_video_state_label;
static lv_obj_t *s_call_video_peer_label;
static lv_obj_t *s_call_video_duration_label;
static lv_obj_t *s_call_video_stats_label;
static lv_obj_t *s_call_video_mic_value_label;
static lv_obj_t *s_call_video_speaker_value_label;
static lv_obj_t *s_call_video_image;
static lv_obj_t *s_call_video_placeholder_label;
static lv_obj_t *s_call_hangup_confirm_box;
static lv_img_dsc_t s_call_video_image_dsc;
static uint32_t s_call_video_sequence;
static bool s_call_video_first_frame_logged;
static bool s_call_video_direct_lcd_active;
static bool s_call_video_direct_lcd_failed;
static int64_t s_call_video_last_presented_at_us;
static int64_t s_call_video_last_stall_log_at_us;
static uint32_t s_call_video_last_presented_sequence;
static bool s_call_video_landscape_active;
static display_call_video_overlays_t s_call_video_overlays;
/* Shared by device-call and WeChat because only one full-screen video owner is
 * active at a time. The renderer's output slot stays immutable while owned by
 * the UI; overlays are composed into this dedicated DMA buffer instead. */
static uint16_t *s_call_video_composition_pixels;
static bool s_call_hangup_pending;
/* Browser-driven taps execute in the LVGL task but do not update the physical
 * pointer driver's act_point. Keep the injected point scoped to one synthetic
 * event sequence so full-screen video controls use the same coordinates as a
 * real touch instead of the controller's previous position. */
static bool s_debug_tap_point_valid;
static lv_point_t s_debug_tap_point;
static display_call_type_t s_call_visible_type = DISPLAY_CALL_TYPE_AUDIO;
static char s_call_visible_room_id[DISPLAY_CALL_ROOM_ID_MAX];
static lv_obj_t *s_call_add_value_labels[DISPLAY_CALL_ADD_FIELD_COUNT];
static lv_obj_t *s_call_add_edit_ta;
static lv_obj_t *s_call_add_edit_keyboard;
static lv_obj_t *s_call_add_edit_hint_label;
static lv_obj_t *s_call_add_edit_length_label;
static lv_obj_t *s_call_add_edit_status_label;
static lv_obj_t *s_call_remark_ta;
static lv_obj_t *s_call_remark_keyboard;
static lv_obj_t *s_call_remark_length_label;
static lv_obj_t *s_call_remark_status_label;
static lv_obj_t *s_call_scan_info_overlay;
static lv_obj_t *s_call_scan_image;
static lv_img_dsc_t s_call_scan_preview_dsc[DISPLAY_SCAN_PREVIEW_BUFFER_COUNT];
static uint16_t *s_call_scan_preview_buffers[DISPLAY_SCAN_PREVIEW_BUFFER_COUNT];
static uint8_t s_call_scan_preview_index;
static video_frame_converter_handle_t s_call_scan_preview_converter;
static SemaphoreHandle_t s_call_scan_preview_mutex;
static bool s_call_scan_preview_first_frame_logged;
static lv_obj_t *s_wechat_qrcode;
static display_qr_image_t s_wechat_qr_image;
static lv_obj_t *s_wechat_duration_label;
static lv_obj_t *s_wechat_mic_value_label;
static lv_obj_t *s_wechat_speaker_value_label;
static lv_obj_t *s_wechat_video_panel;
static lv_obj_t *s_wechat_video_image;
static lv_obj_t *s_wechat_video_placeholder_label;
static lv_obj_t *s_wechat_video_state_label;
static lv_img_dsc_t s_wechat_video_image_dsc;
static uint32_t s_wechat_video_sequence;
static bool s_wechat_video_first_frame_logged;
static bool s_wechat_video_direct_lcd_active;
static bool s_wechat_video_direct_lcd_failed;
static int64_t s_wechat_video_last_presented_at_us;
static int64_t s_wechat_video_last_stall_log_at_us;
static uint32_t s_wechat_video_last_presented_sequence;
static bool s_wechat_video_session_active;
static display_call_video_overlays_t s_wechat_video_overlays;
static lv_obj_t *s_wechat_scan_info_overlay;
static lv_obj_t *s_wechat_add_open_id_label;
static lv_obj_t *s_wechat_add_edit_ta;
static lv_obj_t *s_wechat_add_edit_keyboard;
static lv_obj_t *s_wechat_add_edit_hint_label;
static lv_obj_t *s_wechat_add_edit_length_label;
static lv_obj_t *s_wechat_add_edit_status_label;
static lv_obj_t *s_wechat_empty_label;
static lv_obj_t *s_wechat_contact_rows[DISPLAY_WECHAT_CONTACT_MAX];
static lv_obj_t *s_wechat_contact_remark_labels[DISPLAY_WECHAT_CONTACT_MAX];
static lv_obj_t *s_wechat_contact_open_id_labels[DISPLAY_WECHAT_CONTACT_MAX];
static lv_obj_t *s_wechat_remark_ta;
static lv_obj_t *s_wechat_remark_keyboard;
static lv_obj_t *s_wechat_remark_status_label;
static lv_obj_t *s_ota_status_label;
static lv_obj_t *s_ota_version_label;
static lv_obj_t *s_ota_second_label;
static lv_obj_t *s_ota_second_value_label;
static lv_obj_t *s_ota_progress_bar;
static lv_obj_t *s_ota_start_btn;
static lv_obj_t *s_ota_start_btn_label;
static lv_obj_t *s_ota_reboot_btn;
static lv_obj_t *s_ota_reboot_btn_label;
static lv_obj_t *s_ota_progress_title_label;
static lv_obj_t *s_ota_progress_percent_label;
static lv_obj_t *s_ota_progress_hint_label;
static lv_obj_t *s_ota_action_panel;
static lv_obj_t *s_ai_status_label;
static lv_obj_t *s_ai_avatar_img;
static lv_obj_t *s_ai_avatar_name_label;
static lv_obj_t *s_ai_avatar_state_label;
static uint8_t s_ai_avatar_last_variant = UINT8_MAX;
static ai_chat_avatar_state_t s_ai_avatar_last_state = AI_CHAT_AVATAR_STATE_COUNT;
static lv_obj_t *s_ai_caption_bar;
static lv_obj_t *s_ai_single_caption_label;
static lv_obj_t *s_ai_message_list;
static lv_obj_t *s_ai_message_boxes[DISPLAY_AI_CHAT_MESSAGE_VISIBLE_MAX];
static lv_obj_t *s_ai_message_labels[DISPLAY_AI_CHAT_MESSAGE_VISIBLE_MAX];
static lv_obj_t *s_ai_message_bold_labels[DISPLAY_AI_CHAT_MESSAGE_VISIBLE_MAX]
                                               [DISPLAY_AI_CHAT_CAPTION_BOLD_LAYER_COUNT];
static lv_obj_t *s_ai_message_scroll_spacer;
static EXT_RAM_BSS_ATTR display_ai_message_layout_t s_ai_message_layouts[DISPLAY_AI_CHAT_MESSAGE_MAX];
static uint8_t s_ai_visible_message_indices[DISPLAY_AI_CHAT_MESSAGE_VISIBLE_MAX];
static uint32_t s_ai_visible_message_generations[DISPLAY_AI_CHAT_MESSAGE_VISIBLE_MAX];
static uint8_t s_ai_message_layout_count;
static uint32_t s_ai_message_layout_generation;
static lv_coord_t s_ai_message_content_height;
static lv_coord_t s_ai_new_chat_button_y;
static bool s_ai_message_layout_font_ready;
static bool s_ai_message_layout_new_button_visible;
static lv_obj_t *s_ai_new_chat_btn;
static lv_obj_t *s_ai_new_chat_btn_label;
static bool s_ai_message_touching;
static lv_obj_t *s_ai_settings_mic_value_label;
static lv_obj_t *s_ai_settings_speaker_value_label;
static lv_obj_t *s_ai_settings_avatar_buttons[DISPLAY_AI_AVATAR_COUNT];
static lv_obj_t *s_ai_settings_avatar_labels[DISPLAY_AI_AVATAR_COUNT];
static lv_obj_t *s_password_ta;
static bool s_call_scan_active;
static display_scan_owner_t s_scan_owner;
static display_call_add_field_t s_call_add_edit_field;
static EXT_RAM_BSS_ATTR char s_call_remark_edit_device_id[DISPLAY_CALL_CONTACT_DEVICE_ID_MAX];
static EXT_RAM_BSS_ATTR char s_device_qr_payload[DISPLAY_DEVICE_QR_PAYLOAD_MAX];
static EXT_RAM_BSS_ATTR char s_call_qr_payload[DISPLAY_DEVICE_QR_PAYLOAD_MAX];
static EXT_RAM_BSS_ATTR char s_wechat_qr_payload[DISPLAY_CONTACT_QR_PAYLOAD_MAX];
static int64_t s_call_active_started_us;
static int64_t s_call_active_page_opened_us;
static int64_t s_wechat_active_started_us;
static int64_t s_wechat_active_page_opened_us;

static void display_wifi_ap_select_cb(lv_event_t *event);
static void display_show_home_page(void);
static void display_show_main_page(void);
static void display_show_call_page(void);
static void display_show_call_add_page(void);
static void display_show_call_add_edit_page(display_call_add_field_t field);
static void display_show_call_scan_page(void);
static void display_show_tirtc_config_scan_page(void);
static void display_show_call_list_page(void);
static void display_show_call_remark_page(uint8_t contact_index);
static void display_show_call_active_page(void);
static void display_show_wechat_page(void);
static void display_show_wechat_add_page(void);
static void display_show_wechat_add_edit_page(void);
static void display_show_wechat_list_page(void);
static void display_show_wechat_remark_page(uint8_t contact_index);
static void display_show_wechat_active_page(void);
static void display_show_system_page(void);
static void display_show_wifi_page(void);
static void display_open_wifi_page(display_page_id_t parent_page);
static void display_show_uuid_edit_page(void) __attribute__((unused));
static void display_show_ai_chat_page(void);
static void display_show_network_test_page(void);
static void display_show_tirtc_config_page(void);
static void display_show_tirtc_config_edit_page(display_tirtc_config_field_t field);
static void display_show_ota_page(void);
static void display_show_ai_chat_settings_page(void);
static void display_home_view_btn_cb(lv_event_t *event);
static void display_home_call_btn_cb(lv_event_t *event);
static void display_home_wechat_btn_cb(lv_event_t *event);
static void display_home_ai_btn_cb(lv_event_t *event);
static void display_home_settings_btn_cb(lv_event_t *event);
static void display_binding_wifi_btn_cb(lv_event_t *event);
static void display_binding_refresh_btn_cb(lv_event_t *event);
static void display_ai_back_btn_cb(lv_event_t *event);
static void display_ai_settings_btn_cb(lv_event_t *event);
static void display_ai_settings_back_btn_cb(lv_event_t *event);
static void display_ai_settings_action_btn_cb(lv_event_t *event);
static void display_call_back_btn_cb(lv_event_t *event);
static void display_call_child_back_btn_cb(lv_event_t *event);
static void display_call_add_btn_cb(lv_event_t *event);
static void display_call_list_btn_cb(lv_event_t *event);
static void display_call_list_refresh_btn_cb(lv_event_t *event);
static void display_call_scan_btn_cb(lv_event_t *event);
static void display_call_scan_tap_cb(lv_event_t *event);
static void display_call_scan_info_btn_cb(lv_event_t *event);
static void display_call_scan_info_close_btn_cb(lv_event_t *event);
static void display_call_add_field_btn_cb(lv_event_t *event);
static void display_call_add_edit_back_btn_cb(lv_event_t *event);
static void display_call_add_edit_save_btn_cb(lv_event_t *event);
static void display_call_confirm_add_btn_cb(lv_event_t *event);
static void display_call_pending_contact_response_btn_cb(lv_event_t *event);
static void display_call_contact_call_btn_cb(lv_event_t *event);
static void display_call_contact_remark_btn_cb(lv_event_t *event);
static void display_call_contact_delete_cb(lv_event_t *event);
static void display_call_delete_cancel_btn_cb(lv_event_t *event);
static void display_call_delete_confirm_btn_cb(lv_event_t *event);
static void display_call_remark_back_btn_cb(lv_event_t *event);
static void display_call_remark_save_btn_cb(lv_event_t *event);
static void display_call_remark_textarea_event_cb(lv_event_t *event);
static void display_call_hangup_btn_cb(lv_event_t *event);
static void display_call_hangup_cancel_btn_cb(lv_event_t *event);
static void display_call_hangup_confirm_btn_cb(lv_event_t *event);
static void display_call_volume_btn_cb(lv_event_t *event);
static void display_call_video_surface_tap_cb(lv_event_t *event);
static void display_wechat_child_back_btn_cb(lv_event_t *event);
static void display_wechat_add_btn_cb(lv_event_t *event);
static void display_wechat_list_btn_cb(lv_event_t *event);
static void display_wechat_scan_btn_cb(lv_event_t *event);
static void display_wechat_scan_info_btn_cb(lv_event_t *event);
static void display_wechat_scan_info_close_btn_cb(lv_event_t *event);
static void display_wechat_confirm_add_btn_cb(lv_event_t *event);
static void display_wechat_add_field_btn_cb(lv_event_t *event);
static void display_wechat_add_edit_back_btn_cb(lv_event_t *event);
static void display_wechat_add_edit_save_btn_cb(lv_event_t *event);
static void display_wechat_contact_remark_btn_cb(lv_event_t *event);
static void display_wechat_remark_back_btn_cb(lv_event_t *event);
static void display_wechat_remark_save_btn_cb(lv_event_t *event);
static void display_wechat_contact_call_btn_cb(lv_event_t *event);
static void display_wechat_contact_delete_cb(lv_event_t *event);
static void display_wechat_delete_cancel_btn_cb(lv_event_t *event);
static void display_wechat_delete_confirm_btn_cb(lv_event_t *event);
static void display_wechat_hangup_btn_cb(lv_event_t *event);
static void display_wechat_volume_btn_cb(lv_event_t *event);
static void display_system_back_btn_cb(lv_event_t *event);
static void display_system_ota_btn_cb(lv_event_t *event);
static void display_system_tirtc_config_btn_cb(lv_event_t *event);
static void display_network_test_start_btn_cb(lv_event_t *event);
static void __attribute__((unused)) display_tirtc_config_field_btn_cb(lv_event_t *event);
static void __attribute__((unused)) display_tirtc_config_scan_btn_cb(lv_event_t *event);
static void display_tirtc_config_edit_back_btn_cb(lv_event_t *event);
static void display_tirtc_config_edit_save_btn_cb(lv_event_t *event);
static void display_call_add_edit_textarea_event_cb(lv_event_t *event);
static void display_wechat_add_edit_textarea_event_cb(lv_event_t *event);
static void display_wechat_remark_textarea_event_cb(lv_event_t *event);
static void display_tirtc_edit_textarea_event_cb(lv_event_t *event);
static void display_update_call_add_field_labels(void);
static void display_update_call_add_edit_feedback(const char *status_text, lv_color_t status_color);
static void display_update_wechat_add_field_label(void);
static void display_update_wechat_add_edit_feedback(const char *status_text, lv_color_t status_color);
static void display_update_tirtc_edit_feedback(const char *status_text, lv_color_t status_color);
static const char *display_tirtc_config_field_title(display_tirtc_config_field_t field);
static const char *display_tirtc_config_field_value(const display_status_t *status,
                                                    display_tirtc_config_field_t field);
static size_t display_tirtc_config_field_max_len(display_tirtc_config_field_t field);
static void display_ota_start_btn_cb(lv_event_t *event);
static void display_ota_reboot_btn_cb(lv_event_t *event);
static void display_build_system_page(lv_obj_t *screen);
static void display_build_call_page(lv_obj_t *screen);
static void display_build_call_add_page(lv_obj_t *screen);
static void display_build_call_add_edit_page(lv_obj_t *screen);
static void display_build_call_scan_page(lv_obj_t *screen);
static void display_build_call_list_page(lv_obj_t *screen);
static void display_build_call_remark_page(lv_obj_t *screen);
static void display_build_call_active_page(lv_obj_t *screen);
static void display_build_wechat_page(lv_obj_t *screen);
static void display_build_wechat_add_page(lv_obj_t *screen);
static void display_build_wechat_add_edit_page(lv_obj_t *screen);
static void display_build_wechat_list_page(lv_obj_t *screen);
static void display_build_wechat_remark_page(lv_obj_t *screen);
static void display_build_wechat_active_page(lv_obj_t *screen);
static void display_build_network_test_page(lv_obj_t *screen);
static void display_build_tirtc_config_page(lv_obj_t *screen);
static void display_build_tirtc_config_edit_page(lv_obj_t *screen);
static void display_build_ai_chat_page(lv_obj_t *screen);
static void display_build_ai_chat_settings_page(lv_obj_t *screen);
static void display_build_ota_page(lv_obj_t *screen);
static void display_build_uuid_edit_page(lv_obj_t *screen);
static void display_build_wifi_page(lv_obj_t *screen);
static void display_build_wifi_connect_page(lv_obj_t *screen);
static void display_build_binding_prompt_overlay(lv_obj_t *parent);
static void display_refresh_wifi_list(const display_status_t *status);
static void display_update_wifi_scan_state(const display_status_t *status);
static void display_update_network_test_page(const display_status_t *status);
static void display_update_tirtc_config_page(const display_status_t *status);
static void display_update_call_page(const display_status_t *status);
static void display_update_call_active_page(const display_status_t *status);
static void display_update_call_video_frame(void);
static void display_reset_call_video_surface(void);
static void display_reset_wechat_video_surface(void);
static void display_apply_call_video_layout(bool active);
static bool display_call_state_keeps_active_page(display_call_state_t state);
static void display_update_wechat_page(const display_status_t *status);
static void display_update_wechat_contact_list(const display_status_t *status);
static void display_update_wechat_active_page(const display_status_t *status);
static void display_update_ota_page(const display_status_t *status);
static void display_update_main_page(const display_status_t *status);
static void display_update_system_memory(const display_status_t *status);
static void display_update_ai_chat_page(const display_status_t *status);
static void display_update_ai_chat_settings_page(const display_status_t *status);
static bool display_ai_chat_layout_cache_needs_rebuild(const display_status_t *status,
                                                       uint8_t message_count,
                                                       bool show_new_chat_button,
                                                       bool font_ready);
static void display_rebuild_ai_chat_layout_cache(const display_status_t *status,
                                                 uint8_t message_count,
                                                 bool show_new_chat_button,
                                                 bool font_ready);
static void display_update_ai_chat_scroll_spacer(void);
static void display_render_ai_chat_visible_messages(const display_status_t *status);
static void display_update_binding_prompt(const display_status_t *status);
static void display_snapshot_task(void *arg);
static esp_err_t display_start_snapshot_task(void);
static void display_refresh_timer(lv_timer_t *timer);
static void display_video_refresh_timer(lv_timer_t *timer);
static void display_set_video_refresh_enabled(bool enabled);
static esp_err_t display_start_refresh_timer(void);
static void display_read_latest_status(display_status_t *status);
static void display_show_wifi_alert(const char *title, const char *message);
static void display_hide_wifi_alert(void);
static const lv_font_t *display_ascii_font(uint8_t size);
static const lv_font_t *display_cjk_font(void);
static const lv_font_t *display_ai_chat_font(void);
static void display_apply_ai_dialog_font_if_ready(void);
static void display_text_set(lv_obj_t *obj, const char *text);
static void display_text_set_color(lv_obj_t *obj, lv_color_t color, lv_style_selector_t selector);
static void display_text_set_layout(lv_obj_t *obj,
                                    lv_coord_t x,
                                    lv_coord_t y,
                                    lv_coord_t width,
                                    lv_text_align_t align);
static lv_obj_t *display_create_figma_text(lv_obj_t *parent,
                                           const char *text,
                                           lv_coord_t x,
                                           lv_coord_t y,
                                           lv_coord_t width,
                                           lv_color_t color,
                                           uint8_t font_size,
                                           lv_text_align_t align);
static lv_obj_t *display_create_native_text(lv_obj_t *parent,
                                            const char *text,
                                            lv_coord_t x,
                                            lv_coord_t y,
                                            lv_coord_t width,
                                            lv_color_t color,
                                            uint8_t font_size,
                                            lv_text_align_t align);
static lv_obj_t *display_create_native_live_text(lv_obj_t *parent,
                                                 const char *text,
                                                 lv_coord_t x,
                                                 lv_coord_t y,
                                                 lv_coord_t width,
                                                 lv_color_t color,
                                                 lv_text_align_t align);
static lv_obj_t *display_create_figma_live_text(lv_obj_t *parent,
                                                const char *text,
                                                lv_coord_t x,
                                                lv_coord_t y,
                                                lv_coord_t width,
                                                lv_color_t color,
                                                lv_text_align_t align);
static lv_obj_t *display_create_ai_text(lv_obj_t *parent,
                                        const char *text,
                                        lv_coord_t x,
                                        lv_coord_t y,
                                        lv_coord_t width,
                                        lv_color_t color,
                                        lv_text_align_t align);
static lv_obj_t *display_create_ai_dialog_text(lv_obj_t *parent,
                                               const char *text,
                                               lv_coord_t x,
                                               lv_coord_t y,
                                               lv_coord_t width,
                                               lv_color_t color,
                                               lv_text_align_t align);
static lv_obj_t *display_create_ai_static_text(lv_obj_t *parent,
                                               const char *text,
                                               lv_coord_t x,
                                               lv_coord_t y,
                                               lv_coord_t width,
                                               lv_color_t color,
                                               uint8_t font_size,
                                               lv_text_align_t align);
static lv_obj_t *display_create_figma_box(lv_obj_t *parent,
                                          lv_coord_t x,
                                          lv_coord_t y,
                                          lv_coord_t width,
                                          lv_coord_t height,
                                          lv_color_t fill,
                                          lv_color_t stroke,
                                          lv_coord_t radius);
static lv_obj_t *display_create_native_box(lv_obj_t *parent,
                                           lv_coord_t x,
                                           lv_coord_t y,
                                           lv_coord_t width,
                                           lv_coord_t height,
                                           lv_color_t fill,
                                           lv_color_t stroke,
                                           lv_coord_t radius);
static lv_obj_t *display_create_figma_button(lv_obj_t *parent,
                                             lv_coord_t x,
                                             lv_coord_t y,
                                             lv_coord_t width,
                                             lv_coord_t height,
                                             lv_color_t fill,
                                             lv_color_t stroke,
                                             const char *text,
                                             lv_color_t text_color,
                                             uint8_t font_size,
                                             lv_event_cb_t cb);
static lv_obj_t *display_create_native_button(lv_obj_t *parent,
                                              lv_coord_t x,
                                              lv_coord_t y,
                                              lv_coord_t width,
                                              lv_coord_t height,
                                              lv_color_t fill,
                                              lv_color_t stroke,
                                              const char *text,
                                              lv_color_t text_color,
                                              uint8_t font_size,
                                              lv_event_cb_t cb);
static lv_obj_t *display_create_native_live_button(lv_obj_t *parent,
                                                   lv_coord_t x,
                                                   lv_coord_t y,
                                                   lv_coord_t width,
                                                   lv_coord_t height,
                                                   lv_color_t fill,
                                                   lv_color_t stroke,
                                                   const char *text,
                                                   lv_color_t text_color,
                                                   lv_event_cb_t cb);
static void display_device_volume_btn_cb(lv_event_t *event);
static void display_layout_wifi_keyboard(void);
static void display_set_password_placeholder(const char *text, lv_color_t border_color);
static bool display_page_is_visible(lv_obj_t *page);
static esp_err_t display_debug_tap_in_lvgl(uint16_t x, uint16_t y);
static esp_err_t display_request_call_hangup_locked(void);
static esp_err_t display_capture_bmp_in_lvgl(uint8_t **bmp_data, size_t *bmp_size);
static bool display_debug_process_pending_action(void);
static bool display_debug_process_pending_capture(void);
static uint8_t display_adjust_volume(uint8_t current, int delta);
static void display_update_home_status_bar(const display_status_t *status);
static void display_update_home_indicators(void);
static void display_home_set_page(bool second_page);
static esp_err_t display_enter_app(display_app_id_t app_id);
static void display_return_home(void);
static void display_hide_call_alert(void);
static void display_hide_wechat_delete_confirm(void);
static void display_show_wechat_delete_confirm(uint8_t contact_index);

#define DISPLAY_WIFI_KEYBOARD_HEIGHT         124
#define DISPLAY_WIFI_CONNECT_INPUT_WIDTH     304
#define DISPLAY_WIFI_CONNECT_INPUT_HEIGHT    31
#define DISPLAY_WIFI_CONNECT_INPUT_TOP       58
#define DISPLAY_WIFI_CONNECT_HINT_TOP        36
#define DISPLAY_WIFI_CONNECT_HINT_LEFT       8
#define DISPLAY_WIFI_CONNECT_HINT_WIDTH      210
#define DISPLAY_WIFI_CONNECT_RSSI_WIDTH      70
#define DISPLAY_WIFI_CONNECT_DETAILS_TOP     100
#define DISPLAY_WIFI_CONNECT_DETAILS_WIDTH   280
#define DISPLAY_WIFI_CONNECT_TIMEOUT_US      (25LL * 1000000LL)
#define DISPLAY_WIFI_PASSWORD_MIN_LEN        8
#define DISPLAY_WIFI_KEYBOARD_LEFT           8
#define DISPLAY_WIFI_KEYBOARD_TOP            104
#define DISPLAY_WIFI_KEYBOARD_WIDTH          304
#define DISPLAY_WIFI_LIST_SSID_WIDTH         188
#define DISPLAY_WIFI_LIST_RSSI_WIDTH         68
#define DISPLAY_WIFI_LIST_BUILD_BATCH        1
#define DISPLAY_WIFI_STATUS_LEFT_WIDTH       196
#define DISPLAY_WIFI_STATUS_RIGHT_WIDTH      96
#define DISPLAY_WIFI_SCAN_REFRESH_GRACE_US   (500LL * 1000LL)
#define DISPLAY_WIFI_SCAN_REFRESH_TIMEOUT_US (8LL * 1000000LL)
#define DISPLAY_DEVICE_VOLUME_RESTORE_DEFAULT 50U
#define DISPLAY_MIN_VALID_UNIX_TIME          1672531200LL
#define DISPLAY_UUID_INPUT_WIDTH             304
#define DISPLAY_UUID_INPUT_HEIGHT            31
#define DISPLAY_UUID_INPUT_TOP               55
#define DISPLAY_UUID_HINT_TOP                35
#define DISPLAY_UUID_HINT_LEFT               8
#define DISPLAY_UUID_HINT_WIDTH              196
#define DISPLAY_UUID_LENGTH_WIDTH            72
#define DISPLAY_UUID_STATUS_TOP              91
#define DISPLAY_UUID_STATUS_WIDTH            304
#define DISPLAY_UUID_KEYBOARD_TOP            112
#define DISPLAY_UUID_KEYBOARD_HEIGHT         108
#define DISPLAY_CALL_CONTACT_COUNT           DISPLAY_CALL_CONTACT_MAX
#define DISPLAY_WECHAT_CONTACT_COUNT         DISPLAY_WECHAT_CONTACT_MAX
#define DISPLAY_WECHAT_OPEN_ID_LENGTH        28U
#define DISPLAY_WECHAT_OPEN_ID_ACCEPTED_CHARS "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-"
#define DISPLAY_CALL_VOLUME_STEP             8
#define DISPLAY_KB_BTN_KEYBOARD_ID           35
#define DISPLAY_KB_BTN_CURSOR_LEFT_ID        36
#define DISPLAY_KB_BTN_SPACE_ID              37
#define DISPLAY_KB_BTN_CURSOR_RIGHT_ID       38
#define DISPLAY_KB_BTN_JOIN_ID               39
#define DISPLAY_TIRTC_VERSION_TEXT           "TiRTC " APP_DEMO_TIRTC_SDK_VERSION
#define DISPLAY_AI_CHAT_STATE_IDLE           0U
#define DISPLAY_AI_CHAT_STATE_STARTING       1U
#define DISPLAY_AI_CHAT_STATE_TOKEN          2U
#define DISPLAY_AI_CHAT_STATE_CONNECTING     3U
#define DISPLAY_AI_CHAT_STATE_CONNECTED      4U
#define DISPLAY_AI_CHAT_STATE_STARTING_SESSION 5U
#define DISPLAY_AI_CHAT_STATE_IN_SESSION     6U
#define DISPLAY_AI_CHAT_STATE_STOPPING       7U
#define DISPLAY_AI_CHAT_STATE_ERROR          8U
#define DISPLAY_AI_CHAT_CAPTION_TYPE_ASR     0U
#define DISPLAY_AI_CHAT_CAPTION_TYPE_TTS     1U
#define DISPLAY_SCAN_RESULT_DISPATCH_TASK_STACK 4096
#define DISPLAY_SCAN_RESULT_DISPATCH_TASK_PRIORITY 2
#define DISPLAY_SCAN_RESULT_DISPATCH_TASK_ALLOC_CAPS APP_TASK_STACK_CAPS_BACKGROUND
#define DISPLAY_SCAN_RESULT_DISPATCH_RETRY_COUNT 20U
#define DISPLAY_SCAN_RESULT_DISPATCH_RETRY_MS 20U
#define DISPLAY_KB_BTN(width) (LV_BTNMATRIX_CTRL_POPOVER | (width))

typedef struct {
    esp_err_t result;
    char device_id[DISPLAY_CALL_CONTACT_DEVICE_ID_MAX];
    char credential[DISPLAY_TIRTC_CONFIG_TEXT_MAX];
    char open_id[DISPLAY_WECHAT_OPEN_ID_MAX];
    char raw_payload[DISPLAY_CONTACT_QR_PAYLOAD_MAX];
} display_contact_scan_result_event_t;

static const char * const s_wifi_keyboard_map_lc[] = {
    "1#", "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", LV_SYMBOL_BACKSPACE, "\n",
    "ABC", "a", "s", "d", "f", "g", "h", "j", "k", "l", LV_SYMBOL_NEW_LINE, "\n",
    "_", "-", "z", "x", "c", "v", "b", "n", "m", ".", ",", ":", "\n",
    LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT, "Space", LV_SYMBOL_RIGHT, "OK", ""
};

static const lv_btnmatrix_ctrl_t s_wifi_keyboard_ctrl_lc_map[] = {
    LV_KEYBOARD_CTRL_BTN_FLAGS | 5, DISPLAY_KB_BTN(4), DISPLAY_KB_BTN(4), DISPLAY_KB_BTN(4),
    DISPLAY_KB_BTN(4), DISPLAY_KB_BTN(4), DISPLAY_KB_BTN(4), DISPLAY_KB_BTN(4),
    DISPLAY_KB_BTN(4), DISPLAY_KB_BTN(4), DISPLAY_KB_BTN(4), LV_BTNMATRIX_CTRL_CHECKED | 7,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 6, DISPLAY_KB_BTN(3), DISPLAY_KB_BTN(3), DISPLAY_KB_BTN(3),
    DISPLAY_KB_BTN(3), DISPLAY_KB_BTN(3), DISPLAY_KB_BTN(3), DISPLAY_KB_BTN(3),
    DISPLAY_KB_BTN(3), DISPLAY_KB_BTN(3), LV_BTNMATRIX_CTRL_CHECKED | 7,
    DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1),
    DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1),
    DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1),
    DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1),
    DISPLAY_KB_BTN(1),
    LV_KEYBOARD_CTRL_BTN_FLAGS | 3, 2, 4, 2,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 5,
};

static const char * const s_wifi_keyboard_map_uc[] = {
    "1#", "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", LV_SYMBOL_BACKSPACE, "\n",
    "abc", "A", "S", "D", "F", "G", "H", "J", "K", "L", LV_SYMBOL_NEW_LINE, "\n",
    "_", "-", "Z", "X", "C", "V", "B", "N", "M", ".", ",", ":", "\n",
    LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT, "Space", LV_SYMBOL_RIGHT, "OK", ""
};

static const lv_btnmatrix_ctrl_t s_wifi_keyboard_ctrl_uc_map[] = {
    LV_KEYBOARD_CTRL_BTN_FLAGS | 5, DISPLAY_KB_BTN(4), DISPLAY_KB_BTN(4), DISPLAY_KB_BTN(4),
    DISPLAY_KB_BTN(4), DISPLAY_KB_BTN(4), DISPLAY_KB_BTN(4), DISPLAY_KB_BTN(4),
    DISPLAY_KB_BTN(4), DISPLAY_KB_BTN(4), DISPLAY_KB_BTN(4), LV_BTNMATRIX_CTRL_CHECKED | 7,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 6, DISPLAY_KB_BTN(3), DISPLAY_KB_BTN(3), DISPLAY_KB_BTN(3),
    DISPLAY_KB_BTN(3), DISPLAY_KB_BTN(3), DISPLAY_KB_BTN(3), DISPLAY_KB_BTN(3),
    DISPLAY_KB_BTN(3), DISPLAY_KB_BTN(3), LV_BTNMATRIX_CTRL_CHECKED | 7,
    DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1),
    DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1),
    DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1),
    DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1),
    DISPLAY_KB_BTN(1),
    LV_KEYBOARD_CTRL_BTN_FLAGS | 3, 2, 4, 2,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 5,
};

static const char * const s_wifi_keyboard_map_spec[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    "abc", "+", "&", "/", "*", "=", "%", "!", "?", "#", "<", ">", "\n",
    "\\", "@", "$", "(", ")", "{", "}", "[", "]", ";", "\"", "'", "\n",
    LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT, "Space", LV_SYMBOL_RIGHT, "OK", ""
};

static const lv_btnmatrix_ctrl_t s_wifi_keyboard_ctrl_spec_map[] = {
    DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1),
    DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1),
    DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), LV_BTNMATRIX_CTRL_CHECKED | 2,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 2, DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1),
    DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1),
    DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1),
    DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1),
    DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1),
    DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1), DISPLAY_KB_BTN(1),
    LV_KEYBOARD_CTRL_BTN_FLAGS | 3, 2, 4, 2,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 5,
};

static const char * const s_uuid_keyboard_map[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", "\n",
    "Clear", "Z", "X", "C", "V", "B", "N", "M", ""
};

static EXT_RAM_BSS_ATTR display_call_contact_t s_call_contacts[DISPLAY_CALL_CONTACT_COUNT];
static EXT_RAM_BSS_ATTR display_call_pending_contact_t
    s_call_pending_contacts[DISPLAY_CALL_CONTACT_COUNT];
static uint8_t s_call_contact_count;
static uint8_t s_call_pending_contact_count;
static EXT_RAM_BSS_ATTR char s_call_add_device_id[DISPLAY_CALL_CONTACT_DEVICE_ID_MAX];
static EXT_RAM_BSS_ATTR char s_wechat_add_open_id[DISPLAY_WECHAT_OPEN_ID_MAX];

static void display_set_wifi_keyboard_mode(lv_keyboard_mode_t mode)
{
    if (s_keyboard == NULL) {
        return;
    }

    switch (mode) {
    case LV_KEYBOARD_MODE_USER_2:
        lv_btnmatrix_set_map(s_keyboard, (const char **)s_wifi_keyboard_map_uc);
        lv_btnmatrix_set_ctrl_map(s_keyboard, s_wifi_keyboard_ctrl_uc_map);
        break;
    case LV_KEYBOARD_MODE_USER_3:
        lv_btnmatrix_set_map(s_keyboard, (const char **)s_wifi_keyboard_map_spec);
        lv_btnmatrix_set_ctrl_map(s_keyboard, s_wifi_keyboard_ctrl_spec_map);
        break;
    case LV_KEYBOARD_MODE_USER_1:
    default:
        lv_btnmatrix_set_map(s_keyboard, (const char **)s_wifi_keyboard_map_lc);
        lv_btnmatrix_set_ctrl_map(s_keyboard, s_wifi_keyboard_ctrl_lc_map);
        break;
    }
}

static void display_write_u16_le(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);
    dst[1] = (uint8_t)((value >> 8) & 0xFFU);
}

static void display_write_u32_le(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);
    dst[1] = (uint8_t)((value >> 8) & 0xFFU);
    dst[2] = (uint8_t)((value >> 16) & 0xFFU);
    dst[3] = (uint8_t)((value >> 24) & 0xFFU);
}

/* Contact-sharing QR codes are public identity only. Never include credentials. */
static bool display_build_device_id_qr_payload(char *payload,
                                                size_t payload_size,
                                                const display_status_t *status)
{
    const char *device_id = APP_CONFIG_RTC_DEVICE_ID;

    if (payload == NULL || payload_size == 0) {
        return false;
    }
    if (status != NULL && status->tirtc_device_id[0] != '\0') {
        device_id = status->tirtc_device_id;
    }
    if (device_id == NULL || device_id[0] == '\0' || strlen(device_id) >= payload_size) {
        payload[0] = '\0';
        return false;
    }

    strlcpy(payload, device_id, payload_size);
    return true;
}

static esp_err_t display_prepare_call_scan_preview(void)
{
    if (s_call_scan_preview_converter != NULL &&
        s_call_scan_preview_buffers[0] != NULL &&
        s_call_scan_preview_buffers[1] != NULL) {
        return ESP_OK;
    }

    const video_frame_converter_config_t converter_config = {
        .output_width = DISPLAY_NATIVE_WIDTH,
        .output_height = DISPLAY_NATIVE_HEIGHT,
        .fit_mode = VIDEO_FRAME_FIT_CONTAIN,
        .prevent_upscale = false,
        .source_crop_width = 800,
        .source_crop_height = 640,
        .output_rgb565_byte_swap = true,
    };
    esp_err_t ret = video_frame_converter_create(&converter_config,
                                                  &s_call_scan_preview_converter);
    if (ret != ESP_OK) {
        return ret;
    }

    const size_t pixel_count = (size_t)DISPLAY_NATIVE_WIDTH * DISPLAY_NATIVE_HEIGHT;
    for (size_t index = 0; index < DISPLAY_SCAN_PREVIEW_BUFFER_COUNT; ++index) {
        s_call_scan_preview_buffers[index] = app_memory_aligned_calloc_psram(
            DISPLAY_SCAN_PREVIEW_ALIGNMENT,
            pixel_count,
            sizeof(uint16_t),
            MALLOC_CAP_DMA | MALLOC_CAP_CACHE_ALIGNED);
        if (s_call_scan_preview_buffers[index] == NULL) {
            for (size_t cleanup = 0;
                 cleanup < DISPLAY_SCAN_PREVIEW_BUFFER_COUNT;
                 ++cleanup) {
                free(s_call_scan_preview_buffers[cleanup]);
                s_call_scan_preview_buffers[cleanup] = NULL;
            }
            video_frame_converter_destroy(s_call_scan_preview_converter);
            s_call_scan_preview_converter = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(TAG,
             "scan preview ready: buffers=%ux%u count=%u psram=%u",
             (unsigned)DISPLAY_NATIVE_WIDTH,
             (unsigned)DISPLAY_NATIVE_HEIGHT,
             (unsigned)DISPLAY_SCAN_PREVIEW_BUFFER_COUNT,
             (unsigned)(pixel_count * sizeof(uint16_t) *
                        DISPLAY_SCAN_PREVIEW_BUFFER_COUNT));
    return ESP_OK;
}

/*
 * Scanner preview buffers belong to the scanner page, not to the display
 * lifetime. Stop normally drains the scanner worker first; the preview mutex
 * also closes the cross-core window between an in-flight PPA conversion and a
 * page/lifecycle teardown. Keep the LVGL descriptors static, but detach their
 * image data before returning the frame-sized PSRAM to the heap.
 */
static void display_release_call_scan_preview(void)
{
    if (s_call_scan_image != NULL &&
        !lv_obj_has_flag(s_call_scan_image, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(s_call_scan_image, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_call_scan_preview_mutex == NULL ||
        xSemaphoreTake(s_call_scan_preview_mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "scan preview teardown lock unavailable");
        return;
    }

    for (size_t index = 0; index < DISPLAY_SCAN_PREVIEW_BUFFER_COUNT; ++index) {
        if (s_call_scan_preview_dsc[index].data != NULL) {
            lv_img_cache_invalidate_src(&s_call_scan_preview_dsc[index]);
        }
        memset(&s_call_scan_preview_dsc[index],
               0,
               sizeof(s_call_scan_preview_dsc[index]));
        heap_caps_free(s_call_scan_preview_buffers[index]);
        s_call_scan_preview_buffers[index] = NULL;
    }

    video_frame_converter_destroy(s_call_scan_preview_converter);
    s_call_scan_preview_converter = NULL;
    s_call_scan_preview_index = 0U;
    s_call_scan_preview_first_frame_logged = false;
    xSemaphoreGive(s_call_scan_preview_mutex);
}

static esp_err_t display_convert_call_scan_preview(const scan_preview_frame_t *frame,
                                                   uint16_t *output,
                                                   video_frame_converter_mode_t *mode)
{
    if (frame == NULL || frame->data == NULL || output == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (frame->pixel_format) {
    case SCAN_PREVIEW_PIXEL_FORMAT_YUV420_OUYY_EVYY:
        return video_frame_converter_ouyy_evyy_to_rgb565(
            s_call_scan_preview_converter,
            frame->data,
            frame->width,
            frame->height,
            output,
            mode);
    case SCAN_PREVIEW_PIXEL_FORMAT_RGB565:
        return video_frame_converter_rgb565_to_rgb565(
            s_call_scan_preview_converter,
            (const uint16_t *)frame->data,
            frame->width,
            frame->height,
            frame->width,
            false,
            VIDEO_FRAME_ROTATION_CLOCKWISE_0,
            output,
            mode);
    case SCAN_PREVIEW_PIXEL_FORMAT_GRAYSCALE:
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

static void display_call_scan_preview_cb(const scan_preview_frame_t *frame, void *ctx)
{
    static uint32_t conversion_failures;
    uint8_t next_index = 0;
    uint16_t *output = NULL;
    video_frame_converter_mode_t mode = VIDEO_FRAME_CONVERTER_MODE_SOFTWARE;

    (void)ctx;
    if (frame == NULL || frame->data == NULL || frame->width == 0U ||
        frame->height == 0U || !s_display_initialized ||
        s_call_scan_preview_mutex == NULL) {
        return;
    }
    if (xSemaphoreTake(s_call_scan_preview_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (!s_call_scan_active) {
        xSemaphoreGive(s_call_scan_preview_mutex);
        return;
    }

    esp_err_t ret = display_prepare_call_scan_preview();
    if (ret == ESP_OK) {
        next_index = (uint8_t)((s_call_scan_preview_index + 1U) %
                               DISPLAY_SCAN_PREVIEW_BUFFER_COUNT);
        output = s_call_scan_preview_buffers[next_index];
        ret = display_convert_call_scan_preview(frame, output, &mode);
    }

    if (ret != ESP_OK) {
        conversion_failures++;
        if (conversion_failures == 1U || (conversion_failures % 100U) == 0U) {
            ESP_LOGW(TAG,
                     "scan preview conversion failed: ret=%s count=%lu",
                     esp_err_to_name(ret),
                     (unsigned long)conversion_failures);
        }
        xSemaphoreGive(s_call_scan_preview_mutex);
        return;
    }

    /*
     * PPA conversion is intentionally outside the LVGL critical section. The
     * inactive buffer is safe to fill while LVGL scans out the current one;
     * the lock only protects the descriptor/source swap.
     */
    if (!lvgl_port_lock(100)) {
        xSemaphoreGive(s_call_scan_preview_mutex);
        return;
    }
    if (!s_call_scan_active || !display_page_is_visible(s_call_scan_page) ||
        s_call_scan_image == NULL) {
        lvgl_port_unlock();
        xSemaphoreGive(s_call_scan_preview_mutex);
        return;
    }

    lv_img_dsc_t *frame_dsc = &s_call_scan_preview_dsc[next_index];
    lv_img_cache_invalidate_src(frame_dsc);
    memset(frame_dsc, 0, sizeof(*frame_dsc));
    frame_dsc->header.always_zero = 0;
    frame_dsc->header.w = DISPLAY_NATIVE_WIDTH;
    frame_dsc->header.h = DISPLAY_NATIVE_HEIGHT;
    frame_dsc->header.cf = LV_IMG_CF_TRUE_COLOR;
    frame_dsc->data_size =
        (uint32_t)((size_t)DISPLAY_NATIVE_WIDTH * DISPLAY_NATIVE_HEIGHT *
                   sizeof(uint16_t));
    frame_dsc->data = (const uint8_t *)output;
    lv_img_set_src(s_call_scan_image, frame_dsc);
    lv_obj_clear_flag(s_call_scan_image, LV_OBJ_FLAG_HIDDEN);
    s_call_scan_preview_index = next_index;
    if (!s_call_scan_preview_first_frame_logged) {
        s_call_scan_preview_first_frame_logged = true;
        ESP_LOGI(TAG,
                 "scan preview first frame: source=%ux%u format=%u mode=%s",
                 (unsigned)frame->width,
                 (unsigned)frame->height,
                 (unsigned)frame->pixel_format,
                 video_frame_converter_mode_name(mode));
    }
    lvgl_port_unlock();
    xSemaphoreGive(s_call_scan_preview_mutex);
}

static bool display_text_has_visible_char(const char *text)
{
    if (text == NULL) {
        return false;
    }
    while (*text != '\0') {
        if ((uint8_t)*text > (uint8_t)' ') {
            return true;
        }
        ++text;
    }
    return false;
}

static void display_copy_trimmed_text(char *dst, size_t dst_len, const char *src)
{
    const char *begin = src;
    const char *end = NULL;
    size_t copy_len = 0;

    if (dst == NULL || dst_len == 0) {
        return;
    }
    dst[0] = '\0';
    if (src == NULL) {
        return;
    }

    while (*begin != '\0' && (uint8_t)*begin <= (uint8_t)' ') {
        ++begin;
    }

    end = begin + strlen(begin);
    while (end > begin && (uint8_t)*(end - 1) <= (uint8_t)' ') {
        --end;
    }

    copy_len = (size_t)(end - begin);
    if (copy_len >= dst_len) {
        copy_len = dst_len - 1U;
    }
    memcpy(dst, begin, copy_len);
    dst[copy_len] = '\0';
}

static void display_reset_call_add_inputs(void)
{
    s_call_add_device_id[0] = '\0';
    display_update_call_add_field_labels();
}

static bool display_wechat_open_id_char_valid(char ch)
{
    return (ch >= '0' && ch <= '9') ||
           (ch >= 'A' && ch <= 'Z') ||
           (ch >= 'a' && ch <= 'z') ||
           ch == '_' ||
           ch == '-';
}

static bool display_wechat_open_id_valid(const char *open_id)
{
    if (open_id == NULL || strlen(open_id) != DISPLAY_WECHAT_OPEN_ID_LENGTH) {
        return false;
    }

    for (size_t index = 0; index < DISPLAY_WECHAT_OPEN_ID_LENGTH; ++index) {
        if (!display_wechat_open_id_char_valid(open_id[index])) {
            return false;
        }
    }
    return true;
}

static void display_reset_wechat_add_input(void)
{
    s_wechat_add_open_id[0] = '\0';
    display_update_wechat_add_field_label();
}

static void display_invalidate_call_list_page(void)
{
    if (s_call_list_page != NULL) {
        lv_obj_add_flag(s_call_list_page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_del_async(s_call_list_page);
        s_call_list_page = NULL;
    }
}

static bool display_call_contacts_match_status(const display_status_t *status)
{
    if (status == NULL) {
        return s_call_contact_count == 0 && s_call_pending_contact_count == 0;
    }

    uint8_t count = status->call_contact_count > DISPLAY_CALL_CONTACT_COUNT ?
        DISPLAY_CALL_CONTACT_COUNT : status->call_contact_count;
    uint8_t pending_count = status->call_pending_contact_count > DISPLAY_CALL_CONTACT_COUNT ?
        DISPLAY_CALL_CONTACT_COUNT : status->call_pending_contact_count;
    if (s_call_contact_count != count || s_call_pending_contact_count != pending_count) {
        return false;
    }
    for (uint8_t index = 0; index < count; ++index) {
        if (strcmp(s_call_contacts[index].device_id, status->call_contacts[index].device_id) != 0 ||
            strcmp(s_call_contacts[index].remark, status->call_contacts[index].remark) != 0 ||
            s_call_contacts[index].online != status->call_contacts[index].online ||
            s_call_contacts[index].deletable != status->call_contacts[index].deletable) {
            return false;
        }
    }
    for (uint8_t index = 0; index < pending_count; ++index) {
        if (strcmp(s_call_pending_contacts[index].device_id,
                   status->call_pending_contacts[index].device_id) != 0 ||
            strcmp(s_call_pending_contacts[index].created_at,
                   status->call_pending_contacts[index].created_at) != 0) {
            return false;
        }
    }
    return true;
}

static bool display_sync_call_contacts_from_status(const display_status_t *status)
{
    bool pending_changed = false;

    if (status == NULL) {
        pending_changed = s_call_pending_contact_count != 0U;
    } else {
        uint8_t pending_count = status->call_pending_contact_count > DISPLAY_CALL_CONTACT_COUNT ?
            DISPLAY_CALL_CONTACT_COUNT : status->call_pending_contact_count;
        pending_changed = s_call_pending_contact_count != pending_count;
        for (uint8_t index = 0; !pending_changed && index < pending_count; ++index) {
            pending_changed = strcmp(s_call_pending_contacts[index].device_id,
                                     status->call_pending_contacts[index].device_id) != 0 ||
                              strcmp(s_call_pending_contacts[index].created_at,
                                     status->call_pending_contacts[index].created_at) != 0;
        }
    }

    if (display_call_contacts_match_status(status)) {
        return false;
    }

    memset(s_call_contacts, 0, sizeof(s_call_contacts));
    memset(s_call_pending_contacts, 0, sizeof(s_call_pending_contacts));
    s_call_contact_count = 0;
    s_call_pending_contact_count = 0;
    if (status != NULL) {
        uint8_t count = status->call_contact_count > DISPLAY_CALL_CONTACT_COUNT ?
            DISPLAY_CALL_CONTACT_COUNT : status->call_contact_count;
        s_call_contact_count = count;
        for (uint8_t index = 0; index < count; ++index) {
            s_call_contacts[index] = status->call_contacts[index];
        }
        uint8_t pending_count = status->call_pending_contact_count > DISPLAY_CALL_CONTACT_COUNT ?
            DISPLAY_CALL_CONTACT_COUNT : status->call_pending_contact_count;
        s_call_pending_contact_count = pending_count;
        for (uint8_t index = 0; index < pending_count; ++index) {
            s_call_pending_contacts[index] = status->call_pending_contacts[index];
        }
    }
    if (pending_changed) {
        s_call_contact_request_dismissed = false;
    }
    return true;
}

static const char *display_call_add_field_title(display_call_add_field_t field)
{
    (void)field;
    return "Device ID";
}

static size_t display_call_add_field_max_len(display_call_add_field_t field)
{
    (void)field;
    return DISPLAY_CALL_CONTACT_DEVICE_ID_LENGTH;
}

static char *display_call_add_field_buffer(display_call_add_field_t field)
{
    (void)field;
    return s_call_add_device_id;
}

static const char *display_call_add_field_placeholder(display_call_add_field_t field)
{
    (void)field;
    return "请输入 12 位 Device ID";
}

static void display_update_call_add_field_labels(void)
{
    for (uint8_t index = 0; index < DISPLAY_CALL_ADD_FIELD_COUNT; ++index) {
        display_call_add_field_t field = (display_call_add_field_t)index;
        lv_obj_t *label = s_call_add_value_labels[index];
        const char *value = display_call_add_field_buffer(field);

        if (label == NULL) {
            continue;
        }

        if (value[0] != '\0') {
            display_text_set_color(label, lv_color_hex(0x10233B), 0);
            display_text_set(label, value);
        } else {
            display_text_set_color(label, lv_color_hex(0x8AA0B5), 0);
            display_text_set(label, display_call_add_field_placeholder(field));
        }
    }
}

static void display_update_wechat_add_field_label(void)
{
    if (s_wechat_add_open_id_label == NULL) {
        return;
    }

    if (s_wechat_add_open_id[0] != '\0') {
        display_text_set_color(s_wechat_add_open_id_label, lv_color_hex(0x10233B), 0);
        display_text_set(s_wechat_add_open_id_label, s_wechat_add_open_id);
    } else {
        display_text_set_color(s_wechat_add_open_id_label, lv_color_hex(0x8AA0B5), 0);
        display_text_set(s_wechat_add_open_id_label, "28位微信Open ID");
    }
}

static void display_store_scanned_wechat_contact(const char *open_id)
{
    display_wechat_contact_t contact = {0};
    uint8_t count = s_last_status.wechat_contact_count > DISPLAY_WECHAT_CONTACT_COUNT ?
        DISPLAY_WECHAT_CONTACT_COUNT : s_last_status.wechat_contact_count;
    uint8_t existing_index = DISPLAY_WECHAT_CONTACT_COUNT;

    if (open_id == NULL || open_id[0] == '\0') {
        return;
    }

    for (uint8_t index = 0; index < count; ++index) {
        if (strcmp(s_last_status.wechat_contacts[index].open_id, open_id) == 0) {
            existing_index = index;
            break;
        }
    }

    if (existing_index < count) {
        for (uint8_t index = existing_index; index > 0; --index) {
            s_last_status.wechat_contacts[index] = s_last_status.wechat_contacts[index - 1U];
        }
    } else if (count < DISPLAY_WECHAT_CONTACT_COUNT) {
        ++count;
        for (uint8_t index = count - 1U; index > 0; --index) {
            s_last_status.wechat_contacts[index] = s_last_status.wechat_contacts[index - 1U];
        }
    } else {
        for (uint8_t index = DISPLAY_WECHAT_CONTACT_COUNT - 1U; index > 0; --index) {
            s_last_status.wechat_contacts[index] = s_last_status.wechat_contacts[index - 1U];
        }
        count = DISPLAY_WECHAT_CONTACT_COUNT;
    }

    strlcpy(contact.open_id, open_id, sizeof(contact.open_id));
    s_last_status.wechat_contacts[0] = contact;
    s_last_status.wechat_contact_count = count;
}

static const char *display_contact_scan_error_text(esp_err_t result)
{
    switch (result) {
    case ESP_ERR_NOT_SUPPORTED:
        return "摄像头不可用";
    case ESP_ERR_INVALID_STATE:
        return "扫码服务忙";
    case ESP_ERR_NOT_ALLOWED:
        return "请先在微信小程序完成授权";
    case ESP_ERR_NOT_FOUND:
        return "未识别二维码";
    case ESP_ERR_INVALID_RESPONSE:
        return "二维码格式错误";
    case ESP_ERR_TIMEOUT:
        return "摄像头超时";
    case ESP_ERR_NO_MEM:
        return "内存不足";
    default:
        return "扫码失败";
    }
}

static void display_call_scan_result_async_cb(void *arg)
{
    display_contact_scan_result_event_t *event = (display_contact_scan_result_event_t *)arg;

    if (event == NULL) {
        return;
    }

    s_call_scan_active = false;
    display_release_call_scan_preview();
    if (event->result == ESP_OK) {
        esp_err_t ret = s_actions.on_add_call_contact != NULL ?
            s_actions.on_add_call_contact(event->device_id, s_actions.ctx) :
            ESP_ERR_INVALID_STATE;
        if (ret != ESP_OK) {
            display_show_call_add_page();
            display_show_wifi_alert("扫码添加",
                                    ret == ESP_ERR_INVALID_STATE ?
                                    "联系人服务忙，请稍后再试" : "申请提交失败");
            free(event);
            return;
        }
        display_invalidate_call_list_page();
        display_show_call_list_page();
        display_show_wifi_alert("扫码添加", "申请已提交");
    } else {
        display_show_call_add_page();
        if (event->result == ESP_ERR_INVALID_RESPONSE && event->raw_payload[0] != '\0') {
            display_show_wifi_alert("二维码内容", event->raw_payload);
        } else {
            display_show_wifi_alert("扫码添加", display_contact_scan_error_text(event->result));
        }
    }

    free(event);
}

static void display_call_scan_result_cb(esp_err_t result,
                                        const char *device_id,
                                        const char *raw_payload,
                                        void *ctx)
{
    display_contact_scan_result_event_t *event = NULL;

    (void)ctx;

    event = display_calloc_psram(1, sizeof(*event));
    if (event == NULL) {
        return;
    }

    event->result = result;
    if (device_id != NULL) {
        strlcpy(event->device_id, device_id, sizeof(event->device_id));
    }
    if (raw_payload != NULL) {
        strlcpy(event->raw_payload, raw_payload, sizeof(event->raw_payload));
    }

    if (!lvgl_port_lock(100)) {
        free(event);
        return;
    }
    lv_res_t async_ret = lv_async_call(display_call_scan_result_async_cb, event);
    lvgl_port_unlock();
    if (async_ret != LV_RES_OK) {
        free(event);
    }
}

static void display_tirtc_config_scan_result_async_cb(void *arg)
{
    display_contact_scan_result_event_t *event = (display_contact_scan_result_event_t *)arg;

    if (event == NULL) {
        return;
    }

    s_call_scan_active = false;
    display_release_call_scan_preview();
    if (event->result == ESP_OK) {
        strlcpy(s_last_status.tirtc_device_id, event->device_id, sizeof(s_last_status.tirtc_device_id));
        strlcpy(s_last_status.tirtc_device_secret,
                event->credential,
                sizeof(s_last_status.tirtc_device_secret));
        display_show_tirtc_config_page();
        display_show_wifi_alert("TiRTC 配置", "扫描成功");
    } else {
        display_show_tirtc_config_page();
        if (event->result == ESP_ERR_INVALID_RESPONSE && event->raw_payload[0] != '\0') {
            display_show_wifi_alert("二维码内容", event->raw_payload);
        } else {
            display_show_wifi_alert("TiRTC 配置", display_contact_scan_error_text(event->result));
        }
    }

    free(event);
}

static bool display_queue_tirtc_config_scan_result(display_contact_scan_result_event_t *event,
                                                   bool log_failure)
{
    if (event == NULL) {
        return true;
    }

    if (!lvgl_port_lock(1)) {
        if (log_failure) {
            ESP_LOGW(TAG,
                     "tirtc config scan result dispatch waits for lvgl lock: result=%s",
                     esp_err_to_name(event->result));
        }
        return false;
    }

    lv_res_t async_ret = lv_async_call(display_tirtc_config_scan_result_async_cb, event);
    lvgl_port_unlock();
    if (async_ret != LV_RES_OK) {
        if (log_failure) {
            ESP_LOGW(TAG,
                     "tirtc config scan result dispatch failed: async result=%d",
                     (int)async_ret);
        }
        return false;
    }
    return true;
}

static void display_tirtc_config_scan_result_dispatch_task(void *arg)
{
    display_contact_scan_result_event_t *event = (display_contact_scan_result_event_t *)arg;

    for (uint8_t attempt = 0; attempt < DISPLAY_SCAN_RESULT_DISPATCH_RETRY_COUNT; ++attempt) {
        if (display_queue_tirtc_config_scan_result(event, attempt == 0)) {
            vTaskDeleteWithCaps(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(DISPLAY_SCAN_RESULT_DISPATCH_RETRY_MS));
    }

    ESP_LOGW(TAG, "tirtc config scan result dispatch abandoned after retries");
    free(event);
    vTaskDeleteWithCaps(NULL);
}

static void __attribute__((unused)) display_tirtc_config_scan_result_cb(esp_err_t result,
                                                const char *device_id,
                                                const char *device_secret,
                                                const char *raw_payload,
                                                void *ctx)
{
    display_contact_scan_result_event_t *event = NULL;

    (void)ctx;

    event = display_calloc_psram(1, sizeof(*event));
    if (event == NULL) {
        return;
    }

    event->result = result;
    if (device_id != NULL) {
        strlcpy(event->device_id, device_id, sizeof(event->device_id));
    }
    if (device_secret != NULL) {
        strlcpy(event->credential, device_secret, sizeof(event->credential));
    }
    if (raw_payload != NULL) {
        strlcpy(event->raw_payload, raw_payload, sizeof(event->raw_payload));
    }

    if (!display_queue_tirtc_config_scan_result(event, false)) {
        BaseType_t task_ret = xTaskCreatePinnedToCoreWithCaps(display_tirtc_config_scan_result_dispatch_task,
                                                              "tirtc_ui_evt",
                                                              DISPLAY_SCAN_RESULT_DISPATCH_TASK_STACK,
                                                              event,
                                                              DISPLAY_SCAN_RESULT_DISPATCH_TASK_PRIORITY,
                                                              NULL,
                                                              APP_TASK_CORE_BACKGROUND,
                                                              DISPLAY_SCAN_RESULT_DISPATCH_TASK_ALLOC_CAPS);
        if (task_ret != pdPASS) {
            ESP_LOGW(TAG, "tirtc config scan result dispatch task create failed");
            free(event);
        }
    }
}

static void display_wechat_scan_result_async_cb(void *arg)
{
    display_contact_scan_result_event_t *event = (display_contact_scan_result_event_t *)arg;

    if (event == NULL) {
        return;
    }

    s_call_scan_active = false;
    display_release_call_scan_preview();
    if (event->result == ESP_OK) {
        display_store_scanned_wechat_contact(event->open_id);
        display_reset_wechat_add_input();
        display_show_wechat_list_page();
        display_show_wifi_alert("扫码添加", "扫描成功");
    } else {
        display_show_wechat_add_page();
        if (event->result == ESP_ERR_INVALID_RESPONSE && event->raw_payload[0] != '\0') {
            display_show_wifi_alert("二维码内容", event->raw_payload);
        } else {
            display_show_wifi_alert("扫码添加", display_contact_scan_error_text(event->result));
        }
    }

    free(event);
}

static void display_wechat_scan_result_cb(esp_err_t result,
                                          const char *open_id,
                                          const char *raw_payload,
                                          void *ctx)
{
    display_contact_scan_result_event_t *event = NULL;

    (void)ctx;

    event = display_calloc_psram(1, sizeof(*event));
    if (event == NULL) {
        return;
    }

    event->result = result;
    if (open_id != NULL) {
        strlcpy(event->open_id, open_id, sizeof(event->open_id));
    }
    if (raw_payload != NULL) {
        strlcpy(event->raw_payload, raw_payload, sizeof(event->raw_payload));
    }

    if (!lvgl_port_lock(100)) {
        free(event);
        return;
    }
    lv_res_t async_ret = lv_async_call(display_wechat_scan_result_async_cb, event);
    lvgl_port_unlock();
    if (async_ret != LV_RES_OK) {
        free(event);
    }
}

static void display_set_main_hint(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(s_main_hint_text, sizeof(s_main_hint_text), fmt, args);
    va_end(args);

    if (s_main_hint_label != NULL) {
        display_text_set(s_main_hint_label, s_main_hint_text);
        if (s_main_hint_text[0] != '\0' && strcmp(s_main_hint_text, "Ready") != 0) {
            lv_obj_clear_flag(s_main_hint_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_main_hint_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void display_hide_keyboard(void)
{
    if (s_keyboard != NULL) {
        lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_tirtc_edit_keyboard != NULL) {
        lv_keyboard_set_textarea(s_tirtc_edit_keyboard, NULL);
        lv_obj_add_flag(s_tirtc_edit_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_call_add_edit_keyboard != NULL) {
        lv_keyboard_set_textarea(s_call_add_edit_keyboard, NULL);
        lv_obj_add_flag(s_call_add_edit_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_call_remark_keyboard != NULL) {
        lv_keyboard_set_textarea(s_call_remark_keyboard, NULL);
        lv_obj_add_flag(s_call_remark_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_wechat_add_edit_keyboard != NULL) {
        lv_keyboard_set_textarea(s_wechat_add_edit_keyboard, NULL);
        lv_obj_add_flag(s_wechat_add_edit_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_wechat_remark_keyboard != NULL) {
        lv_keyboard_set_textarea(s_wechat_remark_keyboard, NULL);
        lv_obj_add_flag(s_wechat_remark_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_password_ta != NULL) {
        lv_obj_clear_state(s_password_ta, LV_STATE_FOCUSED);
    }
    if (s_tirtc_edit_ta != NULL) {
        lv_obj_clear_state(s_tirtc_edit_ta, LV_STATE_FOCUSED);
    }
    if (s_call_add_edit_ta != NULL) {
        lv_obj_clear_state(s_call_add_edit_ta, LV_STATE_FOCUSED);
    }
    if (s_call_remark_ta != NULL) {
        lv_obj_clear_state(s_call_remark_ta, LV_STATE_FOCUSED);
    }
    if (s_wechat_add_edit_ta != NULL) {
        lv_obj_clear_state(s_wechat_add_edit_ta, LV_STATE_FOCUSED);
    }
    if (s_wechat_remark_ta != NULL) {
        lv_obj_clear_state(s_wechat_remark_ta, LV_STATE_FOCUSED);
    }
}

static void display_show_text_keyboard(lv_obj_t *keyboard, lv_obj_t *textarea)
{
    if (keyboard == NULL || textarea == NULL) {
        return;
    }

    lv_keyboard_set_textarea(keyboard, textarea);
    lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_USER_1);
    lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(keyboard);
    lv_obj_add_state(textarea, LV_STATE_FOCUSED);
    lv_obj_invalidate(textarea);
    lv_obj_invalidate(keyboard);
}

static void display_layout_wifi_keyboard(void)
{
    if (s_keyboard == NULL) {
        return;
    }

    display_obj_set_design_pos(s_keyboard, DISPLAY_WIFI_KEYBOARD_LEFT, DISPLAY_WIFI_KEYBOARD_TOP);
    display_obj_set_design_size(s_keyboard, DISPLAY_WIFI_KEYBOARD_WIDTH, DISPLAY_WIFI_KEYBOARD_HEIGHT);
    lv_obj_set_style_pad_all(s_keyboard, 0, 0);
    lv_obj_set_style_pad_row(s_keyboard, 4, 0);
    lv_obj_set_style_pad_column(s_keyboard, 3, 0);
    lv_obj_set_style_pad_all(s_keyboard, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(s_keyboard, 6, LV_PART_ITEMS);
}

static void display_keyboard_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        display_hide_keyboard();
    }
}

static void display_wifi_alert_event_cb(lv_event_t *event)
{
    lv_obj_t *target = lv_event_get_target(event);
    lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_DELETE && target == s_wifi_alert_box) {
        s_wifi_alert_box = NULL;
    }
}

static void display_wifi_alert_ok_btn_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || s_wifi_alert_box == NULL) {
        return;
    }

    display_hide_wifi_alert();
}

static void display_hide_wifi_alert(void)
{
    if (s_wifi_alert_box != NULL) {
        lv_obj_del_async(s_wifi_alert_box);
        s_wifi_alert_box = NULL;
    }
}

static void display_call_alert_event_cb(lv_event_t *event)
{
    lv_obj_t *target = lv_event_get_target(event);
    lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_DELETE && target == s_call_alert_box) {
        s_call_alert_box = NULL;
        s_call_alert_wechat = false;
    }
}

static void display_call_alert_accept_btn_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    bool wechat = s_call_alert_wechat;
    if (wechat) {
        if (s_actions.on_wechat_accept_call != NULL) {
            esp_err_t ret = s_actions.on_wechat_accept_call(s_actions.ctx);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "accept wechat call failed: %s", esp_err_to_name(ret));
            } else {
                s_wechat_active_started_us = 0;
                display_show_wechat_active_page();
            }
        }
        display_hide_call_alert();
        return;
    }

    if (s_actions.on_accept_call != NULL) {
        esp_err_t ret = s_actions.on_accept_call(s_actions.ctx);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "accept call failed: %s", esp_err_to_name(ret));
        } else {
            s_call_active_started_us = 0;
            display_show_call_active_page();
        }
    }
    display_hide_call_alert();
}

static void display_call_alert_reject_btn_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_PRESSED) {
        return;
    }

    bool wechat = s_call_alert_wechat;
    if (wechat) {
        if (s_actions.on_wechat_reject_call != NULL) {
            esp_err_t ret = s_actions.on_wechat_reject_call(s_actions.ctx);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "reject wechat call failed: %s", esp_err_to_name(ret));
            }
        }
        display_hide_call_alert();
        return;
    }

    if (s_actions.on_reject_call != NULL) {
        esp_err_t ret = s_actions.on_reject_call(s_actions.ctx);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "reject call failed: %s", esp_err_to_name(ret));
        }
    }
    display_hide_call_alert();
}

static void display_hide_call_alert(void)
{
    if (s_call_alert_box != NULL) {
        lv_obj_del(s_call_alert_box);
        s_call_alert_box = NULL;
    }
    s_call_alert_wechat = false;
}

static void display_call_hangup_confirm_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_DELETE &&
        lv_event_get_target(event) == s_call_hangup_confirm_box) {
        s_call_hangup_confirm_box = NULL;
    }
}

static void display_hide_call_hangup_confirm(void)
{
    if (s_call_hangup_confirm_box != NULL) {
        lv_obj_del(s_call_hangup_confirm_box);
        s_call_hangup_confirm_box = NULL;
    }
}

static void display_show_call_hangup_confirm(void)
{
    lv_obj_t *card = NULL;
    lv_obj_t *hangup_btn = NULL;

    if (s_call_hangup_confirm_box != NULL) {
        lv_obj_move_foreground(s_call_hangup_confirm_box);
        return;
    }

    s_call_hangup_confirm_box = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_call_hangup_confirm_box);
    lv_obj_set_pos(s_call_hangup_confirm_box, 0, 0);
    lv_obj_set_size(s_call_hangup_confirm_box, DISPLAY_DRIVER_WIDTH, DISPLAY_DRIVER_HEIGHT);
    lv_obj_set_style_bg_color(s_call_hangup_confirm_box, lv_color_hex(0x10233B), 0);
    lv_obj_set_style_bg_opa(s_call_hangup_confirm_box, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(s_call_hangup_confirm_box, 0, 0);
    lv_obj_clear_flag(s_call_hangup_confirm_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_call_hangup_confirm_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_call_hangup_confirm_box,
                        display_call_hangup_confirm_event_cb,
                        LV_EVENT_ALL,
                        NULL);
    lv_obj_move_foreground(s_call_hangup_confirm_box);

    card = display_create_native_box(s_call_hangup_confirm_box,
                                     70,
                                     80,
                                     340,
                                     160,
                                     lv_color_hex(DISPLAY_UI_COLOR_SURFACE),
                                     lv_color_hex(DISPLAY_UI_COLOR_BORDER),
                                     8);
    lv_obj_set_style_shadow_width(card, 16, 0);
    lv_obj_set_style_shadow_ofs_y(card, 5, 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(0x10233B), 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_20, 0);

    display_create_native_text(card,
                               "END CURRENT CALL?",
                               20,
                               24,
                               300,
                               lv_color_hex(DISPLAY_UI_COLOR_TEXT),
                               18,
                               LV_TEXT_ALIGN_CENTER);
    display_create_native_button(card,
                                 18,
                                 88,
                                 142,
                                 48,
                                 lv_color_hex(0xEDF5FB),
                                 lv_color_hex(DISPLAY_UI_COLOR_BORDER),
                                 "CANCEL",
                                 lv_color_hex(DISPLAY_UI_COLOR_TEXT),
                                 14,
                                 display_call_hangup_cancel_btn_cb);
    hangup_btn = display_create_native_button(card,
                                              180,
                                              88,
                                              142,
                                              48,
                                              lv_color_hex(0xFFE7E7),
                                              lv_color_hex(0xF15A5A),
                                              "挂断",
                                              lv_color_hex(0xE44747),
                                              16,
                                              NULL);
    lv_obj_add_event_cb(hangup_btn,
                        display_call_hangup_confirm_btn_cb,
                        LV_EVENT_PRESSED,
                        NULL);
}

static void display_show_call_alert(bool wechat)
{
    lv_obj_t *card = NULL;
    lv_obj_t *reject_btn = NULL;
    lv_obj_t *accept_btn = NULL;

    if (s_call_alert_box != NULL) {
        if (s_call_alert_wechat == wechat) {
            return;
        }
        display_hide_call_alert();
    }

    s_call_alert_wechat = wechat;
    /* An incoming alert is presentation only. Never switch application
     * ownership from the LVGL refresh timer: doing so rebuilds the call page
     * and changes media resources before the user accepts. The accept action
     * performs the CALL/WECHAT lifecycle transition exactly once. */

    s_call_alert_box = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_call_alert_box);
    lv_obj_set_pos(s_call_alert_box, 0, 0);
    lv_obj_set_size(s_call_alert_box, DISPLAY_DRIVER_WIDTH, DISPLAY_DRIVER_HEIGHT);
    lv_obj_set_style_bg_color(s_call_alert_box, lv_color_hex(0x10233B), 0);
    lv_obj_set_style_bg_opa(s_call_alert_box, LV_OPA_20, 0);
    lv_obj_set_style_border_width(s_call_alert_box, 0, 0);
    lv_obj_set_style_pad_all(s_call_alert_box, 0, 0);
    lv_obj_clear_flag(s_call_alert_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(s_call_alert_box);
    lv_obj_add_event_cb(s_call_alert_box, display_call_alert_event_cb, LV_EVENT_ALL, NULL);

    card = display_create_native_box(s_call_alert_box,
                                     70,
                                     64,
                                     340,
                                     192,
                                     lv_color_hex(0xFFFFFF),
                                     lv_color_hex(0xD6E4EF),
                                     9);
    lv_obj_set_style_shadow_width(card, 14, 0);
    lv_obj_set_style_shadow_ofs_y(card, 5, 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(0x10233B), 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_20, 0);

    (void)display_create_native_text(card,
                                     "来电",
                                     20,
                                     18,
                                     300,
                                     lv_color_hex(0x10233B),
                                     20,
                                     LV_TEXT_ALIGN_CENTER);
    display_create_native_text(card,
                               wechat ? "WECHAT CALL" :
                                        (s_last_status.call_type == DISPLAY_CALL_TYPE_VIDEO ?
                                         "VIDEO CALL" : "AUDIO CALL"),
                               20,
                               52,
                               300,
                               wechat ? lv_color_hex(0x1BAA70) : lv_color_hex(0x1879B9),
                               14,
                               LV_TEXT_ALIGN_CENTER);
    display_create_native_text(card,
                               wechat ? "WECHAT" :
                                        (s_last_status.call_peer_device_id[0] != '\0' ?
                                         s_last_status.call_peer_device_id : "UNKNOWN PEER"),
                               20,
                               78,
                               300,
                               lv_color_hex(DISPLAY_UI_COLOR_TEXT_MUTED),
                               12,
                               LV_TEXT_ALIGN_CENTER);
    reject_btn = display_create_native_button(card,
                                              18,
                                              122,
                                              142,
                                              48,
                                              lv_color_hex(0xFFE7E7),
                                              lv_color_hex(0xF15A5A),
                                              "挂断",
                                              lv_color_hex(0xE44747),
                                              16,
                                              NULL);
    lv_obj_add_event_cb(reject_btn,
                        display_call_alert_reject_btn_cb,
                        LV_EVENT_PRESSED,
                        NULL);
    accept_btn = display_create_native_button(card,
                                              180,
                                              122,
                                              142,
                                              48,
                                              lv_color_hex(0x21C783),
                                              lv_color_hex(0x21C783),
                                              "接听",
                                              lv_color_hex(0xFFFFFF),
                                              16,
                                              display_call_alert_accept_btn_cb);
    lv_obj_set_style_radius(reject_btn, 7, 0);
    lv_obj_set_style_radius(accept_btn, 7, 0);
}

static bool display_call_pending_contains(const display_status_t *status, const char *device_id)
{
    if (status == NULL || device_id == NULL || device_id[0] == '\0') {
        return false;
    }

    uint8_t count = status->call_pending_contact_count > DISPLAY_CALL_CONTACT_COUNT ?
        DISPLAY_CALL_CONTACT_COUNT : status->call_pending_contact_count;
    for (uint8_t index = 0; index < count; ++index) {
        if (strcmp(status->call_pending_contacts[index].device_id, device_id) == 0) {
            return true;
        }
    }
    return false;
}

static void display_hide_call_contact_request(void)
{
    if (s_call_contact_request_box != NULL) {
        lv_obj_del(s_call_contact_request_box);
        s_call_contact_request_box = NULL;
    }
    s_call_contact_request_peer[0] = '\0';
}

static esp_err_t display_submit_call_contact_response(const char *device_id, bool accept)
{
    char peer[DISPLAY_CALL_CONTACT_DEVICE_ID_MAX] = {0};

    strlcpy(peer, device_id != NULL ? device_id : "", sizeof(peer));
    if (peer[0] == '\0' || s_actions.on_respond_call_contact == NULL) {
        display_show_wifi_alert("添加联系人", "审批接口不可用");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(CALL_FLOW_TAG,
             "stage=contact_response_submit peer=%s action=%s",
             peer,
             accept ? "accept" : "reject");
    esp_err_t ret = s_actions.on_respond_call_contact(peer, accept, s_actions.ctx);
    if (ret != ESP_OK) {
        ESP_LOGW(CALL_FLOW_TAG,
                 "stage=contact_response_rejected peer=%s action=%s ret=%s",
                 peer,
                 accept ? "accept" : "reject",
                 esp_err_to_name(ret));
        display_show_wifi_alert("添加联系人",
                                ret == ESP_ERR_INVALID_STATE ?
                                "联系人服务忙，请稍后再试" : "操作提交失败");
        return ret;
    }

    strlcpy(s_call_contact_request_suppressed_peer,
            peer,
            sizeof(s_call_contact_request_suppressed_peer));
    s_call_contact_request_dismissed = false;
    display_hide_call_contact_request();
    return ESP_OK;
}

static void display_call_contact_request_close_btn_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    ESP_LOGI(CALL_FLOW_TAG,
             "stage=contact_request_popup_dismiss peer=%s",
             s_call_contact_request_peer[0] != '\0' ? s_call_contact_request_peer : "-");
    s_call_contact_request_dismissed = true;
    display_hide_call_contact_request();
}

static void display_call_contact_request_accept_btn_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        (void)display_submit_call_contact_response(s_call_contact_request_peer, true);
    }
}

static void display_show_call_contact_request(const char *device_id)
{
    lv_obj_t *card = NULL;

    if (device_id == NULL || device_id[0] == '\0') {
        return;
    }
    if (s_call_contact_request_box != NULL &&
        strcmp(s_call_contact_request_peer, device_id) == 0) {
        return;
    }

    display_hide_call_contact_request();
    strlcpy(s_call_contact_request_peer, device_id, sizeof(s_call_contact_request_peer));
    ESP_LOGI(CALL_FLOW_TAG,
             "stage=contact_request_popup_show peer=%s pending=%u",
             s_call_contact_request_peer,
             (unsigned)s_call_pending_contact_count);

    s_call_contact_request_box = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_call_contact_request_box);
    lv_obj_set_pos(s_call_contact_request_box, 0, 0);
    lv_obj_set_size(s_call_contact_request_box, DISPLAY_DRIVER_WIDTH, DISPLAY_DRIVER_HEIGHT);
    lv_obj_set_style_bg_color(s_call_contact_request_box, lv_color_hex(0x10233B), 0);
    lv_obj_set_style_bg_opa(s_call_contact_request_box, LV_OPA_40, 0);
    lv_obj_clear_flag(s_call_contact_request_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_call_contact_request_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(s_call_contact_request_box);

    card = display_create_native_box(s_call_contact_request_box,
                                     70,
                                     70,
                                     340,
                                     180,
                                     lv_color_hex(0xFFFFFF),
                                     lv_color_hex(0xD6E4EF),
                                     8);
    lv_obj_set_style_shadow_width(card, 14, 0);
    lv_obj_set_style_shadow_ofs_y(card, 5, 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(0x10233B), 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_20, 0);

    display_create_native_live_text(card,
                                    "收到联系人申请",
                                    20,
                                    20,
                                    300,
                                    lv_color_hex(0x10233B),
                                    LV_TEXT_ALIGN_CENTER);
    display_create_native_text(card,
                               device_id,
                               20,
                               58,
                               300,
                               lv_color_hex(0x64758A),
                               14,
                               LV_TEXT_ALIGN_CENTER);
    display_create_native_live_button(card,
                                      18,
                                      112,
                                      142,
                                      46,
                                      lv_color_hex(0xF3F6F9),
                                      lv_color_hex(0xD5E0EB),
                                      "稍后处理",
                                      lv_color_hex(0x64758A),
                                      display_call_contact_request_close_btn_cb);
    display_create_native_live_button(card,
                                      180,
                                      112,
                                      142,
                                      46,
                                      lv_color_hex(0x21C783),
                                      lv_color_hex(0x21C783),
                                      "同意添加",
                                      lv_color_hex(0xFFFFFF),
                                      display_call_contact_request_accept_btn_cb);
}

static void display_update_call_contact_request(const display_status_t *status)
{
    if (status == NULL) {
        display_hide_call_contact_request();
        return;
    }

    if (s_call_contact_request_suppressed_peer[0] != '\0') {
        if (status->call_contacts_refreshing) {
            return;
        }
        bool still_pending = display_call_pending_contains(
            status,
            s_call_contact_request_suppressed_peer);
        s_call_contact_request_suppressed_peer[0] = '\0';
        if (still_pending && status->call_contacts_last_error != ESP_OK) {
            display_show_wifi_alert("添加联系人", "审批失败，请重试");
            return;
        }
    }

    if (s_call_contact_request_box != NULL &&
        !display_call_pending_contains(status, s_call_contact_request_peer)) {
        display_hide_call_contact_request();
    }

    if (status->call_pending_contact_count == 0U) {
        s_call_contact_request_dismissed = false;
    } else if (s_call_contact_request_dismissed) {
        display_hide_call_contact_request();
        return;
    }

    bool call_busy = status->call_state != DISPLAY_CALL_STATE_IDLE ||
                     status->wechat_call_state != DISPLAY_WECHAT_CALL_STATE_IDLE ||
                     status->rtc_incoming_call_pending ||
                     status->wechat_incoming_call_pending;
    if (call_busy) {
        display_hide_call_contact_request();
        return;
    }

    if (s_call_contact_request_box == NULL &&
        status->call_pending_contact_count > 0U &&
        s_call_alert_box == NULL &&
        s_call_delete_confirm_box == NULL &&
        s_call_hangup_confirm_box == NULL &&
        s_wechat_delete_confirm_box == NULL &&
        s_wifi_alert_box == NULL) {
        display_show_call_contact_request(status->call_pending_contacts[0].device_id);
    }
}

static void display_hide_call_delete_confirm(void)
{
    if (s_call_delete_confirm_box != NULL) {
        lv_obj_del(s_call_delete_confirm_box);
        s_call_delete_confirm_box = NULL;
    }
    s_call_delete_pending_index = UINT8_MAX;
    s_call_delete_pending_device_id[0] = '\0';
}

static void display_show_call_delete_confirm(uint8_t contact_index)
{
    lv_obj_t *card = NULL;

    if (contact_index >= s_call_contact_count ||
        s_call_contacts[contact_index].device_id[0] == '\0') {
        display_show_wifi_alert("联系人", "联系人不存在");
        return;
    }
    if (!s_call_contacts[contact_index].deletable) {
        display_show_wifi_alert("删除联系人", "同账号自动联系人请在平台管理");
        return;
    }

    display_hide_call_delete_confirm();
    s_call_delete_pending_index = contact_index;
    strlcpy(s_call_delete_pending_device_id,
            s_call_contacts[contact_index].device_id,
            sizeof(s_call_delete_pending_device_id));

    s_call_delete_confirm_box = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_call_delete_confirm_box);
    lv_obj_set_pos(s_call_delete_confirm_box, 0, 0);
    lv_obj_set_size(s_call_delete_confirm_box, DISPLAY_DRIVER_WIDTH, DISPLAY_DRIVER_HEIGHT);
    lv_obj_set_style_bg_color(s_call_delete_confirm_box, lv_color_hex(0x10233B), 0);
    lv_obj_set_style_bg_opa(s_call_delete_confirm_box, LV_OPA_40, 0);
    lv_obj_clear_flag(s_call_delete_confirm_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_call_delete_confirm_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(s_call_delete_confirm_box);

    card = display_create_native_box(s_call_delete_confirm_box,
                                     70,
                                     80,
                                     340,
                                     160,
                                     lv_color_hex(0xFFFFFF),
                                     lv_color_hex(0xD6E4EF),
                                     8);
    display_create_native_live_text(card,
                                    "删除联系人",
                                    20,
                                    18,
                                    300,
                                    lv_color_hex(0x10233B),
                                    LV_TEXT_ALIGN_CENTER);
    display_create_native_text(card,
                               s_call_delete_pending_device_id,
                               20,
                               52,
                               300,
                               lv_color_hex(0x64758A),
                               14,
                               LV_TEXT_ALIGN_CENTER);
    display_create_native_live_button(card,
                                      18,
                                      98,
                                      142,
                                      44,
                                      lv_color_hex(0xF3F6F9),
                                      lv_color_hex(0xD5E0EB),
                                      "取消",
                                      lv_color_hex(0x64758A),
                                      display_call_delete_cancel_btn_cb);
    display_create_native_live_button(card,
                                      180,
                                      98,
                                      142,
                                      44,
                                      lv_color_hex(0xFFE7E7),
                                      lv_color_hex(0xF15A5A),
                                      "确认删除",
                                      lv_color_hex(0xE44747),
                                      display_call_delete_confirm_btn_cb);
}

static void display_hide_wechat_delete_confirm(void)
{
    if (s_wechat_delete_confirm_box != NULL) {
        lv_obj_del(s_wechat_delete_confirm_box);
        s_wechat_delete_confirm_box = NULL;
    }
    s_wechat_delete_pending_index = UINT8_MAX;
    s_wechat_delete_pending_open_id[0] = '\0';
}

static void display_show_wechat_delete_confirm(uint8_t contact_index)
{
    lv_obj_t *card = NULL;
    lv_obj_t *cancel_btn = NULL;
    lv_obj_t *delete_btn = NULL;

    if (contact_index >= DISPLAY_WECHAT_CONTACT_COUNT ||
        contact_index >= s_last_status.wechat_contact_count ||
        s_last_status.wechat_contacts[contact_index].open_id[0] == '\0') {
        display_show_wifi_alert("微信联系人", "联系人不存在");
        return;
    }

    display_hide_wechat_delete_confirm();
    s_wechat_delete_pending_index = contact_index;
    strlcpy(s_wechat_delete_pending_open_id,
            s_last_status.wechat_contacts[contact_index].open_id,
            sizeof(s_wechat_delete_pending_open_id));

    s_wechat_delete_confirm_box = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_wechat_delete_confirm_box);
    lv_obj_set_pos(s_wechat_delete_confirm_box, 0, 0);
    lv_obj_set_size(s_wechat_delete_confirm_box, DISPLAY_DRIVER_WIDTH, DISPLAY_DRIVER_HEIGHT);
    lv_obj_set_style_bg_color(s_wechat_delete_confirm_box, lv_color_hex(0x10233B), 0);
    lv_obj_set_style_bg_opa(s_wechat_delete_confirm_box, LV_OPA_20, 0);
    lv_obj_set_style_border_width(s_wechat_delete_confirm_box, 0, 0);
    lv_obj_set_style_pad_all(s_wechat_delete_confirm_box, 0, 0);
    lv_obj_clear_flag(s_wechat_delete_confirm_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_wechat_delete_confirm_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(s_wechat_delete_confirm_box);

    card = display_create_figma_box(s_wechat_delete_confirm_box,
                                    44,
                                    62,
                                    232,
                                    116,
                                    lv_color_hex(0xFFFFFF),
                                    lv_color_hex(0xD6E4EF),
                                    9);
    lv_obj_set_style_shadow_width(card, 14, 0);
    lv_obj_set_style_shadow_ofs_y(card, 5, 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(0x10233B), 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_20, 0);

    (void)display_create_figma_text(card,
                                    "删除联系人",
                                    12,
                                    13,
                                    208,
                                    lv_color_hex(0x10233B),
                                    16,
                                    LV_TEXT_ALIGN_CENTER);
    (void)display_create_figma_text(card,
                                    s_wechat_delete_pending_open_id,
                                    16,
                                    42,
                                    200,
                                    lv_color_hex(0x64758A),
                                    12,
                                    LV_TEXT_ALIGN_CENTER);

    cancel_btn = display_create_figma_button(card,
                                             18,
                                             76,
                                             88,
                                             30,
                                             lv_color_hex(0xE9F5FF),
                                             lv_color_hex(0x2F82D7),
                                             "Cancel",
                                             lv_color_hex(0x2F82D7),
                                             12,
                                             display_wechat_delete_cancel_btn_cb);
    delete_btn = display_create_figma_button(card,
                                             126,
                                             76,
                                             88,
                                             30,
                                             lv_color_hex(0xFFE7E7),
                                             lv_color_hex(0xF15A5A),
                                             "Delete",
                                             lv_color_hex(0xE44747),
                                             12,
                                             display_wechat_delete_confirm_btn_cb);
    lv_obj_set_style_radius(cancel_btn, 7, 0);
    lv_obj_set_style_radius(delete_btn, 7, 0);
}

static void display_show_wifi_alert(const char *title, const char *message)
{
    if (s_wifi_alert_box != NULL) {
        lv_obj_del(s_wifi_alert_box);
        s_wifi_alert_box = NULL;
    }

    s_wifi_alert_box = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_wifi_alert_box);
    display_obj_set_design_size(s_wifi_alert_box, 232, 118);
    lv_obj_set_style_radius(s_wifi_alert_box, 8, 0);
    lv_obj_set_style_bg_color(s_wifi_alert_box, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(s_wifi_alert_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_wifi_alert_box, 1, 0);
    lv_obj_set_style_border_color(s_wifi_alert_box, lv_color_hex(0xD5E0EB), 0);
    lv_obj_set_style_pad_all(s_wifi_alert_box, 0, 0);
    lv_obj_clear_flag(s_wifi_alert_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(s_wifi_alert_box);
    lv_obj_move_foreground(s_wifi_alert_box);
    lv_obj_add_event_cb(s_wifi_alert_box, display_wifi_alert_event_cb, LV_EVENT_ALL, NULL);

    (void)display_create_figma_live_text(s_wifi_alert_box,
                                         title != NULL ? title : "",
                                         12,
                                         14,
                                         208,
                                         lv_color_hex(0x10243E),
                                         LV_TEXT_ALIGN_CENTER);
    (void)display_create_figma_live_text(s_wifi_alert_box,
                                         message != NULL ? message : "",
                                         16,
                                         46,
                                         200,
                                         lv_color_hex(0x64758A),
                                         LV_TEXT_ALIGN_CENTER);
    (void)display_create_figma_button(s_wifi_alert_box,
                                      68,
                                      80,
                                      96,
                                      28,
                                      lv_color_hex(0x1768B7),
                                      lv_color_hex(0x1768B7),
                                      "OK",
                                      lv_color_hex(0xFFFFFF),
                                      12,
                                      display_wifi_alert_ok_btn_cb);
}

static void display_update_uuid_edit_feedback(const char *override_text, lv_color_t override_color)
{
    const char *uuid = "";
    size_t uuid_len = 0;
    lv_color_t length_color = lv_color_hex(0x48656F);
    const char *status_text = override_text;
    lv_color_t status_color = override_color;

    if (s_uuid_ta != NULL) {
        uuid = lv_textarea_get_text(s_uuid_ta);
    }
    uuid_len = strlen(uuid);

    if (s_uuid_edit_length_label != NULL) {
        if (uuid_len > 0 && uuid_len < DEVICE_UUID_MIN_LEN) {
            length_color = lv_color_hex(0xC8513C);
        } else if (uuid_len >= DEVICE_UUID_MIN_LEN) {
            length_color = lv_color_hex(0x2E8F6B);
        }
        display_text_set_color(s_uuid_edit_length_label, length_color, 0);
        lv_label_set_text_fmt(s_uuid_edit_length_label,
                              "%u/%u",
                              (unsigned)uuid_len,
                              (unsigned)DEVICE_UUID_EDIT_MAX_LEN);
    }

    if (s_uuid_edit_status_label == NULL) {
        return;
    }

    if (status_text == NULL) {
        if (uuid_len == 0) {
            status_text = "请输入 4-12 位";
            status_color = lv_color_hex(0x64758A);
        } else if (uuid_len < DEVICE_UUID_MIN_LEN) {
            status_text = "Device ID 过短";
            status_color = lv_color_hex(0xE45656);
        } else {
            status_text = "点击保存生效";
            status_color = lv_color_hex(0x0D8A59);
        }
    }

    display_text_set_color(s_uuid_edit_status_label, status_color, 0);
    display_text_set(s_uuid_edit_status_label, status_text);
}

static void display_submit_uuid(void)
{
    const char *uuid = NULL;
    size_t uuid_len = 0;
    esp_err_t ret = ESP_OK;

    if (s_uuid_ta == NULL) {
        return;
    }

    uuid = lv_textarea_get_text(s_uuid_ta);
    uuid_len = strlen(uuid);
    if (uuid_len < DEVICE_UUID_MIN_LEN) {
        display_update_uuid_edit_feedback("UUID too short", lv_color_hex(0xC8513C));
        display_show_wifi_alert("Device UUID", "UUID must be 4-12 uppercase letters or digits.");
        return;
    }
    if (s_actions.on_set_device_uuid == NULL) {
        display_update_uuid_edit_feedback("UUID update unavailable", lv_color_hex(0xC8513C));
        display_show_wifi_alert("Device UUID", "UUID update is unavailable right now.");
        return;
    }
    if (strcmp(uuid, s_last_status.device_uuid) == 0) {
        display_set_main_hint("Ready");
        if (s_uuid_parent_page == DISPLAY_PAGE_TIRTC_CONFIG) {
            display_show_tirtc_config_page();
        } else {
            display_show_main_page();
        }
        return;
    }

    ret = s_actions.on_set_device_uuid(uuid, s_actions.ctx);
    if (ret == ESP_OK) {
        if (s_uuid_label != NULL) {
            display_text_set(s_uuid_label, uuid);
        }
        display_set_main_hint("Ready");
        if (s_uuid_parent_page == DISPLAY_PAGE_TIRTC_CONFIG) {
            display_show_tirtc_config_page();
        } else {
            display_show_main_page();
        }
        return;
    }

    display_update_uuid_edit_feedback("Save failed", lv_color_hex(0xC8513C));
    if (ret == ESP_ERR_INVALID_ARG || ret == ESP_ERR_INVALID_SIZE) {
        display_show_wifi_alert("Device UUID", "Use only A-Z and 0-9, with 4-12 characters.");
    } else {
        display_show_wifi_alert("Device UUID", "Saving UUID failed. Please try again.");
    }
}

static void display_uuid_keyboard_value_event_cb(lv_event_t *event)
{
    lv_obj_t *keyboard = lv_event_get_target(event);
    uint16_t button_id = lv_btnmatrix_get_selected_btn(keyboard);
    const char *text = NULL;

    if (button_id == LV_BTNMATRIX_BTN_NONE || s_uuid_ta == NULL) {
        return;
    }

    text = lv_btnmatrix_get_btn_text(keyboard, button_id);
    if (text == NULL) {
        return;
    }

    if (strcmp(text, LV_SYMBOL_BACKSPACE) == 0) {
        lv_textarea_del_char(s_uuid_ta);
    } else if (strcmp(text, "Clear") == 0) {
        lv_textarea_set_text(s_uuid_ta, "");
    } else {
        lv_textarea_add_text(s_uuid_ta, text);
    }

    display_update_uuid_edit_feedback(NULL, lv_color_hex(0x48656F));
}

static void display_uuid_keyboard_draw_part_event_cb(lv_event_t *event)
{
    lv_obj_t *keyboard = lv_event_get_target(event);
    lv_obj_draw_part_dsc_t *draw_part = lv_event_get_draw_part_dsc(event);
    const char *text = NULL;

    if (lv_event_get_code(event) != LV_EVENT_DRAW_PART_BEGIN || draw_part == NULL || draw_part->part != LV_PART_ITEMS) {
        return;
    }

    text = lv_btnmatrix_get_btn_text(keyboard, draw_part->id);
    if (text == NULL) {
        return;
    }

    if (strcmp(text, LV_SYMBOL_BACKSPACE) == 0 || strcmp(text, "Clear") == 0) {
        draw_part->rect_dsc->bg_color = lv_color_hex(0x183642);
        draw_part->rect_dsc->bg_opa = LV_OPA_COVER;
        draw_part->label_dsc->color = lv_color_hex(0xF7F5F2);
    }
}

static void display_set_password_border_color(lv_color_t border_color)
{
    if (s_password_ta == NULL) {
        return;
    }

    lv_obj_set_style_border_color(s_password_ta, border_color, 0);
}

static const display_wifi_scan_result_t *display_find_selected_wifi_result(
    const display_status_t *status)
{
    if (status == NULL || s_selected_ssid[0] == '\0') {
        return NULL;
    }

    for (uint16_t index = 0; index < status->wifi_scan_count && index < DISPLAY_WIFI_SCAN_MAX; ++index) {
        if (strcmp(status->wifi_scan_results[index].ssid, s_selected_ssid) == 0) {
            return &status->wifi_scan_results[index];
        }
    }

    return NULL;
}

static bool display_selected_wifi_has_saved_config(const display_status_t *status)
{
    return status != NULL &&
           s_selected_ssid[0] != '\0' &&
           status->saved_network_ssid[0] != '\0' &&
           strcmp(status->saved_network_ssid, s_selected_ssid) == 0;
}

static bool display_selected_wifi_requires_password(const display_status_t *status)
{
    const display_wifi_scan_result_t *selected_result = display_find_selected_wifi_result(status);

    if (selected_result != NULL) {
        return selected_result->secure;
    }

    return display_selected_wifi_has_saved_config(status);
}

static void display_prepare_password_entry(const display_status_t *status)
{
    if (s_password_ta == NULL) {
        return;
    }

    if (display_selected_wifi_has_saved_config(status) &&
        status != NULL &&
        status->saved_network_password[0] != '\0') {
        lv_textarea_set_text(s_password_ta, status->saved_network_password);
        display_set_password_placeholder("Password", lv_color_hex(0xD1D7DB));
    } else if (!display_selected_wifi_requires_password(status)) {
        lv_textarea_set_text(s_password_ta, "");
        display_set_password_placeholder("Open network", lv_color_hex(0xD1D7DB));
    } else {
        lv_textarea_set_text(s_password_ta, "");
        display_set_password_placeholder("Password", lv_color_hex(0xD1D7DB));
    }
}

static void display_update_wifi_connect_details_line(const display_status_t *status)
{
    char detail_text[192] = {0};
    char ssid_text[48] = {0};
    const display_wifi_scan_result_t *selected_result = NULL;
    bool has_saved_config = false;

    if (s_wifi_connect_details_label == NULL || status == NULL) {
        return;
    }

    selected_result = display_find_selected_wifi_result(status);
    has_saved_config = display_selected_wifi_has_saved_config(status);
    display_format_ssid(ssid_text, sizeof(ssid_text), s_selected_ssid);
    if (s_selected_ssid[0] == '\0') {
        strlcpy(detail_text, "SSID: --\nType: Select WiFi first", sizeof(detail_text));
    } else if (selected_result != NULL) {
        snprintf(detail_text,
                 sizeof(detail_text),
                  has_saved_config
                      ? "SSID: %s\nType: %s | CH %u | RSSI %d dBm\nSaved credential available"
                      : "SSID: %s\nType: %s | CH %u | RSSI %d dBm",
                  ssid_text,
                  selected_result->secure ? "Secured" : "Open",
                  (unsigned)selected_result->channel,
                  selected_result->rssi);
    } else if (has_saved_config) {
        snprintf(detail_text,
                 sizeof(detail_text),
                 "SSID: %s\nType: Saved network | CH -- | RSSI --\nSaved credential available",
                 ssid_text);
    } else if (status->network_connected &&
               strcmp(status->network_ssid, s_selected_ssid) == 0 &&
               status->network_rssi > -120) {
        snprintf(detail_text,
                 sizeof(detail_text),
                 "SSID: %s\nType: Connected | CH -- | RSSI %d dBm",
                 ssid_text,
                 status->network_rssi);
    } else {
        snprintf(detail_text,
                 sizeof(detail_text),
                 "SSID: %s\nType: Unavailable | CH -- | RSSI --",
                 ssid_text);
    }

    display_text_set(s_wifi_connect_details_label, detail_text);
}

static void display_update_wifi_connect_status_line(const display_status_t *status)
{
    char hint_text[64] = {0};
    char rssi_text[24] = {0};
    lv_color_t hint_color = lv_color_hex(0x48656F);
    lv_color_t rssi_color = lv_color_hex(0x48656F);
    const display_wifi_scan_result_t *selected_result = NULL;
    bool signal_known = false;
    int signal_rssi = 0;

    if (s_wifi_connect_hint_label == NULL || s_wifi_connect_rssi_label == NULL || status == NULL) {
        return;
    }

    selected_result = display_find_selected_wifi_result(status);
    if (selected_result != NULL) {
        signal_known = true;
        signal_rssi = selected_result->rssi;
    } else if (status->network_connected &&
               s_selected_ssid[0] != '\0' &&
               strcmp(status->network_ssid, s_selected_ssid) == 0 &&
               status->network_rssi > -120) {
        signal_known = true;
        signal_rssi = status->network_rssi;
    }

    if (signal_known) {
        snprintf(rssi_text, sizeof(rssi_text), "%d dBm", signal_rssi);
        if (signal_rssi >= -60) {
            rssi_color = lv_color_hex(0x0D8A59);
        } else if (signal_rssi >= -75) {
            rssi_color = lv_color_hex(0xF59E0B);
        }
    } else if (s_selected_ssid[0] != '\0') {
        strlcpy(rssi_text, "无信号", sizeof(rssi_text));
        rssi_color = lv_color_hex(0xE45656);
    }

    if (s_selected_ssid[0] == '\0') {
        strlcpy(hint_text, "请选择 Wi-Fi", sizeof(hint_text));
        hint_color = lv_color_hex(0xE45656);
    } else {
        switch (s_wifi_connect_state) {
        case DISPLAY_WIFI_CONNECT_STATE_SELECT_FIRST:
            strlcpy(hint_text, "请选择 Wi-Fi", sizeof(hint_text));
            hint_color = lv_color_hex(0xE45656);
            break;
        case DISPLAY_WIFI_CONNECT_STATE_UNAVAILABLE:
            strlcpy(hint_text, "Wi-Fi 不可用", sizeof(hint_text));
            hint_color = lv_color_hex(0xE45656);
            break;
        case DISPLAY_WIFI_CONNECT_STATE_SHORT_PASSWORD:
            strlcpy(hint_text, "密码过短", sizeof(hint_text));
            hint_color = lv_color_hex(0xE45656);
            break;
        case DISPLAY_WIFI_CONNECT_STATE_CONNECTING: {
            static const char *dots = ".........";
            int dot_count = (int)((esp_timer_get_time() / 250000ULL) % 9ULL) + 1;
            snprintf(hint_text, sizeof(hint_text), "连接中%.*s", dot_count, dots);
            hint_color = lv_color_hex(0xF59E0B);
            break;
        }
        case DISPLAY_WIFI_CONNECT_STATE_FAILED:
            strlcpy(hint_text, "密码错误", sizeof(hint_text));
            hint_color = lv_color_hex(0xE45656);
            break;
        case DISPLAY_WIFI_CONNECT_STATE_TIMEOUT:
            strlcpy(hint_text, "连接超时", sizeof(hint_text));
            hint_color = lv_color_hex(0xE45656);
            break;
        case DISPLAY_WIFI_CONNECT_STATE_CONNECTED:
            strlcpy(hint_text, "已连接并保存", sizeof(hint_text));
            hint_color = lv_color_hex(0x0D8A59);
            break;
        case DISPLAY_WIFI_CONNECT_STATE_IDLE:
        default:
            if (display_selected_wifi_has_saved_config(status)) {
                strlcpy(hint_text, "凭据已保存", sizeof(hint_text));
                hint_color = lv_color_hex(0x0D8A59);
            } else if (!display_selected_wifi_requires_password(status)) {
                strlcpy(hint_text, "开放网络", sizeof(hint_text));
            } else {
                strlcpy(hint_text, "输入密码加入", sizeof(hint_text));
            }
            break;
        }
    }

    display_text_set_color(s_wifi_connect_hint_label, hint_color, 0);
    display_text_set(s_wifi_connect_hint_label, hint_text);
    display_text_set_color(s_wifi_connect_rssi_label, rssi_color, 0);
    display_text_set(s_wifi_connect_rssi_label, rssi_text);
}

static void display_keyboard_draw_part_event_cb(lv_event_t *event)
{
    lv_obj_t *keyboard = lv_event_get_target(event);
    lv_obj_draw_part_dsc_t *draw_part = lv_event_get_draw_part_dsc(event);
    const char *text = NULL;

    if (lv_event_get_code(event) != LV_EVENT_DRAW_PART_BEGIN || draw_part == NULL || draw_part->part != LV_PART_ITEMS) {
        return;
    }

    text = lv_btnmatrix_get_btn_text(keyboard, draw_part->id);
    if (text == NULL) {
        return;
    }

    if (draw_part->id == DISPLAY_KB_BTN_CURSOR_LEFT_ID ||
        draw_part->id == DISPLAY_KB_BTN_CURSOR_RIGHT_ID) {
        draw_part->rect_dsc->bg_color = lv_color_hex(0x183642);
        draw_part->rect_dsc->bg_opa = LV_OPA_COVER;
        draw_part->label_dsc->color = lv_color_hex(0xF7F5F2);
    } else if (draw_part->id == DISPLAY_KB_BTN_JOIN_ID) {
        draw_part->rect_dsc->bg_color = lv_color_hex(0x2E8F6B);
        draw_part->rect_dsc->bg_opa = LV_OPA_COVER;
        draw_part->label_dsc->color = lv_color_hex(0xF7F5F2);
    } else if (draw_part->id == DISPLAY_KB_BTN_SPACE_ID ||
               strcmp(text, "'") == 0) {
        draw_part->rect_dsc->bg_color = lv_color_hex(0xF7F5F2);
        draw_part->rect_dsc->bg_opa = LV_OPA_COVER;
        draw_part->label_dsc->color = lv_color_hex(0x183642);
    }

    if (draw_part->id == DISPLAY_KB_BTN_SPACE_ID ||
        draw_part->id == DISPLAY_KB_BTN_JOIN_ID) {
#if LV_FONT_MONTSERRAT_12
        draw_part->label_dsc->font = &lv_font_montserrat_12;
#endif
        draw_part->label_dsc->letter_space = 0;
    }
}

static void display_set_password_placeholder(const char *text, lv_color_t border_color)
{
    if (s_password_ta == NULL) {
        return;
    }

    lv_textarea_set_placeholder_text(s_password_ta, text);
    display_set_password_border_color(border_color);
}

static void display_submit_wifi_connect(void)
{
    size_t password_len = 0;
    bool has_saved_config = false;
    bool requires_password = false;
    bool using_saved_password = false;

    if (s_password_ta == NULL) {
        return;
    }

    const char *password = lv_textarea_get_text(s_password_ta);
    display_set_password_border_color(lv_color_hex(0xD1D7DB));
    password_len = strlen(password);
    has_saved_config = display_selected_wifi_has_saved_config(&s_last_status);
    requires_password = display_selected_wifi_requires_password(&s_last_status);
    using_saved_password = has_saved_config && password_len == 0;

    if (s_selected_ssid[0] == '\0') {
        s_wifi_connect_state = DISPLAY_WIFI_CONNECT_STATE_SELECT_FIRST;
        return;
    }
    if (s_last_status.network_connected && strcmp(s_last_status.network_ssid, s_selected_ssid) == 0) {
        s_wifi_connect_pending = false;
        s_wifi_connect_target_ssid[0] = '\0';
        display_show_wifi_page();
        return;
    }
    if (s_actions.on_wifi_connect == NULL) {
        s_wifi_connect_state = DISPLAY_WIFI_CONNECT_STATE_UNAVAILABLE;
        return;
    }
    if (requires_password && !using_saved_password && password_len < DISPLAY_WIFI_PASSWORD_MIN_LEN) {
        s_wifi_connect_state = DISPLAY_WIFI_CONNECT_STATE_SHORT_PASSWORD;
        display_set_password_border_color(lv_color_hex(0xC8513C));
        display_show_wifi_alert("WiFi Password", "Password must be at least 8 characters.");
        return;
    }

    esp_err_t ret = s_actions.on_wifi_connect(s_selected_ssid, password, s_actions.ctx);
    if (ret == ESP_OK) {
        s_wifi_connect_pending = true;
        s_wifi_connect_request_us = esp_timer_get_time();
        strlcpy(s_wifi_connect_target_ssid, s_selected_ssid, sizeof(s_wifi_connect_target_ssid));
        s_wifi_connect_state = DISPLAY_WIFI_CONNECT_STATE_CONNECTING;
        display_set_password_border_color(lv_color_hex(0xC89F4A));
        display_set_main_hint("Ready");
    } else {
        s_wifi_connect_pending = false;
        s_wifi_connect_target_ssid[0] = '\0';
        s_wifi_connect_state = DISPLAY_WIFI_CONNECT_STATE_FAILED;
        display_set_password_border_color(lv_color_hex(0xC8513C));
    }
}

static void display_keyboard_value_event_cb(lv_event_t *event)
{
    lv_obj_t *keyboard = lv_event_get_target(event);
    const bool wifi_keyboard = keyboard == s_keyboard;
    uint16_t button_id = wifi_keyboard ? lv_btnmatrix_get_selected_btn(keyboard)
                                       : lv_keyboard_get_selected_btn(keyboard);
    lv_obj_t *textarea = wifi_keyboard ? s_password_ta : lv_keyboard_get_textarea(keyboard);
    const char *text = NULL;

    if (button_id == LV_BTNMATRIX_BTN_NONE) {
        return;
    }

    text = wifi_keyboard ? lv_btnmatrix_get_btn_text(keyboard, button_id)
                         : lv_keyboard_get_btn_text(keyboard, button_id);
    if (text == NULL) {
        return;
    }

    if (strcmp(text, "abc") == 0) {
        if (wifi_keyboard) {
            display_set_wifi_keyboard_mode(LV_KEYBOARD_MODE_USER_1);
        } else {
            lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_USER_1);
        }
        return;
    }
    if (strcmp(text, "ABC") == 0) {
        if (wifi_keyboard) {
            display_set_wifi_keyboard_mode(LV_KEYBOARD_MODE_USER_2);
        } else {
            lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_USER_2);
        }
        return;
    }
    if (strcmp(text, "1#") == 0) {
        if (wifi_keyboard) {
            display_set_wifi_keyboard_mode(LV_KEYBOARD_MODE_USER_3);
        } else {
            lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_USER_3);
        }
        return;
    }
    if (strcmp(text, LV_SYMBOL_CLOSE) == 0 || strcmp(text, LV_SYMBOL_KEYBOARD) == 0) {
        display_hide_keyboard();
        return;
    }
    if (strcmp(text, "Connect") == 0 || strcmp(text, "OK") == 0) {
        if (textarea == s_tirtc_edit_ta) {
            display_tirtc_config_edit_save_btn_cb(NULL);
        } else if (textarea == s_call_add_edit_ta) {
            display_call_add_edit_save_btn_cb(NULL);
        } else if (textarea == s_call_remark_ta) {
            display_call_remark_save_btn_cb(NULL);
        } else if (textarea == s_wechat_add_edit_ta) {
            display_wechat_add_edit_save_btn_cb(NULL);
        } else if (textarea == s_wechat_remark_ta) {
            display_wechat_remark_save_btn_cb(NULL);
        } else {
            display_submit_wifi_connect();
        }
        return;
    }
    if (textarea == NULL) {
        return;
    }
    if (button_id == DISPLAY_KB_BTN_CURSOR_LEFT_ID) {
        lv_textarea_cursor_left(textarea);
        return;
    }
    if (button_id == DISPLAY_KB_BTN_CURSOR_RIGHT_ID) {
        lv_textarea_cursor_right(textarea);
        return;
    }
    if (strcmp(text, LV_SYMBOL_BACKSPACE) == 0) {
        lv_textarea_del_char(textarea);
        return;
    }
    if (button_id == DISPLAY_KB_BTN_SPACE_ID || strcmp(text, "Space") == 0) {
        lv_textarea_add_text(textarea, " ");
        return;
    }

    lv_textarea_add_text(textarea, text);
}

static void display_textarea_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t *target = lv_event_get_target(event);

    if (code == LV_EVENT_FOCUSED && s_keyboard != NULL) {
        if (target == s_password_ta) {
            display_set_wifi_keyboard_mode(LV_KEYBOARD_MODE_USER_1);
        }
        display_layout_wifi_keyboard();
        lv_obj_clear_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_keyboard);
    }
}

static void display_call_add_edit_textarea_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t *target = lv_event_get_target(event);

    if (code == LV_EVENT_FOCUSED && s_call_add_edit_keyboard != NULL) {
        lv_keyboard_set_textarea(s_call_add_edit_keyboard, target);
        lv_keyboard_set_mode(s_call_add_edit_keyboard, LV_KEYBOARD_MODE_USER_1);
        lv_obj_clear_flag(s_call_add_edit_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_call_add_edit_keyboard);
    }
    if (code == LV_EVENT_VALUE_CHANGED) {
        display_update_call_add_edit_feedback(NULL, lv_color_hex(0x0D8A59));
    }
}

static void display_wechat_add_edit_textarea_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t *target = lv_event_get_target(event);

    if (code == LV_EVENT_FOCUSED && s_wechat_add_edit_keyboard != NULL) {
        lv_keyboard_set_textarea(s_wechat_add_edit_keyboard, target);
        lv_keyboard_set_mode(s_wechat_add_edit_keyboard, LV_KEYBOARD_MODE_USER_1);
        lv_obj_clear_flag(s_wechat_add_edit_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_wechat_add_edit_keyboard);
    }
    if (code == LV_EVENT_VALUE_CHANGED) {
        display_update_wechat_add_edit_feedback(NULL, lv_color_hex(0x0D8A59));
    }
}

static void display_tirtc_edit_textarea_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t *target = lv_event_get_target(event);

    if (code == LV_EVENT_FOCUSED && s_tirtc_edit_keyboard != NULL) {
        lv_keyboard_set_textarea(s_tirtc_edit_keyboard, target);
        lv_keyboard_set_mode(s_tirtc_edit_keyboard, LV_KEYBOARD_MODE_USER_1);
        lv_obj_clear_flag(s_tirtc_edit_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_tirtc_edit_keyboard);
    }
    if (code == LV_EVENT_VALUE_CHANGED) {
        display_update_tirtc_edit_feedback(NULL, lv_color_hex(0x0D8A59));
    }
}

static const lv_font_t *display_ascii_font(uint8_t size)
{
    if (size <= 12U) {
#if LV_FONT_MONTSERRAT_12
        return &lv_font_montserrat_12;
#endif
    }
    if (size >= 18U) {
#if LV_FONT_MONTSERRAT_20
        return &lv_font_montserrat_20;
#endif
    }
#if LV_FONT_MONTSERRAT_14
    return &lv_font_montserrat_14;
#else
    return LV_FONT_DEFAULT;
#endif
}

static const lv_font_t *display_cjk_font(void)
{
    return ai_chat_font_get_current();
}

static const lv_font_t *display_ai_chat_font(void)
{
    return display_cjk_font();
}

static void display_apply_ai_dialog_font_one(lv_obj_t *label)
{
    if (label != NULL && lv_obj_check_type(label, &lv_label_class)) {
        lv_obj_set_style_text_font(label, display_ai_chat_font(), 0);
        lv_obj_invalidate(label);
    }
}

static void display_apply_ai_dialog_font_group(lv_obj_t *label, lv_obj_t **bold_labels)
{
    display_apply_ai_dialog_font_one(label);
    if (bold_labels != NULL) {
        for (size_t index = 0; index < DISPLAY_AI_CHAT_CAPTION_BOLD_LAYER_COUNT; ++index) {
            display_apply_ai_dialog_font_one(bold_labels[index]);
        }
    }
}

static void display_apply_ai_dialog_font_if_ready(void)
{
    bool external_ready = ai_chat_font_is_ready();
    if (external_ready == s_ai_dialog_external_font_applied) {
        return;
    }

    for (size_t index = 0; index < DISPLAY_AI_CHAT_MESSAGE_VISIBLE_MAX; ++index) {
        display_apply_ai_dialog_font_group(s_ai_message_labels[index],
                                           s_ai_message_bold_labels[index]);
    }
    display_apply_ai_dialog_font_one(s_ai_single_caption_label);
    display_apply_ai_dialog_font_one(s_ai_new_chat_btn_label);
    s_ai_dialog_external_font_applied = external_ready;
}

static display_text_image_ctx_t *display_text_image_ctx(lv_obj_t *obj)
{
    if (obj == NULL || !lv_obj_check_type(obj, &lv_img_class)) {
        return NULL;
    }

    display_text_image_ctx_t *ctx = (display_text_image_ctx_t *)lv_obj_get_user_data(obj);
    return ctx != NULL && ctx->magic == DISPLAY_TEXT_IMAGE_MAGIC ? ctx : NULL;
}

static void display_text_image_delete_cb(lv_event_t *event)
{
    lv_obj_t *target = lv_event_get_target(event);
    display_text_image_ctx_t *ctx = display_text_image_ctx(target);

    if (ctx != NULL) {
        lv_obj_set_user_data(target, NULL);
        free(ctx);
    }
}

static lv_obj_t *display_create_text_asset_obj(lv_obj_t *parent,
                                               lv_coord_t x,
                                               lv_coord_t y,
                                               lv_coord_t width,
                                               lv_color_t color,
                                               uint8_t font_size,
                                               lv_text_align_t align)
{
    display_text_image_ctx_t *ctx = (display_text_image_ctx_t *)display_calloc_psram(1, sizeof(*ctx));
    if (ctx == NULL) {
        return NULL;
    }

    lv_obj_t *img = lv_img_create(parent);

    ctx->magic = DISPLAY_TEXT_IMAGE_MAGIC;
    ctx->x = x;
    ctx->y = y;
    ctx->width = width;
    ctx->font_size = font_size;
    ctx->align = align;
    ctx->color = color;
    lv_obj_set_user_data(img, ctx);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(img, display_text_image_delete_cb, LV_EVENT_DELETE, NULL);
    return img;
}

static void display_apply_text_asset(lv_obj_t *obj,
                                     const ui_text_asset_t *asset,
                                     display_text_image_ctx_t *ctx)
{
    if (obj == NULL || asset == NULL || asset->image == NULL || ctx == NULL) {
        return;
    }

    lv_coord_t x = ctx->x;
    lv_coord_t y = ctx->y + asset->y_offset;
    lv_coord_t image_width = (lv_coord_t)asset->image->header.w;
    lv_coord_t image_height = (lv_coord_t)asset->image->header.h;

    if (ctx->align == LV_TEXT_ALIGN_CENTER) {
        x += (ctx->width - image_width) / 2;
    } else if (ctx->align == LV_TEXT_ALIGN_RIGHT) {
        x += ctx->width - image_width;
    } else {
        x += asset->x_offset;
    }

    lv_img_set_src(obj, asset->image);
    lv_img_set_pivot(obj, 0, 0);
    lv_img_set_zoom(obj, LV_IMG_ZOOM_NONE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, image_width, image_height);
    lv_obj_set_style_img_recolor(obj, ctx->color, 0);
    lv_obj_set_style_img_recolor_opa(obj, LV_OPA_COVER, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    ctx->current_asset = asset;
    ctx->layout_dirty = false;
}

static void display_text_set(lv_obj_t *obj, const char *text)
{
    if (obj == NULL) {
        return;
    }

    display_text_image_ctx_t *ctx = display_text_image_ctx(obj);
    if (ctx != NULL) {
        const ui_text_asset_t *asset = ui_text_asset_find(text, ctx->font_size);
        if (asset != NULL) {
            if (ctx->current_asset == asset &&
                !ctx->layout_dirty &&
                !lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
                return;
            }
            display_apply_text_asset(obj, asset, ctx);
        } else {
            if (ctx->current_asset == NULL &&
                !ctx->layout_dirty &&
                lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
                return;
            }
            ctx->current_asset = NULL;
            ctx->layout_dirty = false;
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char *next_text = text != NULL ? text : "";
        const char *current_text = lv_label_get_text(obj);
        if (current_text != NULL && strcmp(current_text, next_text) == 0) {
            return;
        }
        lv_label_set_text(obj, next_text);
    }
}

static void display_text_set_color(lv_obj_t *obj, lv_color_t color, lv_style_selector_t selector)
{
    if (obj == NULL) {
        return;
    }

    display_text_image_ctx_t *ctx = display_text_image_ctx(obj);
    if (ctx != NULL) {
        if (selector == 0) {
            ctx->color = color;
        }
        lv_obj_set_style_img_recolor(obj, color, selector);
        lv_obj_set_style_img_recolor_opa(obj, LV_OPA_COVER, selector);
        return;
    }

    lv_obj_set_style_text_color(obj, color, selector);
}

static void display_text_set_layout(lv_obj_t *obj,
                                    lv_coord_t x,
                                    lv_coord_t y,
                                    lv_coord_t width,
                                    lv_text_align_t align)
{
    lv_coord_t scaled_x = display_scale_x(x);
    lv_coord_t scaled_y = display_scale_y(y);
    lv_coord_t scaled_width = display_scale_x(width);
    display_text_image_ctx_t *ctx = display_text_image_ctx(obj);
    if (ctx != NULL) {
        ctx->x = scaled_x;
        ctx->y = scaled_y;
        ctx->width = scaled_width;
        ctx->align = align;
        ctx->layout_dirty = true;
        return;
    }

    lv_obj_set_pos(obj, scaled_x, scaled_y);
    lv_obj_set_width(obj, scaled_width);
    lv_obj_set_style_text_align(obj, align, 0);
}

static const char *display_tirtc_config_field_title(display_tirtc_config_field_t field)
{
    switch (field) {
    case DISPLAY_TIRTC_CONFIG_FIELD_DEVICE_SECRET:
        return "Binding";
    case DISPLAY_TIRTC_CONFIG_FIELD_TOKEN_SUBJECT:
        return "Token Subject";
    case DISPLAY_TIRTC_CONFIG_FIELD_ACCESS_KEY_ID:
        return "Token API";
    case DISPLAY_TIRTC_CONFIG_FIELD_ACCESS_KEY_SECRET:
        return "Credential";
    case DISPLAY_TIRTC_CONFIG_FIELD_DEVICE_ID:
    default:
        return "Device ID";
    }
}

static const char *display_tirtc_binding_text(const display_status_t *status)
{
    static char text[64];
    bool has_code = false;

    if (status == NULL) {
        return "Unknown";
    }

    has_code = status->binding_code[0] != '\0';
    switch (status->binding_state) {
    case DISPLAY_DEVICE_BINDING_STATE_REPORTING:
        if (has_code) {
            snprintf(text, sizeof(text), "Code %s", status->binding_code);
            return text;
        }
        return "Reporting";
    case DISPLAY_DEVICE_BINDING_STATE_WAITING_USER:
        if (has_code) {
            snprintf(text, sizeof(text), "Code %s", status->binding_code);
            return text;
        }
        return "Waiting user";
    case DISPLAY_DEVICE_BINDING_STATE_BOUND:
        return "Bound";
    case DISPLAY_DEVICE_BINDING_STATE_ERROR:
        if (has_code) {
            snprintf(text, sizeof(text), "Code %s", status->binding_code);
            return text;
        }
        return status->binding_message[0] != '\0' ? status->binding_message : "Binding failed";
    case DISPLAY_DEVICE_BINDING_STATE_IDLE:
        return "Idle";
    case DISPLAY_DEVICE_BINDING_STATE_DISABLED:
    default:
        return "Disabled";
    }
}

static const char *display_tirtc_config_field_value(const display_status_t *status,
                                                    display_tirtc_config_field_t field)
{
    switch (field) {
    case DISPLAY_TIRTC_CONFIG_FIELD_DEVICE_SECRET:
        return display_tirtc_binding_text(status);
    case DISPLAY_TIRTC_CONFIG_FIELD_TOKEN_SUBJECT:
        return status != NULL && status->tirtc_token_subject[0] != '\0' ?
            status->tirtc_token_subject : "Not set";
    case DISPLAY_TIRTC_CONFIG_FIELD_ACCESS_KEY_ID:
        return status != NULL && status->tirtc_server_api[0] != '\0' ?
            status->tirtc_server_api : "Service issued";
    case DISPLAY_TIRTC_CONFIG_FIELD_ACCESS_KEY_SECRET:
        return "Managed by binding";
    case DISPLAY_TIRTC_CONFIG_FIELD_DEVICE_ID:
    default:
        return status != NULL && status->tirtc_device_id[0] != '\0' ?
            status->tirtc_device_id : "Unbound";
    }
}

static size_t display_tirtc_config_field_max_len(display_tirtc_config_field_t field)
{
    return field == DISPLAY_TIRTC_CONFIG_FIELD_TOKEN_SUBJECT ?
        DISPLAY_TIRTC_CONFIG_TOKEN_SUBJECT_MAX - 1U :
        DISPLAY_TIRTC_CONFIG_TEXT_MAX - 1U;
}

static void display_update_call_add_edit_feedback(const char *status_text, lv_color_t status_color)
{
    const char *value = s_call_add_edit_ta != NULL ? lv_textarea_get_text(s_call_add_edit_ta) : "";
    size_t value_len = strlen(value);
    size_t max_len = display_call_add_field_max_len(s_call_add_edit_field);

    if (s_call_add_edit_length_label != NULL) {
        lv_label_set_text_fmt(s_call_add_edit_length_label,
                              "%u/%u",
                              (unsigned)value_len,
                              (unsigned)max_len);
    }

    if (s_call_add_edit_status_label == NULL) {
        return;
    }

    if (status_text == NULL) {
        if (value_len > max_len) {
            status_text = "内容不合法";
            status_color = lv_color_hex(0xE45656);
        } else {
            status_text = "点击保存生效";
            status_color = lv_color_hex(0x0D8A59);
        }
    }

    display_text_set_color(s_call_add_edit_status_label, status_color, 0);
    display_text_set(s_call_add_edit_status_label, status_text);
}

static void display_update_wechat_add_edit_feedback(const char *status_text, lv_color_t status_color)
{
    const char *value = s_wechat_add_edit_ta != NULL ? lv_textarea_get_text(s_wechat_add_edit_ta) : "";
    size_t value_len = strlen(value);

    if (s_wechat_add_edit_length_label != NULL) {
        lv_label_set_text_fmt(s_wechat_add_edit_length_label,
                              "%u/%u",
                              (unsigned)value_len,
                              (unsigned)DISPLAY_WECHAT_OPEN_ID_LENGTH);
    }

    if (s_wechat_add_edit_status_label == NULL) {
        return;
    }

    if (status_text == NULL) {
        if (value_len == 0) {
            status_text = "请输入微信Open ID";
            status_color = lv_color_hex(0x64758A);
        } else if (value_len != DISPLAY_WECHAT_OPEN_ID_LENGTH) {
            status_text = "必须是28位微信Open ID";
            status_color = lv_color_hex(0xE45656);
        } else {
            status_text = "点击保存生效";
            status_color = lv_color_hex(0x0D8A59);
        }
    }

    display_text_set_color(s_wechat_add_edit_status_label, status_color, 0);
    display_text_set(s_wechat_add_edit_status_label, status_text);
}

static void display_update_tirtc_edit_feedback(const char *status_text, lv_color_t status_color)
{
    const char *value = s_tirtc_edit_ta != NULL ? lv_textarea_get_text(s_tirtc_edit_ta) : "";
    size_t value_len = strlen(value);
    size_t max_len = display_tirtc_config_field_max_len(s_tirtc_edit_field);

    if (s_tirtc_edit_length_label != NULL) {
        lv_label_set_text_fmt(s_tirtc_edit_length_label,
                              "%u/%u",
                              (unsigned)value_len,
                              (unsigned)max_len);
    }

    if (s_tirtc_edit_status_label == NULL) {
        return;
    }

    if (status_text == NULL) {
        if (value_len == 0) {
            status_text = "不能为空";
            status_color = lv_color_hex(0xE45656);
        } else {
            status_text = "点击保存生效";
            status_color = lv_color_hex(0x0D8A59);
        }
    }

    display_text_set_color(s_tirtc_edit_status_label, status_color, 0);
    display_text_set(s_tirtc_edit_status_label, status_text);
}

static void display_prepare_figma_page(lv_obj_t *page)
{
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, DISPLAY_DRIVER_WIDTH, DISPLAY_DRIVER_HEIGHT);
    lv_obj_set_style_bg_color(page, lv_color_hex(DISPLAY_UI_COLOR_PAGE_BG), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *display_create_scaled_text(lv_obj_t *parent,
                                            const char *text,
                                            lv_coord_t x,
                                            lv_coord_t y,
                                            lv_coord_t width,
                                            lv_color_t color,
                                            uint8_t font_size,
                                            lv_text_align_t align,
                                            bool native)
{
    lv_coord_t scaled_x = native ? display_native_scale_x(x) : display_scale_x(x);
    lv_coord_t scaled_y = native ? display_native_scale_y(y) : display_scale_y(y);
    lv_coord_t scaled_width = native ? display_native_scale_x(width) : display_scale_x(width);
    const ui_text_asset_t *asset = ui_text_asset_find(text, font_size);
    if (asset != NULL) {
        lv_obj_t *img = display_create_text_asset_obj(parent,
                                                      scaled_x,
                                                      scaled_y,
                                                      scaled_width,
                                                      color,
                                                      font_size,
                                                      align);
        if (img != NULL) {
            display_text_image_ctx_t *ctx = display_text_image_ctx(img);
            display_apply_text_asset(img, asset, ctx);
            return img;
        }
    }

    if (ui_text_asset_has_cjk(text)) {
        lv_obj_t *label = lv_label_create(parent);

        lv_obj_set_pos(label, scaled_x, scaled_y);
        lv_obj_set_width(label, scaled_width);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(label, color, 0);
        lv_obj_set_style_text_align(label, align, 0);
        lv_obj_set_style_text_font(label, display_cjk_font(), 0);
        lv_obj_clear_flag(label, LV_OBJ_FLAG_SCROLLABLE);
        display_text_set(label, text != NULL ? text : "");
        ESP_LOGD(TAG,
                 "text asset fallback to live font: text=\"%s\" size=%u",
                 text != NULL ? text : "",
                 (unsigned)font_size);
        return label;
    }

    lv_obj_t *label = lv_label_create(parent);

    lv_obj_set_pos(label, scaled_x, scaled_y);
    lv_obj_set_width(label, scaled_width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    display_text_set_color(label, color, 0);
    lv_obj_set_style_text_align(label, align, 0);
    lv_obj_set_style_text_font(label, display_ascii_font(font_size), 0);
    display_text_set(label, text != NULL ? text : "");
    return label;
}

static lv_obj_t *display_create_figma_text(lv_obj_t *parent,
                                           const char *text,
                                           lv_coord_t x,
                                           lv_coord_t y,
                                           lv_coord_t width,
                                           lv_color_t color,
                                           uint8_t font_size,
                                           lv_text_align_t align)
{
    return display_create_scaled_text(parent,
                                      text,
                                      x,
                                      y,
                                      width,
                                      color,
                                      font_size,
                                      align,
                                      false);
}

static lv_obj_t *display_create_native_text(lv_obj_t *parent,
                                            const char *text,
                                            lv_coord_t x,
                                            lv_coord_t y,
                                            lv_coord_t width,
                                            lv_color_t color,
                                            uint8_t font_size,
                                            lv_text_align_t align)
{
    return display_create_scaled_text(parent,
                                      text,
                                      x,
                                      y,
                                      width,
                                      color,
                                      font_size,
                                      align,
                                      true);
}

static lv_obj_t *display_create_native_live_text(lv_obj_t *parent,
                                                 const char *text,
                                                 lv_coord_t x,
                                                 lv_coord_t y,
                                                 lv_coord_t width,
                                                 lv_color_t color,
                                                 lv_text_align_t align)
{
    lv_obj_t *label = lv_label_create(parent);

    display_obj_set_native_pos(label, x, y);
    lv_obj_set_width(label, display_native_scale_x(width));
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_align(label, align, 0);
    lv_obj_set_style_text_font(label, display_cjk_font(), 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_SCROLLABLE);
    display_text_set(label, text != NULL ? text : "");
    return label;
}

static void display_wechat_remark_textarea_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t *target = lv_event_get_target(event);

    if (code == LV_EVENT_FOCUSED) {
        display_show_text_keyboard(s_wechat_remark_keyboard, target);
    }
    if (code == LV_EVENT_VALUE_CHANGED && s_wechat_remark_status_label != NULL) {
        display_text_set_color(s_wechat_remark_status_label, lv_color_hex(0x0D8A59), 0);
        display_text_set(s_wechat_remark_status_label, "点击保存生效");
    }
}

static lv_obj_t *display_create_figma_live_text(lv_obj_t *parent,
                                                const char *text,
                                                lv_coord_t x,
                                                lv_coord_t y,
                                                lv_coord_t width,
                                                lv_color_t color,
                                                lv_text_align_t align)
{
    lv_obj_t *label = lv_label_create(parent);

    display_obj_set_design_pos(label, x, y);
    lv_obj_set_width(label, display_scale_x(width));
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_align(label, align, 0);
    lv_obj_set_style_text_font(label, display_cjk_font(), 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_SCROLLABLE);
    display_text_set(label, text != NULL ? text : "");
    return label;
}

static lv_obj_t *display_create_ai_text(lv_obj_t *parent,
                                        const char *text,
                                        lv_coord_t x,
                                        lv_coord_t y,
                                        lv_coord_t width,
                                        lv_color_t color,
                                        lv_text_align_t align)
{
    return display_create_figma_text(parent, text, x, y, width, color, 12, align);
}

static lv_obj_t *display_create_ai_dialog_text(lv_obj_t *parent,
                                               const char *text,
                                               lv_coord_t x,
                                               lv_coord_t y,
                                               lv_coord_t width,
                                               lv_color_t color,
                                               lv_text_align_t align)
{
    lv_obj_t *label = lv_label_create(parent);

    lv_obj_set_pos(label, display_scale_x(x), display_scale_y(y));
    lv_obj_set_width(label, display_scale_x(width));
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    display_text_set_color(label, color, 0);
    lv_obj_set_style_text_align(label, align, 0);
    lv_obj_set_style_text_font(label, display_ai_chat_font(), 0);
    lv_obj_set_style_text_opa(label, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(label, LV_OPA_COVER, 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_SCROLLABLE);
    display_text_set(label, text != NULL ? text : "");
    return label;
}

static __attribute__((unused)) lv_obj_t *display_create_ai_chat_caption_text(lv_obj_t *parent,
                                                     const char *text,
                                                     lv_coord_t x,
                                                     lv_coord_t y,
                                                     lv_coord_t width,
                                                     lv_color_t color,
                                                     lv_text_align_t align,
                                                     lv_obj_t **bold_labels)
{
    if (bold_labels != NULL) {
        for (size_t index = 0; index < DISPLAY_AI_CHAT_CAPTION_BOLD_LAYER_COUNT; ++index) {
            bold_labels[index] = NULL;
        }
    }

    lv_obj_t *label = display_create_ai_dialog_text(parent, text, x, y, width, color, align);
    if (label != NULL) {
        lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(label);
    }
    return label;
}

static void display_set_ai_chat_caption_label_text(lv_obj_t *label, const char *text)
{
    if (label == NULL) {
        return;
    }
    lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_opa(label, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(label, LV_OPA_COVER, 0);
    display_text_set(label, text);
    lv_obj_invalidate(label);
}

static void display_set_ai_chat_caption_text(lv_obj_t *label,
                                             lv_obj_t **bold_labels,
                                             const char *text)
{
    if (bold_labels != NULL) {
        for (size_t index = 0; index < DISPLAY_AI_CHAT_CAPTION_BOLD_LAYER_COUNT; ++index) {
            if (bold_labels[index] != NULL) {
                display_set_ai_chat_caption_label_text(bold_labels[index], text);
            }
        }
    }
    display_set_ai_chat_caption_label_text(label, text);
}

static void __attribute__((unused)) display_set_ai_chat_caption_long_mode(lv_obj_t *label,
                                                                          lv_obj_t **bold_labels)
{
    if (label != NULL) {
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_line_space(label, display_scale_y(DISPLAY_AI_CHAT_TEXT_LINE_SPACE), 0);
    }
    if (bold_labels != NULL) {
        for (size_t index = 0; index < DISPLAY_AI_CHAT_CAPTION_BOLD_LAYER_COUNT; ++index) {
            if (bold_labels[index] != NULL) {
                lv_label_set_long_mode(bold_labels[index], LV_LABEL_LONG_WRAP);
                lv_obj_set_style_text_line_space(bold_labels[index],
                                                 display_scale_y(DISPLAY_AI_CHAT_TEXT_LINE_SPACE),
                                                 0);
            }
        }
    }
}

static void display_measure_ai_chat_bubble_text(const char *text,
                                                lv_coord_t *text_width,
                                                lv_coord_t *text_height)
{
    const lv_font_t *font = display_ai_chat_font();
    lv_point_t natural = {0};
    lv_point_t wrapped = {0};
    lv_coord_t text_max_width = display_scale_x(DISPLAY_AI_CHAT_TEXT_MAX_WIDTH);
    lv_coord_t width = display_scale_x(DISPLAY_AI_CHAT_MIN_TEXT_WIDTH);
    lv_coord_t height = font != NULL ? font->line_height : 16;

    if (text_width == NULL || text_height == NULL) {
        return;
    }
    if (text == NULL || text[0] == '\0') {
        *text_width = width;
        *text_height = height;
        return;
    }

    lv_txt_get_size(&natural,
                    text,
                    font,
                    0,
                    display_scale_y(DISPLAY_AI_CHAT_TEXT_LINE_SPACE),
                    LV_COORD_MAX,
                    LV_TEXT_FLAG_NONE);
    if (natural.x + display_scale_x(DISPLAY_AI_CHAT_TEXT_SAFE_WIDTH) <= text_max_width) {
        width = natural.x < display_scale_x(DISPLAY_AI_CHAT_MIN_TEXT_WIDTH)
                    ? display_scale_x(DISPLAY_AI_CHAT_MIN_TEXT_WIDTH)
                    : natural.x;
        height = natural.y;
    } else {
        lv_txt_get_size(&wrapped,
                        text,
                        font,
                        0,
                        display_scale_y(DISPLAY_AI_CHAT_TEXT_LINE_SPACE),
                        text_max_width,
                        LV_TEXT_FLAG_NONE);
        width = text_max_width;
        height = wrapped.y;
    }

    if (width + display_scale_x(DISPLAY_AI_CHAT_TEXT_SAFE_WIDTH) <= text_max_width) {
        width += display_scale_x(DISPLAY_AI_CHAT_TEXT_SAFE_WIDTH);
    }
    if (font != NULL && height < font->line_height) {
        height = font->line_height;
    }
    height += display_scale_y(DISPLAY_AI_CHAT_TEXT_SAFE_HEIGHT);

    *text_width = width;
    *text_height = height;
}

static void display_layout_ai_chat_caption_label(lv_obj_t *label,
                                                 lv_coord_t text_width,
                                                 lv_coord_t text_height,
                                                 lv_color_t text_color)
{
    if (label == NULL) {
        return;
    }

    lv_obj_set_pos(label,
                   display_scale_x(DISPLAY_AI_CHAT_BUBBLE_TEXT_X),
                   display_scale_y(DISPLAY_AI_CHAT_BUBBLE_TEXT_Y));
    lv_obj_set_size(label, text_width, text_height);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_line_space(label, display_scale_y(DISPLAY_AI_CHAT_TEXT_LINE_SPACE), 0);
    display_text_set_color(label, text_color, 0);
}

static uint32_t display_ai_chat_hash_text(const char *text)
{
    uint32_t hash = 2166136261UL;

    if (text == NULL) {
        return hash;
    }

    for (size_t index = 0; text[index] != '\0'; ++index) {
        hash ^= (uint8_t)text[index];
        hash *= 16777619UL;
    }
    return hash;
}

static void display_hide_ai_chat_message_slot(uint8_t slot)
{
    if (slot >= DISPLAY_AI_CHAT_MESSAGE_VISIBLE_MAX) {
        return;
    }

    if (s_ai_message_boxes[slot] != NULL) {
        lv_obj_add_flag(s_ai_message_boxes[slot], LV_OBJ_FLAG_HIDDEN);
    }
    s_ai_visible_message_indices[slot] = UINT8_MAX;
    s_ai_visible_message_generations[slot] = 0;
}

static void display_reset_ai_chat_visible_slots(void)
{
    for (uint8_t slot = 0; slot < DISPLAY_AI_CHAT_MESSAGE_VISIBLE_MAX; ++slot) {
        display_hide_ai_chat_message_slot(slot);
    }
}

static void display_apply_ai_chat_caption_bubble_layout(uint8_t slot,
                                                        const display_ai_message_layout_t *layout,
                                                        const char *text)
{
    lv_obj_t *box = NULL;
    lv_obj_t *label = NULL;
    lv_obj_t **bold_labels = NULL;
    lv_color_t fill = lv_color_hex(0x2F82D7);
    lv_color_t text_color = lv_color_hex(0xFFFFFF);

    if (slot >= DISPLAY_AI_CHAT_MESSAGE_VISIBLE_MAX || layout == NULL) {
        return;
    }

    box = s_ai_message_boxes[slot];
    label = s_ai_message_labels[slot];
    bold_labels = s_ai_message_bold_labels[slot];
    if (box == NULL || label == NULL || text == NULL || text[0] == '\0') {
        display_hide_ai_chat_message_slot(slot);
        return;
    }

    fill = layout->align_right ? lv_color_hex(0x21C783) : lv_color_hex(0x2F82D7);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(box, layout->x, layout->y);
    lv_obj_set_size(box, layout->bubble_width, layout->bubble_height);
    lv_obj_set_style_radius(box, display_scale_square(DISPLAY_AI_CHAT_BUBBLE_RADIUS), 0);
    lv_obj_set_style_bg_color(box, fill, 0);
    lv_obj_set_style_border_color(box, fill, 0);

    display_layout_ai_chat_caption_label(label,
                                         layout->text_width,
                                         layout->text_height,
                                         text_color);
    for (size_t index = 0; index < DISPLAY_AI_CHAT_CAPTION_BOLD_LAYER_COUNT; ++index) {
        display_layout_ai_chat_caption_label(bold_labels[index],
                                             layout->text_width,
                                             layout->text_height,
                                             text_color);
    }
    display_set_ai_chat_caption_text(label, bold_labels, text);
    s_ai_visible_message_indices[slot] = layout->message_index;
    s_ai_visible_message_generations[slot] = s_ai_message_layout_generation;
}

static bool __attribute__((unused)) display_ai_chat_layout_cache_needs_rebuild(
    const display_status_t *status,
    uint8_t message_count,
    bool show_new_chat_button,
    bool font_ready)
{
    uint8_t cached_index = 0;

    if (status == NULL ||
        s_ai_message_layout_new_button_visible != show_new_chat_button ||
        s_ai_message_layout_font_ready != font_ready) {
        return true;
    }

    for (uint8_t index = 0; index < message_count; ++index) {
        const display_ai_chat_message_t *message = &status->ai_chat_messages[index];
        if (message->text[0] == '\0') {
            continue;
        }
        if (cached_index >= s_ai_message_layout_count) {
            return true;
        }
        const display_ai_message_layout_t *layout = &s_ai_message_layouts[cached_index];
        if (layout->message_index != index ||
            layout->caption_type != message->caption_type ||
            layout->utterance_id != message->utterance_id ||
            layout->text_hash != display_ai_chat_hash_text(message->text)) {
            return true;
        }
        cached_index++;
    }

    return cached_index != s_ai_message_layout_count;
}

static void __attribute__((unused)) display_rebuild_ai_chat_layout_cache(
    const display_status_t *status,
    uint8_t message_count,
    bool show_new_chat_button,
    bool font_ready)
{
    lv_coord_t next_y = display_scale_y(DISPLAY_AI_CHAT_BUBBLE_TOP_Y);
    uint8_t layout_count = 0;
    lv_coord_t content_width = display_scale_x(DISPLAY_AI_CHAT_CONTENT_WIDTH);
    lv_coord_t text_pad_left = display_scale_x(DISPLAY_AI_CHAT_BUBBLE_TEXT_X);
    lv_coord_t text_pad_right = display_scale_x(DISPLAY_AI_CHAT_BUBBLE_PAD_RIGHT);
    lv_coord_t text_pad_top = display_scale_y(DISPLAY_AI_CHAT_BUBBLE_TEXT_Y);
    lv_coord_t text_pad_bottom = display_scale_y(DISPLAY_AI_CHAT_BUBBLE_PAD_BOTTOM);
    lv_coord_t bubble_gap_y = display_scale_y(DISPLAY_AI_CHAT_BUBBLE_GAP_Y);
    lv_coord_t bubble_left_x = display_scale_x(DISPLAY_AI_CHAT_BUBBLE_LEFT_X);

    if (status == NULL) {
        return;
    }

    for (uint8_t index = 0;
         index < message_count && layout_count < DISPLAY_AI_CHAT_MESSAGE_MAX;
         ++index) {
        const display_ai_chat_message_t *message = &status->ai_chat_messages[index];
        lv_coord_t text_width = 0;
        lv_coord_t text_height = 0;
        lv_coord_t bubble_width = 0;
        lv_coord_t bubble_height = 0;
        bool align_right = false;

        if (message->text[0] == '\0') {
            continue;
        }

        display_measure_ai_chat_bubble_text(message->text, &text_width, &text_height);
        bubble_width = text_width + text_pad_left + text_pad_right;
        bubble_height = text_height + text_pad_top + text_pad_bottom;
        align_right = message->caption_type == DISPLAY_AI_CHAT_CAPTION_TYPE_ASR;

        display_ai_message_layout_t *layout = &s_ai_message_layouts[layout_count++];
        layout->message_index = index;
        layout->caption_type = message->caption_type;
        layout->utterance_id = message->utterance_id;
        layout->text_hash = display_ai_chat_hash_text(message->text);
        layout->align_right = align_right;
        layout->text_width = text_width;
        layout->text_height = text_height;
        layout->bubble_width = bubble_width;
        layout->bubble_height = bubble_height;
        layout->x = align_right
                        ? content_width - bubble_left_x - bubble_width
                        : bubble_left_x;
        layout->y = next_y;
        next_y += bubble_height + bubble_gap_y;
    }

    s_ai_message_layout_count = layout_count;
    s_ai_new_chat_button_y = next_y;
    if (show_new_chat_button) {
        next_y += display_scale_y(DISPLAY_AI_CHAT_NEW_BUTTON_HEIGHT) + bubble_gap_y;
    }
    s_ai_message_content_height = next_y < display_scale_y(DISPLAY_AI_CHAT_VIEWPORT_HEIGHT)
                                      ? display_scale_y(DISPLAY_AI_CHAT_VIEWPORT_HEIGHT)
                                      : next_y;
    s_ai_message_layout_new_button_visible = show_new_chat_button;
    s_ai_message_layout_font_ready = font_ready;
    s_ai_message_layout_generation++;
    if (s_ai_message_layout_generation == 0U) {
        s_ai_message_layout_generation = 1U;
    }
    display_reset_ai_chat_visible_slots();
}

static void display_update_ai_chat_scroll_spacer(void)
{
    if (s_ai_message_scroll_spacer == NULL) {
        return;
    }

    lv_obj_set_pos(s_ai_message_scroll_spacer, 0, 0);
    lv_obj_set_size(s_ai_message_scroll_spacer, 1, s_ai_message_content_height);
}

static void display_clear_ai_chat_message_view(void)
{
    if (s_actions.on_clear_ai_chat_messages != NULL) {
        (void)s_actions.on_clear_ai_chat_messages(s_actions.ctx);
    }

    if (s_last_status_ptr != NULL) {
        s_last_status.ai_chat_asr_caption[0] = '\0';
        s_last_status.ai_chat_tts_caption[0] = '\0';
        s_last_status.ai_chat_message_count = 0;
        memset(s_last_status.ai_chat_messages, 0, sizeof(s_last_status.ai_chat_messages));
    }

    s_ai_message_layout_count = 0;
    s_ai_message_content_height = display_scale_y(DISPLAY_AI_CHAT_VIEWPORT_HEIGHT);
    s_ai_new_chat_button_y = display_scale_y(DISPLAY_AI_CHAT_BUBBLE_TOP_Y);
    s_ai_message_layout_new_button_visible = false;
    s_ai_message_layout_font_ready = ai_chat_font_is_ready();
    s_ai_message_touching = false;
    s_ai_message_layout_generation++;
    if (s_ai_message_layout_generation == 0U) {
        s_ai_message_layout_generation = 1U;
    }

    display_reset_ai_chat_visible_slots();
    display_update_ai_chat_scroll_spacer();
    if (s_ai_new_chat_btn != NULL) {
        lv_obj_add_flag(s_ai_new_chat_btn, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_ai_message_list != NULL) {
        lv_obj_scroll_to_y(s_ai_message_list, 0, LV_ANIM_OFF);
    }
}

static void display_render_ai_chat_visible_messages(const display_status_t *status)
{
    lv_coord_t viewport_top = 0;
    lv_coord_t viewport_bottom = display_scale_y(DISPLAY_AI_CHAT_VIEWPORT_HEIGHT);
    uint8_t slot = 0;

    if (status == NULL || s_ai_message_list == NULL) {
        return;
    }

    viewport_top = lv_obj_get_scroll_y(s_ai_message_list) - DISPLAY_AI_CHAT_VIRTUAL_OVERSCAN;
    viewport_bottom = lv_obj_get_scroll_y(s_ai_message_list) +
                      display_scale_y(DISPLAY_AI_CHAT_VIEWPORT_HEIGHT) +
                      DISPLAY_AI_CHAT_VIRTUAL_OVERSCAN;

    for (uint8_t index = 0;
         index < s_ai_message_layout_count && slot < DISPLAY_AI_CHAT_MESSAGE_VISIBLE_MAX;
         ++index) {
        const display_ai_message_layout_t *layout = &s_ai_message_layouts[index];
        lv_coord_t bubble_bottom = layout->y + layout->bubble_height;
        if (bubble_bottom < viewport_top || layout->y > viewport_bottom) {
            continue;
        }

        const display_ai_chat_message_t *message = &status->ai_chat_messages[layout->message_index];
        if (s_ai_visible_message_indices[slot] != layout->message_index ||
            s_ai_visible_message_generations[slot] != s_ai_message_layout_generation) {
            display_apply_ai_chat_caption_bubble_layout(slot, layout, message->text);
        }
        slot++;
    }

    while (slot < DISPLAY_AI_CHAT_MESSAGE_VISIBLE_MAX) {
        display_hide_ai_chat_message_slot(slot++);
    }
}

static bool display_ai_chat_should_show_new_chat_button(const display_status_t *status)
{
    if (status == NULL || status->ai_chat_active || s_actions.on_start_ai_chat == NULL) {
        return false;
    }

    return status->ai_chat_state == DISPLAY_AI_CHAT_STATE_IDLE ||
           status->ai_chat_state == DISPLAY_AI_CHAT_STATE_ERROR;
}

static lv_coord_t __attribute__((unused)) display_update_ai_chat_new_chat_button(lv_coord_t y,
                                                                                 bool visible)
{
    if (s_ai_new_chat_btn == NULL) {
        return y;
    }
    if (!visible) {
        lv_obj_add_flag(s_ai_new_chat_btn, LV_OBJ_FLAG_HIDDEN);
        return y;
    }

    lv_coord_t button_width = display_scale_x(DISPLAY_AI_CHAT_NEW_BUTTON_WIDTH);
    lv_coord_t button_height = display_scale_y(DISPLAY_AI_CHAT_NEW_BUTTON_HEIGHT);
    lv_coord_t x = (display_scale_x(DISPLAY_AI_CHAT_CONTENT_WIDTH) - button_width) / 2;
    lv_obj_clear_flag(s_ai_new_chat_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_ai_new_chat_btn, x, y);
    lv_obj_set_size(s_ai_new_chat_btn,
                    button_width,
                    button_height);
    return y + button_height + display_scale_y(DISPLAY_AI_CHAT_BUBBLE_GAP_Y);
}

static bool __attribute__((unused)) display_ai_chat_message_list_should_follow_bottom(void)
{
    if (s_ai_message_list == NULL || s_ai_message_touching) {
        return false;
    }

    return lv_obj_get_scroll_bottom(s_ai_message_list) <= 4;
}

static void __attribute__((unused)) display_ai_chat_message_list_scroll_to_bottom(bool should_follow)
{
    if (!should_follow || s_ai_message_list == NULL) {
        return;
    }

    lv_obj_update_layout(s_ai_message_list);
    lv_obj_scroll_to_y(s_ai_message_list, LV_COORD_MAX, LV_ANIM_OFF);
}

static void __attribute__((unused)) display_ai_chat_message_list_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_PRESSED || code == LV_EVENT_SCROLL_BEGIN) {
        s_ai_message_touching = true;
    } else if (code == LV_EVENT_SCROLL) {
        display_render_ai_chat_visible_messages(&s_last_status);
    } else if (code == LV_EVENT_RELEASED ||
               code == LV_EVENT_PRESS_LOST ||
               code == LV_EVENT_SCROLL_END) {
        s_ai_message_touching = false;
        display_render_ai_chat_visible_messages(&s_last_status);
    }
}

static lv_obj_t *display_create_ai_static_text(lv_obj_t *parent,
                                               const char *text,
                                               lv_coord_t x,
                                               lv_coord_t y,
                                               lv_coord_t width,
                                               lv_color_t color,
                                               uint8_t font_size,
                                               lv_text_align_t align)
{
    lv_obj_t *label = lv_label_create(parent);

    (void)font_size;
    display_obj_set_native_pos(label, x, y);
    lv_obj_set_width(label, display_native_scale_x(width));
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(label, display_ai_chat_font(), 0);
    lv_obj_set_style_text_align(label, align, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_SCROLLABLE);
    display_text_set(label, text != NULL ? text : "");
    return label;
}

static lv_obj_t *display_create_ai_native_caption_text(lv_obj_t *parent,
                                                       const char *text,
                                                       lv_coord_t x,
                                                       lv_coord_t y,
                                                       lv_coord_t width,
                                                       lv_coord_t height,
                                                       lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);

    display_obj_set_native_pos(label, x, y);
    display_obj_set_native_size(label, width, height);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(label, display_ai_chat_font(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_line_space(label, 5, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_opa(label, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(label, LV_OPA_COVER, 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    display_text_set(label, text != NULL ? text : "");
    return label;
}

static lv_obj_t *display_create_figma_box(lv_obj_t *parent,
                                                   lv_coord_t x,
                                                   lv_coord_t y,
                                                   lv_coord_t width,
                                                   lv_coord_t height,
                                                   lv_color_t fill,
                                                   lv_color_t stroke,
                                                   lv_coord_t radius)
{
    lv_obj_t *box = lv_obj_create(parent);

    lv_obj_remove_style_all(box);
    display_obj_set_design_pos(box, x, y);
    display_obj_set_design_size(box, width, height);
    lv_obj_set_style_radius(box, display_scale_square(radius), 0);
    lv_obj_set_style_bg_color(box, fill, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, stroke, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    return box;
}

static lv_obj_t *display_create_native_box(lv_obj_t *parent,
                                           lv_coord_t x,
                                           lv_coord_t y,
                                           lv_coord_t width,
                                           lv_coord_t height,
                                           lv_color_t fill,
                                           lv_color_t stroke,
                                           lv_coord_t radius)
{
    lv_obj_t *box = lv_obj_create(parent);

    lv_obj_remove_style_all(box);
    display_obj_set_native_pos(box, x, y);
    display_obj_set_native_size(box, width, height);
    lv_obj_set_style_radius(box, display_native_scale_square(radius), 0);
    lv_obj_set_style_bg_color(box, fill, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, stroke, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    return box;
}

static lv_obj_t *display_create_figma_button(lv_obj_t *parent,
                                                      lv_coord_t x,
                                                      lv_coord_t y,
                                                      lv_coord_t width,
                                                      lv_coord_t height,
                                                      lv_color_t fill,
                                                      lv_color_t stroke,
                                                      const char *text,
                                                      lv_color_t text_color,
                                                      uint8_t font_size,
                                                      lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_t *label = NULL;

    lv_obj_remove_style_all(btn);
    display_obj_set_design_pos(btn, x, y);
    display_obj_set_design_size(btn, width, height);
    lv_obj_set_style_radius(btn, display_scale_square(DISPLAY_UI_RADIUS), 0);
    lv_obj_set_style_bg_color(btn, fill, 0);
    lv_obj_set_style_bg_color(btn, lv_color_darken(fill, 18), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, stroke, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(DISPLAY_UI_COLOR_BORDER_STRONG), LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(btn, 1, LV_STATE_PRESSED);
    lv_obj_set_style_opa(btn, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    if (display_scale_y(height) < DISPLAY_UI_MIN_TAP) {
        lv_obj_set_ext_click_area(btn, (DISPLAY_UI_MIN_TAP - display_scale_y(height)) / 2);
    }
    if (cb != NULL) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    }

    label = display_create_figma_text(btn,
                                      text,
                                      0,
                                      (height - 16) / 2,
                                      width,
                                      text_color,
                                      font_size,
                                      LV_TEXT_ALIGN_CENTER);
    if (label != NULL) {
        lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    }
    return btn;
}

static lv_obj_t *display_create_native_button(lv_obj_t *parent,
                                              lv_coord_t x,
                                              lv_coord_t y,
                                              lv_coord_t width,
                                              lv_coord_t height,
                                              lv_color_t fill,
                                              lv_color_t stroke,
                                              const char *text,
                                              lv_color_t text_color,
                                              uint8_t font_size,
                                              lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);

    lv_obj_remove_style_all(btn);
    display_obj_set_native_pos(btn, x, y);
    display_obj_set_native_size(btn, width, height);
    lv_obj_set_style_radius(btn, display_native_scale_square(DISPLAY_UI_RADIUS), 0);
    lv_obj_set_style_bg_color(btn, fill, 0);
    lv_obj_set_style_bg_color(btn, lv_color_darken(fill, 18), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, stroke, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(DISPLAY_UI_COLOR_BORDER_STRONG), LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(btn, 1, LV_STATE_PRESSED);
    lv_obj_set_style_opa(btn, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    if (display_native_scale_y(height) < DISPLAY_UI_MIN_TAP) {
        lv_obj_set_ext_click_area(btn, (DISPLAY_UI_MIN_TAP - display_native_scale_y(height)) / 2);
    }
    if (cb != NULL) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    }

    lv_obj_t *label = display_create_native_text(btn,
                                                 text,
                                                 0,
                                                 (height - 16) / 2,
                                                 width,
                                                 text_color,
                                                 font_size,
                                                 LV_TEXT_ALIGN_CENTER);
    if (label != NULL) {
        lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    }
    return btn;
}

static lv_obj_t *display_create_native_live_button(lv_obj_t *parent,
                                                   lv_coord_t x,
                                                   lv_coord_t y,
                                                   lv_coord_t width,
                                                   lv_coord_t height,
                                                   lv_color_t fill,
                                                   lv_color_t stroke,
                                                   const char *text,
                                                   lv_color_t text_color,
                                                   lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);

    lv_obj_remove_style_all(btn);
    display_obj_set_native_pos(btn, x, y);
    display_obj_set_native_size(btn, width, height);
    lv_obj_set_style_radius(btn, display_native_scale_square(DISPLAY_UI_RADIUS), 0);
    lv_obj_set_style_bg_color(btn, fill, 0);
    lv_obj_set_style_bg_color(btn, lv_color_darken(fill, 18), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, stroke, 0);
    lv_obj_set_style_border_color(btn,
                                  lv_color_hex(DISPLAY_UI_COLOR_BORDER_STRONG),
                                  LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(btn, 1, LV_STATE_PRESSED);
    lv_obj_set_style_opa(btn, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    if (display_native_scale_y(height) < DISPLAY_UI_MIN_TAP) {
        lv_obj_set_ext_click_area(btn, (DISPLAY_UI_MIN_TAP - display_native_scale_y(height)) / 2);
    }
    if (cb != NULL) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    }

    lv_obj_t *label = display_create_native_live_text(btn,
                                                       text,
                                                       0,
                                                       (height - 18) / 2,
                                                       width,
                                                       text_color,
                                                       LV_TEXT_ALIGN_CENTER);
    if (label != NULL) {
        lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    }
    return btn;
}

static lv_obj_t *display_create_wifi_signal_bar(lv_obj_t *parent,
                                                lv_coord_t x,
                                                lv_coord_t y,
                                                lv_coord_t height,
                                                lv_color_t color)
{
    lv_obj_t *bar = lv_obj_create(parent);

    lv_obj_remove_style_all(bar);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, 5, height);
    lv_obj_set_style_radius(bar, 2, 0);
    lv_obj_set_style_bg_color(bar, color, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return bar;
}

static lv_obj_t *display_create_wifi_x_line(lv_obj_t *parent,
                                            lv_coord_t x,
                                            lv_coord_t y,
                                            const lv_point_t *points)
{
    lv_obj_t *line = lv_line_create(parent);

    lv_line_set_points(line, points, 2);
    lv_obj_set_pos(line, x, y);
    lv_obj_set_style_line_width(line, 2, 0);
    lv_obj_set_style_line_color(line, lv_color_hex(0xF6494C), 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return line;
}

static uint8_t display_wifi_status_level(const display_status_t *status)
{
    if (status == NULL || !status->network_connected) {
        return 0;
    }
    if (status->network_rssi >= -60) {
        return 3;
    }
    if (status->network_rssi >= -75) {
        return 2;
    }
    return 1;
}

static display_wifi_indicator_t *display_wifi_indicator_alloc(lv_color_t active_color)
{
    display_wifi_indicator_t *indicator = NULL;

    for (size_t index = 0; index < s_wifi_indicator_count; ++index) {
        if (!s_wifi_indicators[index].active) {
            indicator = &s_wifi_indicators[index];
            break;
        }
    }

    if (indicator == NULL) {
        if (s_wifi_indicator_count >= DISPLAY_WIFI_INDICATOR_MAX) {
            ESP_LOGW(TAG, "wifi indicator capacity exhausted");
            return NULL;
        }

        indicator = &s_wifi_indicators[s_wifi_indicator_count++];
    }

    memset(indicator, 0, sizeof(*indicator));
    indicator->active = true;
    indicator->active_color = active_color;
    indicator->inactive_color = lv_color_hex(0xBCCAD8);
    return indicator;
}

static void display_wifi_indicator_owner_delete_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_DELETE) {
        return;
    }

    display_wifi_indicator_t *indicator = lv_event_get_user_data(event);
    if (indicator == NULL) {
        return;
    }

    memset(indicator, 0, sizeof(*indicator));
}

static bool display_wifi_indicator_prune_invalid_objects(display_wifi_indicator_t *indicator)
{
    bool has_valid_object = false;

    for (uint8_t index = 0; index < DISPLAY_WIFI_INDICATOR_BAR_COUNT; ++index) {
        if (indicator->bars[index] == NULL) {
            continue;
        }
        if (!lv_obj_is_valid(indicator->bars[index])) {
            indicator->bars[index] = NULL;
            continue;
        }
        has_valid_object = true;
    }

    for (uint8_t index = 0; index < 2; ++index) {
        if (indicator->x_lines[index] == NULL) {
            continue;
        }
        if (!lv_obj_is_valid(indicator->x_lines[index])) {
            indicator->x_lines[index] = NULL;
            continue;
        }
        has_valid_object = true;
    }

    if (!has_valid_object) {
        memset(indicator, 0, sizeof(*indicator));
    }

    return has_valid_object;
}

static void display_update_wifi_indicator(display_wifi_indicator_t *indicator,
                                          const display_status_t *status)
{
    uint8_t level = display_wifi_status_level(status);
    bool connected = level > 0;

    if (indicator == NULL || !indicator->active) {
        return;
    }
    if (!display_wifi_indicator_prune_invalid_objects(indicator)) {
        return;
    }

    if (indicator->status_valid &&
        indicator->connected == connected &&
        indicator->level == level) {
        return;
    }
    indicator->status_valid = true;
    indicator->connected = connected;
    indicator->level = level;

    for (uint8_t index = 0; index < DISPLAY_WIFI_INDICATOR_BAR_COUNT; ++index) {
        if (indicator->bars[index] == NULL) {
            continue;
        }

        lv_obj_set_style_bg_color(indicator->bars[index],
                                  (connected && index < level)
                                      ? indicator->active_color
                                      : indicator->inactive_color,
                                  0);
    }

    for (uint8_t index = 0; index < 2; ++index) {
        if (indicator->x_lines[index] == NULL) {
            continue;
        }
        if (connected) {
            lv_obj_add_flag(indicator->x_lines[index], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(indicator->x_lines[index], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void display_update_wifi_indicators(const display_status_t *status)
{
    for (size_t index = 0; index < s_wifi_indicator_count; ++index) {
        if (s_wifi_indicators[index].active) {
            display_update_wifi_indicator(&s_wifi_indicators[index], status);
        }
    }
}

static display_wifi_indicator_t *display_create_wifi_indicator(lv_obj_t *parent,
                                                              lv_coord_t x,
                                                              lv_coord_t y,
                                                              lv_color_t active_color)
{
    static const lv_point_t wifi_x_line_a[] = {
        {0, 0},
        {6, 6},
    };
    static const lv_point_t wifi_x_line_b[] = {
        {6, 0},
        {0, 6},
    };
    display_wifi_indicator_t *indicator = display_wifi_indicator_alloc(active_color);

    if (indicator == NULL) {
        return NULL;
    }

    indicator->bars[0] = display_create_wifi_signal_bar(parent, x, y + 12, 8, indicator->inactive_color);
    indicator->bars[1] = display_create_wifi_signal_bar(parent, x + 9, y + 6, 14, indicator->inactive_color);
    indicator->bars[2] = display_create_wifi_signal_bar(parent, x + 18, y, 20, indicator->inactive_color);
    indicator->x_lines[0] = display_create_wifi_x_line(parent, x + 25, y + 7, wifi_x_line_a);
    indicator->x_lines[1] = display_create_wifi_x_line(parent, x + 25, y + 7, wifi_x_line_b);
    lv_obj_add_event_cb(parent, display_wifi_indicator_owner_delete_cb, LV_EVENT_DELETE, indicator);
    display_update_wifi_indicator(indicator, &s_last_status);
    return indicator;
}

static void display_create_figma_signal(lv_obj_t *header)
{
    (void)display_create_wifi_indicator(header, 443, 14, lv_color_hex(DISPLAY_UI_COLOR_GREEN));
}

static lv_obj_t *display_create_figma_header_base(lv_obj_t *page,
                                                  const char *title,
                                                  lv_event_cb_t back_cb,
                                                  lv_coord_t title_x,
                                                  lv_coord_t title_width)
{
    lv_obj_t *header = display_create_native_box(page,
                                                 0,
                                                 0,
                                                 DISPLAY_NATIVE_WIDTH,
                                                 DISPLAY_UI_HEADER_HEIGHT,
                                                 lv_color_hex(DISPLAY_UI_COLOR_SURFACE_SOFT),
                                                 lv_color_hex(DISPLAY_UI_COLOR_BORDER),
                                                 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);

    if (back_cb != NULL) {
        lv_obj_t *back_btn = lv_btn_create(header);
        lv_obj_remove_style_all(back_btn);
        display_obj_set_native_pos(back_btn, 0, 0);
        display_obj_set_native_size(back_btn, 64, DISPLAY_UI_HEADER_HEIGHT);
        lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(back_btn, lv_color_hex(DISPLAY_UI_COLOR_SURFACE_SOFT), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(back_btn, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_radius(back_btn, display_native_scale_square(6), LV_STATE_PRESSED);
        lv_obj_add_event_cb(back_btn, back_cb, LV_EVENT_CLICKED, NULL);
        display_create_native_text(back_btn,
                                  "<",
                                  10,
                                  7,
                                  52,
                                  lv_color_hex(DISPLAY_UI_COLOR_TEXT),
                                  24,
                                  LV_TEXT_ALIGN_CENTER);
    }

    display_create_native_text(header,
                               title,
                               title_x,
                               10,
                               title_width,
                               lv_color_hex(DISPLAY_UI_COLOR_TEXT),
                               20,
                               LV_TEXT_ALIGN_CENTER);

    return header;
}

static lv_obj_t *display_create_figma_header(lv_obj_t *page,
                                             const char *title,
                                             lv_event_cb_t back_cb,
                                             const char *action_text,
                                             lv_color_t action_color,
                                             lv_event_cb_t action_cb)
{
    lv_obj_t *header = display_create_figma_header_base(page,
                                                        title,
                                                        back_cb,
                                                        120,
                                                        240);

    if (action_text != NULL) {
        display_create_native_button(header,
                                     390,
                                     5,
                                     80,
                                     34,
                                     action_color,
                                     action_color,
                                     action_text,
                                     lv_color_hex(0xFFFFFF),
                                     14,
                                     action_cb);
    } else {
        display_create_figma_signal(header);
    }

    return header;
}

static lv_obj_t *display_create_system_header(lv_obj_t *page)
{
    lv_obj_t *header = display_create_figma_header_base(page,
                                                        "设置",
                                                        display_system_back_btn_cb,
                                                        84,
                                                        196);

    s_system_memory_free_label = display_create_native_live_text(
        header,
        "剩余 --K / 连续 --K",
        300,
        0,
        170,
        lv_color_hex(DISPLAY_UI_COLOR_TEXT_MUTED),
        LV_TEXT_ALIGN_RIGHT);
    display_obj_set_native_size(s_system_memory_free_label, 170, 20);
    lv_label_set_long_mode(s_system_memory_free_label, LV_LABEL_LONG_CLIP);

    s_system_memory_largest_label = display_create_native_live_text(
        header,
        "DMA --K / PS --M",
        300,
        22,
        170,
        lv_color_hex(DISPLAY_UI_COLOR_TEXT_MUTED),
        LV_TEXT_ALIGN_RIGHT);
    display_obj_set_native_size(s_system_memory_largest_label, 170, 20);
    lv_label_set_long_mode(s_system_memory_largest_label, LV_LABEL_LONG_CLIP);
    return header;
}

static void display_create_ai_signal(lv_obj_t *header)
{
    (void)display_create_wifi_indicator(header, 443, 14, lv_color_hex(DISPLAY_UI_COLOR_GREEN));
}

static lv_obj_t *display_create_ai_header(lv_obj_t *page,
                                          const char *title,
                                          lv_event_cb_t back_cb,
                                          bool show_status,
                                          bool show_settings)
{
    lv_obj_t *header = display_create_native_box(page,
                                                 0,
                                                 0,
                                                 DISPLAY_NATIVE_WIDTH,
                                                 DISPLAY_UI_HEADER_HEIGHT,
                                                 lv_color_hex(DISPLAY_UI_COLOR_SURFACE_SOFT),
                                                 lv_color_hex(DISPLAY_UI_COLOR_BORDER),
                                                 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);

    if (back_cb != NULL) {
        lv_obj_t *back_btn = lv_btn_create(header);
        lv_obj_remove_style_all(back_btn);
        display_obj_set_native_pos(back_btn, 0, 0);
        display_obj_set_native_size(back_btn, 64, DISPLAY_UI_HEADER_HEIGHT);
        lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(back_btn, lv_color_hex(DISPLAY_UI_COLOR_SURFACE_SOFT), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(back_btn, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_radius(back_btn, display_native_scale_square(6), LV_STATE_PRESSED);
        lv_obj_add_event_cb(back_btn, back_cb, LV_EVENT_CLICKED, NULL);
        display_create_native_text(back_btn,
                                   "<",
                                   10,
                                   7,
                                   52,
                                   lv_color_hex(DISPLAY_UI_COLOR_TEXT),
                                   24,
                                   LV_TEXT_ALIGN_CENTER);
    }

    if (show_status) {
        s_ai_status_label = display_create_ai_static_text(header,
                                                          "待命",
                                                          55,
                                                          13,
                                                          70,
                                                          lv_color_hex(DISPLAY_UI_COLOR_GREEN),
                                                          13,
                                                          LV_TEXT_ALIGN_LEFT);
    }

    display_create_native_text(header,
                               title,
                               130,
                               10,
                               220,
                               lv_color_hex(DISPLAY_UI_COLOR_TEXT),
                               20,
                               LV_TEXT_ALIGN_CENTER);

    if (show_settings) {
        (void)display_create_native_button(header,
                                           382,
                                           5,
                                           56,
                                           34,
                                           lv_color_hex(DISPLAY_UI_COLOR_SURFACE),
                                           lv_color_hex(DISPLAY_UI_COLOR_BORDER),
                                           "设置",
                                           lv_color_hex(DISPLAY_UI_COLOR_BLUE_DARK),
                                           14,
                                           display_ai_settings_btn_cb);
    }

    display_create_ai_signal(header);
    return header;
}

static lv_obj_t *display_create_ai_setting_button(lv_obj_t *parent,
                                                  lv_coord_t x,
                                                  lv_coord_t y,
                                                  lv_coord_t width,
                                                  lv_coord_t height,
                                                  const char *text,
                                                  display_ai_setting_action_t action)
{
    lv_obj_t *btn = display_create_native_button(parent,
                                                 x,
                                                 y,
                                                 width,
                                                 height,
                                                 lv_color_hex(0xEAF4FB),
                                                 lv_color_hex(0xD2E1EC),
                                                 text,
                                                 lv_color_hex(0x11233C),
                                                 18,
                                                 NULL);
    lv_obj_set_style_radius(btn, 7, 0);
    lv_obj_add_event_cb(btn,
                        display_ai_settings_action_btn_cb,
                        LV_EVENT_CLICKED,
                        (void *)(uintptr_t)action);
    return btn;
}

static uint8_t display_ai_avatar_normalize(uint8_t avatar)
{
    return avatar < DISPLAY_AI_AVATAR_COUNT ? avatar : DISPLAY_AI_AVATAR_BUDDY;
}

static const char *display_ai_avatar_name(uint8_t avatar)
{
    (void)avatar;
    return DISPLAY_AI_AVATAR_NAME;
}

static ai_chat_avatar_state_t display_ai_avatar_visual_state(const display_status_t *status,
                                                             const display_ai_chat_message_t *latest_message)
{
    if (status == NULL) {
        return AI_CHAT_AVATAR_STATE_RESTING;
    }
    if (status->ai_chat_state == DISPLAY_AI_CHAT_STATE_ERROR) {
        return AI_CHAT_AVATAR_STATE_ERROR;
    }
    if (display_ai_chat_should_show_new_chat_button(status)) {
        return AI_CHAT_AVATAR_STATE_RESTING;
    }
    if (status->ai_chat_cloud_speaking) {
        return AI_CHAT_AVATAR_STATE_SPEAKING;
    }
    if (latest_message != NULL && latest_message->text[0] != '\0') {
        if (latest_message->caption_type == DISPLAY_AI_CHAT_CAPTION_TYPE_TTS) {
            return AI_CHAT_AVATAR_STATE_IDLE;
        }
        if (latest_message->caption_type == DISPLAY_AI_CHAT_CAPTION_TYPE_ASR) {
            return AI_CHAT_AVATAR_STATE_LISTENING;
        }
    }
    if (status->ai_chat_listening) {
        return AI_CHAT_AVATAR_STATE_LISTENING;
    }
    if (status->ai_chat_state == DISPLAY_AI_CHAT_STATE_STARTING ||
        status->ai_chat_state == DISPLAY_AI_CHAT_STATE_TOKEN ||
        status->ai_chat_state == DISPLAY_AI_CHAT_STATE_CONNECTING ||
        status->ai_chat_state == DISPLAY_AI_CHAT_STATE_CONNECTED ||
        status->ai_chat_state == DISPLAY_AI_CHAT_STATE_STARTING_SESSION) {
        return AI_CHAT_AVATAR_STATE_THINKING;
    }
    if (status->ai_chat_active) {
        return AI_CHAT_AVATAR_STATE_IDLE;
    }
    return AI_CHAT_AVATAR_STATE_RESTING;
}

static void display_update_ai_avatar(const display_status_t *status,
                                     const display_ai_chat_message_t *latest_message)
{
    uint8_t avatar = status != NULL ?
        display_ai_avatar_normalize(status->ai_chat_avatar) : DISPLAY_AI_AVATAR_BUDDY;
    ai_chat_avatar_state_t state = display_ai_avatar_visual_state(status, latest_message);

    if (s_ai_avatar_img != NULL &&
        (s_ai_avatar_last_variant != avatar || s_ai_avatar_last_state != state)) {
        lv_img_set_src(s_ai_avatar_img, ai_chat_avatar_asset_get(avatar, state));
        s_ai_avatar_last_variant = avatar;
        s_ai_avatar_last_state = state;
    }
    if (s_ai_avatar_name_label != NULL) {
        display_text_set(s_ai_avatar_name_label, display_ai_avatar_name(avatar));
    }
}

static lv_obj_t *display_create_ai_avatar_choice_button(lv_obj_t *parent,
                                                        lv_coord_t x,
                                                        lv_coord_t y,
                                                        uint8_t avatar,
                                                        display_ai_setting_action_t action)
{
    lv_obj_t *btn = lv_btn_create(parent);

    lv_obj_remove_style_all(btn);
    display_obj_set_native_pos(btn, x, y);
    display_obj_set_native_size(btn, 96, 42);
    lv_obj_set_style_radius(btn, 7, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xF7FBFE), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0xD2E1EC), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xDDF5E9), LV_STATE_PRESSED);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn,
                        display_ai_settings_action_btn_cb,
                        LV_EVENT_CLICKED,
                        (void *)(uintptr_t)action);

    uint8_t normalized = display_ai_avatar_normalize(avatar);
    s_ai_settings_avatar_buttons[normalized] = btn;
    s_ai_settings_avatar_labels[normalized] =
        display_create_ai_static_text(btn,
                                      display_ai_avatar_name(normalized),
                                      0,
                                      12,
                                      96,
                                      lv_color_hex(0x11233C),
                                      14,
                                      LV_TEXT_ALIGN_CENTER);
    return btn;
}

static void display_update_ai_avatar_choice_buttons(uint8_t avatar)
{
    uint8_t active_avatar = display_ai_avatar_normalize(avatar);

    for (uint8_t index = 0; index < DISPLAY_AI_AVATAR_COUNT; ++index) {
        bool selected = index == active_avatar;
        if (s_ai_settings_avatar_buttons[index] != NULL) {
            lv_obj_set_style_bg_color(s_ai_settings_avatar_buttons[index],
                                      selected ? lv_color_hex(0xE5FAF0) : lv_color_hex(0xF7FBFE),
                                      0);
            lv_obj_set_style_border_color(s_ai_settings_avatar_buttons[index],
                                          selected ? lv_color_hex(0x23C17D) : lv_color_hex(0xD2E1EC),
                                          0);
            lv_obj_set_style_border_width(s_ai_settings_avatar_buttons[index], selected ? 2 : 1, 0);
        }
        if (s_ai_settings_avatar_labels[index] != NULL) {
            display_text_set_color(s_ai_settings_avatar_labels[index],
                                   selected ? lv_color_hex(0x0D8A59) : lv_color_hex(0x11233C),
                                   0);
        }
    }
}

static lv_obj_t *display_create_settings_row(lv_obj_t *parent,
                                             lv_coord_t y,
                                             const char *text,
                                             lv_event_cb_t cb)
{
    lv_obj_t *row = display_create_native_button(parent,
                                                 10,
                                                 y,
                                                 460,
                                                 43,
                                                 lv_color_hex(DISPLAY_UI_COLOR_SURFACE),
                                                 lv_color_hex(DISPLAY_UI_COLOR_BORDER),
                                                 "",
                                                 lv_color_hex(DISPLAY_UI_COLOR_TEXT),
                                                 16,
                                                 cb);
    display_create_native_text(row,
                               text,
                               18,
                               12,
                               390,
                               lv_color_hex(DISPLAY_UI_COLOR_TEXT),
                               16,
                               LV_TEXT_ALIGN_LEFT);
    display_create_native_text(row,
                               ">",
                               420,
                               9,
                               28,
                               lv_color_hex(DISPLAY_UI_COLOR_TEXT_MUTED),
                               20,
                               LV_TEXT_ALIGN_CENTER);
    return row;
}

static lv_obj_t *display_create_device_dot(lv_obj_t *parent,
                                           lv_coord_t x,
                                           lv_coord_t y,
                                           lv_color_t color)
{
    lv_obj_t *dot = lv_obj_create(parent);

    lv_obj_remove_style_all(dot);
    display_obj_set_native_pos(dot, x, y);
    display_obj_set_native_size(dot, 10, 10);
    lv_obj_set_style_radius(dot, display_native_scale_square(5), 0);
    lv_obj_set_style_bg_color(dot, color, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return dot;
}

static void display_create_device_status_row(lv_obj_t *parent,
                                             lv_coord_t y,
                                             const char *label,
                                             const char *value,
                                             lv_color_t dot_color,
                                             lv_obj_t **dot,
                                             lv_obj_t **value_label)
{
    if (dot != NULL) {
        *dot = display_create_device_dot(parent, 4, y + 13, dot_color);
    }
    display_create_native_text(parent,
                               label,
                               20,
                               y + 7,
                               100,
                               lv_color_hex(DISPLAY_UI_COLOR_TEXT_MUTED),
                               16,
                               LV_TEXT_ALIGN_LEFT);
    if (value_label != NULL) {
        *value_label = display_create_native_text(parent,
                                                  value,
                                                  128,
                                                  y + 7,
                                                  58,
                                                  lv_color_hex(DISPLAY_UI_COLOR_TEXT),
                                                  16,
                                                  LV_TEXT_ALIGN_RIGHT);
    }
}

static lv_obj_t *display_create_device_volume_button(lv_obj_t *parent,
                                                     lv_coord_t x,
                                                     const char *text,
                                                     bool mute,
                                                     display_device_volume_action_t action,
                                                     lv_obj_t **text_label)
{
    lv_obj_t *btn = lv_btn_create(parent);

    lv_obj_remove_style_all(btn);
    display_obj_set_native_pos(btn, x, 40);
    display_obj_set_native_size(btn, 54, 38);
    lv_obj_set_style_radius(btn, display_native_scale_square(7), 0);
    lv_obj_set_style_bg_color(btn, mute ? lv_color_hex(0xFFF2D8) : lv_color_hex(0xF7FBFF), 0);
    lv_obj_set_style_bg_color(btn, mute ? lv_color_hex(0xFFE3B3) : lv_color_hex(0xE7F1FB), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, mute ? lv_color_hex(0xFFD59D) : lv_color_hex(0xD5E0EB), 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn,
                        display_device_volume_btn_cb,
                        LV_EVENT_CLICKED,
                        (void *)(uintptr_t)action);

    if (text_label != NULL) {
        lv_obj_t *label = display_create_native_text(btn,
                                                     text,
                                                     6,
                                                     11,
                                                     42,
                                                     mute ? lv_color_hex(0x9A5A00) : lv_color_hex(DISPLAY_UI_COLOR_TEXT),
                                                     12,
                                                     LV_TEXT_ALIGN_CENTER);
        *text_label = label;
    } else {
        display_create_native_text(btn,
                                   text,
                                   6,
                                   11,
                                   42,
                                   mute ? lv_color_hex(0x9A5A00) : lv_color_hex(DISPLAY_UI_COLOR_TEXT),
                                   12,
                                   LV_TEXT_ALIGN_CENTER);
    }
    return btn;
}

static void display_create_device_volume_card(lv_obj_t *parent,
                                              lv_coord_t y,
                                              const char *title,
                                              const char *value,
                                              display_device_volume_action_t down_action,
                                              display_device_volume_action_t up_action,
                                              display_device_volume_action_t mute_action,
                                              lv_obj_t **value_label,
                                              lv_obj_t **mute_label)
{
    lv_obj_t *card = display_create_native_box(parent,
                                               10,
                                               y,
                                               210,
                                               88,
                                               lv_color_hex(DISPLAY_UI_COLOR_SURFACE),
                                               lv_color_hex(DISPLAY_UI_COLOR_BORDER),
                                               8);
    lv_obj_t *pill = NULL;

    display_create_native_text(card,
                               title,
                               14,
                               9,
                               110,
                               lv_color_hex(DISPLAY_UI_COLOR_TEXT),
                               16,
                               LV_TEXT_ALIGN_LEFT);
    pill = display_create_native_box(card,
                                     158,
                                     5,
                                     38,
                                     26,
                                     lv_color_hex(0xE7F1FB),
                                     lv_color_hex(0xE7F1FB),
                                     6);
    if (value_label != NULL) {
        *value_label = display_create_native_text(pill,
                                                  value,
                                                  0,
                                                  6,
                                                  38,
                                                  lv_color_hex(DISPLAY_UI_COLOR_BLUE_DARK),
                                                  12,
                                                  LV_TEXT_ALIGN_CENTER);
    }

    (void)display_create_device_volume_button(card, 12, "-10", false, down_action, NULL);
    (void)display_create_device_volume_button(card, 78, "+10", false, up_action, NULL);
    (void)display_create_device_volume_button(card, 144, "禁音", true, mute_action, mute_label);
}

static esp_err_t display_qr_image_update(display_qr_image_t *qr,
                                         lv_obj_t *img,
                                         lv_coord_t pixel_size,
                                         const void *data,
                                         uint32_t data_len)
{
#if LV_USE_QRCODE
    if (qr == NULL || img == NULL || data == NULL || data_len == 0 || pixel_size <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (data_len > qrcodegen_BUFFER_LEN_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    int32_t version = qrcodegen_getMinFitVersion(qrcodegen_Ecc_MEDIUM, data_len);
    if (version <= 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    int32_t module_count = qrcodegen_version2size(version);
    if (module_count <= 0 || pixel_size < module_count) {
        return ESP_ERR_INVALID_SIZE;
    }
    int32_t scale = pixel_size / module_count;
    int32_t remain = pixel_size % module_count;
    uint32_t version_extend = remain / (scale << 2);
    if (version_extend != 0 && version < qrcodegen_VERSION_MAX) {
        version = version + (int32_t)version_extend > qrcodegen_VERSION_MAX ?
                  qrcodegen_VERSION_MAX : version + (int32_t)version_extend;
    }

    uint32_t qr_buf_len = qrcodegen_BUFFER_LEN_FOR_VERSION(version);
    uint8_t *qr0 = (uint8_t *)app_memory_alloc_psram(qr_buf_len);
    uint8_t *data_tmp = (uint8_t *)app_memory_alloc_psram(qr_buf_len);
    if (qr0 == NULL || data_tmp == NULL) {
        heap_caps_free(qr0);
        heap_caps_free(data_tmp);
        return ESP_ERR_NO_MEM;
    }
    memcpy(data_tmp, data, data_len);

    bool ok = qrcodegen_encodeBinary(data_tmp,
                                     data_len,
                                     qr0,
                                     qrcodegen_Ecc_MEDIUM,
                                     version,
                                     version,
                                     qrcodegen_Mask_AUTO,
                                     true);
    heap_caps_free(data_tmp);
    if (!ok) {
        heap_caps_free(qr0);
        return ESP_ERR_INVALID_RESPONSE;
    }

    module_count = qrcodegen_getSize(qr0);
    scale = pixel_size / module_count;
    if (scale <= 0) {
        heap_caps_free(qr0);
        return ESP_ERR_INVALID_SIZE;
    }
    int32_t scaled = module_count * scale;
    int32_t margin = (pixel_size - scaled) / 2;
    size_t pixel_count = (size_t)pixel_size * (size_t)pixel_size;

    if (qr->pixels == NULL || qr->size != pixel_size) {
        heap_caps_free(qr->pixels);
        qr->pixels = (lv_color_t *)app_memory_alloc_psram(pixel_count * sizeof(lv_color_t));
        if (qr->pixels == NULL) {
            qr->size = 0;
            heap_caps_free(qr0);
            return ESP_ERR_NO_MEM;
        }
        qr->size = pixel_size;
        memset(&qr->dsc, 0, sizeof(qr->dsc));
        qr->dsc.header.always_zero = 0;
        qr->dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
        qr->dsc.header.w = (uint32_t)pixel_size;
        qr->dsc.header.h = (uint32_t)pixel_size;
        qr->dsc.data_size = (uint32_t)(pixel_count * sizeof(lv_color_t));
        qr->dsc.data = (const uint8_t *)qr->pixels;
    }

    lv_color_t light = lv_color_hex(0xFFFFFF);
    lv_color_t dark = lv_color_hex(0x111111);
    for (size_t index = 0; index < pixel_count; ++index) {
        qr->pixels[index] = light;
    }
    for (int32_t my = 0; my < module_count; ++my) {
        for (int32_t mx = 0; mx < module_count; ++mx) {
            if (!qrcodegen_getModule(qr0, mx, my)) {
                continue;
            }
            int32_t px0 = margin + mx * scale;
            int32_t py0 = margin + my * scale;
            for (int32_t py = 0; py < scale; ++py) {
                lv_color_t *row = qr->pixels + ((size_t)(py0 + py) * (size_t)pixel_size);
                for (int32_t px = 0; px < scale; ++px) {
                    row[px0 + px] = dark;
                }
            }
        }
    }

    heap_caps_free(qr0);
    lv_img_set_src(img, &qr->dsc);
    lv_obj_set_size(img, pixel_size, pixel_size);
    return ESP_OK;
#else
    (void)qr;
    (void)img;
    (void)pixel_size;
    (void)data;
    (void)data_len;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static lv_obj_t *display_create_info_row(lv_obj_t *parent,
                                                 lv_coord_t y,
                                                 const char *label,
                                                 const char *value,
                                                 lv_color_t value_color,
                                                 lv_obj_t **value_label)
{
    lv_obj_t *row = display_create_figma_box(parent,
                                             8,
                                             y,
                                             304,
                                             34,
                                             lv_color_hex(0xFFFFFF),
                                             lv_color_hex(0xD5E0EB),
                                             6);

    display_create_figma_text(row,
                              label,
                              9,
                              9,
                              86,
                              lv_color_hex(0x64758A),
                              12,
                              LV_TEXT_ALIGN_LEFT);
    lv_obj_t *value_obj = display_create_figma_text(row,
                                                    value,
                                                    103,
                                                    9,
                                                    190,
                                                    value_color,
                                                    12,
                                                    LV_TEXT_ALIGN_LEFT);
    if (value_label != NULL) {
        *value_label = value_obj;
    }
    return row;
}

static lv_obj_t *display_create_check_row(lv_obj_t *parent,
                                             lv_coord_t y,
                                             const char *label,
                                             const char *value,
                                             lv_color_t fill,
                                             lv_color_t value_color,
                                             lv_obj_t **value_label)
{
    lv_obj_t *row = display_create_figma_box(parent,
                                             8,
                                             y,
                                             304,
                                             24,
                                             fill,
                                             lv_color_hex(0xD5E0EB),
                                             5);

    display_create_figma_text(row,
                              label,
                              9,
                              4,
                              126,
                              lv_color_hex(0x10243E),
                              12,
                              LV_TEXT_ALIGN_LEFT);
    lv_obj_t *value_obj = display_create_figma_text(row,
                                                    value,
                                                    143,
                                                    4,
                                                    152,
                                                    value_color,
                                                    12,
                                                    LV_TEXT_ALIGN_RIGHT);
    if (value_label != NULL) {
        *value_label = value_obj;
    }
    return row;
}

static void display_create_device_id_qr_image(lv_obj_t *parent,
                                              lv_coord_t x,
                                              lv_coord_t y,
                                              lv_coord_t size,
                                              lv_obj_t **image_out)
{
    lv_coord_t box_size = display_native_scale_square(size);
    lv_coord_t quiet_zone = display_native_scale_square(12);
    lv_coord_t qr_size = box_size > quiet_zone ? box_size - quiet_zone : box_size;
    lv_obj_t *qr_box = lv_obj_create(parent);

    lv_obj_remove_style_all(qr_box);
    display_obj_set_native_pos(qr_box, x, y);
    lv_obj_set_size(qr_box, box_size, box_size);
    lv_obj_set_style_radius(qr_box, display_native_scale_square(4), 0);
    lv_obj_set_style_bg_opa(qr_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(qr_box, 0, 0);
    lv_obj_set_style_pad_all(qr_box, 0, 0);
    lv_obj_clear_flag(qr_box, LV_OBJ_FLAG_SCROLLABLE);
#if LV_USE_QRCODE
    lv_obj_t *image = lv_img_create(qr_box);
    lv_obj_set_size(image, qr_size, qr_size);
    lv_obj_center(image);
    lv_obj_clear_flag(image, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    if (image_out != NULL) {
        *image_out = image;
    }
#else
    if (image_out != NULL) {
        *image_out = NULL;
    }
    display_create_figma_text(qr_box,
                              "QR",
                              0,
                              (size - 24) / 2,
                              size,
                              lv_color_hex(0x10243E),
                              18,
                              LV_TEXT_ALIGN_CENTER);
#endif
}

static void display_create_call_qr(lv_obj_t *parent,
                                   lv_coord_t x,
                                   lv_coord_t y,
                                   lv_coord_t size)
{
    display_create_device_id_qr_image(parent, x, y, size, &s_call_qrcode);
    s_call_device_id_label = display_create_native_text(parent,
                                                        "--",
                                                        12,
                                                        y + size + 4,
                                                        276,
                                                        lv_color_hex(DISPLAY_UI_COLOR_TEXT),
                                                        14,
                                                        LV_TEXT_ALIGN_CENTER);
}

static void display_create_wechat_qr(lv_obj_t *parent,
                                     lv_coord_t x,
                                     lv_coord_t y,
                                     lv_coord_t size)
{
    display_create_device_id_qr_image(parent, x, y, size, &s_wechat_qrcode);
}

static lv_obj_t *display_create_call_menu_button(lv_obj_t *parent,
                                                 lv_coord_t x,
                                                 lv_coord_t y,
                                                 lv_coord_t width,
                                                 lv_coord_t height,
                                                 const char *line1,
                                                 const char *line2,
                                                 bool primary,
                                                 lv_event_cb_t cb)
{
    lv_color_t fill = primary ? lv_color_hex(0x21C783) : lv_color_hex(0xFFFFFF);
    lv_color_t stroke = primary ? lv_color_hex(0x21C783) : lv_color_hex(0xD6E4EF);
    lv_color_t text_color = primary ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x10233B);
    lv_obj_t *btn = lv_btn_create(parent);

    lv_obj_remove_style_all(btn);
    display_obj_set_native_pos(btn, x, y);
    display_obj_set_native_size(btn, width, height);
    lv_obj_set_style_radius(btn, display_native_scale_square(8), 0);
    lv_obj_set_style_bg_color(btn, fill, 0);
    lv_obj_set_style_bg_color(btn, lv_color_darken(fill, 18), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, stroke, 0);
    lv_obj_set_style_shadow_width(btn, 8, 0);
    lv_obj_set_style_shadow_ofs_y(btn, 2, 0);
    lv_obj_set_style_shadow_color(btn, lv_color_hex(0x10233B), 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_10, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    if (cb != NULL) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    }

    if (line2 != NULL && line2[0] != '\0') {
        display_create_native_live_text(btn, line1, 6, 38, width - 12, text_color, LV_TEXT_ALIGN_CENTER);
        display_create_native_live_text(btn, line2, 6, 65, width - 12, text_color, LV_TEXT_ALIGN_CENTER);
    } else {
        display_create_native_live_text(btn,
                                        line1,
                                        6,
                                        (height - 16) / 2,
                                        width - 12,
                                        text_color,
                                        LV_TEXT_ALIGN_CENTER);
    }

    return btn;
}

static lv_obj_t *display_create_call_add_field_row(lv_obj_t *parent,
                                                   lv_coord_t y,
                                                   display_call_add_field_t field)
{
    const char *label = display_call_add_field_title(field);
    const char *value = display_call_add_field_buffer(field);
    lv_obj_t *row = display_create_native_box(parent,
                                              10,
                                              y,
                                              460,
                                              54,
                                              lv_color_hex(0xFFFFFF),
                                              lv_color_hex(0xD6E4EF),
                                              7);

    display_create_native_text(row,
                               label,
                               14,
                               19,
                               92,
                               lv_color_hex(0x65768A),
                               13,
                               LV_TEXT_ALIGN_LEFT);

    lv_obj_t *value_label = display_create_native_text(row,
                                                       value[0] != '\0' ?
                                                           value : display_call_add_field_placeholder(field),
                                                       112,
                                                       19,
                                                       294,
                                                       value[0] != '\0' ?
                                                           lv_color_hex(0x10233B) : lv_color_hex(0x8AA0B5),
                                                       13,
                                                       LV_TEXT_ALIGN_LEFT);
    display_create_native_text(row,
                               ">",
                               420,
                               16,
                               26,
                               lv_color_hex(0x64758A),
                               18,
                               LV_TEXT_ALIGN_CENTER);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row,
                        display_call_add_field_btn_cb,
                        LV_EVENT_CLICKED,
                        (void *)(uintptr_t)field);

    return value_label;
}

static lv_obj_t *display_create_wechat_add_field_row(lv_obj_t *parent, lv_coord_t y)
{
    lv_obj_t *row = display_create_figma_box(parent,
                                             8,
                                             y,
                                             304,
                                             42,
                                             lv_color_hex(0xFFFFFF),
                                             lv_color_hex(0xD6E4EF),
                                             7);

    display_create_figma_text(row,
                              "OpenID",
                              11,
                              13,
                              70,
                              lv_color_hex(0x65768A),
                              12,
                              LV_TEXT_ALIGN_LEFT);

    lv_obj_t *value_label = display_create_figma_text(row,
                                                      s_wechat_add_open_id[0] != '\0' ?
                                                          s_wechat_add_open_id : "28位微信Open ID",
                                                      85,
                                                      13,
                                                      176,
                                                      s_wechat_add_open_id[0] != '\0' ?
                                                          lv_color_hex(0x10233B) : lv_color_hex(0x8AA0B5),
                                                      12,
                                                      LV_TEXT_ALIGN_LEFT);
    display_create_figma_text(row,
                              ">",
                              270,
                              13,
                              20,
                              lv_color_hex(0x64758A),
                              14,
                              LV_TEXT_ALIGN_CENTER);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, display_wechat_add_field_btn_cb, LV_EVENT_CLICKED, NULL);

    return value_label;
}

static void display_create_call_contact_row(lv_obj_t *parent, uint8_t index, lv_coord_t y)
{
    bool online = s_call_contacts[index].online;
    lv_color_t button_fill = online ? lv_color_hex(0xDDF8EA) : lv_color_hex(0xEDF1F4);
    lv_color_t button_text = online ? lv_color_hex(0x159864) : lv_color_hex(0x8A97A3);
    const char *title = s_call_contacts[index].remark[0] != '\0' ?
        s_call_contacts[index].remark : s_call_contacts[index].device_id;
    const char *subtitle = s_call_contacts[index].remark[0] != '\0' ?
        s_call_contacts[index].device_id : "未设置名称";
    lv_obj_t *row = display_create_native_box(parent,
                                              8,
                                              y,
                                              464,
                                              64,
                                              lv_color_hex(0xFFFFFF),
                                              lv_color_hex(0xD6E4EF),
                                              7);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row,
                        display_call_contact_delete_cb,
                        LV_EVENT_LONG_PRESSED,
                        (void *)(uintptr_t)index);

    display_create_ai_static_text(row,
                                  title,
                                  12,
                                  8,
                                  158,
                                  lv_color_hex(0x10233B),
                                  14,
                                  LV_TEXT_ALIGN_LEFT);
    display_create_ai_static_text(row,
                                  subtitle,
                                  12,
                                  36,
                                  158,
                                  lv_color_hex(0x65768A),
                                  10,
                                  LV_TEXT_ALIGN_LEFT);

    display_create_native_live_text(row,
                                    online ? "在线" : "离线",
                                    172,
                                    23,
                                    48,
                                    online ? lv_color_hex(0x1BAA70) : lv_color_hex(0x8A97A3),
                                    LV_TEXT_ALIGN_CENTER);

    lv_obj_t *remark_btn = display_create_native_live_button(row,
                                                              224,
                                                              11,
                                                              58,
                                                              42,
                                                              lv_color_hex(0xE9F5FF),
                                                              lv_color_hex(0xB9D7F1),
                                                              "改名",
                                                              lv_color_hex(0x1768B7),
                                                              NULL);
    lv_obj_add_event_cb(remark_btn,
                        display_call_contact_remark_btn_cb,
                        LV_EVENT_CLICKED,
                        (void *)(uintptr_t)index);
    lv_obj_add_event_cb(remark_btn,
                        display_call_contact_delete_cb,
                        LV_EVENT_LONG_PRESSED,
                        (void *)(uintptr_t)index);

    lv_obj_t *audio_btn = display_create_native_button(row,
                                                       288,
                                                       11,
                                                       78,
                                                       42,
                                                       button_fill,
                                                       button_fill,
                                                       "AUDIO",
                                                       button_text,
                                                       11,
                                                       NULL);
    lv_obj_set_style_radius(audio_btn, 7, 0);
    lv_obj_add_event_cb(audio_btn,
                        display_call_contact_call_btn_cb,
                        LV_EVENT_CLICKED,
                        (void *)(uintptr_t)(((uintptr_t)index << 1U) |
                                            DISPLAY_CALL_TYPE_AUDIO));

    lv_obj_t *video_btn = display_create_native_button(row,
                                                       372,
                                                       11,
                                                       80,
                                                       42,
                                                       online ? lv_color_hex(0xE5F3FD) : button_fill,
                                                       online ? lv_color_hex(0xE5F3FD) : button_fill,
                                                       "VIDEO",
                                                       online ? lv_color_hex(0x1879B9) : button_text,
                                                       11,
                                                       NULL);
    lv_obj_set_style_radius(video_btn, 7, 0);
    lv_obj_add_event_cb(video_btn,
                        display_call_contact_call_btn_cb,
                        LV_EVENT_CLICKED,
                        (void *)(uintptr_t)(((uintptr_t)index << 1U) |
                                            DISPLAY_CALL_TYPE_VIDEO));

    if (!online) {
        lv_obj_add_state(audio_btn, LV_STATE_DISABLED);
        lv_obj_add_state(video_btn, LV_STATE_DISABLED);
    }
}

static void display_create_call_pending_contact_row(lv_obj_t *parent,
                                                    uint8_t index,
                                                    lv_coord_t y)
{
    if (parent == NULL || index >= s_call_pending_contact_count) {
        return;
    }

    lv_obj_t *row = display_create_native_box(parent,
                                              8,
                                              y,
                                              464,
                                              60,
                                              lv_color_hex(0xFFF9ED),
                                              lv_color_hex(0xF2D18B),
                                              7);
    display_create_native_text(row,
                               s_call_pending_contacts[index].device_id,
                               12,
                               8,
                               236,
                               lv_color_hex(0x10233B),
                               14,
                               LV_TEXT_ALIGN_LEFT);
    display_create_native_live_text(row,
                                    s_call_pending_contacts[index].created_at[0] != '\0' ?
                                        s_call_pending_contacts[index].created_at : "等待处理",
                                    12,
                                    36,
                                    236,
                                    lv_color_hex(0x65768A),
                                    LV_TEXT_ALIGN_LEFT);

    lv_obj_t *reject_btn = display_create_native_live_button(row,
                                                              256,
                                                              9,
                                                              82,
                                                              42,
                                                              lv_color_hex(0xFFF0F0),
                                                              lv_color_hex(0xF15A5A),
                                                              "拒绝",
                                                              lv_color_hex(0xE44747),
                                                              NULL);
    lv_obj_t *accept_btn = display_create_native_live_button(row,
                                                              346,
                                                              9,
                                                              106,
                                                              42,
                                                              lv_color_hex(0x21C783),
                                                              lv_color_hex(0x21C783),
                                                              "同意添加",
                                                              lv_color_hex(0xFFFFFF),
                                                              NULL);
    lv_obj_add_event_cb(reject_btn,
                        display_call_pending_contact_response_btn_cb,
                        LV_EVENT_CLICKED,
                        (void *)(uintptr_t)((uint32_t)index << 1U));
    lv_obj_add_event_cb(accept_btn,
                        display_call_pending_contact_response_btn_cb,
                        LV_EVENT_CLICKED,
                        (void *)(uintptr_t)(((uint32_t)index << 1U) | 1U));
}

static void display_create_wechat_contact_row(lv_obj_t *parent, uint8_t index, lv_coord_t y)
{
    lv_color_t button_fill = lv_color_hex(0xDDF8EA);
    lv_color_t button_text = lv_color_hex(0x1FC985);
    lv_obj_t *remark_label = NULL;
    lv_obj_t *open_id_label = NULL;
    lv_obj_t *row = display_create_figma_box(parent,
                                             8,
                                             y,
                                             304,
                                             44,
                                             lv_color_hex(0xFFFFFF),
                                             lv_color_hex(0xD6E4EF),
                                             7);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row,
                        display_wechat_contact_delete_cb,
                        LV_EVENT_LONG_PRESSED,
                        (void *)(uintptr_t)index);

    remark_label = display_create_native_live_text(row,
                                                   "",
                                                   16,
                                                   3,
                                                   226,
                                                   lv_color_hex(0x10233B),
                                                   LV_TEXT_ALIGN_LEFT);
    open_id_label = display_create_figma_text(row,
                                              "",
                                              11,
                                              25,
                                              151,
                                              lv_color_hex(0x64758A),
                                              9,
                                              LV_TEXT_ALIGN_LEFT);
    lv_obj_t *remark_btn = display_create_figma_button(row,
                                                        169,
                                                        7,
                                                        49,
                                                        30,
                                                        lv_color_hex(0xE9F5FF),
                                                        lv_color_hex(0xB9D7F1),
                                                        "改名",
                                                        lv_color_hex(0x1768B7),
                                                        10,
                                                        NULL);
    lv_obj_set_style_radius(remark_btn, 7, 0);
    lv_obj_add_event_cb(remark_btn,
                        display_wechat_contact_remark_btn_cb,
                        LV_EVENT_CLICKED,
                        (void *)(uintptr_t)index);
    lv_obj_t *btn = display_create_figma_button(row,
                                                226,
                                                5,
                                                66,
                                                34,
                                                button_fill,
                                                button_fill,
                                                "呼叫",
                                                button_text,
                                                12,
                                                NULL);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_add_event_cb(btn,
                        display_wechat_contact_call_btn_cb,
                        LV_EVENT_CLICKED,
                        (void *)(uintptr_t)index);
    lv_obj_add_event_cb(btn,
                        display_wechat_contact_delete_cb,
                        LV_EVENT_LONG_PRESSED,
                        (void *)(uintptr_t)index);
    s_wechat_contact_rows[index] = row;
    s_wechat_contact_remark_labels[index] = remark_label;
    s_wechat_contact_open_id_labels[index] = open_id_label;
    lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
}

static void display_create_call_scan_info_overlay(lv_obj_t *parent)
{
    lv_obj_t *card = NULL;
    lv_obj_t *field = NULL;
    lv_obj_t *format_label = NULL;
    lv_obj_t *close_btn = NULL;

    s_call_scan_info_overlay = display_create_native_box(parent,
                                                         0,
                                                         0,
                                                         480,
                                                         320,
                                                         lv_color_hex(0x10233B),
                                                         lv_color_hex(0x10233B),
                                                         0);
    lv_obj_set_style_bg_opa(s_call_scan_info_overlay, LV_OPA_20, 0);
    lv_obj_set_style_border_width(s_call_scan_info_overlay, 0, 0);
    lv_obj_add_flag(s_call_scan_info_overlay, LV_OBJ_FLAG_HIDDEN);

    card = display_create_native_box(s_call_scan_info_overlay,
                                     45,
                                     55,
                                     390,
                                     210,
                                     lv_color_hex(0xFFFFFF),
                                     lv_color_hex(0xD6E4EF),
                                     9);
    lv_obj_set_style_shadow_width(card, 14, 0);
    lv_obj_set_style_shadow_ofs_y(card, 5, 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(0x10233B), 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_20, 0);

    display_create_native_text(card,
                               "扫码添加联系人",
                               20,
                               16,
                               350,
                               lv_color_hex(0x10233B),
                               18,
                               LV_TEXT_ALIGN_CENTER);

    field = display_create_native_box(card,
                                      20,
                                      50,
                                      350,
                                      104,
                                      lv_color_hex(0xF4F9FD),
                                      lv_color_hex(0xD6E4EF),
                                      6);
    format_label = display_create_native_text(field,
                                              "扫描对方设备呼叫首页的二维码\n"
                                              "二维码内容为 12 位 Device ID",
                                              12,
                                              13,
                                              326,
                                              lv_color_hex(0x10233B),
                                              12,
                                              LV_TEXT_ALIGN_LEFT);
    lv_label_set_long_mode(format_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(format_label, display_native_scale_y(80));

    close_btn = display_create_native_button(card,
                                             125,
                                             164,
                                             140,
                                             38,
                                             lv_color_hex(0xE9F5FF),
                                             lv_color_hex(0x2F82D7),
                                             "关闭",
                                             lv_color_hex(0x2F82D7),
                                             13,
                                             display_call_scan_info_close_btn_cb);
    lv_obj_set_style_radius(close_btn, 7, 0);
}

static void display_create_wechat_scan_info_overlay(lv_obj_t *parent)
{
    lv_obj_t *card = NULL;
    lv_obj_t *field = NULL;
    lv_obj_t *format_label = NULL;
    lv_obj_t *close_btn = NULL;

    s_wechat_scan_info_overlay = display_create_figma_box(parent,
                                                          0,
                                                          0,
                                                          320,
                                                          240,
                                                          lv_color_hex(0x10233B),
                                                          lv_color_hex(0x10233B),
                                                          0);
    lv_obj_set_style_bg_opa(s_wechat_scan_info_overlay, LV_OPA_20, 0);
    lv_obj_set_style_border_width(s_wechat_scan_info_overlay, 0, 0);
    lv_obj_add_flag(s_wechat_scan_info_overlay, LV_OBJ_FLAG_HIDDEN);

    card = display_create_figma_box(s_wechat_scan_info_overlay,
                                    30,
                                    52,
                                    260,
                                    140,
                                    lv_color_hex(0xFFFFFF),
                                    lv_color_hex(0xD6E4EF),
                                    9);
    lv_obj_set_style_shadow_width(card, 14, 0);
    lv_obj_set_style_shadow_ofs_y(card, 5, 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(0x10233B), 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_20, 0);

    display_create_figma_text(card,
                              "扫码格式",
                              12,
                              12,
                              236,
                              lv_color_hex(0x10233B),
                              16,
                              LV_TEXT_ALIGN_CENTER);

    field = display_create_figma_box(card,
                                     12,
                                     42,
                                     236,
                                     66,
                                     lv_color_hex(0xF4F9FD),
                                     lv_color_hex(0xD6E4EF),
                                     6);
    format_label = display_create_ai_text(field,
                                          "28-character WeChat Open ID",
                                          9,
                                          24,
                                          216,
                                          lv_color_hex(0x10233B),
                                          LV_TEXT_ALIGN_CENTER);
    lv_label_set_long_mode(format_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(format_label, 44);

    close_btn = display_create_figma_button(card,
                                            92,
                                            110,
                                            76,
                                            24,
                                            lv_color_hex(0xE9F5FF),
                                            lv_color_hex(0x2F82D7),
                                            "关闭",
                                            lv_color_hex(0x2F82D7),
                                            12,
                                            display_wechat_scan_info_close_btn_cb);
    lv_obj_set_style_radius(close_btn, 7, 0);
}

static lv_obj_t *display_create_call_native_volume_card(lv_obj_t *parent,
                                                        lv_coord_t x,
                                                        lv_coord_t y,
                                                        const char *title,
                                                        const char *value,
                                                        display_call_volume_action_t down_action,
                                                        display_call_volume_action_t up_action,
                                                        lv_obj_t **value_label)
{
    lv_obj_t *card = display_create_native_box(parent,
                                               x,
                                               y,
                                               220,
                                               68,
                                               lv_color_hex(DISPLAY_UI_COLOR_SURFACE),
                                               lv_color_hex(DISPLAY_UI_COLOR_BORDER),
                                               8);
    lv_obj_t *down_btn = NULL;
    lv_obj_t *up_btn = NULL;
    lv_obj_t *value_box = NULL;

    display_create_native_text(card,
                               title,
                               12,
                               24,
                               76,
                               lv_color_hex(DISPLAY_UI_COLOR_TEXT),
                               14,
                               LV_TEXT_ALIGN_LEFT);
    down_btn = display_create_native_button(card,
                                            94,
                                            14,
                                            34,
                                            40,
                                            lv_color_hex(0xEAF4FB),
                                            lv_color_hex(DISPLAY_UI_COLOR_BORDER),
                                            "-",
                                            lv_color_hex(DISPLAY_UI_COLOR_TEXT),
                                            18,
                                            NULL);
    lv_obj_add_event_cb(down_btn,
                        display_call_volume_btn_cb,
                        LV_EVENT_PRESSED,
                        (void *)(uintptr_t)down_action);

    value_box = display_create_native_box(card,
                                          134,
                                          14,
                                          40,
                                          40,
                                          lv_color_hex(0xE5FAF0),
                                          lv_color_hex(0xE5FAF0),
                                          7);
    if (value_label != NULL) {
        *value_label = display_create_native_text(value_box,
                                                  value,
                                                  0,
                                                  12,
                                                  40,
                                                  lv_color_hex(DISPLAY_UI_COLOR_GREEN),
                                                  14,
                                                  LV_TEXT_ALIGN_CENTER);
    }

    up_btn = display_create_native_button(card,
                                          180,
                                          14,
                                          34,
                                          40,
                                          lv_color_hex(0xEAF4FB),
                                          lv_color_hex(DISPLAY_UI_COLOR_BORDER),
                                          "+",
                                          lv_color_hex(DISPLAY_UI_COLOR_TEXT),
                                          18,
                                          NULL);
    lv_obj_add_event_cb(up_btn,
                        display_call_volume_btn_cb,
                        LV_EVENT_PRESSED,
                        (void *)(uintptr_t)up_action);
    return card;
}

static void display_style_wifi_list_button(lv_obj_t *btn)
{
    lv_obj_set_height(btn, display_scale_y(46));
    lv_obj_set_style_radius(btn, display_scale_square(8), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(DISPLAY_UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(DISPLAY_UI_COLOR_BLUE), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(btn, lv_color_hex(DISPLAY_UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xE7F2FA), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(btn, 1, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_left(btn, display_scale_x(14), 0);
    lv_obj_set_style_pad_right(btn, display_scale_x(12), 0);
    lv_obj_set_style_pad_top(btn, 0, 0);
    lv_obj_set_style_pad_bottom(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
}

static bool display_page_is_visible(lv_obj_t *page)
{
    return display_page_registry_is_visible(&s_page_registry, page);
}

static bool display_wifi_scan_result_equals(const display_wifi_scan_result_t *lhs,
                                                     const display_wifi_scan_result_t *rhs)
{
    return lhs->rssi == rhs->rssi &&
           lhs->secure == rhs->secure &&
           lhs->channel == rhs->channel &&
           strcmp(lhs->ssid, rhs->ssid) == 0;
}

static bool display_wifi_scan_equals(const display_status_t *lhs,
                                               const display_status_t *rhs)
{
    if (lhs->wifi_scan_in_progress != rhs->wifi_scan_in_progress) {
        return false;
    }
    if (lhs->wifi_scan_count != rhs->wifi_scan_count) {
        return false;
    }

    for (uint16_t index = 0; index < lhs->wifi_scan_count && index < DISPLAY_WIFI_SCAN_MAX; ++index) {
        if (!display_wifi_scan_result_equals(&lhs->wifi_scan_results[index], &rhs->wifi_scan_results[index])) {
            return false;
        }
    }
    return true;
}

static lv_color_t display_wifi_signal_color(int rssi)
{
    (void)rssi;
    return lv_color_hex(0xF59E0B);
}

static void display_add_wifi_list_item(const display_status_t *status, uint16_t index)
{
    lv_obj_t *btn = NULL;
    lv_obj_t *ssid_label = NULL;
    lv_obj_t *rssi_label = NULL;
    char ssid_text[48] = {0};
    char rssi_text[24] = {0};
    const display_wifi_scan_result_t *result = &status->wifi_scan_results[index];
    bool connected = status->network_connected && strcmp(status->network_ssid, result->ssid) == 0;

    if (s_wifi_list == NULL || index >= DISPLAY_WIFI_SCAN_MAX) {
        return;
    }

    btn = s_wifi_list_buttons[index];
    ssid_label = s_wifi_list_ssid_labels[index];
    rssi_label = s_wifi_list_rssi_labels[index];

    if (btn == NULL || ssid_label == NULL || rssi_label == NULL) {
        return;
    }

    if (result->rssi > -120) {
        snprintf(rssi_text, sizeof(rssi_text), "%d dBm", result->rssi);
    } else {
        strlcpy(rssi_text, "--", sizeof(rssi_text));
    }
    display_format_ssid(ssid_text, sizeof(ssid_text), result->ssid);

    lv_obj_set_style_bg_color(btn, connected ? lv_color_hex(0xDDF7EC) : lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_color(btn, connected ? lv_color_hex(0x20BF7A) : lv_color_hex(0xD5E0EB), 0);
    lv_obj_set_style_bg_color(btn, connected ? lv_color_hex(0xC9F0DF) : lv_color_hex(0xE7F1FB), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, connected ? lv_color_hex(0x0D8A59) : lv_color_hex(0x1768B7), LV_STATE_PRESSED);
    display_text_set_color(ssid_label, connected ? lv_color_hex(0x0D8A59) : lv_color_hex(0x10243E), 0);
    display_text_set_color(rssi_label, display_wifi_signal_color(result->rssi), 0);
    display_text_set(ssid_label, ssid_text);
    display_text_set(rssi_label, rssi_text);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_HIDDEN);
}

static void display_request_wifi_scan(void)
{
    if (s_actions.on_wifi_scan == NULL) {
        return;
    }

    esp_err_t ret = s_actions.on_wifi_scan(s_actions.ctx);
    if (ret == ESP_OK) {
        s_last_wifi_scan_request_us = esp_timer_get_time();
        if (s_wifi_scan_state_label != NULL) {
            display_text_set_color(s_wifi_scan_state_label, lv_color_hex(0x1768B7), 0);
            display_text_set(s_wifi_scan_state_label, "扫描中");
        }
        if (s_wifi_scan_count_label != NULL) {
            lv_obj_add_flag(s_wifi_scan_count_label, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        if (s_wifi_scan_state_label != NULL) {
            display_text_set_color(s_wifi_scan_state_label, lv_color_hex(0xE45656), 0);
            display_text_set(s_wifi_scan_state_label, "扫描失败");
        }
        if (s_wifi_scan_count_label != NULL) {
            lv_obj_add_flag(s_wifi_scan_count_label, LV_OBJ_FLAG_HIDDEN);
        }
        s_last_wifi_scan_request_us = 0;
    }
}

static esp_err_t display_enter_app(display_app_id_t app_id)
{
    if (s_actions.on_enter_app == NULL) {
        return ESP_OK;
    }

    return s_actions.on_enter_app(app_id, s_actions.ctx);
}

static void display_return_home(void)
{
    if (s_actions.on_return_home != NULL) {
        (void)s_actions.on_return_home(s_actions.ctx);
    }
}

static void display_stop_call_scan_if_active(void)
{
    esp_err_t stop_ret = ESP_ERR_INVALID_STATE;

    if (!s_call_scan_active) {
        return;
    }

    s_call_scan_active = false;
    if (s_call_scan_image != NULL &&
        !lv_obj_has_flag(s_call_scan_image, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(s_call_scan_image, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_scan_owner == DISPLAY_SCAN_OWNER_TIRTC_CONFIG &&
        s_actions.on_stop_tirtc_config_scan != NULL) {
        stop_ret = s_actions.on_stop_tirtc_config_scan(s_actions.ctx);
        if (stop_ret != ESP_OK && stop_ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG,
                     "stop tirtc config scan failed: %s",
                     esp_err_to_name(stop_ret));
        }
    } else if (s_scan_owner == DISPLAY_SCAN_OWNER_WECHAT_CONTACT &&
               s_actions.on_stop_wechat_contact_scan != NULL) {
        stop_ret = s_actions.on_stop_wechat_contact_scan(s_actions.ctx);
        if (stop_ret != ESP_OK && stop_ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG,
                     "stop wechat contact scan failed: %s",
                     esp_err_to_name(stop_ret));
        }
    } else if (s_actions.on_stop_contact_scan != NULL) {
        stop_ret = s_actions.on_stop_contact_scan(s_actions.ctx);
        if (stop_ret != ESP_OK && stop_ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG,
                     "stop contact scan failed: %s",
                     esp_err_to_name(stop_ret));
        }
    }

    if (stop_ret == ESP_OK || stop_ret == ESP_ERR_INVALID_STATE) {
        display_release_call_scan_preview();
    }
}

static void display_exit_call_scan_to_previous(void)
{
    display_stop_call_scan_if_active();
    if (s_scan_owner == DISPLAY_SCAN_OWNER_TIRTC_CONFIG) {
        display_show_tirtc_config_page();
    } else if (s_scan_owner == DISPLAY_SCAN_OWNER_WECHAT_CONTACT) {
        display_show_wechat_add_page();
    } else {
        display_show_call_add_page();
    }
}

static esp_err_t display_capture_call_video_overlay(
    lv_obj_t *object,
    display_call_video_overlay_snapshot_t *snapshot)
{
    if (object == NULL || snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Video-call panels are deliberately opaque. Capture packed RGB565 so
     * the hot presentation path can copy complete rows instead of walking a
     * 24-bit RGB565+A8 image and blending every pixel in software. Rounded
     * corners outside the object remain the zero-filled dark background. */
    uint32_t needed = lv_snapshot_buf_size_needed(object, LV_IMG_CF_TRUE_COLOR);
    if (needed == 0U) {
        return ESP_FAIL;
    }
    if (snapshot->buffer == NULL || snapshot->capacity < needed) {
        return ESP_ERR_NO_MEM;
    }

    lv_img_dsc_t image = {0};
    if (lv_snapshot_take_to_buf(object,
                                LV_IMG_CF_TRUE_COLOR,
                                &image,
                                snapshot->buffer,
                                snapshot->capacity) != LV_RES_OK) {
        snapshot->valid = false;
        return ESP_FAIL;
    }

    lv_area_t coordinates = {0};
    lv_obj_get_coords(object, &coordinates);
    lv_coord_t ext_draw_size = _lv_obj_get_ext_draw_size(object);
    snapshot->image = image;
    snapshot->x = coordinates.x1 - ext_draw_size;
    snapshot->y = coordinates.y1 - ext_draw_size;
    snapshot->valid = true;
#if CONFIG_APP_MEDIA_PERIODIC_DIAGNOSTICS
    const uint16_t *pixels = (const uint16_t *)image.data;
    size_t pixel_count = (size_t)image.header.w * image.header.h;
    uint16_t expected_background = lv_obj_get_style_bg_color(object, LV_PART_MAIN).full;
    uint32_t checksum = 2166136261U;
    size_t background_pixels = 0U;
    for (size_t index = 0; index < pixel_count; ++index) {
        uint16_t value = pixels[index];
        checksum = (checksum ^ (uint8_t)value) * 16777619U;
        checksum = (checksum ^ (uint8_t)(value >> 8)) * 16777619U;
        if (value == expected_background) {
            ++background_pixels;
        }
    }
    snapshot->diagnostic_checksum = checksum;
    ESP_LOGI(TAG,
             "call video overlay snapshot: pos=%ld,%ld size=%ux%u bytes=%lu "
             "buffer=%p bg=%04x match=%u%% checksum=%08lx",
             (long)snapshot->x,
             (long)snapshot->y,
             (unsigned)image.header.w,
             (unsigned)image.header.h,
             (unsigned long)(pixel_count * sizeof(uint16_t)),
             image.data,
             (unsigned)expected_background,
             pixel_count > 0U ? (unsigned)((background_pixels * 100U) / pixel_count) : 0U,
             (unsigned long)checksum);
#endif
    return ESP_OK;
}

static const char *display_call_video_overlay_owner(
    const display_call_video_overlays_t *overlays)
{
    return overlays == &s_wechat_video_overlays ? "wechat" : "device-call";
}

static void display_release_call_video_overlay_snapshots(
    display_call_video_overlays_t *overlays)
{
    if (overlays == NULL || overlays->snapshot_storage == NULL) {
        return;
    }

    app_memory_snapshot_t before = {0};
    app_memory_snapshot_t after = {0};
    const size_t released_bytes = overlays->snapshot_storage_capacity;

    app_memory_get_snapshot(&before);
    free(overlays->snapshot_storage);
    overlays->snapshot_storage = NULL;
    overlays->snapshot_storage_capacity = 0U;
    memset(overlays->snapshots, 0, sizeof(overlays->snapshots));
    overlays->snapshot_at_us = 0;
    overlays->snapshot_dirty = true;
    app_memory_get_snapshot(&after);

    ESP_LOGI(TAG,
             "%s video overlay cache released: bytes=%u psram_largest=%u->%u",
             display_call_video_overlay_owner(overlays),
             (unsigned)released_bytes,
             (unsigned)before.psram_largest,
             (unsigned)after.psram_largest);
}

static esp_err_t display_prepare_call_video_overlay_snapshots(
    display_call_video_overlays_t *overlays,
    lv_obj_t *const *objects,
    size_t object_count)
{
    size_t needed[3] = {0};
    size_t total_needed = 0U;

    if (overlays == NULL || objects == NULL ||
        object_count != sizeof(overlays->snapshots) / sizeof(overlays->snapshots[0])) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t index = 0; index < object_count; ++index) {
        needed[index] = lv_snapshot_buf_size_needed(objects[index], LV_IMG_CF_TRUE_COLOR);
        if (needed[index] == 0U || total_needed > SIZE_MAX - needed[index]) {
            return ESP_FAIL;
        }
        total_needed += needed[index];
    }

    if (overlays->snapshot_storage_capacity < total_needed) {
        uint8_t *storage = app_memory_alloc_psram(total_needed);
        if (storage == NULL) {
            return ESP_ERR_NO_MEM;
        }
        free(overlays->snapshot_storage);
        overlays->snapshot_storage = storage;
        overlays->snapshot_storage_capacity = total_needed;
    }

    size_t offset = 0U;
    for (size_t index = 0; index < object_count; ++index) {
        overlays->snapshots[index].buffer = overlays->snapshot_storage + offset;
        overlays->snapshots[index].capacity = needed[index];
        overlays->snapshots[index].valid = false;
        offset += needed[index];
    }
    return ESP_OK;
}

static uint32_t display_update_call_video_overlay_snapshots(
    display_call_video_overlays_t *overlays)
{
    if (overlays == NULL || overlays->hidden) {
        return 0U;
    }

    lv_obj_t *objects[] = {
        overlays->top,
        overlays->controls,
        overlays->hangup,
    };
    int64_t started_us = esp_timer_get_time();
    bool ready = true;
    esp_err_t prepare_ret = display_prepare_call_video_overlay_snapshots(
        overlays,
        objects,
        sizeof(objects) / sizeof(objects[0]));
    if (prepare_ret != ESP_OK) {
        overlays->snapshot_dirty = true;
        ESP_LOGW(TAG,
                 "%s video overlay cache prepare failed: ret=%s",
                 display_call_video_overlay_owner(overlays),
                 esp_err_to_name(prepare_ret));
        return (uint32_t)(esp_timer_get_time() - started_us);
    }
    for (size_t index = 0; index < sizeof(objects) / sizeof(objects[0]); ++index) {
        esp_err_t ret = display_capture_call_video_overlay(
            objects[index],
            &overlays->snapshots[index]);
        if (ret != ESP_OK) {
            ready = false;
            ESP_LOGW(TAG,
                     "call video overlay snapshot failed: index=%u ret=%s",
                     (unsigned)index,
                     esp_err_to_name(ret));
        }
    }
    overlays->snapshot_dirty = !ready;
    overlays->snapshot_at_us = esp_timer_get_time();
    return (uint32_t)(overlays->snapshot_at_us - started_us);
}

static uint32_t display_compose_call_video_overlays(
    display_call_video_overlays_t *overlays,
    const uint16_t *pixels,
    uint16_t width,
    uint16_t height,
    uint32_t *snapshot_us,
    const uint16_t **output_pixels)
{
    if (snapshot_us != NULL) {
        *snapshot_us = 0U;
    }
    if (output_pixels != NULL) {
        *output_pixels = pixels;
    }
    if (overlays == NULL || overlays->hidden || pixels == NULL ||
        output_pixels == NULL || s_call_video_composition_pixels == NULL ||
        width != DISPLAY_CALL_VIDEO_SCREEN_WIDTH ||
        height != DISPLAY_CALL_VIDEO_SCREEN_HEIGHT) {
        return 0U;
    }

    /* The five-second overlay is a point-in-time status panel. Rebuilding its
     * LVGL snapshot every second steals one video-frame budget on P4. Refresh
     * only when the panel opens or a displayed value changes. */
    if (overlays->snapshot_dirty || overlays->snapshot_at_us == 0) {
        uint32_t elapsed = display_update_call_video_overlay_snapshots(overlays);
        if (snapshot_us != NULL) {
            *snapshot_us = elapsed;
        }
    }

    /* The renderer owns pixels until release_presented_rgb565(). Mutating that
     * slot in place lets the converter/cache or a later slot reuse race the
     * panel transfer. Preserve that ownership boundary: copy the frame once
     * during the five-second control window, then blend and DMA only from the
     * persistent composition buffer. Both inputs already use the panel's
     * LV_COLOR_16_SWAP RGB565 representation. */
    int64_t started_us = esp_timer_get_time();
    memcpy(s_call_video_composition_pixels,
           pixels,
           DISPLAY_CALL_VIDEO_FRAME_BYTES);
    *output_pixels = s_call_video_composition_pixels;
    for (size_t index = 0;
         index < sizeof(overlays->snapshots) / sizeof(overlays->snapshots[0]);
         ++index) {
        const display_call_video_overlay_snapshot_t *snapshot =
            &overlays->snapshots[index];
        if (!snapshot->valid || snapshot->image.data == NULL) {
            continue;
        }

        const uint16_t *source = (const uint16_t *)snapshot->image.data;
        uint16_t source_width = snapshot->image.header.w;
        uint16_t source_height = snapshot->image.header.h;
        for (uint16_t source_y = 0; source_y < source_height; ++source_y) {
            int32_t destination_y = snapshot->y + source_y;
            if (destination_y < 0 || destination_y >= height) {
                continue;
            }
            int32_t destination_x = snapshot->x;
            uint16_t source_x = 0U;
            uint16_t copy_width = source_width;
            if (destination_x < 0) {
                source_x = (uint16_t)(-destination_x);
                if (source_x >= copy_width) {
                    continue;
                }
                copy_width = (uint16_t)(copy_width - source_x);
                destination_x = 0;
            }
            if (destination_x >= width) {
                continue;
            }
            if ((uint32_t)destination_x + copy_width > width) {
                copy_width = (uint16_t)(width - destination_x);
            }
            memcpy(&s_call_video_composition_pixels[(size_t)destination_y * width +
                                                     (size_t)destination_x],
                   &source[(size_t)source_y * source_width + source_x],
                   (size_t)copy_width * sizeof(uint16_t));
        }
    }
    return (uint32_t)(esp_timer_get_time() - started_us);
}

static void display_set_call_video_overlays_hidden(
    display_call_video_overlays_t *overlays,
    lv_obj_t *image,
    lv_obj_t *placeholder,
    bool direct_lcd_active,
    bool hidden)
{
    if (overlays == NULL) {
        return;
    }

    lv_obj_t *objects[] = {
        overlays->top,
        overlays->controls,
        overlays->hangup,
    };
    for (size_t index = 0; index < sizeof(objects) / sizeof(objects[0]); ++index) {
        if (objects[index] != NULL &&
            !lv_obj_has_flag(objects[index], LV_OBJ_FLAG_HIDDEN)) {
            /* The live video path composites cached overlay pixels into the
             * RGB565 frame. Keep the source objects out of LVGL's panel flush
             * path so a control reveal cannot trigger a second full-screen
             * transfer or race the direct video DMA. LVGL invalidates an
             * object's old area whenever HIDDEN is added, even if that flag
             * is already set, so avoid adding it twice while direct video is
             * active. */
            lv_obj_add_flag(objects[index], LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (placeholder != NULL) {
        bool frame_visible =
            direct_lcd_active ||
            (image != NULL && !lv_obj_has_flag(image, LV_OBJ_FLAG_HIDDEN));
        if (hidden || frame_visible) {
            lv_obj_add_flag(placeholder, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(placeholder, LV_OBJ_FLAG_HIDDEN);
        }
    }
    overlays->hidden = hidden;
    overlays->snapshot_dirty = !hidden;
    overlays->refresh_trace_pending = !hidden;
    overlays->hide_at_us =
        hidden ? 0 : esp_timer_get_time() + DISPLAY_CALL_VIDEO_CONTROLS_VISIBLE_US;
}

static void display_reset_call_video_surface(void)
{
    s_call_video_sequence = 0;
    s_call_video_first_frame_logged = false;
    s_call_video_direct_lcd_active = false;
    s_call_video_direct_lcd_failed = false;
    s_call_video_last_presented_at_us = 0;
    s_call_video_last_stall_log_at_us = 0;
    s_call_video_last_presented_sequence = 0;
    display_set_call_video_overlays_hidden(&s_call_video_overlays,
                                           s_call_video_image,
                                           s_call_video_placeholder_label,
                                           false,
                                           true);
    display_release_call_video_overlay_snapshots(&s_call_video_overlays);
    if (s_call_video_image != NULL) {
        if (!lv_obj_has_flag(s_call_video_image, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(s_call_video_image, LV_OBJ_FLAG_HIDDEN);
        }
        lv_img_cache_invalidate_src(&s_call_video_image_dsc);
        s_call_video_image_dsc.data = NULL;
    }
    call_video_renderer_release_presented_rgb565();
    if (s_call_video_placeholder_label != NULL) {
        lv_obj_clear_flag(s_call_video_placeholder_label, LV_OBJ_FLAG_HIDDEN);
    }
}

static void display_reset_wechat_video_surface(void)
{
    s_wechat_video_sequence = 0;
    s_wechat_video_first_frame_logged = false;
    s_wechat_video_direct_lcd_active = false;
    s_wechat_video_direct_lcd_failed = false;
    s_wechat_video_last_presented_at_us = 0;
    s_wechat_video_last_stall_log_at_us = 0;
    s_wechat_video_last_presented_sequence = 0;
    display_set_call_video_overlays_hidden(&s_wechat_video_overlays,
                                           s_wechat_video_image,
                                           s_wechat_video_placeholder_label,
                                           false,
                                           true);
    display_release_call_video_overlay_snapshots(&s_wechat_video_overlays);
    if (s_wechat_video_image != NULL) {
        if (!lv_obj_has_flag(s_wechat_video_image, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(s_wechat_video_image, LV_OBJ_FLAG_HIDDEN);
        }
        lv_img_cache_invalidate_src(&s_wechat_video_image_dsc);
        s_wechat_video_image_dsc.data = NULL;
    }
    call_video_renderer_release_presented_rgb565();
    if (s_wechat_video_placeholder_label != NULL) {
        lv_obj_clear_flag(s_wechat_video_placeholder_label, LV_OBJ_FLAG_HIDDEN);
    }
}

static void display_apply_call_video_layout(bool active)
{
    const display_driver_orientation_t target =
        DISPLAY_DRIVER_ORIENTATION_LANDSCAPE;
    if (s_call_video_landscape_active == active &&
        display_driver_get_orientation() == target) {
        return;
    }

    display_set_video_refresh_enabled(false);
    display_reset_call_video_surface();
    display_reset_wechat_video_surface();
    if (display_driver_get_orientation() != target) {
        esp_err_t ret = display_driver_set_orientation(target);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG,
                     "call video landscape restore failed: %s",
                     esp_err_to_name(ret));
            return;
        }
    }

    s_call_video_landscape_active = active;
    if (s_call_active_page != NULL) {
        lv_obj_set_size(s_call_active_page,
                        DISPLAY_CALL_VIDEO_SCREEN_WIDTH,
                        DISPLAY_CALL_VIDEO_SCREEN_HEIGHT);
    }
    if (s_call_audio_panel != NULL) {
        lv_obj_set_size(s_call_audio_panel, DISPLAY_NATIVE_WIDTH, DISPLAY_NATIVE_HEIGHT);
    }
    if (s_call_video_panel != NULL) {
        lv_obj_set_size(s_call_video_panel,
                        DISPLAY_CALL_VIDEO_SCREEN_WIDTH,
                        DISPLAY_CALL_VIDEO_SCREEN_HEIGHT);
    }
    if (s_wechat_active_page != NULL) {
        lv_obj_set_size(s_wechat_active_page,
                        DISPLAY_CALL_VIDEO_SCREEN_WIDTH,
                        DISPLAY_CALL_VIDEO_SCREEN_HEIGHT);
    }
    if (s_wechat_video_panel != NULL) {
        lv_obj_set_size(s_wechat_video_panel,
                        DISPLAY_CALL_VIDEO_SCREEN_WIDTH,
                        DISPLAY_CALL_VIDEO_SCREEN_HEIGHT);
    }
    lv_obj_invalidate(lv_scr_act());
}

static void display_show_page(lv_obj_t *page)
{
    bool keep_video_layout =
        (page == s_call_active_page && s_call_visible_type == DISPLAY_CALL_TYPE_VIDEO) ||
        (page == s_wechat_active_page &&
         CONFIG_APP_WECHAT_VOIP_REMOTE_VIDEO_ENABLE);
    if (!keep_video_layout) {
        display_apply_call_video_layout(false);
    }
    if (page != s_call_scan_page) {
        display_stop_call_scan_if_active();
    }
    display_hide_wechat_delete_confirm();
    display_hide_call_delete_confirm();
    display_hide_wifi_alert();
    display_page_registry_show(&s_page_registry, page);
    s_wifi_connect_state = DISPLAY_WIFI_CONNECT_STATE_IDLE;
    display_hide_keyboard();
}

static void display_show_home_page(void)
{
    display_update_home_status_bar(&s_last_status);
    display_show_page(s_home_page);
    display_update_binding_prompt(&s_last_status);
}

static void display_show_main_page(void)
{
    display_show_page(s_main_page);
}

static esp_err_t display_request_call_contacts_refresh(const char *source)
{
    esp_err_t ret = ESP_ERR_INVALID_STATE;

    if (s_actions.on_refresh_call_contacts != NULL) {
        ret = s_actions.on_refresh_call_contacts(s_actions.ctx);
        ESP_LOGI(CALL_FLOW_TAG,
                 "stage=contacts_refresh_requested source=%s ret=%s",
                 source != NULL ? source : "unknown",
                 esp_err_to_name(ret));
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "contact refresh request failed: %s", esp_err_to_name(ret));
        }
    }
    return ret;
}

static void display_show_call_page(void)
{
    if (s_call_page == NULL) {
        display_build_call_page(lv_scr_act());
    }
    display_show_page(s_call_page);
    display_update_call_page(&s_last_status);
}

static void display_show_call_add_page(void)
{
    if (s_call_add_page == NULL) {
        display_build_call_add_page(lv_scr_act());
    }
    if (s_call_scan_info_overlay != NULL) {
        lv_obj_add_flag(s_call_scan_info_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    display_hide_keyboard();
    display_update_call_add_field_labels();
    display_show_page(s_call_add_page);
}

static void display_show_call_add_edit_page(display_call_add_field_t field)
{
    const char *current_value = NULL;
    size_t max_len = display_call_add_field_max_len(field);

    if (field >= DISPLAY_CALL_ADD_FIELD_COUNT) {
        return;
    }
    if (s_call_add_edit_page == NULL) {
        display_build_call_add_edit_page(lv_scr_act());
    }

    s_call_add_edit_field = field;
    current_value = display_call_add_field_buffer(field);
    display_show_page(s_call_add_edit_page);
    if (s_call_add_edit_hint_label != NULL) {
        display_text_set(s_call_add_edit_hint_label, display_call_add_field_title(field));
    }
    if (s_call_add_edit_ta != NULL) {
        lv_textarea_set_max_length(s_call_add_edit_ta, max_len);
        lv_textarea_set_placeholder_text(s_call_add_edit_ta, display_call_add_field_title(field));
        lv_textarea_set_text(s_call_add_edit_ta, current_value != NULL ? current_value : "");
        lv_textarea_set_cursor_pos(s_call_add_edit_ta,
                                   (uint32_t)strlen(current_value != NULL ? current_value : ""));
    }
    display_update_call_add_edit_feedback(NULL, lv_color_hex(0x0D8A59));
    if (s_call_add_edit_keyboard != NULL && s_call_add_edit_ta != NULL) {
        lv_keyboard_set_textarea(s_call_add_edit_keyboard, s_call_add_edit_ta);
        lv_keyboard_set_mode(s_call_add_edit_keyboard, LV_KEYBOARD_MODE_USER_1);
        lv_obj_clear_flag(s_call_add_edit_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_call_add_edit_keyboard);
        lv_obj_add_state(s_call_add_edit_ta, LV_STATE_FOCUSED);
        lv_event_send(s_call_add_edit_ta, LV_EVENT_FOCUSED, NULL);
    }
}

static void display_show_call_scan_page(void)
{
    if (s_call_scan_page == NULL) {
        display_build_call_scan_page(lv_scr_act());
    }
    s_scan_owner = DISPLAY_SCAN_OWNER_CALL;
    s_call_scan_active = true;
    s_call_scan_preview_first_frame_logged = false;
    if (s_call_scan_image != NULL) {
        lv_obj_add_flag(s_call_scan_image, LV_OBJ_FLAG_HIDDEN);
    }
    display_show_page(s_call_scan_page);
}

static void __attribute__((unused)) display_show_tirtc_config_scan_page(void)
{
    if (s_call_scan_page == NULL) {
        display_build_call_scan_page(lv_scr_act());
    }
    s_scan_owner = DISPLAY_SCAN_OWNER_TIRTC_CONFIG;
    s_call_scan_active = true;
    s_call_scan_preview_first_frame_logged = false;
    if (s_call_scan_image != NULL) {
        lv_obj_add_flag(s_call_scan_image, LV_OBJ_FLAG_HIDDEN);
    }
    display_show_page(s_call_scan_page);
}

static void display_show_wechat_scan_page(void)
{
    if (s_call_scan_page == NULL) {
        display_build_call_scan_page(lv_scr_act());
    }
    s_scan_owner = DISPLAY_SCAN_OWNER_WECHAT_CONTACT;
    s_call_scan_active = true;
    s_call_scan_preview_first_frame_logged = false;
    if (s_call_scan_image != NULL) {
        lv_obj_add_flag(s_call_scan_image, LV_OBJ_FLAG_HIDDEN);
    }
    display_show_page(s_call_scan_page);
}

static void display_show_call_list_page(void)
{
    if (s_call_list_page == NULL) {
        display_build_call_list_page(lv_scr_act());
    }
    display_show_page(s_call_list_page);
}

static void display_show_call_remark_page(uint8_t contact_index)
{
    if (contact_index >= s_call_contact_count ||
        s_call_contacts[contact_index].device_id[0] == '\0') {
        display_show_wifi_alert("联系人", "联系人不存在");
        return;
    }
    if (s_call_remark_page == NULL) {
        display_build_call_remark_page(lv_scr_act());
    }

    strlcpy(s_call_remark_edit_device_id,
            s_call_contacts[contact_index].device_id,
            sizeof(s_call_remark_edit_device_id));
    display_show_page(s_call_remark_page);
    if (s_call_remark_ta != NULL) {
        lv_textarea_set_text(s_call_remark_ta, s_call_contacts[contact_index].remark);
        lv_textarea_set_cursor_pos(s_call_remark_ta,
                                   (uint32_t)strlen(s_call_contacts[contact_index].remark));
    }
    if (s_call_remark_status_label != NULL) {
        display_text_set_color(s_call_remark_status_label, lv_color_hex(0x0D8A59), 0);
        display_text_set(s_call_remark_status_label, "保存后同步到云端");
    }
    display_show_text_keyboard(s_call_remark_keyboard, s_call_remark_ta);
}

static void display_show_call_active_page(void)
{
    bool was_visible = display_page_is_visible(s_call_active_page);

    if (s_call_active_page == NULL) {
        /* Audio and video share the native landscape coordinate space. */
        display_apply_call_video_layout(false);
        display_build_call_active_page(lv_scr_act());
        was_visible = false;
    }
    if (!was_visible) {
        display_set_call_video_overlays_hidden(&s_call_video_overlays,
                                               s_call_video_image,
                                               s_call_video_placeholder_label,
                                               s_call_video_direct_lcd_active,
                                               true);
    }
    display_update_call_active_page(&s_last_status);
    display_show_page(s_call_active_page);
    if (!was_visible) {
        s_call_active_page_opened_us = esp_timer_get_time();
    }
}

static void display_show_wechat_page(void)
{
    if (s_wechat_page == NULL) {
        display_build_wechat_page(lv_scr_act());
    }
    display_show_page(s_wechat_page);
    display_update_wechat_page(&s_last_status);
}

static void display_show_wechat_add_page(void)
{
    if (s_wechat_add_page == NULL) {
        display_build_wechat_add_page(lv_scr_act());
    }
    if (s_wechat_scan_info_overlay != NULL) {
        lv_obj_add_flag(s_wechat_scan_info_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    display_update_wechat_add_field_label();
    display_show_page(s_wechat_add_page);
}

static void display_show_wechat_add_edit_page(void)
{
    if (s_wechat_add_edit_page == NULL) {
        display_build_wechat_add_edit_page(lv_scr_act());
    }

    display_show_page(s_wechat_add_edit_page);
    if (s_wechat_add_edit_ta != NULL) {
        lv_textarea_set_max_length(s_wechat_add_edit_ta, DISPLAY_WECHAT_OPEN_ID_LENGTH);
        lv_textarea_set_text(s_wechat_add_edit_ta, s_wechat_add_open_id);
        lv_textarea_set_cursor_pos(s_wechat_add_edit_ta, (uint32_t)strlen(s_wechat_add_open_id));
    }
    display_update_wechat_add_edit_feedback(NULL, lv_color_hex(0x0D8A59));
    if (s_wechat_add_edit_keyboard != NULL && s_wechat_add_edit_ta != NULL) {
        lv_keyboard_set_textarea(s_wechat_add_edit_keyboard, s_wechat_add_edit_ta);
        lv_keyboard_set_mode(s_wechat_add_edit_keyboard, LV_KEYBOARD_MODE_USER_1);
        lv_obj_clear_flag(s_wechat_add_edit_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_wechat_add_edit_keyboard);
        lv_obj_add_state(s_wechat_add_edit_ta, LV_STATE_FOCUSED);
        lv_event_send(s_wechat_add_edit_ta, LV_EVENT_FOCUSED, NULL);
    }
}

static void display_show_wechat_list_page(void)
{
    if (s_wechat_list_page == NULL) {
        display_build_wechat_list_page(lv_scr_act());
    }
    display_update_wechat_contact_list(&s_last_status);
    display_show_page(s_wechat_list_page);
}

static void display_show_wechat_remark_page(uint8_t contact_index)
{
    const display_wechat_contact_t *contact = NULL;

    if (contact_index >= DISPLAY_WECHAT_CONTACT_COUNT ||
        contact_index >= s_last_status.wechat_contact_count) {
        display_show_wifi_alert("微信联系人", "联系人不存在");
        return;
    }
    contact = &s_last_status.wechat_contacts[contact_index];
    if (contact->open_id[0] == '\0') {
        display_show_wifi_alert("微信联系人", "联系人不存在");
        return;
    }
    if (s_wechat_remark_page == NULL) {
        display_build_wechat_remark_page(lv_scr_act());
    }

    strlcpy(s_wechat_remark_edit_open_id,
            contact->open_id,
            sizeof(s_wechat_remark_edit_open_id));
    display_show_page(s_wechat_remark_page);
    if (s_wechat_remark_ta != NULL) {
        lv_textarea_set_text(s_wechat_remark_ta, contact->remark);
        lv_textarea_set_cursor_pos(s_wechat_remark_ta, (uint32_t)strlen(contact->remark));
    }
    if (s_wechat_remark_status_label != NULL) {
        display_text_set_color(s_wechat_remark_status_label, lv_color_hex(0x0D8A59), 0);
        display_text_set(s_wechat_remark_status_label, "点击保存生效");
    }
    display_show_text_keyboard(s_wechat_remark_keyboard, s_wechat_remark_ta);
}

static void display_show_wechat_active_page(void)
{
    bool was_visible = display_page_is_visible(s_wechat_active_page);

    display_apply_call_video_layout(
        CONFIG_APP_WECHAT_VOIP_REMOTE_VIDEO_ENABLE);
    if (s_wechat_active_page == NULL) {
        display_build_wechat_active_page(lv_scr_act());
        was_visible = false;
    }
    if (!was_visible) {
        display_set_call_video_overlays_hidden(&s_wechat_video_overlays,
                                               s_wechat_video_image,
                                               s_wechat_video_placeholder_label,
                                               s_wechat_video_direct_lcd_active,
                                               true);
    }
    display_show_page(s_wechat_active_page);
    display_update_wechat_active_page(&s_last_status);
    if (!was_visible) {
        s_wechat_active_page_opened_us = esp_timer_get_time();
    }
}

static void display_show_system_page(void)
{
    if (s_system_page == NULL) {
        display_build_system_page(lv_scr_act());
    }
    display_show_page(s_system_page);
    display_update_system_memory(&s_last_status);
}

static void display_open_wifi_page(display_page_id_t parent_page)
{
    s_wifi_parent_page = parent_page;
    display_show_wifi_page();
}

static void display_show_wifi_page(void)
{
    if (s_wifi_page == NULL) {
        display_build_wifi_page(lv_scr_act());
    }
    display_show_page(s_wifi_page);
    display_update_wifi_scan_state(&s_last_status);
    display_refresh_wifi_list(&s_last_status);
}

static void display_show_wifi_connect_page(void)
{
    if (s_wifi_connect_page == NULL) {
        display_build_wifi_connect_page(lv_scr_act());
    }
    display_show_page(s_wifi_connect_page);
    if (s_password_ta != NULL) {
        display_prepare_password_entry(&s_last_status);
    }
    s_wifi_connect_state = DISPLAY_WIFI_CONNECT_STATE_IDLE;
    if (s_keyboard != NULL && s_password_ta != NULL) {
        display_set_wifi_keyboard_mode(LV_KEYBOARD_MODE_USER_1);
        display_layout_wifi_keyboard();
        lv_obj_clear_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_keyboard);
        lv_obj_add_state(s_password_ta, LV_STATE_FOCUSED);
        lv_event_send(s_password_ta, LV_EVENT_FOCUSED, NULL);
    }
}

static void display_show_uuid_edit_page(void)
{
    const char *current_uuid = s_last_status.device_uuid[0] != '\0' ? s_last_status.device_uuid : "";

    if (s_uuid_edit_page == NULL) {
        display_build_uuid_edit_page(lv_scr_act());
    }

    display_show_page(s_uuid_edit_page);
    if (s_uuid_ta != NULL) {
        lv_textarea_set_text(s_uuid_ta, current_uuid);
        lv_textarea_set_cursor_pos(s_uuid_ta, (uint32_t)strlen(current_uuid));
    }
    display_update_uuid_edit_feedback(NULL, lv_color_hex(0x48656F));
}

static void display_show_ai_chat_page(void)
{
    if (s_ai_chat_page == NULL) {
        display_build_ai_chat_page(lv_scr_act());
    }
    display_update_ai_chat_page(&s_last_status);
    display_show_page(s_ai_chat_page);
}

static void display_show_ai_chat_settings_page(void)
{
    if (s_ai_chat_settings_page == NULL) {
        display_build_ai_chat_settings_page(lv_scr_act());
    }
    display_update_ai_chat_settings_page(&s_last_status);
    display_show_page(s_ai_chat_settings_page);
}

static void display_update_home_indicators(void)
{
    if (s_home_indicator_dots[0] == NULL || s_home_indicator_dots[1] == NULL) {
        return;
    }

    bool second_page = s_home_indicator_second_page;

    if (s_home_indicator_valid && s_home_indicator_second_page == second_page) {
        return;
    }
    s_home_indicator_valid = true;
    s_home_indicator_second_page = second_page;

    lv_obj_set_pos(s_home_indicator_dots[0], 0, 0);
    lv_obj_set_size(s_home_indicator_dots[0], second_page ? 8 : 22, 8);
    lv_obj_set_style_bg_color(s_home_indicator_dots[0],
                              second_page ? lv_color_hex(0xBCCAD8) : lv_color_hex(0x1768B7),
                              0);

    lv_obj_set_pos(s_home_indicator_dots[1], second_page ? 15 : 29, 0);
    lv_obj_set_size(s_home_indicator_dots[1], second_page ? 22 : 8, 8);
    lv_obj_set_style_bg_color(s_home_indicator_dots[1],
                              second_page ? lv_color_hex(0x1768B7) : lv_color_hex(0xBCCAD8),
                              0);
}

static void display_home_set_page(bool second_page)
{
    if (s_home_pages[0] != NULL && s_home_pages[1] != NULL) {
        lv_obj_set_pos(s_home_pages[0], 0, 0);
        lv_obj_set_pos(s_home_pages[1], 0, 0);
        if (second_page) {
            lv_obj_add_flag(s_home_pages[0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_home_pages[1], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_home_pages[0], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_home_pages[1], LV_OBJ_FLAG_HIDDEN);
        }
    }

    s_home_indicator_second_page = second_page;
    s_home_indicator_valid = false;
    display_update_home_indicators();
}

static bool display_home_consume_suppressed_click(void)
{
    return false;
}

static lv_obj_t *display_create_binding_prompt_button(lv_obj_t *parent,
                                                      lv_coord_t x,
                                                      lv_coord_t y,
                                                      lv_coord_t width,
                                                      lv_coord_t height,
                                                      lv_coord_t radius,
                                                      lv_color_t fill,
                                                      lv_color_t stroke,
                                                      const char *text,
                                                      lv_coord_t text_y,
                                                      lv_color_t text_color,
                                                      uint8_t font_size,
                                                      lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_t *label = NULL;

    lv_obj_remove_style_all(btn);
    display_obj_set_design_pos(btn, x, y);
    display_obj_set_design_size(btn, width, height);
    lv_obj_set_style_radius(btn, display_scale_square(radius), 0);
    lv_obj_set_style_bg_color(btn, fill, 0);
    lv_obj_set_style_bg_color(btn, lv_color_darken(fill, 18), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, stroke, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    if (cb != NULL) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    }

    label = display_create_figma_text(btn,
                                      text,
                                      0,
                                      text_y,
                                      width,
                                      text_color,
                                      font_size,
                                      LV_TEXT_ALIGN_CENTER);
    if (label != NULL) {
        lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    }
    return btn;
}

static void display_build_binding_nowifi_dialog(lv_obj_t *overlay)
{
    lv_obj_t *panel = NULL;
    lv_obj_t *icon = NULL;
    lv_obj_t *icon_label = NULL;

    s_binding_nowifi_dialog = display_create_figma_box(overlay,
                                                       24,
                                                       46,
                                                       272,
                                                       148,
                                                       lv_color_hex(0xFFFFFF),
                                                       lv_color_hex(0xD5E0EB),
                                                       8);

    display_create_figma_text(s_binding_nowifi_dialog,
                              "设备绑定",
                              0,
                              11,
                              272,
                              lv_color_hex(0x10243E),
                              16,
                              LV_TEXT_ALIGN_CENTER);

    panel = display_create_figma_box(s_binding_nowifi_dialog,
                                     15,
                                     41,
                                     240,
                                     48,
                                     lv_color_hex(0xFFF1F1),
                                     lv_color_hex(0xFFAEAE),
                                     6);

    icon = lv_obj_create(panel);
    lv_obj_remove_style_all(icon);
    display_obj_set_design_pos(icon, 17, 12);
    display_obj_set_design_size(icon, 22, 22);
    lv_obj_set_style_radius(icon, display_scale_square(11), 0);
    lv_obj_set_style_bg_color(icon, lv_color_hex(0xE41C1C), 0);
    lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    icon_label = lv_label_create(icon);
    lv_obj_set_width(icon_label, display_scale_x(22));
    lv_obj_set_style_text_align(icon_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(icon_label, display_ascii_font(14), 0);
    display_text_set_color(icon_label, lv_color_hex(0xFFFFFF), 0);
    display_text_set(icon_label, "!");
    lv_obj_center(icon_label);

    display_create_figma_text(panel,
                              "请先连接 WiFi",
                              47,
                              7,
                              170,
                              lv_color_hex(0x10243E),
                              14,
                              LV_TEXT_ALIGN_LEFT);
    display_create_figma_text(panel,
                              "联网后自动获取6位绑定码",
                              47,
                              27,
                              180,
                              lv_color_hex(0x64758A),
                              10,
                              LV_TEXT_ALIGN_LEFT);

    display_create_binding_prompt_button(s_binding_nowifi_dialog,
                                         15,
                                         107,
                                         240,
                                         28,
                                         6,
                                         lv_color_hex(0x20BF7A),
                                         lv_color_hex(0x20BF7A),
                                         "设置WiFi",
                                         4,
                                         lv_color_hex(0xFFFFFF),
                                         13,
                                         display_binding_wifi_btn_cb);
}

static void display_build_binding_code_dialog(lv_obj_t *overlay)
{
    lv_obj_t *code_panel = NULL;
    lv_obj_t *qr_card = NULL;

    s_binding_code_dialog = display_create_figma_box(overlay,
                                                     22,
                                                     32,
                                                     276,
                                                     176,
                                                     lv_color_hex(0xFFFFFF),
                                                     lv_color_hex(0xD5E0EB),
                                                     8);

    display_create_figma_text(s_binding_code_dialog,
                              "设备绑定",
                              0,
                              8,
                              276,
                              lv_color_hex(0x10243E),
                              16,
                              LV_TEXT_ALIGN_CENTER);

    code_panel = display_create_figma_box(s_binding_code_dialog,
                                          15,
                                          33,
                                          136,
                                          50,
                                          lv_color_hex(0xF7FBFF),
                                          lv_color_hex(0xD5E0EB),
                                          6);
    display_create_figma_text(code_panel,
                              "绑定码",
                              0,
                              6,
                              136,
                              lv_color_hex(0x64758A),
                              10,
                              LV_TEXT_ALIGN_CENTER);

    s_binding_code_label = lv_label_create(code_panel);
    display_obj_set_design_pos(s_binding_code_label, 0, 19);
    lv_obj_set_width(s_binding_code_label, display_scale_x(136));
    lv_label_set_long_mode(s_binding_code_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_binding_code_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_binding_code_label, display_ascii_font(30), 0);
    lv_obj_set_style_text_letter_space(s_binding_code_label, 1, 0);
    display_text_set_color(s_binding_code_label, lv_color_hex(0x10243E), 0);
    display_text_set(s_binding_code_label, DISPLAY_BINDING_CODE_PLACEHOLDER);

    display_create_figma_text(s_binding_code_dialog,
                              "绑定网址",
                              15,
                              89,
                              136,
                              lv_color_hex(0x64758A),
                              10,
                              LV_TEXT_ALIGN_LEFT);
    display_create_figma_text(s_binding_code_dialog,
                              DISPLAY_BINDING_PLATFORM_URL,
                              15,
                              104,
                              138,
                              lv_color_hex(0x10243E),
                              8,
                              LV_TEXT_ALIGN_LEFT);

    qr_card = display_create_figma_box(s_binding_code_dialog,
                                       158,
                                       30,
                                       101,
                                       101,
                                       lv_color_hex(0xFFFFFF),
                                       lv_color_hex(0xD5E0EB),
                                       5);
#if LV_USE_QRCODE
    s_binding_platform_qrcode = lv_img_create(qr_card);
    lv_obj_set_size(s_binding_platform_qrcode,
                    display_scale_square(94),
                    display_scale_square(94));
    display_obj_set_design_pos(s_binding_platform_qrcode, 3, 3);
    lv_obj_clear_flag(s_binding_platform_qrcode, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    if (display_qr_image_update(&s_binding_platform_qr_image,
                                s_binding_platform_qrcode,
                                display_scale_square(94),
                                DISPLAY_BINDING_PLATFORM_URL,
                                strlen(DISPLAY_BINDING_PLATFORM_URL)) != ESP_OK) {
        ESP_LOGW(TAG, "binding platform qr update failed");
    }
#else
    display_create_figma_text(qr_card,
                              "QR",
                              0,
                              39,
                              101,
                              lv_color_hex(0x10243E),
                              16,
                              LV_TEXT_ALIGN_CENTER);
#endif

    display_create_binding_prompt_button(s_binding_code_dialog,
                                         15,
                                         147,
                                         244,
                                         22,
                                         5,
                                         lv_color_hex(0xFFFFFF),
                                         lv_color_hex(0xD5E0EB),
                                         "刷新",
                                         3,
                                         lv_color_hex(0x10243E),
                                         12,
                                         display_binding_refresh_btn_cb);
}

static void display_build_binding_prompt_overlay(lv_obj_t *parent)
{
    if (parent == NULL || s_binding_prompt_overlay != NULL) {
        return;
    }

    s_binding_prompt_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(s_binding_prompt_overlay);
    lv_obj_set_pos(s_binding_prompt_overlay, 0, 0);
    lv_obj_set_size(s_binding_prompt_overlay, DISPLAY_DRIVER_WIDTH, DISPLAY_DRIVER_HEIGHT);
    lv_obj_set_style_bg_color(s_binding_prompt_overlay, lv_color_hex(0x10243E), 0);
    lv_obj_set_style_bg_opa(s_binding_prompt_overlay, 82, 0);
    lv_obj_set_style_pad_all(s_binding_prompt_overlay, 0, 0);
    lv_obj_clear_flag(s_binding_prompt_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_binding_prompt_overlay, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);

    display_build_binding_nowifi_dialog(s_binding_prompt_overlay);
    display_build_binding_code_dialog(s_binding_prompt_overlay);
    lv_obj_add_flag(s_binding_nowifi_dialog, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_binding_code_dialog, LV_OBJ_FLAG_HIDDEN);
}

static void display_update_binding_prompt(const display_status_t *status)
{
    bool bound = false;
    bool binding_rebind_active = false;
    bool binding_disabled = false;
    bool show_code_dialog = false;
    bool should_show = false;
    const char *code_text = DISPLAY_BINDING_CODE_PLACEHOLDER;

    if (s_binding_prompt_overlay == NULL || status == NULL) {
        return;
    }

    binding_disabled = status->binding_state == DISPLAY_DEVICE_BINDING_STATE_DISABLED;
    binding_rebind_active = status->binding_state == DISPLAY_DEVICE_BINDING_STATE_REPORTING ||
                            status->binding_state == DISPLAY_DEVICE_BINDING_STATE_WAITING_USER ||
                            status->binding_state == DISPLAY_DEVICE_BINDING_STATE_ERROR ||
                            status->binding_code[0] != '\0';
    bound = status->binding_state == DISPLAY_DEVICE_BINDING_STATE_BOUND ||
            (status->tirtc_device_id[0] != '\0' && !binding_rebind_active);
    should_show = display_page_is_visible(s_home_page) && !binding_disabled && !bound;
    if (!should_show) {
        if (s_binding_prompt_visible) {
            lv_obj_add_flag(s_binding_prompt_overlay, LV_OBJ_FLAG_HIDDEN);
            s_binding_prompt_visible = false;
            s_binding_prompt_code_dialog_visible = false;
        }
        return;
    }

    show_code_dialog = status->network_connected;
    if (show_code_dialog && status->binding_code[0] != '\0') {
        code_text = status->binding_code;
    }

    if (s_binding_code_label != NULL &&
        strcmp(s_binding_prompt_code_text, code_text) != 0) {
        display_text_set(s_binding_code_label, code_text);
        strlcpy(s_binding_prompt_code_text, code_text, sizeof(s_binding_prompt_code_text));
    }

    if (!s_binding_prompt_visible ||
        s_binding_prompt_code_dialog_visible != show_code_dialog) {
        if (s_binding_nowifi_dialog != NULL) {
            if (show_code_dialog) {
                lv_obj_add_flag(s_binding_nowifi_dialog, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_clear_flag(s_binding_nowifi_dialog, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (s_binding_code_dialog != NULL) {
            if (show_code_dialog) {
                lv_obj_clear_flag(s_binding_code_dialog, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_binding_code_dialog, LV_OBJ_FLAG_HIDDEN);
            }
        }
        s_binding_prompt_code_dialog_visible = show_code_dialog;
    }

    if (!s_binding_prompt_visible) {
        lv_obj_clear_flag(s_binding_prompt_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_binding_prompt_overlay);
        s_binding_prompt_visible = true;
    }
}

static void display_binding_wifi_btn_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    if (s_binding_prompt_overlay != NULL) {
        lv_obj_add_flag(s_binding_prompt_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    display_open_wifi_page(DISPLAY_PAGE_HOME);
    display_request_wifi_scan();
}

static void display_binding_refresh_btn_cb(lv_event_t *event)
{
    esp_err_t ret = ESP_OK;

    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    if (s_actions.on_start_device_binding == NULL) {
        ESP_LOGW(TAG, "binding refresh unavailable");
        return;
    }

    ret = s_actions.on_start_device_binding(s_actions.ctx);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "binding refresh failed: %s", esp_err_to_name(ret));
    }
    display_update_binding_prompt(&s_last_status);
}

static void display_home_view_btn_cb(lv_event_t *event)
{
    (void)event;
    if (display_home_consume_suppressed_click()) {
        return;
    }
    if (display_enter_app(DISPLAY_APP_DEVICE) != ESP_OK) {
        display_show_wifi_alert("APP", "Open failed.");
    }
}

static void display_home_call_btn_cb(lv_event_t *event)
{
    (void)event;
    if (display_home_consume_suppressed_click()) {
        return;
    }
    if (display_enter_app(DISPLAY_APP_CALL) != ESP_OK) {
        display_show_wifi_alert("APP", "Open failed.");
    }
}

static void display_home_wechat_btn_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    if (display_home_consume_suppressed_click()) {
        return;
    }
    esp_err_t ret = display_enter_app(DISPLAY_APP_WECHAT);
    if (ret != ESP_OK) {
        display_show_wifi_alert("APP", ret == ESP_ERR_INVALID_STATE ? "Connect WiFi first." : "Open failed.");
    }
}

static void display_home_ai_btn_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    if (display_home_consume_suppressed_click()) {
        return;
    }

    esp_err_t ret = display_enter_app(DISPLAY_APP_AI_CHAT);
    if (ret != ESP_OK) {
        display_show_wifi_alert(DISPLAY_AI_APP_TITLE,
                                ret == ESP_ERR_INVALID_STATE ? "Connect WiFi first." : "Open failed.");
    }
}

static void display_home_settings_btn_cb(lv_event_t *event)
{
    (void)event;
    if (display_home_consume_suppressed_click()) {
        return;
    }
    esp_err_t ret = display_enter_app(DISPLAY_APP_SYSTEM);
    if (ret != ESP_OK) {
        display_show_wifi_alert("APP", "Open failed.");
    }
}

static void display_ai_back_btn_cb(lv_event_t *event)
{
    (void)event;
    display_return_home();
}

static void display_ai_settings_btn_cb(lv_event_t *event)
{
    (void)event;
    display_show_ai_chat_settings_page();
}

static void display_ai_settings_back_btn_cb(lv_event_t *event)
{
    (void)event;
    display_show_ai_chat_page();
}

static void display_ai_start_new_btn_cb(lv_event_t *event)
{
    (void)event;
    if (s_actions.on_start_ai_chat == NULL) {
        return;
    }

    esp_err_t ret = s_actions.on_start_ai_chat(s_actions.ctx);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        display_show_wifi_alert(DISPLAY_AI_APP_TITLE, "Start failed.");
        return;
    }
    if (ret == ESP_ERR_INVALID_STATE && !s_last_status.network_connected) {
        display_show_wifi_alert(DISPLAY_AI_APP_TITLE, "Connect WiFi first.");
    }
}

static void display_show_network_test_page(void)
{
    if (s_network_test_page == NULL) {
        display_build_network_test_page(lv_scr_act());
    }
    display_update_network_test_page(&s_last_status);
    display_show_page(s_network_test_page);
}

static void display_show_tirtc_config_page(void)
{
    if (s_tirtc_config_page == NULL) {
        display_build_tirtc_config_page(lv_scr_act());
    }
    display_update_tirtc_config_page(&s_last_status);
    display_show_page(s_tirtc_config_page);
}

static void display_show_tirtc_config_edit_page(display_tirtc_config_field_t field)
{
    const char *current_value = display_tirtc_config_field_value(&s_last_status, field);
    size_t max_len = display_tirtc_config_field_max_len(field);

    if (s_tirtc_config_edit_page == NULL) {
        display_build_tirtc_config_edit_page(lv_scr_act());
    }

    s_tirtc_edit_field = field;
    display_show_page(s_tirtc_config_edit_page);
    if (s_tirtc_edit_hint_label != NULL) {
        display_text_set(s_tirtc_edit_hint_label, display_tirtc_config_field_title(field));
    }
    if (s_tirtc_edit_ta != NULL) {
        lv_textarea_set_max_length(s_tirtc_edit_ta, max_len);
        lv_textarea_set_placeholder_text(s_tirtc_edit_ta, display_tirtc_config_field_title(field));
        lv_textarea_set_text(s_tirtc_edit_ta, current_value != NULL ? current_value : "");
        lv_textarea_set_cursor_pos(s_tirtc_edit_ta, (uint32_t)strlen(current_value != NULL ? current_value : ""));
    }
    display_update_tirtc_edit_feedback(NULL, lv_color_hex(0x0D8A59));
    if (s_tirtc_edit_keyboard != NULL && s_tirtc_edit_ta != NULL) {
        lv_keyboard_set_textarea(s_tirtc_edit_keyboard, s_tirtc_edit_ta);
        lv_keyboard_set_mode(s_tirtc_edit_keyboard, LV_KEYBOARD_MODE_USER_1);
        lv_obj_clear_flag(s_tirtc_edit_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_tirtc_edit_keyboard);
        lv_obj_add_state(s_tirtc_edit_ta, LV_STATE_FOCUSED);
        lv_event_send(s_tirtc_edit_ta, LV_EVENT_FOCUSED, NULL);
    }
}

static void display_show_ota_page(void)
{
    if (s_ota_page == NULL) {
        display_build_ota_page(lv_scr_act());
    }
    display_update_ota_page(&s_last_status);
    display_show_page(s_ota_page);
}

static void display_uuid_back_btn_cb(lv_event_t *event)
{
    (void)event;
    if (s_uuid_parent_page == DISPLAY_PAGE_TIRTC_CONFIG) {
        display_show_tirtc_config_page();
    } else {
        display_show_main_page();
    }
}

static void display_uuid_save_btn_cb(lv_event_t *event)
{
    (void)event;
    display_submit_uuid();
}

static void display_device_back_btn_cb(lv_event_t *event)
{
    (void)event;
    display_return_home();
}

static void display_system_back_btn_cb(lv_event_t *event)
{
    (void)event;
    display_return_home();
}

static void display_system_child_back_btn_cb(lv_event_t *event)
{
    (void)event;
    display_show_system_page();
}

static void display_system_wifi_btn_cb(lv_event_t *event)
{
    (void)event;
    display_open_wifi_page(DISPLAY_PAGE_SYSTEM);
    display_request_wifi_scan();
}

static void display_system_network_test_btn_cb(lv_event_t *event)
{
    (void)event;
    display_show_network_test_page();
}

static void display_system_ota_btn_cb(lv_event_t *event)
{
    (void)event;
    display_show_ota_page();
}

static void display_call_back_btn_cb(lv_event_t *event)
{
    (void)event;
    display_return_home();
}

static void display_call_child_back_btn_cb(lv_event_t *event)
{
    (void)event;
    if (display_call_state_keeps_active_page(s_last_status.call_state)) {
        display_show_call_hangup_confirm();
        return;
    }
    display_show_call_page();
}

static void display_call_add_btn_cb(lv_event_t *event)
{
    (void)event;
    display_hide_keyboard();
    display_reset_call_add_inputs();
    display_show_call_add_page();
}

static void display_call_list_btn_cb(lv_event_t *event)
{
    (void)event;
    (void)display_request_call_contacts_refresh("call_list_enter");
    display_show_call_list_page();
}

static void display_call_list_refresh_btn_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        (void)display_request_call_contacts_refresh("call_list_button");
    }
}

static void display_call_scan_btn_cb(lv_event_t *event)
{
    esp_err_t ret = ESP_OK;

    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    if (s_actions.on_start_contact_scan == NULL) {
        display_show_wifi_alert("扫码添加", "扫码接口不可用");
        return;
    }

    display_show_call_scan_page();
    ret = s_actions.on_start_contact_scan(display_call_scan_preview_cb,
                                          display_call_scan_result_cb,
                                          NULL,
                                          s_actions.ctx);
    if (ret == ESP_OK) {
        return;
    }

    s_call_scan_active = false;
    display_show_call_add_page();
    switch (ret) {
    case ESP_ERR_NOT_SUPPORTED:
        display_show_wifi_alert("扫码添加", "摄像头不可用");
        break;
    case ESP_ERR_INVALID_STATE:
        display_show_wifi_alert("扫码添加", "扫码服务忙");
        break;
    case ESP_ERR_NOT_FOUND:
        display_show_wifi_alert("扫码添加", "未识别二维码");
        break;
    case ESP_ERR_INVALID_RESPONSE:
        display_show_wifi_alert("扫码添加", "二维码格式错误");
        break;
    case ESP_ERR_TIMEOUT:
        display_show_wifi_alert("扫码添加", "摄像头超时");
        break;
    default:
        display_show_wifi_alert("扫码添加", "扫码失败");
        break;
    }
}

static void display_call_scan_tap_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    lv_event_stop_bubbling(event);
    lv_event_stop_processing(event);
    display_exit_call_scan_to_previous();
}

static void display_call_scan_info_btn_cb(lv_event_t *event)
{
    (void)event;
    if (s_call_scan_info_overlay != NULL) {
        lv_obj_clear_flag(s_call_scan_info_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_call_scan_info_overlay);
    }
}

static void display_call_scan_info_close_btn_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || s_call_scan_info_overlay == NULL) {
        return;
    }
    lv_obj_add_flag(s_call_scan_info_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void display_call_add_field_btn_cb(lv_event_t *event)
{
    display_call_add_field_t field =
        (display_call_add_field_t)(uintptr_t)lv_event_get_user_data(event);

    if (field >= DISPLAY_CALL_ADD_FIELD_COUNT) {
        return;
    }
    display_show_call_add_edit_page(field);
}

static void display_call_add_edit_back_btn_cb(lv_event_t *event)
{
    (void)event;
    display_show_call_add_page();
}

static void display_call_add_edit_save_btn_cb(lv_event_t *event)
{
    const char *value = NULL;
    char trimmed[DISPLAY_CALL_CONTACT_DEVICE_ID_MAX] = {0};
    char *target = NULL;
    size_t max_len = display_call_add_field_max_len(s_call_add_edit_field);

    (void)event;

    if (s_call_add_edit_ta == NULL || s_call_add_edit_field >= DISPLAY_CALL_ADD_FIELD_COUNT) {
        return;
    }

    value = lv_textarea_get_text(s_call_add_edit_ta);
    display_copy_trimmed_text(trimmed, sizeof(trimmed), value);
    if (!display_text_has_visible_char(trimmed) || strlen(trimmed) != max_len) {
        display_update_call_add_edit_feedback("请输入 12 位 Device ID",
                                              lv_color_hex(0xE45656));
        return;
    }

    target = display_call_add_field_buffer(s_call_add_edit_field);
    strlcpy(target, trimmed, max_len + 1U);
    display_update_call_add_field_labels();
    display_show_call_add_page();
}

static void display_call_confirm_add_btn_cb(lv_event_t *event)
{
    char device_id_trimmed[DISPLAY_CALL_CONTACT_DEVICE_ID_MAX] = {0};

    (void)event;

    display_copy_trimmed_text(device_id_trimmed, sizeof(device_id_trimmed), s_call_add_device_id);

    if (strlen(device_id_trimmed) != DISPLAY_CALL_CONTACT_DEVICE_ID_LENGTH) {
        display_show_wifi_alert("添加联系人", "请输入 12 位 Device ID");
        return;
    }

    display_hide_keyboard();
    if (s_actions.on_add_call_contact == NULL) {
        display_show_wifi_alert("添加联系人", "联系人服务不可用");
        return;
    }
    esp_err_t ret = s_actions.on_add_call_contact(device_id_trimmed, s_actions.ctx);
    if (ret != ESP_OK) {
        display_show_wifi_alert("添加联系人",
                                ret == ESP_ERR_INVALID_STATE ?
                                "联系人服务忙，请稍后再试" : "申请提交失败");
        return;
    }
    display_reset_call_add_inputs();
    display_invalidate_call_list_page();
    display_show_call_list_page();
    display_show_wifi_alert("添加联系人", "申请已提交");
}

static void display_call_pending_contact_response_btn_cb(lv_event_t *event)
{
    uintptr_t action_data = (uintptr_t)lv_event_get_user_data(event);
    uint8_t contact_index = (uint8_t)(action_data >> 1U);
    bool accept = (action_data & 1U) != 0U;

    if (contact_index >= s_call_pending_contact_count ||
        s_call_pending_contacts[contact_index].device_id[0] == '\0') {
        display_show_wifi_alert("添加联系人", "申请不存在");
        return;
    }

    if (display_submit_call_contact_response(
            s_call_pending_contacts[contact_index].device_id,
            accept) == ESP_OK) {
        lv_obj_t *button = lv_event_get_target(event);
        if (button != NULL) {
            lv_obj_add_state(button, LV_STATE_DISABLED);
            lv_obj_clear_flag(button, LV_OBJ_FLAG_CLICKABLE);
        }
    }
}

static void display_call_contact_remark_btn_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    display_show_call_remark_page((uint8_t)(uintptr_t)lv_event_get_user_data(event));
}

static void display_call_contact_delete_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_LONG_PRESSED) {
        return;
    }

    lv_event_stop_bubbling(event);
    display_show_call_delete_confirm((uint8_t)(uintptr_t)lv_event_get_user_data(event));
}

static void display_call_delete_cancel_btn_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        display_hide_call_delete_confirm();
    }
}

static void display_call_delete_confirm_btn_cb(lv_event_t *event)
{
    char device_id[DISPLAY_CALL_CONTACT_DEVICE_ID_MAX] = {0};

    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    strlcpy(device_id, s_call_delete_pending_device_id, sizeof(device_id));
    display_hide_call_delete_confirm();

    if (device_id[0] == '\0' || s_actions.on_delete_call_contact == NULL) {
        display_show_wifi_alert("联系人", "删除接口不可用");
        return;
    }
    ESP_LOGI(CALL_FLOW_TAG, "stage=contact_delete_submit peer=%s", device_id);
    esp_err_t ret = s_actions.on_delete_call_contact(device_id, s_actions.ctx);
    if (ret != ESP_OK) {
        display_show_wifi_alert("联系人",
                                ret == ESP_ERR_NOT_ALLOWED ?
                                "自动联系人请在平台管理" :
                                (ret == ESP_ERR_INVALID_STATE ?
                                 "联系人服务忙，请稍后再试" : "删除失败"));
        return;
    }
    display_show_wifi_alert("删除联系人", "删除请求已提交");
}

static void display_call_remark_back_btn_cb(lv_event_t *event)
{
    (void)event;
    s_call_remark_edit_device_id[0] = '\0';
    display_show_call_list_page();
}

static void display_call_remark_save_btn_cb(lv_event_t *event)
{
    char remark[DISPLAY_CALL_CONTACT_REMARK_MAX] = {0};
    const char *input = NULL;

    (void)event;
    if (s_call_remark_ta == NULL || s_call_remark_edit_device_id[0] == '\0') {
        return;
    }

    input = lv_textarea_get_text(s_call_remark_ta);
    if (input == NULL || strlen(input) >= sizeof(remark)) {
        if (s_call_remark_status_label != NULL) {
            display_text_set_color(s_call_remark_status_label, lv_color_hex(0xE45656), 0);
            display_text_set(s_call_remark_status_label, "名称过长");
        }
        return;
    }
    display_copy_trimmed_text(remark, sizeof(remark), input);
    if (s_actions.on_update_call_contact_remark == NULL) {
        display_show_wifi_alert("联系人", "改名接口不可用");
        return;
    }

    esp_err_t ret = s_actions.on_update_call_contact_remark(
        s_call_remark_edit_device_id,
        remark,
        s_actions.ctx);
    if (ret != ESP_OK) {
        display_show_wifi_alert("联系人",
                                ret == ESP_ERR_INVALID_STATE ?
                                "联系人服务忙，请稍后再试" : "保存失败");
        return;
    }

    s_call_remark_edit_device_id[0] = '\0';
    display_show_call_list_page();
    display_show_wifi_alert("联系人", "名称更新已提交");
}

static void display_call_contact_call_btn_cb(lv_event_t *event)
{
    uintptr_t action = (uintptr_t)lv_event_get_user_data(event);
    uint8_t contact_index = (uint8_t)(action >> 1U);
    display_call_type_t call_type = (display_call_type_t)(action & 1U);
    esp_err_t ret = ESP_OK;

    if (contact_index >= s_call_contact_count ||
        s_call_contacts[contact_index].device_id[0] == '\0') {
        display_show_wifi_alert("呼叫", "联系人不存在");
        return;
    }
    if (!s_call_contacts[contact_index].online) {
        display_show_wifi_alert("呼叫", "对方设备当前离线");
        return;
    }
    if (s_actions.on_call_contact == NULL) {
        display_show_wifi_alert("呼叫", "呼叫接口不可用");
        return;
    }

    ret = s_actions.on_call_contact(s_call_contacts[contact_index].device_id,
                                    call_type,
                                    s_actions.ctx);
    if (ret != ESP_OK) {
        display_show_wifi_alert("呼叫",
                                ret == ESP_ERR_INVALID_STATE ? "请先连接 Wi-Fi" : "呼叫启动失败");
        return;
    }
    s_last_status.call_state = DISPLAY_CALL_STATE_OUTGOING;
    s_last_status.call_type = call_type;
    strlcpy(s_last_status.call_peer_device_id,
            s_call_contacts[contact_index].device_id,
            sizeof(s_last_status.call_peer_device_id));
    s_last_status.call_room_id[0] = '\0';
    s_call_active_started_us = 0;
    display_show_call_active_page();
}

static void display_call_hangup_btn_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_PRESSED) {
        return;
    }

    ESP_LOGI(CALL_FLOW_TAG, "stage=ui_hangup_tap");
    (void)display_request_call_hangup_locked();
}

static esp_err_t display_request_call_hangup_locked(void)
{
    esp_err_t ret = ESP_ERR_INVALID_STATE;

    if (s_call_hangup_pending) {
        return ESP_ERR_INVALID_STATE;
    }

    s_call_hangup_pending = true;
    display_hide_call_hangup_confirm();
    display_set_video_refresh_enabled(false);
    if (s_call_audio_state_label != NULL) {
        display_text_set(s_call_audio_state_label, "正在挂断");
    }
    if (s_call_video_state_label != NULL) {
        display_text_set(s_call_video_state_label, "正在挂断");
    }
    if (s_actions.on_hangup_call != NULL) {
        ret = s_actions.on_hangup_call(s_actions.ctx);
    }
    if (ret != ESP_OK) {
        s_call_hangup_pending = false;
        display_update_call_active_page(&s_last_status);
        display_show_wifi_alert("CALL", "HANGUP FAILED");
        return ret;
    }

    /*
     * Stay on the active page until the business snapshot reaches idle. Going
     * to the list here makes the next refresh reopen the still-active call
     * page, which looks like a slow or ignored hangup.
     */
    return ESP_OK;
}

static void display_call_hangup_cancel_btn_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        display_hide_call_hangup_confirm();
    }
}

static void display_call_hangup_confirm_btn_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_PRESSED) {
        display_call_hangup_btn_cb(event);
    }
}

static void display_apply_call_volume_action(display_call_volume_action_t action, bool wechat)
{
    uint8_t current = 0;

    switch (action) {
    case DISPLAY_CALL_VOLUME_MIC_DOWN:
    case DISPLAY_CALL_VOLUME_MIC_UP:
        current = s_last_status.audio_capture_gain_percent;
        if (action == DISPLAY_CALL_VOLUME_MIC_UP) {
            current = current > (100U - DISPLAY_CALL_VOLUME_STEP) ? 100U : current + DISPLAY_CALL_VOLUME_STEP;
        } else {
            current = current < DISPLAY_CALL_VOLUME_STEP ? 0U : current - DISPLAY_CALL_VOLUME_STEP;
        }
        if (s_actions.on_set_capture_gain != NULL) {
            (void)s_actions.on_set_capture_gain(current, s_actions.ctx);
        }
        s_last_status.audio_capture_gain_percent = current;
        break;
    case DISPLAY_CALL_VOLUME_SPEAKER_DOWN:
    case DISPLAY_CALL_VOLUME_SPEAKER_UP:
    default:
        current = s_last_status.audio_speaker_volume_percent;
        if (action == DISPLAY_CALL_VOLUME_SPEAKER_UP) {
            current = current > (100U - DISPLAY_CALL_VOLUME_STEP) ? 100U : current + DISPLAY_CALL_VOLUME_STEP;
        } else {
            current = current < DISPLAY_CALL_VOLUME_STEP ? 0U : current - DISPLAY_CALL_VOLUME_STEP;
        }
        if (s_actions.on_set_speaker_volume != NULL) {
            (void)s_actions.on_set_speaker_volume(current, s_actions.ctx);
        }
        s_last_status.audio_speaker_volume_percent = current;
        break;
    }

    if (wechat) {
        display_update_wechat_active_page(&s_last_status);
    } else {
        display_update_call_active_page(&s_last_status);
    }
}

static void display_call_volume_btn_cb(lv_event_t *event)
{
    display_call_volume_action_t action =
        (display_call_volume_action_t)(uintptr_t)lv_event_get_user_data(event);
    display_apply_call_volume_action(action, false);
}

static bool display_call_video_point_in_rect(const lv_point_t *point,
                                             lv_coord_t x,
                                             lv_coord_t y,
                                             lv_coord_t width,
                                             lv_coord_t height)
{
    return point != NULL && point->x >= x && point->y >= y &&
           point->x < x + width && point->y < y + height;
}

static bool display_dispatch_call_video_overlay_tap(
    lv_event_t *event,
    display_video_surface_t surface,
    display_call_video_overlays_t *overlays)
{
    if (event == NULL || overlays == NULL || overlays->hidden) {
        return false;
    }

    lv_point_t point = {0};
    if (s_debug_tap_point_valid) {
        point = s_debug_tap_point;
    } else {
        lv_indev_t *indev = lv_indev_get_act();
        if (indev == NULL) {
            return false;
        }
        lv_indev_get_point(indev, &point);
    }
    bool wechat = surface == DISPLAY_VIDEO_SURFACE_WECHAT;

    bool back_pressed = display_call_video_point_in_rect(
        &point,
        DISPLAY_CALL_VIDEO_TOP_X + 4,
        DISPLAY_CALL_VIDEO_TOP_Y + 4,
        40,
        40);
    bool hangup_pressed = display_call_video_point_in_rect(
        &point,
        DISPLAY_CALL_VIDEO_HANGUP_X,
        DISPLAY_CALL_VIDEO_HANGUP_Y,
        DISPLAY_CALL_VIDEO_HANGUP_WIDTH,
        DISPLAY_CALL_VIDEO_HANGUP_HEIGHT);
    if (back_pressed || hangup_pressed) {
        if (wechat) {
            display_wechat_hangup_btn_cb(event);
        } else {
            display_call_hangup_btn_cb(event);
        }
        return true;
    }

    struct {
        lv_coord_t x;
        display_call_volume_action_t action;
    } volume_buttons[] = {
        {48, DISPLAY_CALL_VOLUME_MIC_DOWN},
        {94, DISPLAY_CALL_VOLUME_MIC_UP},
        {194, DISPLAY_CALL_VOLUME_SPEAKER_DOWN},
        {240, DISPLAY_CALL_VOLUME_SPEAKER_UP},
    };
    for (size_t index = 0;
         index < sizeof(volume_buttons) / sizeof(volume_buttons[0]);
         ++index) {
        if (display_call_video_point_in_rect(
                &point,
                DISPLAY_CALL_VIDEO_CONTROLS_X + volume_buttons[index].x,
                DISPLAY_CALL_VIDEO_CONTROLS_Y + 6,
                40,
                40)) {
            display_apply_call_volume_action(volume_buttons[index].action, wechat);
            overlays->snapshot_dirty = true;
            overlays->hide_at_us =
                esp_timer_get_time() + DISPLAY_CALL_VIDEO_CONTROLS_VISIBLE_US;
            return true;
        }
    }
    return false;
}

static void display_call_video_surface_tap_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_PRESSED) {
        return;
    }

    display_video_surface_t surface =
        (display_video_surface_t)(uintptr_t)lv_event_get_user_data(event);
    lv_obj_t *target = lv_event_get_target(event);
    display_call_video_overlays_t *overlays = NULL;
    lv_obj_t *image = NULL;
    lv_obj_t *placeholder = NULL;
    bool direct_lcd_active = false;
    const char *owner = NULL;

    if (surface == DISPLAY_VIDEO_SURFACE_WECHAT) {
        if (target != s_wechat_video_panel ||
            !display_page_is_visible(s_wechat_active_page) ||
            !CONFIG_APP_WECHAT_VOIP_REMOTE_VIDEO_ENABLE) {
            return;
        }
        overlays = &s_wechat_video_overlays;
        image = s_wechat_video_image;
        placeholder = s_wechat_video_placeholder_label;
        direct_lcd_active = s_wechat_video_direct_lcd_active;
        owner = "wechat";
    } else {
        if (target != s_call_video_panel ||
            !display_page_is_visible(s_call_active_page) ||
            s_call_visible_type != DISPLAY_CALL_TYPE_VIDEO) {
            return;
        }
        overlays = &s_call_video_overlays;
        image = s_call_video_image;
        placeholder = s_call_video_placeholder_label;
        direct_lcd_active = s_call_video_direct_lcd_active;
        owner = "device-call";
    }

    if (display_dispatch_call_video_overlay_tap(event, surface, overlays)) {
        return;
    }

    bool show_overlays = overlays->hidden;
    display_set_call_video_overlays_hidden(overlays,
                                           image,
                                           placeholder,
                                           direct_lcd_active,
                                           !show_overlays);
    if (image != NULL && !lv_obj_has_flag(image, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
    }
    /* The full-frame image and overlay sources stay outside LVGL's live panel
     * flush path. The next video frame composites the cached overlay once and
     * reaches the panel through the same full-frame DMA transfer. */
    ESP_LOGI(TAG,
             "%s video controls %s",
             owner,
             overlays->hidden ? "hidden" : "visible");
}

static void display_wechat_child_back_btn_cb(lv_event_t *event)
{
    (void)event;
    display_show_wechat_page();
}

static void display_wechat_add_btn_cb(lv_event_t *event)
{
    (void)event;
    display_show_wechat_add_page();
}

static void display_wechat_list_btn_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    display_show_wechat_list_page();
}

static void display_wechat_scan_btn_cb(lv_event_t *event)
{
    esp_err_t ret = ESP_OK;

    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    if (s_actions.on_start_wechat_contact_scan == NULL) {
        display_show_wifi_alert("扫码添加", "扫码接口不可用");
        return;
    }

    display_show_wechat_scan_page();
    ret = s_actions.on_start_wechat_contact_scan(display_call_scan_preview_cb,
                                                 display_wechat_scan_result_cb,
                                                 NULL,
                                                 s_actions.ctx);
    if (ret == ESP_OK) {
        return;
    }

    s_call_scan_active = false;
    display_show_wechat_add_page();
    switch (ret) {
    case ESP_ERR_NOT_SUPPORTED:
        display_show_wifi_alert("扫码添加", "摄像头不可用");
        break;
    case ESP_ERR_INVALID_STATE:
        display_show_wifi_alert("扫码添加", "扫码服务忙");
        break;
    case ESP_ERR_NOT_FOUND:
        display_show_wifi_alert("扫码添加", "未识别二维码");
        break;
    case ESP_ERR_INVALID_RESPONSE:
        display_show_wifi_alert("扫码添加", "二维码格式错误");
        break;
    case ESP_ERR_TIMEOUT:
        display_show_wifi_alert("扫码添加", "摄像头超时");
        break;
    default:
        display_show_wifi_alert("扫码添加", "扫码失败");
        break;
    }
}

static void display_wechat_scan_info_btn_cb(lv_event_t *event)
{
    (void)event;
    if (s_wechat_scan_info_overlay != NULL) {
        lv_obj_clear_flag(s_wechat_scan_info_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_wechat_scan_info_overlay);
    }
}

static void display_wechat_scan_info_close_btn_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || s_wechat_scan_info_overlay == NULL) {
        return;
    }
    lv_obj_add_flag(s_wechat_scan_info_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void display_wechat_add_field_btn_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    display_show_wechat_add_edit_page();
}

static void display_wechat_add_edit_back_btn_cb(lv_event_t *event)
{
    (void)event;
    display_show_wechat_add_page();
}

static void display_wechat_add_edit_save_btn_cb(lv_event_t *event)
{
    const char *value = NULL;
    char trimmed[DISPLAY_WECHAT_OPEN_ID_MAX] = {0};

    (void)event;

    if (s_wechat_add_edit_ta == NULL) {
        return;
    }

    value = lv_textarea_get_text(s_wechat_add_edit_ta);
    display_copy_trimmed_text(trimmed, sizeof(trimmed), value);
    if (!display_wechat_open_id_valid(trimmed)) {
        display_update_wechat_add_edit_feedback("必须是28位微信Open ID", lv_color_hex(0xE45656));
        return;
    }

    strlcpy(s_wechat_add_open_id, trimmed, sizeof(s_wechat_add_open_id));
    display_update_wechat_add_field_label();
    display_show_wechat_add_page();
}

static void display_wechat_confirm_add_btn_cb(lv_event_t *event)
{
    char open_id_trimmed[DISPLAY_WECHAT_OPEN_ID_MAX] = {0};
    esp_err_t ret = ESP_OK;

    (void)event;

    display_copy_trimmed_text(open_id_trimmed, sizeof(open_id_trimmed), s_wechat_add_open_id);
    if (!display_wechat_open_id_valid(open_id_trimmed)) {
        display_show_wifi_alert("微信联系人", "请输入28位微信Open ID");
        return;
    }
    if (s_actions.on_add_wechat_contact == NULL) {
        display_show_wifi_alert("微信联系人", "添加接口不可用");
        return;
    }

    ret = s_actions.on_add_wechat_contact(open_id_trimmed, s_actions.ctx);
    if (ret != ESP_OK) {
        display_show_wifi_alert("微信联系人",
                                ret == ESP_ERR_INVALID_STATE
                                    ? "请先进入微信呼叫"
                                    : ret == ESP_ERR_NOT_ALLOWED
                                          ? "请先在微信小程序完成授权"
                                          : "添加失败");
        return;
    }

    display_store_scanned_wechat_contact(open_id_trimmed);
    display_reset_wechat_add_input();
    display_show_wechat_list_page();
    display_show_wifi_alert("微信联系人", "添加成功");
}

static void display_wechat_contact_remark_btn_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    display_show_wechat_remark_page((uint8_t)(uintptr_t)lv_event_get_user_data(event));
}

static void display_wechat_remark_back_btn_cb(lv_event_t *event)
{
    (void)event;
    s_wechat_remark_edit_open_id[0] = '\0';
    display_show_wechat_list_page();
}

static void display_wechat_remark_save_btn_cb(lv_event_t *event)
{
    char remark[DISPLAY_WECHAT_REMARK_MAX] = {0};
    const char *input = NULL;

    (void)event;
    if (s_wechat_remark_ta == NULL || s_wechat_remark_edit_open_id[0] == '\0') {
        display_show_wifi_alert("微信联系人", "联系人不存在");
        return;
    }
    if (s_actions.on_update_wechat_contact_remark == NULL) {
        display_show_wifi_alert("微信联系人", "改名接口不可用");
        return;
    }

    input = lv_textarea_get_text(s_wechat_remark_ta);
    if (input == NULL || strlen(input) >= sizeof(remark)) {
        if (s_wechat_remark_status_label != NULL) {
            display_text_set_color(s_wechat_remark_status_label, lv_color_hex(0xE45656), 0);
            display_text_set(s_wechat_remark_status_label, "名称过长");
        }
        return;
    }
    display_copy_trimmed_text(remark, sizeof(remark), input);

    esp_err_t ret = s_actions.on_update_wechat_contact_remark(
        s_wechat_remark_edit_open_id,
        remark,
        s_actions.ctx);
    if (ret != ESP_OK) {
        display_show_wifi_alert("微信联系人",
                                ret == ESP_ERR_INVALID_STATE ?
                                "联系人服务忙，请稍后再试" :
                                ret == ESP_ERR_INVALID_SIZE ?
                                "名称过长" : "名称更新失败");
        return;
    }

    s_wechat_remark_edit_open_id[0] = '\0';
    display_show_wechat_list_page();
    display_show_wifi_alert("微信联系人", "名称更新中");
}

static void display_wechat_contact_call_btn_cb(lv_event_t *event)
{
    uint8_t contact_index = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    const char *open_id = NULL;
    esp_err_t ret = ESP_OK;

    if (contact_index >= DISPLAY_WECHAT_CONTACT_COUNT ||
        contact_index >= s_last_status.wechat_contact_count ||
        s_last_status.wechat_contacts[contact_index].open_id[0] == '\0') {
        display_show_wifi_alert("微信呼叫", "联系人不存在");
        return;
    }
    if (s_actions.on_wechat_contact == NULL) {
        display_show_wifi_alert("微信呼叫", "呼叫接口不可用");
        return;
    }

    open_id = s_last_status.wechat_contacts[contact_index].open_id;
    ret = s_actions.on_wechat_contact(open_id, s_actions.ctx);
    if (ret != ESP_OK) {
        const char *message = "呼叫启动失败";
        if (ret == ESP_ERR_INVALID_STATE) {
            if (!s_last_status.network_connected) {
                message = "请先连接 Wi-Fi";
            } else if (s_last_status.wechat_call_state ==
                       DISPLAY_WECHAT_CALL_STATE_CLOSING) {
                message = "上一通呼叫正在结束";
            } else {
                message = "通话资源暂未就绪";
            }
        }
        display_show_wifi_alert("微信呼叫",
                                message);
        return;
    }
    s_last_status.wechat_call_state = DISPLAY_WECHAT_CALL_STATE_CALLING;
    s_wechat_active_started_us = 0;
    display_show_wechat_active_page();
}

static void display_wechat_hangup_task(void *arg)
{
    display_actions_t actions = s_actions;

    (void)arg;
    if (actions.on_wechat_hangup_call != NULL) {
        esp_err_t ret = actions.on_wechat_hangup_call(actions.ctx);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "wechat hangup action failed: %s", esp_err_to_name(ret));
        }
    } else {
        ESP_LOGW(TAG, "wechat hangup action missing");
    }
    s_wechat_hangup_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

static void display_queue_wechat_hangup(void)
{
    if (s_actions.on_wechat_hangup_call == NULL) {
        ESP_LOGW(TAG, "queue wechat hangup skipped: no action");
        return;
    }
    if (s_wechat_hangup_task != NULL) {
        ESP_LOGW(TAG, "queue wechat hangup skipped: task already running");
        return;
    }

    BaseType_t task_ret = xTaskCreatePinnedToCoreWithCaps(display_wechat_hangup_task,
                                                          "ui_wx_hangup",
                                                          DISPLAY_WECHAT_HANGUP_TASK_STACK,
                                                          NULL,
                                                          DISPLAY_WECHAT_HANGUP_TASK_PRIORITY,
                                                          &s_wechat_hangup_task,
                                                          APP_TASK_CORE_BACKGROUND,
                                                          APP_TASK_STACK_CAPS_BACKGROUND);
    if (task_ret != pdPASS) {
        s_wechat_hangup_task = NULL;
        ESP_LOGW(TAG, "queue wechat hangup task failed");
    }
}

static void display_remove_wechat_contact_from_last_status(uint8_t contact_index)
{
    if (contact_index >= s_last_status.wechat_contact_count ||
        contact_index >= DISPLAY_WECHAT_CONTACT_COUNT) {
        return;
    }

    uint8_t count = s_last_status.wechat_contact_count > DISPLAY_WECHAT_CONTACT_COUNT ?
        DISPLAY_WECHAT_CONTACT_COUNT : s_last_status.wechat_contact_count;
    for (uint8_t index = contact_index; index + 1U < count; ++index) {
        s_last_status.wechat_contacts[index] = s_last_status.wechat_contacts[index + 1U];
    }
    if (count > 0) {
        memset(&s_last_status.wechat_contacts[count - 1U],
               0,
               sizeof(s_last_status.wechat_contacts[count - 1U]));
        --s_last_status.wechat_contact_count;
    }
}

static void display_wechat_contact_delete_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_LONG_PRESSED) {
        return;
    }

    uint8_t contact_index = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    display_show_wechat_delete_confirm(contact_index);
}

static void display_wechat_delete_cancel_btn_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    display_hide_wechat_delete_confirm();
}

static void display_wechat_delete_confirm_btn_cb(lv_event_t *event)
{
    uint8_t contact_index = s_wechat_delete_pending_index;
    char open_id[DISPLAY_WECHAT_OPEN_ID_MAX] = {0};
    esp_err_t ret = ESP_OK;

    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    strlcpy(open_id, s_wechat_delete_pending_open_id, sizeof(open_id));

    if (contact_index >= DISPLAY_WECHAT_CONTACT_COUNT ||
        contact_index >= s_last_status.wechat_contact_count ||
        strcmp(s_last_status.wechat_contacts[contact_index].open_id, open_id) != 0) {
        uint8_t count = s_last_status.wechat_contact_count > DISPLAY_WECHAT_CONTACT_COUNT ?
            DISPLAY_WECHAT_CONTACT_COUNT : s_last_status.wechat_contact_count;
        contact_index = DISPLAY_WECHAT_CONTACT_COUNT;
        for (uint8_t index = 0; index < count; ++index) {
            if (strcmp(s_last_status.wechat_contacts[index].open_id, open_id) == 0) {
                contact_index = index;
                break;
            }
        }
    }

    if (open_id[0] == '\0' ||
        contact_index >= DISPLAY_WECHAT_CONTACT_COUNT ||
        contact_index >= s_last_status.wechat_contact_count ||
        s_last_status.wechat_contacts[contact_index].open_id[0] == '\0') {
        display_hide_wechat_delete_confirm();
        display_show_wifi_alert("微信联系人", "联系人不存在");
        return;
    }
    if (s_actions.on_remove_wechat_contact == NULL) {
        display_hide_wechat_delete_confirm();
        display_show_wifi_alert("微信联系人", "删除接口不可用");
        return;
    }

    ret = s_actions.on_remove_wechat_contact(open_id, s_actions.ctx);
    if (ret != ESP_OK) {
        display_hide_wechat_delete_confirm();
        display_show_wifi_alert("微信联系人",
                                ret == ESP_ERR_NOT_SUPPORTED
                                    ? "请在微信小程序取消授权"
                                    : ret == ESP_ERR_NOT_FOUND ? "联系人不存在" : "删除失败");
        return;
    }

    display_hide_wechat_delete_confirm();
    display_remove_wechat_contact_from_last_status(contact_index);
    display_update_wechat_contact_list(&s_last_status);
    display_show_wifi_alert("微信联系人", "已删除");
}

static void display_wechat_hangup_btn_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_PRESSED) {
        return;
    }

    s_wechat_active_started_us = 0;
    display_queue_wechat_hangup();
    display_show_wechat_page();
}

static void display_wechat_volume_btn_cb(lv_event_t *event)
{
    display_call_volume_action_t action =
        (display_call_volume_action_t)(uintptr_t)lv_event_get_user_data(event);
    display_apply_call_volume_action(action, true);
}

static void display_ai_settings_action_btn_cb(lv_event_t *event)
{
    display_ai_setting_action_t action =
        (display_ai_setting_action_t)(uintptr_t)lv_event_get_user_data(event);
    uint8_t next = 0;
    esp_err_t ret = ESP_OK;

    switch (action) {
    case DISPLAY_AI_SETTING_MIC_DOWN:
        next = display_adjust_volume(s_last_status.audio_capture_gain_percent, -10);
        if (s_actions.on_set_capture_gain != NULL) {
            ret = s_actions.on_set_capture_gain(next, s_actions.ctx);
        }
        if (ret == ESP_OK) {
            s_last_status.audio_capture_gain_percent = next;
        }
        break;
    case DISPLAY_AI_SETTING_MIC_UP:
        next = display_adjust_volume(s_last_status.audio_capture_gain_percent, 10);
        if (s_actions.on_set_capture_gain != NULL) {
            ret = s_actions.on_set_capture_gain(next, s_actions.ctx);
        }
        if (ret == ESP_OK) {
            s_last_status.audio_capture_gain_percent = next;
        }
        break;
    case DISPLAY_AI_SETTING_SPEAKER_DOWN:
        next = display_adjust_volume(s_last_status.audio_speaker_volume_percent, -10);
        if (s_actions.on_set_speaker_volume != NULL) {
            ret = s_actions.on_set_speaker_volume(next, s_actions.ctx);
        }
        if (ret == ESP_OK) {
            s_last_status.audio_speaker_volume_percent = next;
        }
        break;
    case DISPLAY_AI_SETTING_SPEAKER_UP:
        next = display_adjust_volume(s_last_status.audio_speaker_volume_percent, 10);
        if (s_actions.on_set_speaker_volume != NULL) {
            ret = s_actions.on_set_speaker_volume(next, s_actions.ctx);
        }
        if (ret == ESP_OK) {
            s_last_status.audio_speaker_volume_percent = next;
        }
        break;
    case DISPLAY_AI_SETTING_AVATAR_BUDDY:
    case DISPLAY_AI_SETTING_AVATAR_SPROUT:
        next = action == DISPLAY_AI_SETTING_AVATAR_SPROUT ?
            DISPLAY_AI_AVATAR_SPROUT : DISPLAY_AI_AVATAR_BUDDY;
        if (s_actions.on_set_ai_chat_avatar != NULL) {
            ret = s_actions.on_set_ai_chat_avatar(next, s_actions.ctx);
        }
        if (ret == ESP_OK) {
            s_last_status.ai_chat_avatar = next;
        }
        break;
    default:
        return;
    }

    display_update_ai_chat_settings_page(&s_last_status);
    display_update_ai_avatar(&s_last_status, NULL);
}

static void display_system_tirtc_config_btn_cb(lv_event_t *event)
{
    (void)event;
    display_show_tirtc_config_page();
}

static void display_network_test_start_btn_cb(lv_event_t *event)
{
    (void)event;

    if (s_actions.on_ping_test != NULL) {
        (void)s_actions.on_ping_test(s_actions.ctx);
    }
    display_update_network_test_page(&s_last_status);
}

static void __attribute__((unused)) display_tirtc_config_field_btn_cb(lv_event_t *event)
{
    display_tirtc_config_field_t field =
        (display_tirtc_config_field_t)(uintptr_t)lv_event_get_user_data(event);

    if (field >= DISPLAY_TIRTC_CONFIG_FIELD_COUNT) {
        return;
    }
    if (field != DISPLAY_TIRTC_CONFIG_FIELD_TOKEN_SUBJECT) {
        display_show_wifi_alert("TiRTC Config", "Managed by device binding service");
        return;
    }
    display_show_tirtc_config_edit_page(field);
}

static void __attribute__((unused)) display_tirtc_config_scan_btn_cb(lv_event_t *event)
{
    esp_err_t ret = ESP_OK;

    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    if (s_actions.on_reset_device_binding == NULL) {
        display_show_wifi_alert("TiRTC Config", "Device binding reset unavailable");
        return;
    }

    ret = s_actions.on_reset_device_binding(s_actions.ctx);
    if (ret == ESP_OK) {
        s_last_status.tirtc_device_id[0] = '\0';
        s_last_status.tirtc_device_secret[0] = '\0';
        s_last_status.binding_state = DISPLAY_DEVICE_BINDING_STATE_IDLE;
        display_update_tirtc_config_page(&s_last_status);
        return;
    }

    display_show_wifi_alert("TiRTC Config", display_contact_scan_error_text(ret));
}

static void display_tirtc_config_edit_back_btn_cb(lv_event_t *event)
{
    (void)event;
    display_show_tirtc_config_page();
}

static void display_tirtc_config_edit_save_btn_cb(lv_event_t *event)
{
    const char *value = NULL;
    size_t value_len = 0;
    esp_err_t ret = ESP_OK;

    (void)event;
    if (s_tirtc_edit_ta == NULL) {
        return;
    }

    value = lv_textarea_get_text(s_tirtc_edit_ta);
    value_len = strlen(value);
    if (value_len == 0 || value_len > display_tirtc_config_field_max_len(s_tirtc_edit_field)) {
        display_update_tirtc_edit_feedback("内容不合法", lv_color_hex(0xE45656));
        return;
    }
    if (s_actions.on_set_tirtc_config_field == NULL) {
        display_update_tirtc_edit_feedback("保存接口不可用", lv_color_hex(0xE45656));
        return;
    }

    ret = s_actions.on_set_tirtc_config_field(s_tirtc_edit_field, value, s_actions.ctx);
    if (ret != ESP_OK) {
        display_update_tirtc_edit_feedback("保存失败", lv_color_hex(0xE45656));
        return;
    }

    switch (s_tirtc_edit_field) {
    case DISPLAY_TIRTC_CONFIG_FIELD_DEVICE_SECRET:
        strlcpy(s_last_status.tirtc_device_secret, value, sizeof(s_last_status.tirtc_device_secret));
        break;
    case DISPLAY_TIRTC_CONFIG_FIELD_TOKEN_SUBJECT:
        strlcpy(s_last_status.tirtc_token_subject, value, sizeof(s_last_status.tirtc_token_subject));
        break;
    case DISPLAY_TIRTC_CONFIG_FIELD_ACCESS_KEY_ID:
        strlcpy(s_last_status.tirtc_access_key_id, value, sizeof(s_last_status.tirtc_access_key_id));
        break;
    case DISPLAY_TIRTC_CONFIG_FIELD_ACCESS_KEY_SECRET:
        strlcpy(s_last_status.tirtc_access_key_secret, value, sizeof(s_last_status.tirtc_access_key_secret));
        break;
    case DISPLAY_TIRTC_CONFIG_FIELD_DEVICE_ID:
    default:
        strlcpy(s_last_status.tirtc_device_id, value, sizeof(s_last_status.tirtc_device_id));
        break;
    }
    display_show_tirtc_config_page();
}

static void display_ota_start_btn_cb(lv_event_t *event)
{
    (void)event;

    if (!s_last_status.network_connected) {
        display_show_wifi_alert("OTA", "Connect WiFi first.");
        return;
    }
    if (s_actions.on_start_ota == NULL) {
        display_show_wifi_alert("OTA", "OTA is unavailable.");
        return;
    }

    esp_err_t ret = s_actions.on_start_ota(s_actions.ctx);
    if (ret == ESP_ERR_INVALID_STATE) {
        display_show_wifi_alert("OTA", s_last_status.ota_running ? "OTA is already running." : "Connect WiFi first.");
    } else if (ret != ESP_OK) {
        display_show_wifi_alert("OTA", "OTA start failed.");
    }
    display_update_ota_page(&s_last_status);
}

static void display_ota_reboot_btn_cb(lv_event_t *event)
{
    (void)event;

    if (s_last_status.ota_state != DISPLAY_OTA_STATE_READY_TO_REBOOT) {
        display_show_wifi_alert("OTA", "No staged update yet.");
        return;
    }
    if (s_actions.on_restart_for_ota == NULL) {
        display_show_wifi_alert("OTA", "Restart action is unavailable.");
        return;
    }

    (void)s_actions.on_restart_for_ota(s_actions.ctx);
}

static void display_wifi_back_btn_cb(lv_event_t *event)
{
    (void)event;
    if (s_wifi_parent_page == DISPLAY_PAGE_HOME) {
        display_show_home_page();
    } else if (s_wifi_parent_page == DISPLAY_PAGE_SYSTEM) {
        display_show_system_page();
    } else {
        display_show_main_page();
    }
}

static void display_wifi_connect_back_btn_cb(lv_event_t *event)
{
    (void)event;
    display_show_wifi_page();
}

static void display_wifi_refresh_btn_cb(lv_event_t *event)
{
    (void)event;
    display_request_wifi_scan();
}

static void display_wifi_ap_select_cb(lv_event_t *event)
{
    uintptr_t index = (uintptr_t)lv_event_get_user_data(event);
    if (index >= s_last_status.wifi_scan_count || index >= DISPLAY_WIFI_SCAN_MAX) {
        return;
    }

    strlcpy(s_selected_ssid,
            s_last_status.wifi_scan_results[index].ssid,
            sizeof(s_selected_ssid));
    display_show_wifi_connect_page();
}

static uint8_t display_adjust_volume(uint8_t current, int delta)
{
    int next = (int)current + delta;

    if (next < 0) {
        next = 0;
    } else if (next > 100) {
        next = 100;
    }
    return (uint8_t)next;
}

static void display_device_volume_btn_cb(lv_event_t *event)
{
    display_device_volume_action_t action =
        (display_device_volume_action_t)(uintptr_t)lv_event_get_user_data(event);
    bool receive = action == DISPLAY_DEVICE_VOLUME_RECEIVE_DOWN ||
                   action == DISPLAY_DEVICE_VOLUME_RECEIVE_UP ||
                   action == DISPLAY_DEVICE_VOLUME_RECEIVE_MUTE;
    uint8_t current = receive ? s_last_status.audio_speaker_volume_percent
                              : s_last_status.audio_capture_gain_percent;
    uint8_t next = current;
    bool mute_action = action == DISPLAY_DEVICE_VOLUME_RECEIVE_MUTE ||
                       action == DISPLAY_DEVICE_VOLUME_SEND_MUTE;
    esp_err_t ret = ESP_OK;

    switch (action) {
    case DISPLAY_DEVICE_VOLUME_RECEIVE_DOWN:
    case DISPLAY_DEVICE_VOLUME_SEND_DOWN:
        next = display_adjust_volume(current, -10);
        break;
    case DISPLAY_DEVICE_VOLUME_RECEIVE_UP:
    case DISPLAY_DEVICE_VOLUME_SEND_UP:
        next = display_adjust_volume(current, 10);
        break;
    case DISPLAY_DEVICE_VOLUME_RECEIVE_MUTE:
    case DISPLAY_DEVICE_VOLUME_SEND_MUTE:
        if (current > 0U) {
            if (receive) {
                s_device_receive_restore_volume = current;
                s_device_receive_restore_valid = true;
            } else {
                s_device_send_restore_volume = current;
                s_device_send_restore_valid = true;
            }
            next = 0;
        } else if (receive) {
            next = s_device_receive_restore_valid
                       ? s_device_receive_restore_volume
                       : DISPLAY_DEVICE_VOLUME_RESTORE_DEFAULT;
        } else {
            next = s_device_send_restore_valid
                       ? s_device_send_restore_volume
                       : DISPLAY_DEVICE_VOLUME_RESTORE_DEFAULT;
        }
        break;
    default:
        return;
    }

    if (receive) {
        if (s_actions.on_set_speaker_volume == NULL) {
            display_set_main_hint("Speaker control unavailable");
            return;
        }
        ret = s_actions.on_set_speaker_volume(next, s_actions.ctx);
        if (ret == ESP_OK) {
            s_last_status.audio_speaker_volume_percent = next;
            if (!mute_action && next > 0U) {
                s_device_receive_restore_volume = next;
                s_device_receive_restore_valid = true;
            }
        }
    } else {
        if (s_actions.on_set_capture_gain == NULL) {
            display_set_main_hint("Capture control unavailable");
            return;
        }
        ret = s_actions.on_set_capture_gain(next, s_actions.ctx);
        if (ret == ESP_OK) {
            s_last_status.audio_capture_gain_percent = next;
            if (!mute_action && next > 0U) {
                s_device_send_restore_volume = next;
                s_device_send_restore_valid = true;
            }
        }
    }

    if (ret != ESP_OK) {
        display_set_main_hint("Audio control failed: %s", esp_err_to_name(ret));
        return;
    }
    display_update_main_page(&s_last_status);
}

static void display_refresh_wifi_list(const display_status_t *status)
{
    uint16_t visible_count = 0;

    if (s_wifi_list == NULL || status == NULL) {
        return;
    }

    visible_count = status->wifi_scan_count < DISPLAY_WIFI_SCAN_MAX
                        ? status->wifi_scan_count
                        : DISPLAY_WIFI_SCAN_MAX;

    for (uint16_t index = 0; index < visible_count; ++index) {
        display_add_wifi_list_item(status, index);
    }

    for (uint16_t index = visible_count; index < DISPLAY_WIFI_SCAN_MAX; ++index) {
        if (s_wifi_list_buttons[index] != NULL) {
            lv_obj_add_flag(s_wifi_list_buttons[index], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void display_update_wifi_scan_state(const display_status_t *status)
{
    char left_text[96] = {0};
    char right_text[32] = {0};
    char ssid_text[48] = {0};
    lv_color_t left_color = lv_color_hex(0x64758A);
    lv_color_t right_color = lv_color_hex(0x64758A);

    if (status == NULL || s_wifi_connection_state_label == NULL ||
        s_wifi_scan_state_label == NULL || s_wifi_scan_count_label == NULL) {
        return;
    }

    if (status->network_connected) {
        left_color = lv_color_hex(0x0D8A59);
        if (status->network_ssid[0] != '\0') {
            display_format_ssid(ssid_text, sizeof(ssid_text), status->network_ssid);
            snprintf(left_text, sizeof(left_text), "已连接 %s", ssid_text);
        } else {
            strlcpy(left_text, "已连接 Wi-Fi", sizeof(left_text));
        }
    } else {
        strlcpy(left_text, "未连接 Wi-Fi", sizeof(left_text));
    }

    if (status->wifi_scan_in_progress) {
        strlcpy(right_text, "扫描中", sizeof(right_text));
        right_color = lv_color_hex(0x1768B7);
    } else {
        snprintf(right_text, sizeof(right_text), "%u APs", status->wifi_scan_count);
        right_color = status->wifi_scan_count > 0 ? lv_color_hex(0x20BF7A) : lv_color_hex(0x64758A);
    }

    display_text_set_color(s_wifi_connection_state_label, left_color, 0);
    display_text_set(s_wifi_connection_state_label, left_text);
    if (status->wifi_scan_in_progress) {
        lv_obj_add_flag(s_wifi_scan_count_label, LV_OBJ_FLAG_HIDDEN);
        display_text_set_color(s_wifi_scan_state_label, right_color, 0);
        display_text_set(s_wifi_scan_state_label, right_text);
        lv_obj_clear_flag(s_wifi_scan_state_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_wifi_scan_state_label, LV_OBJ_FLAG_HIDDEN);
        display_text_set_color(s_wifi_scan_count_label, right_color, 0);
        display_text_set(s_wifi_scan_count_label, right_text);
        lv_obj_clear_flag(s_wifi_scan_count_label, LV_OBJ_FLAG_HIDDEN);
    }
}

static void display_update_main_page(const display_status_t *status)
{
    const char *device_id = "--";
    char bitrate_text[16] = "0k";
    char fps_text[16] = "0fps";
    char resolution_text[24] = "0x0";
    bool rtc_ready = false;
    lv_color_t connection_dot_color = lv_color_hex(0xBCCAD8);

    if (status == NULL) {
        return;
    }

    if (status->tirtc_device_id[0] != '\0') {
        device_id = status->tirtc_device_id;
    } else {
        device_id = APP_CONFIG_RTC_DEVICE_ID;
    }
    rtc_ready = status->rtc_connected || status->rtc_call_active;
    if (rtc_ready) {
        connection_dot_color = lv_color_hex(0x20BF7A);
    } else if (status->network_connected) {
        connection_dot_color = lv_color_hex(0xF59E0B);
    }

    if (s_uuid_label != NULL) {
        display_text_set(s_uuid_label, device_id);
    }
#if LV_USE_QRCODE
    if (s_device_qrcode != NULL) {
        char payload[DISPLAY_DEVICE_QR_PAYLOAD_MAX] = {0};

        if (display_build_device_id_qr_payload(payload, sizeof(payload), status) &&
            strcmp(payload, s_device_qr_payload) != 0) {
            esp_err_t qr_ret = display_qr_image_update(&s_device_qr_image,
                                                       s_device_qrcode,
                                                       display_native_scale_square(DISPLAY_DEVICE_QR_SIZE),
                                                       payload,
                                                       strlen(payload));
            if (qr_ret == ESP_OK) {
                strlcpy(s_device_qr_payload, payload, sizeof(s_device_qr_payload));
            } else {
                ESP_LOGW(TAG, "device qr update failed: %s", esp_err_to_name(qr_ret));
            }
        }
    }
#endif
    if (rtc_ready) {
        uint32_t bitrate_kbps = status->rtc_tx_video_measured_bitrate_kbps;
        if (bitrate_kbps == 0U) {
            bitrate_kbps = status->rtc_tx_video_configured_bitrate_kbps;
        }
        if (bitrate_kbps >= 1000U) {
            (void)snprintf(bitrate_text,
                           sizeof(bitrate_text),
                           "%u.%uM",
                           (unsigned)(bitrate_kbps / 1000U),
                           (unsigned)((bitrate_kbps % 1000U) / 100U));
        } else if (bitrate_kbps > 0U) {
            (void)snprintf(bitrate_text, sizeof(bitrate_text), "%uk", (unsigned)bitrate_kbps);
        }

        uint32_t fps_x10 = status->rtc_tx_video_measured_fps_x10;
        if (fps_x10 == 0U && status->rtc_tx_video_target_fps > 0U) {
            fps_x10 = (uint32_t)status->rtc_tx_video_target_fps * 10U;
        }
        if (fps_x10 > 0U) {
            (void)snprintf(fps_text,
                           sizeof(fps_text),
                           "%u.%ufps",
                           (unsigned)(fps_x10 / 10U),
                           (unsigned)(fps_x10 % 10U));
        }

        if (status->rtc_tx_video_width > 0U && status->rtc_tx_video_height > 0U) {
            (void)snprintf(resolution_text,
                           sizeof(resolution_text),
                           "%ux%u",
                           (unsigned)status->rtc_tx_video_width,
                           (unsigned)status->rtc_tx_video_height);
        }
    }

    if (s_device_media_bitrate_label != NULL) {
        display_text_set(s_device_media_bitrate_label, bitrate_text);
    }
    if (s_device_media_fps_label != NULL) {
        display_text_set(s_device_media_fps_label, fps_text);
    }
    if (s_device_media_resolution_label != NULL) {
        display_text_set(s_device_media_resolution_label, resolution_text);
    }

    if (s_device_connection_dot != NULL) {
        lv_obj_set_style_bg_color(s_device_connection_dot, connection_dot_color, 0);
    }
    if (s_device_connection_value_label != NULL) {
        display_text_set(s_device_connection_value_label,
                          rtc_ready ? "已连接" : (status->network_connected ? "待连接" : "未连接"));
    }
    if (s_device_door_dot != NULL) {
        lv_obj_set_style_bg_color(s_device_door_dot,
                                  status->device_door_open ? lv_color_hex(0x20BF7A) : lv_color_hex(0xF59E0B),
                                  0);
    }
    if (s_device_door_value_label != NULL) {
        display_text_set(s_device_door_value_label, status->device_door_open ? "已开门" : "未开门");
    }
    if (s_device_receive_volume_label != NULL) {
        lv_label_set_text_fmt(s_device_receive_volume_label,
                              "%u",
                              (unsigned)status->audio_speaker_volume_percent);
    }
    if (s_device_receive_mute_label != NULL) {
        display_text_set(s_device_receive_mute_label,
                         status->audio_speaker_volume_percent == 0U ? "恢复" : "禁音");
    }
    if (s_device_send_volume_label != NULL) {
        lv_label_set_text_fmt(s_device_send_volume_label,
                              "%u",
                              (unsigned)status->audio_capture_gain_percent);
    }
    if (s_device_send_mute_label != NULL) {
        display_text_set(s_device_send_mute_label,
                         status->audio_capture_gain_percent == 0U ? "恢复" : "禁音");
    }
}

static void display_call_remark_textarea_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t *target = lv_event_get_target(event);

    if (code == LV_EVENT_FOCUSED && s_call_remark_keyboard != NULL) {
        display_show_text_keyboard(s_call_remark_keyboard, target);
    }
    if (code == LV_EVENT_VALUE_CHANGED && s_call_remark_length_label != NULL) {
        const char *value = lv_textarea_get_text(target);
        lv_label_set_text_fmt(s_call_remark_length_label,
                              "%u/%u",
                              (unsigned)(value != NULL ? strlen(value) : 0U),
                              (unsigned)(DISPLAY_CALL_CONTACT_REMARK_MAX - 1U));
    }
}

static void display_update_system_memory(const display_status_t *status)
{
    char free_text[48] = "剩余 --K / 连续 --K";
    char largest_text[48] = "DMA --K / PS --M";
    lv_color_t color = lv_color_hex(DISPLAY_UI_COLOR_TEXT_MUTED);

    if (status == NULL ||
        s_system_memory_free_label == NULL ||
        s_system_memory_largest_label == NULL) {
        return;
    }

    if (status->memory_internal_free > 0U ||
        status->memory_internal_largest > 0U) {
        (void)snprintf(free_text,
                       sizeof(free_text),
                       "剩余 %uK / 连续 %uK",
                       (unsigned)(status->memory_internal_free / 1024U),
                       (unsigned)(status->memory_internal_largest / 1024U));
        (void)snprintf(largest_text,
                       sizeof(largest_text),
                       "DMA %uK / PS %uM",
                       (unsigned)(status->memory_dma_largest / 1024U),
                       (unsigned)(status->memory_psram_largest / (1024U * 1024U)));
    }

    if (status->memory_health == DISPLAY_MEMORY_HEALTH_CRITICAL) {
        color = lv_color_hex(DISPLAY_UI_COLOR_RED);
    } else if (status->memory_health == DISPLAY_MEMORY_HEALTH_WARNING) {
        color = lv_color_hex(DISPLAY_UI_COLOR_AMBER);
    }

    display_text_set_color(s_system_memory_free_label, color, 0);
    display_text_set_color(s_system_memory_largest_label, color, 0);
    display_text_set(s_system_memory_free_label, free_text);
    display_text_set(s_system_memory_largest_label, largest_text);
}

static void display_update_ai_chat_page(const display_status_t *status)
{
    const char *state_text = "待命";
    const char *caption_text = "";
    lv_color_t state_color = lv_color_hex(0x23C17D);
    lv_color_t caption_color = lv_color_hex(0x1768B7);
    uint8_t message_count = 0;
    const display_ai_chat_message_t *latest_message = NULL;
    bool show_new_chat_button = false;

    if (status == NULL) {
        return;
    }

    display_apply_ai_dialog_font_if_ready();
    show_new_chat_button = display_ai_chat_should_show_new_chat_button(status);

    if (show_new_chat_button) {
        state_text = "休息";
        state_color = lv_color_hex(0x64758A);
    } else if (status->ai_chat_listening && !status->ai_chat_cloud_speaking) {
        state_text = "聆听";
        state_color = lv_color_hex(0x2F82D7);
    } else if (status->ai_chat_cloud_speaking) {
        state_text = "回复";
        state_color = lv_color_hex(0x23C17D);
    } else if (status->ai_chat_active) {
        state_text = "待命";
    } else if (status->ai_chat_state != 0) {
        state_text = "连接";
        state_color = lv_color_hex(0xF59E0B);
    }
    if (status->ai_chat_state == DISPLAY_AI_CHAT_STATE_ERROR) {
        state_text = "异常";
        state_color = lv_color_hex(0xE45757);
    }

    if (s_ai_status_label != NULL) {
        display_text_set_color(s_ai_status_label, state_color, 0);
        display_text_set(s_ai_status_label, state_text);
    }
    if (s_ai_avatar_state_label != NULL) {
        display_text_set_color(s_ai_avatar_state_label, state_color, 0);
        display_text_set(s_ai_avatar_state_label, state_text);
    }

    message_count = status->ai_chat_message_count > DISPLAY_AI_CHAT_MESSAGE_MAX ?
        DISPLAY_AI_CHAT_MESSAGE_MAX : status->ai_chat_message_count;

    for (int index = (int)message_count - 1; index >= 0; --index) {
        if (status->ai_chat_messages[index].text[0] != '\0') {
            latest_message = &status->ai_chat_messages[index];
            break;
        }
    }

    display_update_ai_avatar(status, latest_message);

    if (show_new_chat_button) {
        caption_text = status->ai_chat_state == DISPLAY_AI_CHAT_STATE_ERROR ?
            "连接异常，请点击下方按钮重新开始。" :
            "我在，准备好后可以开始新的对话。";
        caption_color = status->ai_chat_state == DISPLAY_AI_CHAT_STATE_ERROR ?
            lv_color_hex(0xE45757) : lv_color_hex(0x64758A);
    } else if (latest_message != NULL) {
        caption_text = latest_message->text;
        caption_color = latest_message->caption_type == DISPLAY_AI_CHAT_CAPTION_TYPE_ASR ?
            lv_color_hex(0x0D8A59) : lv_color_hex(0x1768B7);
    } else if (status->ai_chat_cloud_speaking && status->ai_chat_tts_caption[0] != '\0') {
        caption_text = status->ai_chat_tts_caption;
        caption_color = lv_color_hex(0x1768B7);
    } else if (status->ai_chat_listening && status->ai_chat_asr_caption[0] != '\0') {
        caption_text = status->ai_chat_asr_caption;
        caption_color = lv_color_hex(0x0D8A59);
    } else if (status->ai_chat_active) {
        caption_text = status->ai_chat_listening ? "正在聆听，请直接说话。" : "已连接，等待你说话。";
        caption_color = state_color;
    } else {
        caption_text = "正在连接 AI 服务。";
        caption_color = lv_color_hex(0xF59E0B);
    }

    if (message_count > 0 && !ai_chat_font_is_ready()) {
        caption_text = "字幕加载中";
        caption_color = lv_color_hex(0xF59E0B);
    }
    if (s_ai_single_caption_label != NULL) {
        lv_obj_set_height(s_ai_single_caption_label, show_new_chat_button ? 116 : 178);
        display_text_set_color(s_ai_single_caption_label, caption_color, 0);
        display_set_ai_chat_caption_label_text(s_ai_single_caption_label, caption_text);
    }
    if (s_ai_new_chat_btn != NULL) {
        if (show_new_chat_button) {
            lv_obj_clear_flag(s_ai_new_chat_btn, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_ai_new_chat_btn);
        } else {
            lv_obj_add_flag(s_ai_new_chat_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void display_update_ai_chat_settings_page(const display_status_t *status)
{
    if (status == NULL) {
        status = &s_last_status;
    }

    if (s_ai_settings_mic_value_label != NULL) {
        lv_label_set_text_fmt(s_ai_settings_mic_value_label,
                              "%u",
                              (unsigned)status->audio_capture_gain_percent);
    }

    if (s_ai_settings_speaker_value_label != NULL) {
        lv_label_set_text_fmt(s_ai_settings_speaker_value_label,
                              "%u",
                              (unsigned)status->audio_speaker_volume_percent);
    }

    display_update_ai_avatar_choice_buttons(status->ai_chat_avatar);
}

static bool display_network_test_status_changed(const display_status_t *status,
                                                const display_status_t *previous_status)
{
    if (status == NULL || previous_status == NULL) {
        return true;
    }

    return status->network_connected != previous_status->network_connected ||
           status->ping_running != previous_status->ping_running ||
           status->ping_valid != previous_status->ping_valid ||
           status->ping_transmitted != previous_status->ping_transmitted ||
           status->ping_received != previous_status->ping_received ||
           status->ping_latency_avg_ms != previous_status->ping_latency_avg_ms ||
           status->ping_jitter_ms != previous_status->ping_jitter_ms ||
           status->ping_loss_percent != previous_status->ping_loss_percent ||
           strcmp(status->network_ssid, previous_status->network_ssid) != 0 ||
           strcmp(status->network_ip_addr, previous_status->network_ip_addr) != 0;
}

static void display_update_network_test_page(const display_status_t *status)
{
    char wifi_text[48] = {0};
    char ssid_text[40] = {0};
    char latency_text[24] = {0};
    char jitter_text[24] = {0};
    char loss_text[24] = {0};
    bool has_attempt = false;
    bool has_latency = false;
    bool has_jitter = false;
    bool has_result = false;
    bool service_ok = false;
    bool service_warn = false;
    bool jitter_warn = false;
    bool loss_warn = false;
    bool network_ok = false;
    const lv_color_t ok_text = lv_color_hex(0x0B6B45);
    const lv_color_t ok_fill = lv_color_hex(0xEAF8F1);
    const lv_color_t ok_stroke = lv_color_hex(0xBFEAD4);
    const lv_color_t warn_text = lv_color_hex(0xF59E0B);
    const lv_color_t warn_fill = lv_color_hex(0xFFF2D8);
    const lv_color_t neutral_fill = lv_color_hex(0xFFFFFF);
    const lv_color_t neutral_stroke = lv_color_hex(0xD5E0EB);

    if (status == NULL ||
        s_network_summary_wifi_label == NULL ||
        s_network_summary_ip_label == NULL ||
        s_network_gateway_value_label == NULL ||
        s_network_dns_value_label == NULL ||
        s_network_wan_value_label == NULL ||
        s_network_service_row == NULL ||
        s_network_service_value_label == NULL ||
        s_network_jitter_value_label == NULL ||
        s_network_loss_value_label == NULL ||
        s_network_result_box == NULL ||
        s_network_result_label == NULL ||
        s_network_result_detail_label == NULL) {
        return;
    }

    if (status->network_connected) {
        if (status->network_ssid[0] != '\0') {
            display_format_ssid(ssid_text, sizeof(ssid_text), status->network_ssid);
            snprintf(wifi_text, sizeof(wifi_text), "Wi-Fi %s", ssid_text);
        } else {
            strlcpy(wifi_text, "Wi-Fi 已连接", sizeof(wifi_text));
        }
    } else {
        strlcpy(wifi_text, "Wi-Fi 未连接", sizeof(wifi_text));
    }
    display_text_set(s_network_summary_wifi_label, wifi_text);
    display_text_set(s_network_summary_ip_label,
                      status->network_ip_addr[0] != '\0' ? status->network_ip_addr : "IP --");

    display_text_set(s_network_gateway_value_label, status->network_connected ? "正常" : "等待");
    display_text_set_color(s_network_gateway_value_label,
                                status->network_connected ? lv_color_hex(0x0D8A59) : lv_color_hex(0xF59E0B),
                                0);
    display_text_set(s_network_dns_value_label, status->network_connected ? "正常" : "等待");
    display_text_set_color(s_network_dns_value_label,
                                status->network_connected ? lv_color_hex(0x0D8A59) : lv_color_hex(0xF59E0B),
                                0);

    has_attempt = status->ping_transmitted > 0U;
    has_latency = status->ping_received > 0U;
    has_jitter = status->ping_received > 1U;
    has_result = status->ping_valid;
    service_ok = has_latency && status->ping_latency_avg_ms <= 120U;
    service_warn = has_latency && status->ping_latency_avg_ms > 120U;
    jitter_warn = has_jitter && status->ping_jitter_ms > 30U;
    loss_warn = has_attempt && status->ping_loss_percent > 0U;
    network_ok = has_result && service_ok && has_jitter && !jitter_warn && !loss_warn;

    if (has_latency) {
        snprintf(latency_text, sizeof(latency_text), "%lu ms",
                 (unsigned long)status->ping_latency_avg_ms);
    } else {
        strlcpy(latency_text, "--", sizeof(latency_text));
    }
    if (has_jitter) {
        snprintf(jitter_text, sizeof(jitter_text), "%lu ms",
                 (unsigned long)status->ping_jitter_ms);
    } else {
        strlcpy(jitter_text, "--", sizeof(jitter_text));
    }
    if (has_attempt) {
        snprintf(loss_text, sizeof(loss_text), "%lu%%",
                 (unsigned long)status->ping_loss_percent);
    } else {
        strlcpy(loss_text, "--", sizeof(loss_text));
    }

    if (status->ping_running) {
        display_text_set(s_network_wan_value_label, "测试中");
        display_text_set(s_network_service_value_label, has_latency ? latency_text : "测试中");
        display_text_set(s_network_jitter_value_label, jitter_text);
        display_text_set(s_network_loss_value_label, loss_text);
        display_text_set(s_network_result_label, "正在测试网络");
        display_text_set(s_network_result_detail_label, "请稍候");
        lv_obj_set_style_bg_color(s_network_service_row, lv_color_hex(0xE7F1FB), 0);
        lv_obj_set_style_border_color(s_network_service_row, neutral_stroke, 0);
        lv_obj_set_style_bg_color(s_network_result_box, lv_color_hex(0xE7F1FB), 0);
        lv_obj_set_style_border_color(s_network_result_box, neutral_stroke, 0);
        display_text_set_color(s_network_result_label, lv_color_hex(0x1768B7), 0);
        display_text_set_color(s_network_result_detail_label, lv_color_hex(0x64758A), 0);
    } else if (!status->network_connected) {
        display_text_set(s_network_wan_value_label, "等待");
        display_text_set(s_network_service_value_label, "等待");
        display_text_set(s_network_jitter_value_label, "--");
        display_text_set(s_network_loss_value_label, "--");
        display_text_set(s_network_result_label, "网络未连接");
        display_text_set(s_network_result_detail_label, "先连接 Wi-Fi");
        lv_obj_set_style_bg_color(s_network_service_row, neutral_fill, 0);
        lv_obj_set_style_border_color(s_network_service_row, neutral_stroke, 0);
        lv_obj_set_style_bg_color(s_network_result_box, warn_fill, 0);
        lv_obj_set_style_border_color(s_network_result_box, warn_text, 0);
        display_text_set_color(s_network_result_label, warn_text, 0);
        display_text_set_color(s_network_result_detail_label, lv_color_hex(0x64758A), 0);
    } else if (has_result) {
        display_text_set(s_network_wan_value_label, has_latency ? "正常" : "异常");
        display_text_set(s_network_service_value_label, latency_text);
        display_text_set(s_network_jitter_value_label, jitter_text);
        display_text_set(s_network_loss_value_label, loss_text);
        display_text_set(s_network_result_label, network_ok ? "基础网络正常" : "网络质量波动");
        display_text_set(s_network_result_detail_label,
                         service_warn ? "服务延迟略高" :
                         (jitter_warn || loss_warn || !has_latency || !has_jitter) ?
                             "网络质量波动" : "服务响应正常");
        lv_obj_set_style_bg_color(s_network_service_row,
                                  service_ok ? ok_fill : (service_warn ? warn_fill : neutral_fill),
                                  0);
        lv_obj_set_style_border_color(s_network_service_row,
                                      service_ok ? ok_stroke : (service_warn ? warn_text : neutral_stroke),
                                      0);
        lv_obj_set_style_bg_color(s_network_result_box, network_ok ? ok_fill : warn_fill, 0);
        lv_obj_set_style_border_color(s_network_result_box,
                                      network_ok ? ok_stroke : warn_text,
                                      0);
        display_text_set_color(s_network_result_label,
                                    network_ok ? ok_text : warn_text,
                                    0);
        display_text_set_color(s_network_result_detail_label,
                               network_ok ? ok_text : warn_text,
                               0);
    } else {
        display_text_set(s_network_wan_value_label, "未测试");
        display_text_set(s_network_service_value_label, "--");
        display_text_set(s_network_jitter_value_label, "--");
        display_text_set(s_network_loss_value_label, "--");
        display_text_set(s_network_result_label, "基础网络待测");
        display_text_set(s_network_result_detail_label, "点击重测");
        lv_obj_set_style_bg_color(s_network_service_row, neutral_fill, 0);
        lv_obj_set_style_border_color(s_network_service_row, neutral_stroke, 0);
        lv_obj_set_style_bg_color(s_network_result_box, warn_fill, 0);
        lv_obj_set_style_border_color(s_network_result_box, warn_text, 0);
        display_text_set_color(s_network_result_label, warn_text, 0);
        display_text_set_color(s_network_result_detail_label, lv_color_hex(0x64758A), 0);
    }

    display_text_set_color(s_network_wan_value_label,
                           status->network_connected && (!has_result || has_latency) ?
                               lv_color_hex(0x0D8A59) : lv_color_hex(0xF59E0B),
                           0);
    display_text_set_color(s_network_service_value_label,
                           service_warn || (has_result && !has_latency) ?
                               warn_text : (has_latency ? lv_color_hex(0x0D8A59) : lv_color_hex(0x64758A)),
                           0);
    display_text_set_color(s_network_jitter_value_label,
                           jitter_warn || (has_result && !has_jitter) ?
                               warn_text : (has_jitter ? lv_color_hex(0x0D8A59) : lv_color_hex(0x64758A)),
                           0);
    display_text_set_color(s_network_loss_value_label,
                           loss_warn ? warn_text :
                               (has_attempt ? lv_color_hex(0x0D8A59) : lv_color_hex(0x64758A)),
                           0);
}

static void display_update_tirtc_config_page(const display_status_t *status)
{
    if (s_tirtc_config_value_labels[DISPLAY_TIRTC_CONFIG_FIELD_DEVICE_ID] == NULL) {
        return;
    }

    for (uint8_t index = 0; index < DISPLAY_TIRTC_CONFIG_FIELD_COUNT; ++index) {
        if (s_tirtc_config_value_labels[index] != NULL) {
            display_text_set(s_tirtc_config_value_labels[index],
                              display_tirtc_config_field_value(status,
                                                               (display_tirtc_config_field_t)index));
        }
    }
}

static void display_update_call_page(const display_status_t *status)
{
    char payload[DISPLAY_DEVICE_QR_PAYLOAD_MAX] = {0};

    if (!display_build_device_id_qr_payload(payload, sizeof(payload), status)) {
        ESP_LOGW(TAG, "call qr payload build failed");
        return;
    }
    if (s_call_device_id_label != NULL) {
        display_text_set(s_call_device_id_label, payload);
    }
    if (strcmp(payload, s_call_qr_payload) == 0) {
        return;
    }

#if LV_USE_QRCODE
    if (s_call_qrcode == NULL) {
        return;
    }
    esp_err_t qr_ret = display_qr_image_update(&s_call_qr_image,
                                               s_call_qrcode,
                                               display_native_scale_square(
                                                   DISPLAY_CALL_QR_IMAGE_SIZE - 12),
                                               payload,
                                               strlen(payload));
    if (qr_ret != ESP_OK) {
        ESP_LOGW(TAG, "call qr update failed: %s", esp_err_to_name(qr_ret));
        return;
    }
#endif
    strlcpy(s_call_qr_payload, payload, sizeof(s_call_qr_payload));
}

static void display_update_wechat_page(const display_status_t *status)
{
#if LV_USE_QRCODE
    char payload[DISPLAY_CONTACT_QR_PAYLOAD_MAX] = {0};

    if (s_wechat_qrcode == NULL) {
        return;
    }
    if (!display_build_device_id_qr_payload(payload, sizeof(payload), status)) {
        ESP_LOGW(TAG, "wechat qr payload build failed");
        return;
    }
    if (strcmp(payload, s_wechat_qr_payload) == 0) {
        return;
    }

    esp_err_t qr_ret = display_qr_image_update(&s_wechat_qr_image,
                                               s_wechat_qrcode,
                                               display_native_scale_square(DISPLAY_CONTACT_QR_IMAGE_SIZE),
                                               payload,
                                               strlen(payload));
    if (qr_ret == ESP_OK) {
        strlcpy(s_wechat_qr_payload, payload, sizeof(s_wechat_qr_payload));
    } else {
        ESP_LOGW(TAG, "wechat qr update failed: %s", esp_err_to_name(qr_ret));
    }
#else
    (void)status;
#endif
}

static void display_update_wechat_contact_list(const display_status_t *status)
{
    uint8_t visible_count = 0;
    uint8_t shown_count = 0;

    if (status == NULL) {
        status = &s_last_status;
    }

    visible_count = status->wechat_contact_count > DISPLAY_WECHAT_CONTACT_COUNT ?
        DISPLAY_WECHAT_CONTACT_COUNT : status->wechat_contact_count;

    for (uint8_t index = 0; index < DISPLAY_WECHAT_CONTACT_COUNT; ++index) {
        const char *remark = "";
        const char *open_id = "";
        bool show = index < visible_count &&
                    status->wechat_contacts[index].open_id[0] != '\0';

        if (show) {
            remark = status->wechat_contacts[index].remark[0] != '\0' ?
                status->wechat_contacts[index].remark : "微信联系人";
            open_id = status->wechat_contacts[index].open_id;
            ++shown_count;
        }
        if (s_wechat_contact_remark_labels[index] != NULL) {
            display_text_set(s_wechat_contact_remark_labels[index], remark);
        }
        if (s_wechat_contact_open_id_labels[index] != NULL) {
            display_text_set(s_wechat_contact_open_id_labels[index], open_id);
        }
        if (s_wechat_contact_rows[index] != NULL) {
            if (show) {
                lv_obj_clear_flag(s_wechat_contact_rows[index], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_wechat_contact_rows[index], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    if (s_wechat_empty_label != NULL) {
        if (shown_count == 0) {
            const char *empty_text = "暂无微信联系人";
            if (!status->wechat_contacts_ready) {
                empty_text = status->wechat_contacts_last_error == ESP_OK ?
                             "联系人加载中" : "联系人加载失败";
            } else if (status->wechat_contacts_last_error != ESP_OK) {
                empty_text = "授权联系人同步失败";
            } else if (!status->wechat_contacts_server_synced) {
                empty_text = "授权联系人同步中";
            }
            display_text_set(s_wechat_empty_label, empty_text);
            lv_obj_clear_flag(s_wechat_empty_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_wechat_empty_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static const char *display_call_state_text(display_call_state_t state)
{
    switch (state) {
    case DISPLAY_CALL_STATE_OUTGOING:
        return "正在呼叫";
    case DISPLAY_CALL_STATE_INCOMING:
        return "来电";
    case DISPLAY_CALL_STATE_CONNECTING:
        return "正在连接";
    case DISPLAY_CALL_STATE_IN_CALL:
        return "通话中";
    case DISPLAY_CALL_STATE_ERROR:
        return "通话异常";
    case DISPLAY_CALL_STATE_IDLE:
    default:
        return "空闲";
    }
}

static bool display_call_state_keeps_active_page(display_call_state_t state)
{
    return state == DISPLAY_CALL_STATE_OUTGOING ||
           state == DISPLAY_CALL_STATE_CONNECTING ||
           state == DISPLAY_CALL_STATE_IN_CALL;
}

static void display_update_call_video_frame(void)
{
    call_video_renderer_stats_t stats = {0};
    call_video_frame_trace_t frame_trace = {0};
    const uint16_t *pixels = NULL;
    const uint16_t *presentation_pixels = NULL;
    size_t pixel_count = 0;
    lv_obj_t *image = NULL;
    lv_obj_t *placeholder = NULL;
    lv_img_dsc_t *image_dsc = NULL;
#if CONFIG_APP_CALL_VIDEO_DIRECT_LCD
    display_call_video_overlays_t *overlays = NULL;
#endif
    uint32_t *sequence = NULL;
    bool *first_frame_logged = NULL;
    bool *direct_lcd_active = NULL;
#if CONFIG_APP_CALL_VIDEO_DIRECT_LCD
    bool *direct_lcd_failed = NULL;
#endif
    int64_t *last_presented_at_us = NULL;
    int64_t *last_stall_log_at_us = NULL;
    uint32_t *last_presented_sequence = NULL;
    const char *owner = NULL;
    const char *presentation_path = "lvgl-async";
    uint32_t presentation_transfer_us = 0;
    uint32_t overlay_snapshot_us = 0;
    uint32_t overlay_compose_us = 0;
    bool frame_presented = false;
    bool direct_transition = false;

    if (s_call_video_image != NULL &&
        s_call_video_landscape_active &&
        display_page_is_visible(s_call_active_page)) {
        image = s_call_video_image;
        placeholder = s_call_video_placeholder_label;
        image_dsc = &s_call_video_image_dsc;
#if CONFIG_APP_CALL_VIDEO_DIRECT_LCD
        overlays = &s_call_video_overlays;
#endif
        sequence = &s_call_video_sequence;
        first_frame_logged = &s_call_video_first_frame_logged;
        direct_lcd_active = &s_call_video_direct_lcd_active;
#if CONFIG_APP_CALL_VIDEO_DIRECT_LCD
        direct_lcd_failed = &s_call_video_direct_lcd_failed;
#endif
        last_presented_at_us = &s_call_video_last_presented_at_us;
        last_stall_log_at_us = &s_call_video_last_stall_log_at_us;
        last_presented_sequence = &s_call_video_last_presented_sequence;
        owner = "device-call";
    } else if (s_wechat_video_image != NULL &&
               s_call_video_landscape_active &&
               display_page_is_visible(s_wechat_active_page)) {
        image = s_wechat_video_image;
        placeholder = s_wechat_video_placeholder_label;
        image_dsc = &s_wechat_video_image_dsc;
#if CONFIG_APP_CALL_VIDEO_DIRECT_LCD
        overlays = &s_wechat_video_overlays;
#endif
        sequence = &s_wechat_video_sequence;
        first_frame_logged = &s_wechat_video_first_frame_logged;
        direct_lcd_active = &s_wechat_video_direct_lcd_active;
#if CONFIG_APP_CALL_VIDEO_DIRECT_LCD
        direct_lcd_failed = &s_wechat_video_direct_lcd_failed;
#endif
        last_presented_at_us = &s_wechat_video_last_presented_at_us;
        last_stall_log_at_us = &s_wechat_video_last_stall_log_at_us;
        last_presented_sequence = &s_wechat_video_last_presented_sequence;
        owner = "wechat";
    } else {
        return;
    }

    call_video_renderer_get_stats(&stats);
    if (!stats.running || !stats.frame_ready) {
        return;
    }

#if CONFIG_APP_CALL_VIDEO_DIRECT_LCD
    if (overlays != NULL &&
        !overlays->hidden &&
        overlays->hide_at_us > 0 &&
        esp_timer_get_time() >= overlays->hide_at_us) {
        display_set_call_video_overlays_hidden(overlays,
                                               image,
                                               placeholder,
                                               *direct_lcd_active,
                                               true);
        if (image != NULL &&
            !lv_obj_has_flag(image, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
        }
    }

#endif

    esp_err_t ret = call_video_renderer_present_next_rgb565(&pixels,
                                                             &pixel_count,
                                                             sequence,
                                                             &frame_trace);
    if (ret != ESP_OK) {
        return;
    }
    if (pixels == NULL || pixel_count != (size_t)CALL_VIDEO_RENDER_WIDTH *
                                           CALL_VIDEO_RENDER_HEIGHT) {
        call_video_renderer_release_presented_rgb565();
        return;
    }

#if CONFIG_APP_CALL_VIDEO_DIRECT_LCD
    if (!*direct_lcd_failed && overlays != NULL) {
        direct_transition = !*direct_lcd_active;
        if (placeholder != NULL &&
            !lv_obj_has_flag(placeholder, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(placeholder, LV_OBJ_FLAG_HIDDEN);
        }
        /* Keep LVGL's hidden backing image on the presented buffer for a true
         * fallback. Visible controls are composed in the dedicated shared
         * RGB565 buffer, then sent through one full-frame LCD DMA transfer. */
        lv_img_cache_invalidate_src(image_dsc);
        presentation_pixels = pixels;
        if (!overlays->hidden) {
            overlay_compose_us = display_compose_call_video_overlays(
                overlays,
                pixels,
                CALL_VIDEO_RENDER_WIDTH,
                CALL_VIDEO_RENDER_HEIGHT,
                &overlay_snapshot_us,
                &presentation_pixels);
        }
        image_dsc->data = (const uint8_t *)presentation_pixels;
        ret = display_driver_blit_rgb565(DISPLAY_CALL_VIDEO_X,
                                         DISPLAY_CALL_VIDEO_Y,
                                         CALL_VIDEO_RENDER_WIDTH,
                                         CALL_VIDEO_RENDER_HEIGHT,
                                         presentation_pixels,
                                         &presentation_transfer_us);
        if (ret == ESP_OK) {
            frame_presented = true;
            presentation_path = overlays->hidden ?
                                    "direct-psram-dma" :
                                    "direct-composited-overlay";
            *direct_lcd_active = true;
            if (direct_transition || overlays->refresh_trace_pending) {
                ESP_LOGI(TAG,
                         "%s video path=direct transfer=%luus overlay=snapshot:%lu/blend:%luus",
                         owner,
                         (unsigned long)presentation_transfer_us,
                         (unsigned long)overlay_snapshot_us,
                         (unsigned long)overlay_compose_us);
                overlays->refresh_trace_pending = false;
            }
        } else {
            *direct_lcd_failed = true;
            *direct_lcd_active = false;
            ESP_LOGW(TAG,
                     "%s video direct LCD unavailable: %s; using LVGL fallback",
                     owner,
                     esp_err_to_name(ret));
        }
    }
#endif

    if (!frame_presented) {
        bool image_was_hidden = lv_obj_has_flag(image, LV_OBJ_FLAG_HIDDEN);
        lv_img_cache_invalidate_src(image_dsc);
        image_dsc->data = (const uint8_t *)pixels;
        if (image_was_hidden || lv_img_get_src(image) != image_dsc) {
            lv_img_set_src(image, image_dsc);
        } else {
            lv_obj_invalidate(image);
        }
        if (image_was_hidden) {
            lv_obj_clear_flag(image, LV_OBJ_FLAG_HIDDEN);
        }
        *direct_lcd_active = false;
    }
    if (placeholder != NULL &&
        !lv_obj_has_flag(placeholder, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(placeholder, LV_OBJ_FLAG_HIDDEN);
    }
    if (!frame_presented) {
#if CONFIG_APP_CALL_VIDEO_DIRECT_LCD
        if (*direct_lcd_failed) {
            int64_t refresh_started_us = esp_timer_get_time();
            lv_refr_now(s_display);
            presentation_transfer_us =
                (uint32_t)(esp_timer_get_time() - refresh_started_us);
            presentation_path = "lvgl-sync-fallback";
        }
#else
        int64_t refresh_started_us = esp_timer_get_time();
        lv_refr_now(s_display);
        presentation_transfer_us =
            (uint32_t)(esp_timer_get_time() - refresh_started_us);
        presentation_path = "lvgl-sync";
#endif
        frame_presented = true;
    }
    if (!*first_frame_logged) {
        *first_frame_logged = true;
        ESP_LOGI(TAG,
                 "%s video first frame presented: path=%s source=%ux%u output=%ux%u "
                 "sequence=%lu transfer=%luus",
                 owner,
                 presentation_path,
                 stats.source_width,
                 stats.source_height,
                 CALL_VIDEO_RENDER_WIDTH,
                 CALL_VIDEO_RENDER_HEIGHT,
                 (unsigned long)*sequence,
                 (unsigned long)presentation_transfer_us);
    }
    int64_t presented_at_us = esp_timer_get_time();
    if (*direct_lcd_active && !direct_transition &&
        last_presented_at_us != NULL && *last_presented_at_us > 0) {
        uint64_t gap_us = (uint64_t)(presented_at_us - *last_presented_at_us);
        if (gap_us >= DISPLAY_CALL_VIDEO_STALL_GAP_US &&
            (last_stall_log_at_us == NULL ||
             *last_stall_log_at_us == 0 ||
             presented_at_us - *last_stall_log_at_us >=
                 DISPLAY_CALL_VIDEO_STALL_LOG_US)) {
            call_video_renderer_get_stats(&stats);
            uint32_t sequence_gap = last_presented_sequence != NULL ?
                *sequence - *last_presented_sequence : 0U;
            ESP_LOGW(TAG,
                     "%s video stall: stage=present gap=%lluus lcd=%luus seq_gap=%lu "
                     "q=%lu/%lu drop=%lu/%lu",
                     owner,
                     (unsigned long long)gap_us,
                     (unsigned long)presentation_transfer_us,
                     (unsigned long)sequence_gap,
                     (unsigned long)stats.queue_depth,
                     (unsigned long)stats.conversion_queue_depth,
                     (unsigned long)stats.dropped_frames,
                     (unsigned long)stats.conversion_dropped_frames);
            if (last_stall_log_at_us != NULL) {
                *last_stall_log_at_us = presented_at_us;
            }
        }
    }
    if (last_presented_at_us != NULL) {
        *last_presented_at_us = presented_at_us;
    }
    if (last_presented_sequence != NULL) {
        *last_presented_sequence = *sequence;
    }
    if (frame_trace.frame_index > 0U) {
        call_video_renderer_get_stats(&stats);
        uint64_t total_us = (uint64_t)frame_trace.submit_us +
                            frame_trace.input_wait_us +
                            frame_trace.decode_us +
                            frame_trace.decode_copy_us +
                            frame_trace.decoded_wait_us +
                             frame_trace.convert_us +
                             frame_trace.output_wait_us +
                             presentation_transfer_us +
                             overlay_snapshot_us +
                             overlay_compose_us;
        ESP_LOGI(TAG,
                 "%s video boot f=%lu key=%d bootstrap=%d bytes=%lu pts=%lu path=%s "
                 "us=sub:%lu iq:%lu dec:%lu copy:%lu dq:%lu cvt:%lu oq:%lu lcd:%lu ui:%lu total:%llu "
                 "q=%lu/%lu drop=%lu/%lu",
                 owner,
                 (unsigned long)frame_trace.frame_index,
                 frame_trace.key_frame ? 1 : 0,
                 frame_trace.decoder_bootstrap ? 1 : 0,
                 (unsigned long)frame_trace.payload_bytes,
                 (unsigned long)frame_trace.pts,
                 presentation_path,
                 (unsigned long)frame_trace.submit_us,
                 (unsigned long)frame_trace.input_wait_us,
                 (unsigned long)frame_trace.decode_us,
                 (unsigned long)frame_trace.decode_copy_us,
                 (unsigned long)frame_trace.decoded_wait_us,
                 (unsigned long)frame_trace.convert_us,
                 (unsigned long)frame_trace.output_wait_us,
                 (unsigned long)presentation_transfer_us,
                  (unsigned long)(overlay_snapshot_us + overlay_compose_us),
                 (unsigned long long)total_us,
                 (unsigned long)stats.queue_depth,
                 (unsigned long)stats.conversion_queue_depth,
                 (unsigned long)stats.dropped_frames,
                 (unsigned long)stats.conversion_dropped_frames);
    }
}

static void display_update_call_active_page(const display_status_t *status)
{
    uint8_t mic = 0;
    uint8_t speaker = 0;
    int64_t elapsed_seconds = 0;
    bool in_call = false;
    bool video = false;
    bool session_changed = false;
    display_call_type_t visible_type = DISPLAY_CALL_TYPE_AUDIO;
    call_video_renderer_stats_t video_stats = {0};
    char mic_text[4] = {0};
    char speaker_text[4] = {0};
    char duration[8] = "00:00";
    char stats_text[64] = "--x-- | TX -- | RX --";
    char peer[DISPLAY_CALL_CONTACT_DEVICE_ID_MAX + 8] = "设备 --";
    const char *state_text = NULL;

    if (status == NULL) {
        status = &s_last_status;
    }

    call_video_renderer_get_stats(&video_stats);
    /* The renderer intentionally stays warm between calls. The signalling
     * snapshot, not renderer lifetime, owns whether this session is audio or video. */
    video = status->call_type == DISPLAY_CALL_TYPE_VIDEO;
    visible_type = video ? DISPLAY_CALL_TYPE_VIDEO : DISPLAY_CALL_TYPE_AUDIO;
    display_apply_call_video_layout(video);
    display_set_video_refresh_enabled(!s_call_hangup_pending &&
                                      video &&
                                      s_call_video_landscape_active &&
                                      display_call_state_keeps_active_page(status->call_state) &&
                                      display_page_is_visible(s_call_active_page));
    in_call = status->call_state == DISPLAY_CALL_STATE_IN_CALL;
    state_text = s_call_hangup_pending ?
                     "正在挂断" :
                     display_call_state_text(status->call_state);
    session_changed = s_call_visible_type != visible_type ||
                      strcmp(s_call_visible_room_id, status->call_room_id) != 0;
    if (session_changed) {
        s_call_visible_type = visible_type;
        strlcpy(s_call_visible_room_id,
                status->call_room_id,
                sizeof(s_call_visible_room_id));
        display_reset_call_video_surface();
    }

    if (s_call_audio_panel != NULL && s_call_video_panel != NULL) {
        if (video) {
            if (!lv_obj_has_flag(s_call_audio_panel, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_add_flag(s_call_audio_panel, LV_OBJ_FLAG_HIDDEN);
            }
            if (lv_obj_has_flag(s_call_video_panel, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_clear_flag(s_call_video_panel, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            if (!lv_obj_has_flag(s_call_video_panel, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_add_flag(s_call_video_panel, LV_OBJ_FLAG_HIDDEN);
            }
            if (lv_obj_has_flag(s_call_audio_panel, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_clear_flag(s_call_audio_panel, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    if (in_call && s_call_active_started_us == 0) {
        s_call_active_started_us = esp_timer_get_time();
    } else if (!in_call && !display_call_state_keeps_active_page(status->call_state)) {
        s_call_active_started_us = 0;
    }

    if (s_call_active_started_us > 0) {
        elapsed_seconds = (esp_timer_get_time() - s_call_active_started_us) / 1000000LL;
        if (elapsed_seconds < 0) {
            elapsed_seconds = 0;
        }
        snprintf(duration,
                 sizeof(duration),
                 "%02lld:%02lld",
                 (long long)((elapsed_seconds / 60LL) % 100LL),
                 (long long)(elapsed_seconds % 60LL));
    }

    if (status->call_peer_device_id[0] != '\0') {
        snprintf(peer, sizeof(peer), "设备 %s", status->call_peer_device_id);
    }
    if (s_call_audio_state_label != NULL) {
        display_text_set(s_call_audio_state_label, state_text);
    }
    if (s_call_video_state_label != NULL) {
        display_text_set(s_call_video_state_label, state_text);
    }
    if (s_call_audio_peer_label != NULL) {
        display_text_set(s_call_audio_peer_label, peer);
    }
    if (s_call_video_peer_label != NULL) {
        display_text_set(s_call_video_peer_label, peer);
    }

    mic = status->audio_capture_gain_percent;
    speaker = status->audio_speaker_volume_percent;
    if (s_call_duration_label != NULL) {
        display_text_set(s_call_duration_label, duration);
    }
    if (s_call_video_duration_label != NULL) {
        display_text_set(s_call_video_duration_label, duration);
    }
    if (in_call && video) {
        if (video_stats.source_width > 0U && video_stats.source_height > 0U) {
            snprintf(stats_text,
                     sizeof(stats_text),
                     "%ux%u | TX %uf/%luk | RX %uf/%luk",
                     (unsigned)video_stats.source_width,
                     (unsigned)video_stats.source_height,
                     (unsigned)status->rtc_tx_video_fps,
                     (unsigned long)status->rtc_tx_video_transport_bitrate_kbps,
                     (unsigned)status->rtc_rx_video_fps,
                     (unsigned long)status->rtc_rx_video_transport_bitrate_kbps);
        } else {
            snprintf(stats_text,
                     sizeof(stats_text),
                     "--x-- | TX %uf/%luk | RX %uf/%luk",
                     (unsigned)status->rtc_tx_video_fps,
                     (unsigned long)status->rtc_tx_video_transport_bitrate_kbps,
                     (unsigned)status->rtc_rx_video_fps,
                     (unsigned long)status->rtc_rx_video_transport_bitrate_kbps);
        }
    }
    if (s_call_video_stats_label != NULL) {
        display_text_set(s_call_video_stats_label, stats_text);
    }
    snprintf(mic_text, sizeof(mic_text), "%u", (unsigned)mic);
    snprintf(speaker_text, sizeof(speaker_text), "%u", (unsigned)speaker);
    if (s_call_mic_value_label != NULL) {
        display_text_set(s_call_mic_value_label, mic_text);
    }
    if (s_call_speaker_value_label != NULL) {
        display_text_set(s_call_speaker_value_label, speaker_text);
    }
    if (s_call_video_mic_value_label != NULL) {
        display_text_set(s_call_video_mic_value_label, mic_text);
    }
    if (s_call_video_speaker_value_label != NULL) {
        display_text_set(s_call_video_speaker_value_label, speaker_text);
    }

    if (video && s_call_video_placeholder_label != NULL &&
        !s_call_video_direct_lcd_active &&
        (s_call_video_image == NULL || lv_obj_has_flag(s_call_video_image, LV_OBJ_FLAG_HIDDEN))) {
        display_text_set(s_call_video_placeholder_label,
                         status->call_state == DISPLAY_CALL_STATE_ERROR ?
                         "视频暂不可用" : "正在建立视频...");
    }
}

static bool display_wechat_call_state_keeps_active_page(display_wechat_call_state_t state)
{
    switch (state) {
    case DISPLAY_WECHAT_CALL_STATE_CALLING:
    case DISPLAY_WECHAT_CALL_STATE_CONNECTING:
    case DISPLAY_WECHAT_CALL_STATE_IN_CALL:
    case DISPLAY_WECHAT_CALL_STATE_CLOSING:
        return true;
    case DISPLAY_WECHAT_CALL_STATE_IDLE:
    case DISPLAY_WECHAT_CALL_STATE_INCOMING:
    default:
        return false;
    }
}

static bool display_wechat_call_state_opens_active_page(display_wechat_call_state_t state)
{
    return state == DISPLAY_WECHAT_CALL_STATE_CALLING ||
           state == DISPLAY_WECHAT_CALL_STATE_CONNECTING ||
           state == DISPLAY_WECHAT_CALL_STATE_IN_CALL;
}

static void display_update_wechat_active_page(const display_status_t *status)
{
    uint8_t mic = 0;
    uint8_t speaker = 0;
    int64_t elapsed_seconds = 0;
    char duration[16] = "00:00";
    const char *state_text = "正在呼叫";

    if (status == NULL) {
        status = &s_last_status;
    }

    bool in_call = status->wechat_call_state == DISPLAY_WECHAT_CALL_STATE_IN_CALL ||
                   status->rtc_call_active;
    bool session_active =
        display_wechat_call_state_keeps_active_page(status->wechat_call_state);

    switch (status->wechat_call_state) {
    case DISPLAY_WECHAT_CALL_STATE_CONNECTING:
        state_text = "正在连接";
        break;
    case DISPLAY_WECHAT_CALL_STATE_IN_CALL:
        state_text = "通话中";
        break;
    case DISPLAY_WECHAT_CALL_STATE_CLOSING:
        state_text = "正在挂断";
        break;
    case DISPLAY_WECHAT_CALL_STATE_CALLING:
    default:
        state_text = "正在呼叫";
        break;
    }

    if (session_active && !s_wechat_video_session_active) {
        s_wechat_video_sequence = 0;
        s_wechat_video_first_frame_logged = false;
        s_wechat_video_direct_lcd_active = false;
        s_wechat_video_direct_lcd_failed = false;
        s_wechat_video_last_presented_at_us = 0;
        s_wechat_video_last_stall_log_at_us = 0;
        s_wechat_video_last_presented_sequence = 0;
        display_set_call_video_overlays_hidden(&s_wechat_video_overlays,
                                               s_wechat_video_image,
                                               s_wechat_video_placeholder_label,
                                               false,
                                               true);
        if (s_wechat_video_image != NULL) {
            if (!lv_obj_has_flag(s_wechat_video_image, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_add_flag(s_wechat_video_image, LV_OBJ_FLAG_HIDDEN);
            }
            lv_img_cache_invalidate_src(&s_wechat_video_image_dsc);
            s_wechat_video_image_dsc.data = NULL;
            call_video_renderer_release_presented_rgb565();
        }
        if (s_wechat_video_placeholder_label != NULL) {
            lv_obj_clear_flag(s_wechat_video_placeholder_label, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (!session_active && s_wechat_video_session_active) {
        display_set_call_video_overlays_hidden(&s_wechat_video_overlays,
                                               s_wechat_video_image,
                                               s_wechat_video_placeholder_label,
                                               s_wechat_video_direct_lcd_active,
                                               false);
        if (s_wechat_video_image != NULL) {
            if (!lv_obj_has_flag(s_wechat_video_image, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_add_flag(s_wechat_video_image, LV_OBJ_FLAG_HIDDEN);
            }
            lv_img_cache_invalidate_src(&s_wechat_video_image_dsc);
            s_wechat_video_image_dsc.data = NULL;
            call_video_renderer_release_presented_rgb565();
        }
        s_wechat_video_direct_lcd_active = false;
        s_wechat_video_last_presented_at_us = 0;
        s_wechat_video_last_stall_log_at_us = 0;
        s_wechat_video_last_presented_sequence = 0;
    }
    s_wechat_video_session_active = session_active;

    display_set_video_refresh_enabled(
        CONFIG_APP_WECHAT_VOIP_REMOTE_VIDEO_ENABLE &&
        s_call_video_landscape_active &&
        session_active &&
        display_page_is_visible(s_wechat_active_page));

    if (in_call && s_wechat_active_started_us == 0) {
        s_wechat_active_started_us = esp_timer_get_time();
    }
    if (!in_call) {
        s_wechat_active_started_us = 0;
    }

    if (in_call && s_wechat_active_started_us > 0) {
        elapsed_seconds = (esp_timer_get_time() - s_wechat_active_started_us) / 1000000LL;
        if (elapsed_seconds < 0) {
            elapsed_seconds = 0;
        }
        snprintf(duration,
                 sizeof(duration),
                 "%02lld:%02lld",
                 (long long)((elapsed_seconds / 60LL) % 100LL),
                 (long long)(elapsed_seconds % 60LL));
    }

    mic = status->audio_capture_gain_percent;
    speaker = status->audio_speaker_volume_percent;

    if (s_wechat_duration_label != NULL) {
        display_text_set(s_wechat_duration_label, duration);
    }
    if (s_wechat_video_state_label != NULL) {
        display_text_set(s_wechat_video_state_label, state_text);
    }
    if (s_wechat_mic_value_label != NULL) {
        lv_label_set_text_fmt(s_wechat_mic_value_label, "%u", (unsigned)mic);
    }
    if (s_wechat_speaker_value_label != NULL) {
        lv_label_set_text_fmt(s_wechat_speaker_value_label, "%u", (unsigned)speaker);
    }
    if (s_wechat_video_placeholder_label != NULL &&
        !s_wechat_video_direct_lcd_active &&
        (s_wechat_video_image == NULL ||
         lv_obj_has_flag(s_wechat_video_image, LV_OBJ_FLAG_HIDDEN))) {
        display_text_set(s_wechat_video_placeholder_label,
                         CONFIG_APP_WECHAT_VOIP_REMOTE_VIDEO_ENABLE ?
                         "WAITING FOR WECHAT VIDEO" : "WECHAT AUDIO CALL");
    }
}

static void display_update_wifi_connect_feedback(const display_status_t *status)
{
    if (!s_wifi_connect_pending) {
        return;
    }

    bool connect_target_matched = status->network_connected &&
                                  (s_wifi_connect_target_ssid[0] == '\0' ||
                                   status->network_ssid[0] == '\0' ||
                                   strcmp(status->network_ssid, s_wifi_connect_target_ssid) == 0);

    if (connect_target_matched) {
        s_wifi_connect_pending = false;
        s_wifi_connect_state = DISPLAY_WIFI_CONNECT_STATE_CONNECTED;
        display_set_password_border_color(lv_color_hex(0x2E8F6B));
        s_wifi_connect_target_ssid[0] = '\0';
        display_show_wifi_page();
        return;
    }

    if (status->network_connect_failed) {
        s_wifi_connect_pending = false;
        s_wifi_connect_state = DISPLAY_WIFI_CONNECT_STATE_FAILED;
        display_set_password_border_color(lv_color_hex(0xC8513C));
        s_wifi_connect_target_ssid[0] = '\0';
        return;
    }

    if ((esp_timer_get_time() - s_wifi_connect_request_us) > DISPLAY_WIFI_CONNECT_TIMEOUT_US) {
        s_wifi_connect_pending = false;
        s_wifi_connect_state = DISPLAY_WIFI_CONNECT_STATE_TIMEOUT;
        display_set_password_border_color(lv_color_hex(0xC8513C));
        s_wifi_connect_target_ssid[0] = '\0';
    }
}

static void display_update_ota_page(const display_status_t *status)
{
    char progress_text[16] = {0};
    const char *current_version = "--";
    const char *target_version = "--";
    bool ready_to_reboot = false;
    bool checking = false;
    bool upgrading = false;
    bool failed = false;
    bool up_to_date = false;

    if (status == NULL || s_ota_status_label == NULL ||
        s_ota_version_label == NULL || s_ota_second_label == NULL ||
        s_ota_second_value_label == NULL || s_ota_progress_bar == NULL ||
        s_ota_start_btn == NULL || s_ota_start_btn_label == NULL ||
        s_ota_reboot_btn == NULL || s_ota_reboot_btn_label == NULL ||
        s_ota_action_panel == NULL || s_ota_progress_title_label == NULL ||
        s_ota_progress_percent_label == NULL || s_ota_progress_hint_label == NULL) {
        return;
    }

    ready_to_reboot = status->ota_state == DISPLAY_OTA_STATE_READY_TO_REBOOT;
    checking = status->ota_state == DISPLAY_OTA_STATE_CHECKING;
    upgrading = status->ota_state == DISPLAY_OTA_STATE_DOWNLOADING ||
                status->ota_state == DISPLAY_OTA_STATE_VERIFYING;
    failed = status->ota_state == DISPLAY_OTA_STATE_FAILED;
    up_to_date = status->ota_state == DISPLAY_OTA_STATE_IDLE &&
                 strstr(status->ota_message, "No update") != NULL;
    current_version = status->ota_current_version[0] != '\0' ? status->ota_current_version : "--";
    target_version = status->ota_target_version[0] != '\0' ? status->ota_target_version : "--";

    display_text_set(s_ota_version_label, current_version);
    display_text_set(s_ota_second_label,
                      status->ota_target_version[0] != '\0' || ready_to_reboot || failed ? "目标版本" : "协议版本");
    display_text_set(s_ota_second_value_label,
                      status->ota_target_version[0] != '\0' || ready_to_reboot || failed ? target_version : DISPLAY_TIRTC_VERSION_TEXT);

    lv_obj_add_flag(s_ota_action_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_ota_progress_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_ota_progress_title_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_ota_progress_percent_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_ota_progress_hint_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_ota_start_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_ota_reboot_btn, LV_OBJ_FLAG_HIDDEN);

    if (checking) {
        display_text_set(s_ota_status_label, "检查中");
        display_text_set_color(s_ota_status_label, lv_color_hex(0x1768B7), 0);
        lv_obj_clear_flag(s_ota_action_panel, LV_OBJ_FLAG_HIDDEN);
        display_text_set_layout(s_ota_progress_title_label, 114, 20, 150, LV_TEXT_ALIGN_LEFT);
        display_text_set(s_ota_progress_title_label, "正在检查更新");
        lv_obj_clear_flag(s_ota_progress_title_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if (upgrading) {
        uint8_t percent = status->ota_progress_percent > 100U ? 100U : status->ota_progress_percent;
        snprintf(progress_text, sizeof(progress_text), "%u%%", (unsigned)percent);
        display_text_set(s_ota_status_label, "升级中");
        display_text_set_color(s_ota_status_label, lv_color_hex(0x0D8A59), 0);
        lv_obj_clear_flag(s_ota_action_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_ota_progress_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_ota_progress_title_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_ota_progress_percent_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_ota_progress_hint_label, LV_OBJ_FLAG_HIDDEN);
        display_text_set_layout(s_ota_progress_title_label, 10, 7, 180, LV_TEXT_ALIGN_LEFT);
        display_text_set(s_ota_progress_title_label,
                          status->ota_state == DISPLAY_OTA_STATE_VERIFYING ? "正在校验固件" : "正在升级固件");
        display_text_set(s_ota_progress_percent_label, progress_text);
        display_text_set(s_ota_progress_hint_label, "升级中请保持供电");
        lv_bar_set_value(s_ota_progress_bar, percent, LV_ANIM_OFF);
        return;
    }

    if (ready_to_reboot) {
        display_text_set(s_ota_status_label, "待重启");
        display_text_set_color(s_ota_status_label, lv_color_hex(0x0D8A59), 0);
        lv_obj_clear_flag(s_ota_reboot_btn, LV_OBJ_FLAG_HIDDEN);
        display_text_set(s_ota_reboot_btn_label, "重启生效");
        return;
    }

    lv_obj_clear_flag(s_ota_start_btn, LV_OBJ_FLAG_HIDDEN);
    if (failed) {
        display_text_set(s_ota_status_label, "升级失败");
        display_text_set_color(s_ota_status_label, lv_color_hex(0xE45656), 0);
        lv_obj_set_style_bg_color(s_ota_start_btn, lv_color_hex(0xFFF1F1), 0);
        lv_obj_set_style_bg_color(s_ota_start_btn, lv_color_hex(0xFFE0E0), LV_STATE_PRESSED);
        lv_obj_set_style_border_color(s_ota_start_btn, lv_color_hex(0xE45656), 0);
        display_text_set_color(s_ota_start_btn, lv_color_hex(0xE45656), 0);
        display_text_set_color(s_ota_start_btn_label, lv_color_hex(0xE45656), 0);
        display_text_set(s_ota_start_btn_label, "重新检查");
    } else if (up_to_date) {
        display_text_set(s_ota_status_label, "已是最新");
        display_text_set_color(s_ota_status_label, lv_color_hex(0x0D8A59), 0);
        lv_obj_set_style_bg_color(s_ota_start_btn, lv_color_hex(0xE7F1FB), 0);
        lv_obj_set_style_bg_color(s_ota_start_btn, lv_color_hex(0xD7EAFB), LV_STATE_PRESSED);
        lv_obj_set_style_border_color(s_ota_start_btn, lv_color_hex(0x1768B7), 0);
        display_text_set_color(s_ota_start_btn, lv_color_hex(0x10243E), 0);
        display_text_set_color(s_ota_start_btn_label, lv_color_hex(0x10243E), 0);
        display_text_set(s_ota_start_btn_label, "重新检查");
    } else {
        display_text_set(s_ota_status_label,
                          status->ota_target_version[0] != '\0' ? "可升级" : "可检查");
        display_text_set_color(s_ota_status_label,
                                    status->ota_target_version[0] != '\0' ? lv_color_hex(0xF59E0B) : lv_color_hex(0x0D8A59),
                                    0);
        lv_obj_set_style_bg_color(s_ota_start_btn, lv_color_hex(0x20BF7A), 0);
        lv_obj_set_style_bg_color(s_ota_start_btn, lv_color_hex(0x0D8A59), LV_STATE_PRESSED);
        lv_obj_set_style_border_color(s_ota_start_btn, lv_color_hex(0x20BF7A), 0);
        display_text_set_color(s_ota_start_btn, lv_color_hex(0xFFFFFF), 0);
        display_text_set_color(s_ota_start_btn_label, lv_color_hex(0xFFFFFF), 0);
        display_text_set(s_ota_start_btn_label,
                          status->ota_target_version[0] != '\0' ? "立即升级" : "检查更新");
    }
}

static lv_obj_t *display_create_home_img(lv_obj_t *parent,
                                                  const lv_img_dsc_t *src,
                                                  lv_coord_t x,
                                                  lv_coord_t y)
{
    lv_obj_t *img = lv_img_create(parent);

    lv_img_set_src(img, src);
    lv_obj_set_pos(img, x, y);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);

    return img;
}

static lv_obj_t *display_create_home_centered_img(lv_obj_t *parent,
                                                  const lv_img_dsc_t *src,
                                                  lv_coord_t area_x,
                                                  lv_coord_t area_y,
                                                  lv_coord_t area_w,
                                                  lv_coord_t area_h)
{
    lv_coord_t img_w = (lv_coord_t)src->header.w;
    lv_coord_t img_h = (lv_coord_t)src->header.h;
    lv_coord_t x = area_x + ((area_w - img_w) / 2);
    lv_coord_t y = area_y + ((area_h - img_h) / 2);
    lv_obj_t *img = display_create_home_img(parent, src, x, y);

    lv_img_set_pivot(img, 0, 0);
    lv_img_set_zoom(img, LV_IMG_ZOOM_NONE);
    lv_obj_set_size(img, img_w, img_h);
    return img;
}

static bool display_home_use_landscape_layout(void)
{
    return DISPLAY_DRIVER_WIDTH >= DISPLAY_HOME_LANDSCAPE_MIN_WIDTH &&
           DISPLAY_DRIVER_HEIGHT >= DISPLAY_HOME_LANDSCAPE_MIN_HEIGHT;
}

static lv_coord_t display_home_header_height(void)
{
    return display_home_use_landscape_layout() ? DISPLAY_HOME_LANDSCAPE_HEADER_HEIGHT
                                               : DISPLAY_HOME_PORTRAIT_HEADER_HEIGHT;
}

static lv_obj_t *display_create_home_signal_bar(lv_obj_t *parent,
                                                         lv_coord_t x,
                                                         lv_coord_t y,
                                                         lv_coord_t height)
{
    lv_obj_t *bar = lv_obj_create(parent);

    lv_obj_remove_style_all(bar);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, 5, height);
    lv_obj_set_style_radius(bar, 2, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x20BF7A), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    return bar;
}

static lv_obj_t *display_create_home_wifi_x_line(lv_obj_t *parent,
                                                          const lv_point_t *points,
                                                          lv_coord_t x,
                                                          lv_coord_t y)
{
    lv_obj_t *line = lv_line_create(parent);

    lv_line_set_points(line, points, 2);
    lv_obj_set_pos(line, x, y);
    lv_obj_set_style_line_width(line, 2, 0);
    lv_obj_set_style_line_color(line, lv_color_hex(0xF6494C), 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);

    return line;
}

static void display_update_home_clock(void)
{
    char time_text[9] = "--:--:--";
    time_t now = 0;
    struct tm local_tm = {0};
    static bool timezone_configured = false;

    if (s_home_time_label == NULL) {
        return;
    }

    if (!timezone_configured) {
        setenv("TZ", "CST-8", 1);
        tzset();
        timezone_configured = true;
    }

    time(&now);
    if (now == s_home_clock_last_second) {
        return;
    }
    s_home_clock_last_second = now;

    if (now >= (time_t)DISPLAY_MIN_VALID_UNIX_TIME &&
        localtime_r(&now, &local_tm) != NULL) {
        snprintf(time_text,
                 sizeof(time_text),
                 "%02d:%02d",
                 local_tm.tm_hour,
                 local_tm.tm_min);
    }

    display_text_set(s_home_time_label, time_text);
}

static uint8_t display_home_wifi_level(const display_status_t *status)
{
    return display_wifi_status_level(status);
}

static void display_update_home_wifi_status(const display_status_t *status)
{
    uint8_t level = display_home_wifi_level(status);
    bool connected = level > 0;

    if (s_home_wifi_status_valid &&
        s_home_wifi_connected == connected &&
        s_home_wifi_level == level) {
        return;
    }
    s_home_wifi_status_valid = true;
    s_home_wifi_connected = connected;
    s_home_wifi_level = level;

    for (uint8_t index = 0; index < 3; ++index) {
        if (s_home_wifi_bars[index] == NULL) {
            continue;
        }

        lv_obj_set_style_bg_color(s_home_wifi_bars[index],
                                  (connected && index < level)
                                      ? lv_color_hex(0x20BF7A)
                                      : lv_color_hex(0xBCCAD8),
                                  0);
    }

    for (uint8_t index = 0; index < 2; ++index) {
        if (s_home_wifi_x_lines[index] == NULL) {
            continue;
        }
        if (connected) {
            lv_obj_add_flag(s_home_wifi_x_lines[index], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_home_wifi_x_lines[index], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void display_update_home_status_bar(const display_status_t *status)
{
    display_update_home_clock();
    display_update_home_wifi_status(status);
    display_update_wifi_indicators(status);
}

static void display_create_home_header(lv_obj_t *parent)
{
    lv_obj_t *header = lv_obj_create(parent);
    lv_coord_t header_h = display_home_header_height();
    lv_coord_t wifi_x = 443;
    static const lv_point_t wifi_x_line_a[] = {
        {0, 0},
        {6, 6},
    };
    static const lv_point_t wifi_x_line_b[] = {
        {6, 0},
        {0, 6},
    };

    lv_obj_remove_style_all(header);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_size(header, DISPLAY_DRIVER_WIDTH, header_h);
    lv_obj_set_style_bg_color(header, lv_color_hex(DISPLAY_UI_COLOR_SURFACE_SOFT), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_border_color(header, lv_color_hex(DISPLAY_UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    s_home_time_label = lv_label_create(header);
    lv_obj_set_pos(s_home_time_label, 11, 13);
    lv_obj_set_width(s_home_time_label, 72);
    lv_label_set_long_mode(s_home_time_label, LV_LABEL_LONG_CLIP);
    display_text_set_color(s_home_time_label, lv_color_hex(DISPLAY_UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(s_home_time_label, display_ascii_font(16), 0);
    display_text_set(s_home_time_label, "--:--:--");

    display_create_home_centered_img(header,
                                     &home_text_title_img,
                                     120,
                                     0,
                                     240,
                                     header_h);

    s_home_wifi_bars[0] = display_create_home_signal_bar(header, wifi_x, 26, 8);
    s_home_wifi_bars[1] = display_create_home_signal_bar(header, wifi_x + 9, 20, 14);
    s_home_wifi_bars[2] = display_create_home_signal_bar(header, wifi_x + 18, 14, 20);
    s_home_wifi_x_lines[0] = display_create_home_wifi_x_line(header,
                                                            wifi_x_line_a,
                                                            wifi_x + 25,
                                                            21);
    s_home_wifi_x_lines[1] = display_create_home_wifi_x_line(header,
                                                            wifi_x_line_b,
                                                            wifi_x + 25,
                                                            21);
    display_update_home_status_bar(&s_last_status);
}

static lv_obj_t *display_create_home_page(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_coord_t page_h = display_home_use_landscape_layout()
                            ? DISPLAY_DRIVER_HEIGHT - display_home_header_height()
                            : 212;

    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, DISPLAY_DRIVER_WIDTH, page_h);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    return page;
}

static lv_obj_t *display_create_home_app_tile_at(lv_obj_t *parent,
                                                  lv_coord_t x,
                                                  lv_coord_t y,
                                                  lv_coord_t width,
                                                  lv_coord_t height,
                                                  const lv_img_dsc_t *icon_src,
                                                  const lv_img_dsc_t *title_src,
                                                  const lv_img_dsc_t *subtitle_src,
                                                  lv_color_t accent,
                                                  lv_color_t icon_fill,
                                                  lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_t *accent_line = NULL;
    lv_obj_t *icon_plate = NULL;
    lv_coord_t icon_x = (width - 54) / 2;

    lv_obj_remove_style_all(btn);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, width, height);
    lv_obj_set_style_radius(btn, DISPLAY_UI_RADIUS, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(DISPLAY_UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(DISPLAY_UI_COLOR_SURFACE_SOFT), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(DISPLAY_UI_COLOR_BORDER), 0);
    lv_obj_set_style_border_color(btn, accent, LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(btn, 1, LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    if (cb != NULL) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    }

    accent_line = lv_obj_create(btn);
    lv_obj_remove_style_all(accent_line);
    lv_obj_set_pos(accent_line, 10, 8);
    lv_obj_set_size(accent_line, width - 20, 5);
    lv_obj_set_style_radius(accent_line, 3, 0);
    lv_obj_set_style_bg_color(accent_line, accent, 0);
    lv_obj_set_style_bg_opa(accent_line, LV_OPA_COVER, 0);
    lv_obj_clear_flag(accent_line, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    icon_plate = lv_obj_create(btn);
    lv_obj_remove_style_all(icon_plate);
    lv_obj_set_pos(icon_plate, icon_x, 20);
    lv_obj_set_size(icon_plate, 54, 54);
    lv_obj_set_style_radius(icon_plate, 14, 0);
    lv_obj_set_style_bg_color(icon_plate, icon_fill, 0);
    lv_obj_set_style_bg_opa(icon_plate, LV_OPA_COVER, 0);
    lv_obj_clear_flag(icon_plate, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    display_create_home_centered_img(icon_plate, icon_src, 0, 0, 54, 54);
    display_create_home_centered_img(btn, title_src, 8, 78, width - 16, 26);
    display_create_home_centered_img(btn, subtitle_src, 8, 102, width - 16, 18);

    return btn;
}

static void display_create_home_landscape_pages(lv_obj_t *home_page_1, lv_obj_t *home_page_2)
{
    const lv_coord_t margin_x = display_native_scale_x(DISPLAY_UI_PAGE_MARGIN);
    const lv_coord_t margin_y = display_native_scale_y(DISPLAY_UI_PAGE_MARGIN);
    const lv_coord_t gap_x = display_native_scale_x(DISPLAY_UI_GAP);
    const lv_coord_t gap_y = display_native_scale_y(DISPLAY_UI_GAP);
    const lv_coord_t top_h = display_native_scale_y(124);
    const lv_coord_t bottom_h = display_native_scale_y(124);
    const lv_coord_t top_w = (DISPLAY_DRIVER_WIDTH - (margin_x * 2) - (gap_x * 2)) / 3;
    const lv_coord_t bottom_w = (DISPLAY_DRIVER_WIDTH - (margin_x * 2) - gap_x) / 2;
    const lv_coord_t bottom_y = margin_y + top_h + gap_y;

    (void)home_page_2;

    display_create_home_app_tile_at(home_page_1,
                                    margin_x,
                                    margin_y,
                                    top_w,
                                    top_h,
                                    &home_icon_view_img,
                                    &home_text_view_img,
                                    &home_text_view_desc_img,
                                    lv_color_hex(0xF6494C),
                                    lv_color_hex(0xFBE7E7),
                                    display_home_view_btn_cb);
    display_create_home_app_tile_at(home_page_1,
                                    margin_x + top_w + gap_x,
                                    margin_y,
                                    top_w,
                                    top_h,
                                    &home_icon_call_img,
                                    &home_text_call_img,
                                    &home_text_call_desc_img,
                                    lv_color_hex(0x1296DB),
                                    lv_color_hex(0xE5F3FD),
                                    display_home_call_btn_cb);
    display_create_home_app_tile_at(home_page_1,
                                    margin_x + ((top_w + gap_x) * 2),
                                    margin_y,
                                    top_w,
                                    top_h,
                                    &home_icon_wechat_img,
                                    &home_text_wechat_img,
                                    &home_text_wechat_desc_img,
                                    lv_color_hex(0x24DB5A),
                                    lv_color_hex(0xE9FFDF),
                                    display_home_wechat_btn_cb);
    display_create_home_app_tile_at(home_page_1,
                                    margin_x,
                                    bottom_y,
                                    bottom_w,
                                    bottom_h,
                                    &home_icon_ai_img,
                                    &home_text_ai_img,
                                    &home_text_ai_desc_img,
                                    lv_color_hex(0x009D9A),
                                    lv_color_hex(0xE6F7F6),
                                    display_home_ai_btn_cb);
    display_create_home_app_tile_at(home_page_1,
                                    margin_x + bottom_w + gap_x,
                                    bottom_y,
                                    bottom_w,
                                    bottom_h,
                                    &home_icon_settings_img,
                                    &home_text_settings_img,
                                    &home_text_settings_desc_img,
                                    lv_color_hex(0x64758A),
                                    lv_color_hex(0xEDF2F7),
                                    display_home_settings_btn_cb);
}

static void display_build_home_page(lv_obj_t *screen)
{
    lv_obj_t *home_content = NULL;
    lv_coord_t header_h = display_home_header_height();
    lv_coord_t content_h = DISPLAY_DRIVER_HEIGHT - header_h;

    s_home_page = lv_obj_create(screen);
    lv_obj_remove_style_all(s_home_page);
    lv_obj_set_size(s_home_page, DISPLAY_DRIVER_WIDTH, DISPLAY_DRIVER_HEIGHT);
    lv_obj_set_style_bg_color(s_home_page, lv_color_hex(DISPLAY_UI_COLOR_PAGE_BG), 0);
    lv_obj_set_style_bg_opa(s_home_page, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_home_page, 0, 0);
    lv_obj_clear_flag(s_home_page, LV_OBJ_FLAG_SCROLLABLE);

    display_create_home_header(s_home_page);

    s_home_carousel = lv_obj_create(s_home_page);
    lv_obj_remove_style_all(s_home_carousel);
    lv_obj_set_pos(s_home_carousel, 0, header_h);
    lv_obj_set_size(s_home_carousel, DISPLAY_DRIVER_WIDTH, content_h);
    lv_obj_set_style_bg_opa(s_home_carousel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_home_carousel, 0, 0);
    lv_obj_clear_flag(s_home_carousel,
                      LV_OBJ_FLAG_SCROLLABLE |
                      LV_OBJ_FLAG_SCROLL_ELASTIC |
                      LV_OBJ_FLAG_SCROLL_MOMENTUM);
    home_content = display_create_home_page(s_home_carousel);
    lv_obj_set_size(home_content, DISPLAY_DRIVER_WIDTH, content_h);
    s_home_pages[0] = home_content;
    s_home_pages[1] = NULL;
    s_home_indicator_dots[0] = NULL;
    s_home_indicator_dots[1] = NULL;
    display_create_home_landscape_pages(home_content, NULL);
    display_build_binding_prompt_overlay(s_home_page);
    display_update_binding_prompt(&s_last_status);
}

static void display_build_system_page(lv_obj_t *screen)
{
    s_system_page = lv_obj_create(screen);
    display_prepare_figma_page(s_system_page);
    lv_obj_add_flag(s_system_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_system_page,
                      LV_OBJ_FLAG_SCROLLABLE |
                      LV_OBJ_FLAG_SCROLL_ELASTIC |
                      LV_OBJ_FLAG_SCROLL_MOMENTUM);

    (void)display_create_system_header(s_system_page);

    display_create_settings_row(s_system_page, 54, "Wi-Fi 设置", display_system_wifi_btn_cb);
    display_create_settings_row(s_system_page, 105, "网络测试", display_system_network_test_btn_cb);
    display_create_settings_row(s_system_page, 156, "TiRTC 配置", display_system_tirtc_config_btn_cb);
    display_create_settings_row(s_system_page, 207, "关于 / OTA", display_system_ota_btn_cb);
    display_update_system_memory(&s_last_status);
}

static void display_build_call_page(lv_obj_t *screen)
{
    lv_obj_t *qr_card = NULL;

    s_call_page = lv_obj_create(screen);
    display_prepare_figma_page(s_call_page);
    lv_obj_add_flag(s_call_page, LV_OBJ_FLAG_HIDDEN);

    (void)display_create_figma_header(s_call_page,
                                      "设备呼叫",
                                      display_call_back_btn_cb,
                                      NULL,
                                      lv_color_hex(0x21C783),
                                      NULL);

    display_create_call_menu_button(s_call_page,
                                    10,
                                    54,
                                    150,
                                    122,
                                    "添加联系人",
                                    NULL,
                                    true,
                                    display_call_add_btn_cb);
    display_create_call_menu_button(s_call_page,
                                    10,
                                    188,
                                    150,
                                    122,
                                    "联系人列表",
                                    NULL,
                                    false,
                                    display_call_list_btn_cb);

    qr_card = display_create_native_box(s_call_page,
                                        170,
                                        54,
                                        300,
                                        256,
                                        lv_color_hex(DISPLAY_UI_COLOR_SURFACE),
                                        lv_color_hex(DISPLAY_UI_COLOR_BORDER),
                                        8);
    display_create_call_qr(qr_card,
                           42,
                           8,
                           DISPLAY_CALL_QR_IMAGE_SIZE);
}

static void display_build_wechat_page(lv_obj_t *screen)
{
    lv_obj_t *qr_card = NULL;

    s_wechat_page = lv_obj_create(screen);
    display_prepare_figma_page(s_wechat_page);
    lv_obj_add_flag(s_wechat_page, LV_OBJ_FLAG_HIDDEN);

    (void)display_create_figma_header(s_wechat_page,
                                      "微信呼叫",
                                      display_call_back_btn_cb,
                                      NULL,
                                      lv_color_hex(0x21C783),
                                      NULL);

    display_create_call_menu_button(s_wechat_page,
                                    10,
                                    54,
                                    150,
                                    122,
                                    "添加联系人",
                                    NULL,
                                    true,
                                    display_wechat_add_btn_cb);
    display_create_call_menu_button(s_wechat_page,
                                    10,
                                    188,
                                    150,
                                    122,
                                    "联系人列表",
                                    NULL,
                                    false,
                                    display_wechat_list_btn_cb);

    qr_card = display_create_native_box(s_wechat_page,
                                        170,
                                        54,
                                        300,
                                        256,
                                        lv_color_hex(DISPLAY_UI_COLOR_SURFACE),
                                        lv_color_hex(DISPLAY_UI_COLOR_BORDER),
                                        8);
    display_create_wechat_qr(qr_card, 34, 12, DISPLAY_CONTACT_QR_IMAGE_SIZE);
}

static void display_build_call_add_page(lv_obj_t *screen)
{
    s_call_add_page = lv_obj_create(screen);
    display_prepare_figma_page(s_call_add_page);
    lv_obj_add_flag(s_call_add_page, LV_OBJ_FLAG_HIDDEN);

    (void)display_create_figma_header(s_call_add_page,
                                      "添加联系人",
                                      display_call_child_back_btn_cb,
                                      NULL,
                                      lv_color_hex(0x21C783),
                                      NULL);

    s_call_add_value_labels[DISPLAY_CALL_ADD_FIELD_DEVICE_ID] =
        display_create_call_add_field_row(s_call_add_page, 58, DISPLAY_CALL_ADD_FIELD_DEVICE_ID);
    display_create_native_button(s_call_add_page,
                                 10,
                                 128,
                                 224,
                                 48,
                                 lv_color_hex(0x21C783),
                                 lv_color_hex(0x21C783),
                                 "扫码添加联系人",
                                 lv_color_hex(0xFFFFFF),
                                 14,
                                 display_call_scan_btn_cb);
    display_create_native_live_button(s_call_add_page,
                                      246,
                                      128,
                                      224,
                                      48,
                                      lv_color_hex(0xE9F5FF),
                                      lv_color_hex(0x2F82D7),
                                      "格式说明",
                                      lv_color_hex(0x2F82D7),
                                      display_call_scan_info_btn_cb);
    display_create_native_button(s_call_add_page,
                                 10,
                                 196,
                                 460,
                                 54,
                                 lv_color_hex(0x21C783),
                                 lv_color_hex(0x21C783),
                                 "确认添加",
                                 lv_color_hex(0xFFFFFF),
                                 16,
                                 display_call_confirm_add_btn_cb);

    display_update_call_add_field_labels();
    display_create_call_scan_info_overlay(s_call_add_page);
}

static void display_build_call_add_edit_page(lv_obj_t *screen)
{
    s_call_add_edit_page = lv_obj_create(screen);
    display_prepare_figma_page(s_call_add_edit_page);
    lv_obj_add_flag(s_call_add_edit_page, LV_OBJ_FLAG_HIDDEN);

    (void)display_create_figma_header(s_call_add_edit_page,
                                      "添加联系人",
                                      display_call_add_edit_back_btn_cb,
                                      "保存",
                                      lv_color_hex(0x20BF7A),
                                      display_call_add_edit_save_btn_cb);

    s_call_add_edit_hint_label = display_create_native_text(s_call_add_edit_page,
                                                            "Device ID",
                                                            10,
                                                            56,
                                                            300,
                                                            lv_color_hex(0x64758A),
                                                            13,
                                                            LV_TEXT_ALIGN_LEFT);
    s_call_add_edit_length_label = display_create_native_text(s_call_add_edit_page,
                                                              "0/12",
                                                              380,
                                                              56,
                                                              90,
                                                              lv_color_hex(0x64758A),
                                                              13,
                                                              LV_TEXT_ALIGN_RIGHT);

    s_call_add_edit_ta = lv_textarea_create(s_call_add_edit_page);
    display_obj_set_native_pos(s_call_add_edit_ta, 10, 80);
    display_obj_set_native_size(s_call_add_edit_ta, 460, 48);
    lv_textarea_set_one_line(s_call_add_edit_ta, true);
    lv_textarea_set_max_length(s_call_add_edit_ta, DISPLAY_CALL_CONTACT_DEVICE_ID_LENGTH);
    lv_textarea_set_placeholder_text(s_call_add_edit_ta, "Device ID");
    lv_obj_set_style_radius(s_call_add_edit_ta, 8, 0);
    lv_obj_set_style_border_width(s_call_add_edit_ta, 1, 0);
    lv_obj_set_style_border_color(s_call_add_edit_ta, lv_color_hex(0xD5E0EB), 0);
    lv_obj_set_style_bg_color(s_call_add_edit_ta, lv_color_hex(0xFFFFFF), 0);
    display_text_set_color(s_call_add_edit_ta, lv_color_hex(0x10243E), 0);
    lv_obj_set_style_text_font(s_call_add_edit_ta, display_ascii_font(12), 0);
    lv_obj_set_style_pad_left(s_call_add_edit_ta, 12, 0);
    lv_obj_set_style_pad_right(s_call_add_edit_ta, 12, 0);
    lv_obj_add_event_cb(s_call_add_edit_ta,
                        display_call_add_edit_textarea_event_cb,
                        LV_EVENT_ALL,
                        NULL);

    s_call_add_edit_status_label = display_create_native_live_text(s_call_add_edit_page,
                                                                   "点击保存生效",
                                                                   10,
                                                                   138,
                                                                   460,
                                                                   lv_color_hex(0x0D8A59),
                                                                   LV_TEXT_ALIGN_LEFT);

    s_call_add_edit_keyboard = lv_keyboard_create(s_call_add_edit_page);
    display_obj_set_native_pos(s_call_add_edit_keyboard, 10, 166);
    display_obj_set_native_size(s_call_add_edit_keyboard, 460, 144);
    lv_obj_set_style_bg_opa(s_call_add_edit_keyboard, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_call_add_edit_keyboard, 0, 0);
    lv_obj_set_style_border_width(s_call_add_edit_keyboard, 0, 0);
    lv_obj_set_style_radius(s_call_add_edit_keyboard, 0, 0);
    lv_obj_set_style_pad_all(s_call_add_edit_keyboard, 0, LV_PART_ITEMS);
    display_text_set_color(s_call_add_edit_keyboard, lv_color_hex(0x10243E), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_call_add_edit_keyboard, lv_color_hex(0xFFFFFF), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_call_add_edit_keyboard,
                              lv_color_hex(0xD7EAFB),
                              LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_radius(s_call_add_edit_keyboard, 5, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(s_call_add_edit_keyboard, 0, LV_PART_ITEMS);
    lv_keyboard_set_map(s_call_add_edit_keyboard,
                        LV_KEYBOARD_MODE_USER_1,
                        (const char **)s_wifi_keyboard_map_lc,
                        s_wifi_keyboard_ctrl_lc_map);
    lv_keyboard_set_map(s_call_add_edit_keyboard,
                        LV_KEYBOARD_MODE_USER_2,
                        (const char **)s_wifi_keyboard_map_uc,
                        s_wifi_keyboard_ctrl_uc_map);
    lv_keyboard_set_map(s_call_add_edit_keyboard,
                        LV_KEYBOARD_MODE_USER_3,
                        (const char **)s_wifi_keyboard_map_spec,
                        s_wifi_keyboard_ctrl_spec_map);
    lv_obj_remove_event_cb(s_call_add_edit_keyboard, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(s_call_add_edit_keyboard,
                        display_keyboard_value_event_cb,
                        LV_EVENT_VALUE_CHANGED,
                        NULL);
    lv_obj_add_event_cb(s_call_add_edit_keyboard,
                        display_keyboard_draw_part_event_cb,
                        LV_EVENT_DRAW_PART_BEGIN,
                        NULL);
    lv_obj_add_flag(s_call_add_edit_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_call_add_edit_keyboard, display_keyboard_event_cb, LV_EVENT_ALL, NULL);
}

static void display_build_call_scan_page(lv_obj_t *screen)
{
    s_call_scan_page = lv_obj_create(screen);
    lv_obj_remove_style_all(s_call_scan_page);
    lv_obj_set_pos(s_call_scan_page, 0, 0);
    lv_obj_set_size(s_call_scan_page, DISPLAY_DRIVER_WIDTH, DISPLAY_DRIVER_HEIGHT);
    lv_obj_set_style_bg_color(s_call_scan_page, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_call_scan_page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_call_scan_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_call_scan_page, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_call_scan_page, display_call_scan_tap_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_call_scan_page, LV_OBJ_FLAG_HIDDEN);

    s_call_scan_image = lv_img_create(s_call_scan_page);
    lv_obj_set_pos(s_call_scan_image, 0, 0);
    lv_obj_set_size(s_call_scan_image, DISPLAY_NATIVE_WIDTH, DISPLAY_NATIVE_HEIGHT);
    lv_obj_add_flag(s_call_scan_image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_call_scan_image,
                        display_call_scan_tap_cb,
                        LV_EVENT_CLICKED,
                        NULL);
    lv_obj_add_flag(s_call_scan_image, LV_OBJ_FLAG_HIDDEN);
}

static void display_build_call_list_page(lv_obj_t *screen)
{
    lv_obj_t *contact_list = NULL;
    lv_coord_t next_y = 8;

    s_call_list_page = lv_obj_create(screen);
    display_prepare_figma_page(s_call_list_page);
    lv_obj_add_flag(s_call_list_page, LV_OBJ_FLAG_HIDDEN);

    (void)display_create_figma_header(s_call_list_page,
                                      "联系人",
                                      display_call_child_back_btn_cb,
                                      "刷新",
                                      lv_color_hex(0x1879B9),
                                      display_call_list_refresh_btn_cb);

    contact_list = lv_obj_create(s_call_list_page);
    lv_obj_remove_style_all(contact_list);
    display_obj_set_native_pos(contact_list, 0, DISPLAY_UI_HEADER_HEIGHT);
    display_obj_set_native_size(contact_list,
                                DISPLAY_NATIVE_WIDTH,
                                DISPLAY_NATIVE_HEIGHT - DISPLAY_UI_HEADER_HEIGHT);
    lv_obj_set_style_bg_opa(contact_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(contact_list, 0, 0);
    lv_obj_set_style_pad_bottom(contact_list, 8, 0);
    lv_obj_set_scroll_dir(contact_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(contact_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_clear_flag(contact_list,
                      LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM);

    if (s_call_pending_contact_count > 0U) {
        display_create_native_live_text(contact_list,
                                        "待处理申请",
                                        12,
                                        next_y,
                                        456,
                                        lv_color_hex(0x9A6700),
                                        LV_TEXT_ALIGN_LEFT);
        next_y += 24;
        for (uint8_t index = 0; index < s_call_pending_contact_count; ++index) {
            display_create_call_pending_contact_row(contact_list, index, next_y);
            next_y += 68;
        }
    }

    for (uint8_t index = 0; index < s_call_contact_count; ++index) {
        display_create_call_contact_row(contact_list, index, next_y);
        next_y += 72;
    }
    if (s_call_contact_count == 0U && s_call_pending_contact_count == 0U) {
        const char *empty_text = s_last_status.call_contacts_refreshing ?
            "正在刷新云端联系人..." :
            (s_last_status.call_contacts_last_error == ESP_OK ?
             "暂无联系人" : "联系人刷新失败，请点击右上角重试");
        display_create_native_live_text(contact_list,
                                        empty_text,
                                        40,
                                        next_y + 48,
                                        400,
                                        lv_color_hex(0x65768A),
                                        LV_TEXT_ALIGN_CENTER);
    }
}

static void display_build_call_remark_page(lv_obj_t *screen)
{
    s_call_remark_page = lv_obj_create(screen);
    display_prepare_figma_page(s_call_remark_page);
    lv_obj_add_flag(s_call_remark_page, LV_OBJ_FLAG_HIDDEN);

    (void)display_create_figma_header(s_call_remark_page,
                                      "联系人",
                                      display_call_remark_back_btn_cb,
                                      "保存",
                                      lv_color_hex(0x20BF7A),
                                      display_call_remark_save_btn_cb);

    display_create_native_live_text(s_call_remark_page,
                                    "编辑名称",
                                    10,
                                    58,
                                    300,
                                    lv_color_hex(0x64758A),
                                    LV_TEXT_ALIGN_LEFT);
    s_call_remark_length_label = display_create_native_text(s_call_remark_page,
                                                             "0/63",
                                                             370,
                                                             58,
                                                             100,
                                                             lv_color_hex(0x64758A),
                                                             13,
                                                             LV_TEXT_ALIGN_RIGHT);

    s_call_remark_ta = lv_textarea_create(s_call_remark_page);
    display_obj_set_native_pos(s_call_remark_ta, 10, 80);
    display_obj_set_native_size(s_call_remark_ta, 460, 50);
    lv_textarea_set_one_line(s_call_remark_ta, true);
    lv_textarea_set_max_length(s_call_remark_ta, DISPLAY_CALL_CONTACT_REMARK_MAX - 1U);
    lv_textarea_set_placeholder_text(s_call_remark_ta, "Name");
    lv_obj_set_style_radius(s_call_remark_ta, 8, 0);
    lv_obj_set_style_border_width(s_call_remark_ta, 1, 0);
    lv_obj_set_style_border_color(s_call_remark_ta, lv_color_hex(0xD5E0EB), 0);
    lv_obj_set_style_bg_color(s_call_remark_ta, lv_color_hex(0xFFFFFF), 0);
    display_text_set_color(s_call_remark_ta, lv_color_hex(0x10243E), 0);
    lv_obj_set_style_text_font(s_call_remark_ta, display_cjk_font(), 0);
    lv_obj_set_style_pad_left(s_call_remark_ta, 12, 0);
    lv_obj_set_style_pad_right(s_call_remark_ta, 12, 0);
    lv_obj_add_event_cb(s_call_remark_ta,
                        display_call_remark_textarea_event_cb,
                        LV_EVENT_ALL,
                        NULL);

    s_call_remark_status_label = display_create_native_live_text(s_call_remark_page,
                                                                  "保存后同步到云端",
                                                                  10,
                                                                  140,
                                                                  460,
                                                                  lv_color_hex(0x0D8A59),
                                                                  LV_TEXT_ALIGN_LEFT);

    s_call_remark_keyboard = lv_keyboard_create(s_call_remark_page);
    lv_obj_set_align(s_call_remark_keyboard, LV_ALIGN_TOP_LEFT);
    display_obj_set_native_pos(s_call_remark_keyboard, 10, 166);
    display_obj_set_native_size(s_call_remark_keyboard, 460, 144);
    lv_obj_set_style_bg_opa(s_call_remark_keyboard, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_call_remark_keyboard, 0, 0);
    lv_obj_set_style_border_width(s_call_remark_keyboard, 0, 0);
    lv_obj_set_style_radius(s_call_remark_keyboard, 0, 0);
    lv_obj_set_style_pad_all(s_call_remark_keyboard, 0, LV_PART_ITEMS);
    display_text_set_color(s_call_remark_keyboard, lv_color_hex(0x10243E), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_call_remark_keyboard, lv_color_hex(0xFFFFFF), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_call_remark_keyboard,
                              lv_color_hex(0xD7EAFB),
                              LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_radius(s_call_remark_keyboard, 5, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(s_call_remark_keyboard, 0, LV_PART_ITEMS);
    lv_keyboard_set_map(s_call_remark_keyboard,
                        LV_KEYBOARD_MODE_USER_1,
                        (const char **)s_wifi_keyboard_map_lc,
                        s_wifi_keyboard_ctrl_lc_map);
    lv_keyboard_set_map(s_call_remark_keyboard,
                        LV_KEYBOARD_MODE_USER_2,
                        (const char **)s_wifi_keyboard_map_uc,
                        s_wifi_keyboard_ctrl_uc_map);
    lv_keyboard_set_map(s_call_remark_keyboard,
                        LV_KEYBOARD_MODE_USER_3,
                        (const char **)s_wifi_keyboard_map_spec,
                        s_wifi_keyboard_ctrl_spec_map);
    lv_obj_remove_event_cb(s_call_remark_keyboard, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(s_call_remark_keyboard,
                        display_keyboard_value_event_cb,
                        LV_EVENT_VALUE_CHANGED,
                        NULL);
    lv_obj_add_event_cb(s_call_remark_keyboard,
                        display_keyboard_draw_part_event_cb,
                        LV_EVENT_DRAW_PART_BEGIN,
                        NULL);
    lv_obj_add_flag(s_call_remark_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_call_remark_keyboard, display_keyboard_event_cb, LV_EVENT_ALL, NULL);
}

typedef struct {
    const char *title;
    const char *initial_state;
    lv_event_cb_t back_cb;
    lv_event_cb_t hangup_cb;
    lv_event_cb_t volume_cb;
    lv_obj_t **title_label;
    lv_obj_t **state_label;
    lv_obj_t **duration_label;
    lv_obj_t **stats_label;
    lv_obj_t **mic_value_label;
    lv_obj_t **speaker_value_label;
    display_call_video_overlays_t *overlays;
} display_call_video_overlay_config_t;

static void display_style_call_video_overlay(lv_obj_t *overlay)
{
    if (overlay == NULL) {
        return;
    }
    /* The video surface has an 83 ms frame budget at 12 fps. A translucent
     * full-width panel forces per-pixel RGB565 blending on every frame and,
     * together with the LCD transfer, exhausts that budget. Keep the status
     * surfaces opaque so their body is copied directly; only anti-aliased
     * text and rounded edges still need alpha blending. */
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(overlay, 1, 0);
    lv_obj_set_style_border_color(overlay, lv_color_hex(0x435363), 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *display_create_call_video_control_button(
    lv_obj_t *parent,
    lv_coord_t x,
    const char *text,
    lv_event_cb_t cb,
    display_call_volume_action_t action)
{
    lv_obj_t *button = display_create_native_button(parent,
                                                    x,
                                                    6,
                                                    40,
                                                    40,
                                                    lv_color_hex(0x1B2731),
                                                    lv_color_hex(0x536474),
                                                    text,
                                                    lv_color_hex(0xFFFFFF),
                                                    18,
                                                    NULL);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    if (cb != NULL) {
        lv_obj_add_event_cb(button,
                            cb,
                            LV_EVENT_CLICKED,
                            (void *)(uintptr_t)action);
    }
    return button;
}

static void display_create_call_video_overlay(
    lv_obj_t *parent,
    const display_call_video_overlay_config_t *config)
{
    if (parent == NULL || config == NULL) {
        return;
    }

    lv_obj_t *top_overlay = display_create_native_box(
        parent,
        DISPLAY_CALL_VIDEO_TOP_X,
        DISPLAY_CALL_VIDEO_TOP_Y,
        DISPLAY_CALL_VIDEO_TOP_WIDTH,
        DISPLAY_CALL_VIDEO_TOP_HEIGHT,
        lv_color_hex(0x0B1117),
        lv_color_hex(0x435363),
        8);
    display_style_call_video_overlay(top_overlay);

    lv_obj_t *button = display_create_native_button(top_overlay,
                                                    4,
                                                    4,
                                                    40,
                                                    40,
                                                    lv_color_hex(0x1B2731),
                                                    lv_color_hex(0x536474),
                                                    "<",
                                                    lv_color_hex(0xFFFFFF),
                                                    20,
                                                    NULL);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    if (config->back_cb != NULL) {
        lv_obj_add_event_cb(button, config->back_cb, LV_EVENT_PRESSED, NULL);
    }

    /*
     * The title and state change at runtime and can contain device IDs or
     * Chinese status text. Keep them as labels backed by the compiled CJK font
     * instead of binding the object type to its initial static bitmap.
     */
    lv_obj_t *title_label = display_create_native_live_text(top_overlay,
                                                            config->title,
                                                            52,
                                                            3,
                                                            268,
                                                            lv_color_hex(0xFFFFFF),
                                                            LV_TEXT_ALIGN_LEFT);
    lv_obj_t *state_label = display_create_native_live_text(top_overlay,
                                                            config->initial_state,
                                                            52,
                                                            24,
                                                            268,
                                                            lv_color_hex(0x6EDCB0),
                                                            LV_TEXT_ALIGN_LEFT);
    lv_obj_t *stats_label = NULL;
    if (config->stats_label != NULL) {
        stats_label = display_create_native_live_text(top_overlay,
                                                      "--x-- | TX -- | RX --",
                                                      52,
                                                      43,
                                                      268,
                                                      lv_color_hex(0xC3D2DD),
                                                      LV_TEXT_ALIGN_LEFT);
        lv_obj_set_style_text_font(stats_label, display_ascii_font(10), 0);
    }
    lv_obj_t *duration_label = display_create_native_text(top_overlay,
                                                          "00:00",
                                                          354,
                                                          14,
                                                          100,
                                                          lv_color_hex(0xFFFFFF),
                                                          16,
                                                          LV_TEXT_ALIGN_CENTER);

    lv_obj_t *controls_overlay = display_create_native_box(
        parent,
        DISPLAY_CALL_VIDEO_CONTROLS_X,
        DISPLAY_CALL_VIDEO_CONTROLS_Y,
        DISPLAY_CALL_VIDEO_CONTROLS_WIDTH,
        DISPLAY_CALL_VIDEO_CONTROLS_HEIGHT,
        lv_color_hex(0x0B1117),
        lv_color_hex(0x435363),
        8);
    display_style_call_video_overlay(controls_overlay);

    display_create_native_text(controls_overlay,
                               "MIC",
                               8,
                               5,
                               34,
                               lv_color_hex(0xAFC0CE),
                               10,
                               LV_TEXT_ALIGN_CENTER);
    lv_obj_t *mic_value_label = display_create_native_text(controls_overlay,
                                                           "62",
                                                           8,
                                                           25,
                                                           34,
                                                           lv_color_hex(0xFFFFFF),
                                                           12,
                                                           LV_TEXT_ALIGN_CENTER);
    display_create_call_video_control_button(controls_overlay,
                                             48,
                                             "-",
                                             config->volume_cb,
                                             DISPLAY_CALL_VOLUME_MIC_DOWN);
    display_create_call_video_control_button(controls_overlay,
                                             94,
                                             "+",
                                             config->volume_cb,
                                             DISPLAY_CALL_VOLUME_MIC_UP);

    display_create_native_text(controls_overlay,
                               "SPK",
                               154,
                               5,
                               34,
                               lv_color_hex(0xAFC0CE),
                               10,
                               LV_TEXT_ALIGN_CENTER);
    lv_obj_t *speaker_value_label = display_create_native_text(controls_overlay,
                                                               "70",
                                                               154,
                                                               25,
                                                               34,
                                                               lv_color_hex(0xFFFFFF),
                                                               12,
                                                               LV_TEXT_ALIGN_CENTER);
    display_create_call_video_control_button(controls_overlay,
                                             194,
                                             "-",
                                             config->volume_cb,
                                             DISPLAY_CALL_VOLUME_SPEAKER_DOWN);
    display_create_call_video_control_button(controls_overlay,
                                             240,
                                             "+",
                                             config->volume_cb,
                                             DISPLAY_CALL_VOLUME_SPEAKER_UP);

    button = display_create_native_button(parent,
                                          DISPLAY_CALL_VIDEO_HANGUP_X,
                                          DISPLAY_CALL_VIDEO_HANGUP_Y,
                                          DISPLAY_CALL_VIDEO_HANGUP_WIDTH,
                                          DISPLAY_CALL_VIDEO_HANGUP_HEIGHT,
                                          lv_color_hex(0xD94444),
                                          lv_color_hex(0xFF7777),
                                          "挂断",
                                          lv_color_hex(0xFFFFFF),
                                          15,
                                          NULL);
    lv_obj_set_style_radius(button, 8, 0);
    if (config->hangup_cb != NULL) {
        lv_obj_add_event_cb(button, config->hangup_cb, LV_EVENT_PRESSED, NULL);
    }

    if (config->title_label != NULL) {
        *config->title_label = title_label;
    }
    if (config->state_label != NULL) {
        *config->state_label = state_label;
    }
    if (config->duration_label != NULL) {
        *config->duration_label = duration_label;
    }
    if (config->stats_label != NULL) {
        *config->stats_label = stats_label;
    }
    if (config->mic_value_label != NULL) {
        *config->mic_value_label = mic_value_label;
    }
    if (config->speaker_value_label != NULL) {
        *config->speaker_value_label = speaker_value_label;
    }
    if (config->overlays != NULL) {
        config->overlays->top = top_overlay;
        config->overlays->controls = controls_overlay;
        config->overlays->hangup = button;
        config->overlays->hidden = false;
    }
}

static void display_build_call_active_page(lv_obj_t *screen)
{
    lv_obj_t *control_btn = NULL;

    s_call_active_page = lv_obj_create(screen);
    display_prepare_figma_page(s_call_active_page);
    lv_obj_add_flag(s_call_active_page, LV_OBJ_FLAG_HIDDEN);

    s_call_audio_panel = display_create_native_box(s_call_active_page,
                                                   0,
                                                   0,
                                                   DISPLAY_NATIVE_WIDTH,
                                                   DISPLAY_NATIVE_HEIGHT,
                                                   lv_color_hex(DISPLAY_UI_COLOR_PAGE_BG),
                                                   lv_color_hex(DISPLAY_UI_COLOR_PAGE_BG),
                                                   0);
    lv_obj_set_style_border_width(s_call_audio_panel, 0, 0);
    (void)display_create_figma_header(s_call_audio_panel,
                                      "通话",
                                      display_call_child_back_btn_cb,
                                      NULL,
                                      lv_color_hex(0x21C783),
                                      NULL);

    display_create_native_text(s_call_audio_panel,
                               "音频通话中",
                               40,
                               58,
                               400,
                               lv_color_hex(DISPLAY_UI_COLOR_TEXT),
                               20,
                               LV_TEXT_ALIGN_CENTER);
    s_call_audio_peer_label = display_create_native_live_text(
        s_call_audio_panel,
        "设备 --",
        40,
        91,
        400,
        lv_color_hex(DISPLAY_UI_COLOR_TEXT_MUTED),
        LV_TEXT_ALIGN_CENTER);
    s_call_audio_state_label = display_create_native_live_text(
        s_call_audio_panel,
        "正在呼叫",
        40,
        117,
        400,
        lv_color_hex(DISPLAY_UI_COLOR_BLUE),
        LV_TEXT_ALIGN_CENTER);
    s_call_duration_label = display_create_native_text(s_call_audio_panel,
                                                       "00:00",
                                                       40,
                                                       139,
                                                       400,
                                                       lv_color_hex(DISPLAY_UI_COLOR_GREEN),
                                                       28,
                                                       LV_TEXT_ALIGN_CENTER);
    display_create_call_native_volume_card(s_call_audio_panel,
                                           10,
                                           174,
                                           "麦克风",
                                           "62",
                                           DISPLAY_CALL_VOLUME_MIC_DOWN,
                                           DISPLAY_CALL_VOLUME_MIC_UP,
                                           &s_call_mic_value_label);
    display_create_call_native_volume_card(s_call_audio_panel,
                                           250,
                                           174,
                                           "扬声器",
                                           "70",
                                           DISPLAY_CALL_VOLUME_SPEAKER_DOWN,
                                           DISPLAY_CALL_VOLUME_SPEAKER_UP,
                                           &s_call_speaker_value_label);
    control_btn = display_create_native_button(s_call_audio_panel,
                                               100,
                                               254,
                                               280,
                                               54,
                                               lv_color_hex(0xFFE7E7),
                                               lv_color_hex(0xF15A5A),
                                               "挂断",
                                               lv_color_hex(0xE44747),
                                               18,
                                               NULL);
    lv_obj_add_event_cb(control_btn, display_call_hangup_btn_cb, LV_EVENT_PRESSED, NULL);

    s_call_video_panel = display_create_native_box(s_call_active_page,
                                                    0,
                                                    0,
                                                    DISPLAY_CALL_VIDEO_SCREEN_WIDTH,
                                                    DISPLAY_CALL_VIDEO_SCREEN_HEIGHT,
                                                   lv_color_hex(0x05080C),
                                                   lv_color_hex(0x05080C),
                                                   0);
    lv_obj_set_style_border_width(s_call_video_panel, 0, 0);
    lv_obj_add_flag(s_call_video_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_call_video_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_call_video_panel,
                        display_call_video_surface_tap_cb,
                        LV_EVENT_PRESSED,
                        (void *)(uintptr_t)DISPLAY_VIDEO_SURFACE_DEVICE_CALL);
    memset(&s_call_video_image_dsc, 0, sizeof(s_call_video_image_dsc));
    s_call_video_image_dsc.header.always_zero = 0;
    s_call_video_image_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_call_video_image_dsc.header.w = CALL_VIDEO_RENDER_WIDTH;
    s_call_video_image_dsc.header.h = CALL_VIDEO_RENDER_HEIGHT;
    s_call_video_image_dsc.data_size =
        (uint32_t)((size_t)CALL_VIDEO_RENDER_WIDTH *
                   (size_t)CALL_VIDEO_RENDER_HEIGHT * sizeof(uint16_t));
    s_call_video_image = lv_img_create(s_call_video_panel);
    display_obj_set_native_pos(s_call_video_image,
                               DISPLAY_CALL_VIDEO_X,
                               DISPLAY_CALL_VIDEO_Y);
    display_obj_set_native_size(s_call_video_image,
                                CALL_VIDEO_RENDER_WIDTH,
                                CALL_VIDEO_RENDER_HEIGHT);
    lv_obj_clear_flag(s_call_video_image, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_call_video_image, LV_OBJ_FLAG_HIDDEN);

    s_call_video_placeholder_label = display_create_native_live_text(
        s_call_video_panel,
        "正在建立视频...",
        40,
        148,
        400,
        lv_color_hex(0xB8C4CF),
        LV_TEXT_ALIGN_CENTER);

    const display_call_video_overlay_config_t video_overlay = {
        .title = "设备 --",
        .initial_state = "正在连接",
        .back_cb = display_call_child_back_btn_cb,
        .hangup_cb = display_call_hangup_btn_cb,
        .volume_cb = display_call_volume_btn_cb,
        .title_label = &s_call_video_peer_label,
        .state_label = &s_call_video_state_label,
        .duration_label = &s_call_video_duration_label,
        .stats_label = &s_call_video_stats_label,
        .mic_value_label = &s_call_video_mic_value_label,
        .speaker_value_label = &s_call_video_speaker_value_label,
        .overlays = &s_call_video_overlays,
    };
    display_create_call_video_overlay(s_call_video_panel, &video_overlay);

    if (s_call_visible_type == DISPLAY_CALL_TYPE_VIDEO) {
        lv_obj_add_flag(s_call_audio_panel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_call_video_panel, LV_OBJ_FLAG_HIDDEN);
    }
    display_update_call_active_page(&s_last_status);
}

static void display_build_wechat_add_page(lv_obj_t *screen)
{
    s_wechat_add_page = lv_obj_create(screen);
    display_prepare_figma_page(s_wechat_add_page);
    lv_obj_add_flag(s_wechat_add_page, LV_OBJ_FLAG_HIDDEN);

    (void)display_create_figma_header(s_wechat_add_page,
                                      "添加微信联系人",
                                      display_wechat_child_back_btn_cb,
                                      NULL,
                                      lv_color_hex(0x21C783),
                                      NULL);

    s_wechat_add_open_id_label = display_create_wechat_add_field_row(s_wechat_add_page, 42);
    display_create_figma_button(s_wechat_add_page,
                                8,
                                94,
                                148,
                                34,
                                lv_color_hex(0x21C783),
                                lv_color_hex(0x21C783),
                                "扫码添加",
                                lv_color_hex(0xFFFFFF),
                                12,
                                display_wechat_scan_btn_cb);
    display_create_figma_button(s_wechat_add_page,
                                164,
                                94,
                                148,
                                34,
                                lv_color_hex(0xE9F5FF),
                                lv_color_hex(0x2F82D7),
                                "扫码格式",
                                lv_color_hex(0x2F82D7),
                                12,
                                display_wechat_scan_info_btn_cb);
    display_create_figma_button(s_wechat_add_page,
                                8,
                                138,
                                304,
                                34,
                                lv_color_hex(0x21C783),
                                lv_color_hex(0x21C783),
                                "确认添加",
                                lv_color_hex(0xFFFFFF),
                                12,
                                display_wechat_confirm_add_btn_cb);
    display_update_wechat_add_field_label();
    display_create_wechat_scan_info_overlay(s_wechat_add_page);
}

static void display_build_wechat_add_edit_page(lv_obj_t *screen)
{
    s_wechat_add_edit_page = lv_obj_create(screen);
    display_prepare_figma_page(s_wechat_add_edit_page);
    lv_obj_add_flag(s_wechat_add_edit_page, LV_OBJ_FLAG_HIDDEN);

    (void)display_create_figma_header(s_wechat_add_edit_page,
                                      "添加微信联系人",
                                      display_wechat_add_edit_back_btn_cb,
                                      "保存",
                                      lv_color_hex(0x20BF7A),
                                      display_wechat_add_edit_save_btn_cb);

    s_wechat_add_edit_hint_label = display_create_figma_text(s_wechat_add_edit_page,
                                                             "OpenID",
                                                             8,
                                                             36,
                                                             196,
                                                             lv_color_hex(0x64758A),
                                                             12,
                                                             LV_TEXT_ALIGN_LEFT);
    s_wechat_add_edit_length_label = display_create_figma_text(s_wechat_add_edit_page,
                                                               "0/28",
                                                               230,
                                                               36,
                                                               82,
                                                               lv_color_hex(0x64758A),
                                                               12,
                                                               LV_TEXT_ALIGN_RIGHT);

    s_wechat_add_edit_ta = lv_textarea_create(s_wechat_add_edit_page);
    display_obj_set_design_pos(s_wechat_add_edit_ta, 8, DISPLAY_UUID_INPUT_TOP);
    display_obj_set_design_size(s_wechat_add_edit_ta, DISPLAY_UUID_INPUT_WIDTH, DISPLAY_UUID_INPUT_HEIGHT);
    lv_textarea_set_one_line(s_wechat_add_edit_ta, true);
    lv_textarea_set_max_length(s_wechat_add_edit_ta, DISPLAY_WECHAT_OPEN_ID_LENGTH);
    lv_textarea_set_accepted_chars(s_wechat_add_edit_ta, DISPLAY_WECHAT_OPEN_ID_ACCEPTED_CHARS);
    lv_textarea_set_placeholder_text(s_wechat_add_edit_ta, "28位微信Open ID");
    lv_obj_set_style_radius(s_wechat_add_edit_ta, 8, 0);
    lv_obj_set_style_border_width(s_wechat_add_edit_ta, 1, 0);
    lv_obj_set_style_border_color(s_wechat_add_edit_ta, lv_color_hex(0xD5E0EB), 0);
    lv_obj_set_style_bg_color(s_wechat_add_edit_ta, lv_color_hex(0xFFFFFF), 0);
    display_text_set_color(s_wechat_add_edit_ta, lv_color_hex(0x10243E), 0);
    lv_obj_set_style_text_font(s_wechat_add_edit_ta, display_ascii_font(12), 0);
    lv_obj_set_style_pad_left(s_wechat_add_edit_ta, 12, 0);
    lv_obj_set_style_pad_right(s_wechat_add_edit_ta, 12, 0);
    lv_obj_add_event_cb(s_wechat_add_edit_ta,
                        display_wechat_add_edit_textarea_event_cb,
                        LV_EVENT_ALL,
                        NULL);

    s_wechat_add_edit_status_label = display_create_figma_text(s_wechat_add_edit_page,
                                                               "点击保存生效",
                                                               8,
                                                               DISPLAY_UUID_STATUS_TOP,
                                                               DISPLAY_UUID_STATUS_WIDTH,
                                                               lv_color_hex(0x0D8A59),
                                                               12,
                                                               LV_TEXT_ALIGN_LEFT);

    s_wechat_add_edit_keyboard = lv_keyboard_create(s_wechat_add_edit_page);
    lv_obj_set_align(s_wechat_add_edit_keyboard, LV_ALIGN_TOP_LEFT);
    display_obj_set_design_pos(s_wechat_add_edit_keyboard, 8, DISPLAY_UUID_KEYBOARD_TOP);
    display_obj_set_design_size(s_wechat_add_edit_keyboard, DISPLAY_UUID_INPUT_WIDTH, DISPLAY_UUID_KEYBOARD_HEIGHT);
    lv_obj_set_style_bg_opa(s_wechat_add_edit_keyboard, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_wechat_add_edit_keyboard, 0, 0);
    lv_obj_set_style_border_width(s_wechat_add_edit_keyboard, 0, 0);
    lv_obj_set_style_radius(s_wechat_add_edit_keyboard, 0, 0);
    lv_obj_set_style_pad_all(s_wechat_add_edit_keyboard, 0, LV_PART_ITEMS);
    display_text_set_color(s_wechat_add_edit_keyboard, lv_color_hex(0x10243E), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_wechat_add_edit_keyboard, lv_color_hex(0xFFFFFF), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_wechat_add_edit_keyboard,
                              lv_color_hex(0xD7EAFB),
                              LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_radius(s_wechat_add_edit_keyboard, 5, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(s_wechat_add_edit_keyboard, 0, LV_PART_ITEMS);
    lv_keyboard_set_map(s_wechat_add_edit_keyboard,
                        LV_KEYBOARD_MODE_USER_1,
                        (const char **)s_wifi_keyboard_map_lc,
                        s_wifi_keyboard_ctrl_lc_map);
    lv_keyboard_set_map(s_wechat_add_edit_keyboard,
                        LV_KEYBOARD_MODE_USER_2,
                        (const char **)s_wifi_keyboard_map_uc,
                        s_wifi_keyboard_ctrl_uc_map);
    lv_keyboard_set_map(s_wechat_add_edit_keyboard,
                        LV_KEYBOARD_MODE_USER_3,
                        (const char **)s_wifi_keyboard_map_spec,
                        s_wifi_keyboard_ctrl_spec_map);
    lv_obj_remove_event_cb(s_wechat_add_edit_keyboard, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(s_wechat_add_edit_keyboard,
                        display_keyboard_value_event_cb,
                        LV_EVENT_VALUE_CHANGED,
                        NULL);
    lv_obj_add_event_cb(s_wechat_add_edit_keyboard,
                        display_keyboard_draw_part_event_cb,
                        LV_EVENT_DRAW_PART_BEGIN,
                        NULL);
    lv_obj_add_flag(s_wechat_add_edit_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_wechat_add_edit_keyboard, display_keyboard_event_cb, LV_EVENT_ALL, NULL);
}

static void display_build_wechat_list_page(lv_obj_t *screen)
{
    s_wechat_list_page = lv_obj_create(screen);
    display_prepare_figma_page(s_wechat_list_page);
    lv_obj_add_flag(s_wechat_list_page, LV_OBJ_FLAG_HIDDEN);

    (void)display_create_figma_header(s_wechat_list_page,
                                      "微信联系人",
                                      display_wechat_child_back_btn_cb,
                                      NULL,
                                      lv_color_hex(0x21C783),
                                      NULL);

    for (uint8_t index = 0; index < DISPLAY_WECHAT_CONTACT_COUNT; ++index) {
        display_create_wechat_contact_row(s_wechat_list_page, index, 36 + ((lv_coord_t)index * 50));
    }

    s_wechat_empty_label = display_create_ai_text(s_wechat_list_page,
                                                  "No WeChat contacts",
                                                  8,
                                                  104,
                                                  304,
                                                  lv_color_hex(0x65768A),
                                                  LV_TEXT_ALIGN_CENTER);
    display_update_wechat_contact_list(&s_last_status);
}

static void display_build_wechat_remark_page(lv_obj_t *screen)
{
    s_wechat_remark_page = lv_obj_create(screen);
    display_prepare_figma_page(s_wechat_remark_page);
    lv_obj_add_flag(s_wechat_remark_page, LV_OBJ_FLAG_HIDDEN);

    (void)display_create_figma_header(s_wechat_remark_page,
                                      "联系人名称",
                                      display_wechat_remark_back_btn_cb,
                                      "保存",
                                      lv_color_hex(0x20BF7A),
                                      display_wechat_remark_save_btn_cb);
    (void)display_create_figma_text(s_wechat_remark_page,
                                    "联系人名称",
                                    8,
                                    36,
                                    196,
                                    lv_color_hex(0x64758A),
                                    12,
                                    LV_TEXT_ALIGN_LEFT);

    s_wechat_remark_ta = lv_textarea_create(s_wechat_remark_page);
    display_obj_set_design_pos(s_wechat_remark_ta, 8, DISPLAY_UUID_INPUT_TOP);
    display_obj_set_design_size(s_wechat_remark_ta,
                                DISPLAY_UUID_INPUT_WIDTH,
                                DISPLAY_UUID_INPUT_HEIGHT);
    lv_textarea_set_one_line(s_wechat_remark_ta, true);
    lv_textarea_set_max_length(s_wechat_remark_ta, DISPLAY_WECHAT_REMARK_MAX_CHARS);
    lv_textarea_set_placeholder_text(s_wechat_remark_ta, "请输入联系人名称");
    lv_obj_set_style_radius(s_wechat_remark_ta, 8, 0);
    lv_obj_set_style_border_width(s_wechat_remark_ta, 1, 0);
    lv_obj_set_style_border_color(s_wechat_remark_ta, lv_color_hex(0xD5E0EB), 0);
    lv_obj_set_style_bg_color(s_wechat_remark_ta, lv_color_hex(0xFFFFFF), 0);
    display_text_set_color(s_wechat_remark_ta, lv_color_hex(0x10243E), 0);
    lv_obj_set_style_text_font(s_wechat_remark_ta, display_cjk_font(), 0);
    lv_obj_set_style_pad_left(s_wechat_remark_ta, 12, 0);
    lv_obj_set_style_pad_right(s_wechat_remark_ta, 12, 0);
    lv_obj_set_style_pad_top(s_wechat_remark_ta, 2, 0);
    lv_obj_set_style_pad_bottom(s_wechat_remark_ta, 2, 0);
    lv_obj_add_event_cb(s_wechat_remark_ta,
                        display_wechat_remark_textarea_event_cb,
                        LV_EVENT_ALL,
                        NULL);

    s_wechat_remark_status_label = display_create_figma_text(s_wechat_remark_page,
                                                              "点击保存生效",
                                                              8,
                                                              DISPLAY_UUID_STATUS_TOP,
                                                              DISPLAY_UUID_STATUS_WIDTH,
                                                              lv_color_hex(0x0D8A59),
                                                              12,
                                                              LV_TEXT_ALIGN_LEFT);

    s_wechat_remark_keyboard = lv_keyboard_create(s_wechat_remark_page);
    lv_obj_set_align(s_wechat_remark_keyboard, LV_ALIGN_TOP_LEFT);
    display_obj_set_design_pos(s_wechat_remark_keyboard, 8, DISPLAY_UUID_KEYBOARD_TOP);
    display_obj_set_design_size(s_wechat_remark_keyboard,
                                DISPLAY_UUID_INPUT_WIDTH,
                                DISPLAY_UUID_KEYBOARD_HEIGHT);
    lv_obj_set_style_bg_opa(s_wechat_remark_keyboard, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_wechat_remark_keyboard, 0, 0);
    lv_obj_set_style_border_width(s_wechat_remark_keyboard, 0, 0);
    lv_obj_set_style_radius(s_wechat_remark_keyboard, 0, 0);
    lv_obj_set_style_pad_all(s_wechat_remark_keyboard, 0, LV_PART_ITEMS);
    display_text_set_color(s_wechat_remark_keyboard, lv_color_hex(0x10243E), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_wechat_remark_keyboard, lv_color_hex(0xFFFFFF), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_wechat_remark_keyboard,
                              lv_color_hex(0xD7EAFB),
                              LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_radius(s_wechat_remark_keyboard, 5, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(s_wechat_remark_keyboard, 0, LV_PART_ITEMS);
    lv_keyboard_set_map(s_wechat_remark_keyboard,
                        LV_KEYBOARD_MODE_USER_1,
                        (const char **)s_wifi_keyboard_map_lc,
                        s_wifi_keyboard_ctrl_lc_map);
    lv_keyboard_set_map(s_wechat_remark_keyboard,
                        LV_KEYBOARD_MODE_USER_2,
                        (const char **)s_wifi_keyboard_map_uc,
                        s_wifi_keyboard_ctrl_uc_map);
    lv_keyboard_set_map(s_wechat_remark_keyboard,
                        LV_KEYBOARD_MODE_USER_3,
                        (const char **)s_wifi_keyboard_map_spec,
                        s_wifi_keyboard_ctrl_spec_map);
    lv_obj_remove_event_cb(s_wechat_remark_keyboard, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(s_wechat_remark_keyboard,
                        display_keyboard_value_event_cb,
                        LV_EVENT_VALUE_CHANGED,
                        NULL);
    lv_obj_add_event_cb(s_wechat_remark_keyboard,
                        display_keyboard_draw_part_event_cb,
                        LV_EVENT_DRAW_PART_BEGIN,
                        NULL);
    lv_obj_add_flag(s_wechat_remark_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_wechat_remark_keyboard,
                        display_keyboard_event_cb,
                        LV_EVENT_ALL,
                        NULL);
}

static void display_build_wechat_active_page(lv_obj_t *screen)
{
    s_wechat_active_page = lv_obj_create(screen);
    display_prepare_figma_page(s_wechat_active_page);
    lv_obj_set_size(s_wechat_active_page,
                    DISPLAY_CALL_VIDEO_SCREEN_WIDTH,
                    DISPLAY_CALL_VIDEO_SCREEN_HEIGHT);
    lv_obj_add_flag(s_wechat_active_page, LV_OBJ_FLAG_HIDDEN);

    s_wechat_video_panel = display_create_native_box(
        s_wechat_active_page,
        0,
        0,
        DISPLAY_CALL_VIDEO_SCREEN_WIDTH,
        DISPLAY_CALL_VIDEO_SCREEN_HEIGHT,
        lv_color_hex(0x05080C),
        lv_color_hex(0x05080C),
        0);
    lv_obj_set_style_border_width(s_wechat_video_panel, 0, 0);
    lv_obj_add_flag(s_wechat_video_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_wechat_video_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_wechat_video_panel,
                        display_call_video_surface_tap_cb,
                        LV_EVENT_PRESSED,
                        (void *)(uintptr_t)DISPLAY_VIDEO_SURFACE_WECHAT);

    memset(&s_wechat_video_image_dsc, 0, sizeof(s_wechat_video_image_dsc));
    s_wechat_video_image_dsc.header.always_zero = 0;
    s_wechat_video_image_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_wechat_video_image_dsc.header.w = CALL_VIDEO_RENDER_WIDTH;
    s_wechat_video_image_dsc.header.h = CALL_VIDEO_RENDER_HEIGHT;
    s_wechat_video_image_dsc.data_size =
        (uint32_t)((size_t)CALL_VIDEO_RENDER_WIDTH *
                   (size_t)CALL_VIDEO_RENDER_HEIGHT * sizeof(uint16_t));
    s_wechat_video_image = lv_img_create(s_wechat_video_panel);
    display_obj_set_native_pos(s_wechat_video_image,
                               DISPLAY_CALL_VIDEO_X,
                               DISPLAY_CALL_VIDEO_Y);
    display_obj_set_native_size(s_wechat_video_image,
                                CALL_VIDEO_RENDER_WIDTH,
                                CALL_VIDEO_RENDER_HEIGHT);
    lv_obj_clear_flag(s_wechat_video_image,
                      LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_wechat_video_image, LV_OBJ_FLAG_HIDDEN);

    s_wechat_video_placeholder_label = display_create_native_text(
        s_wechat_video_panel,
        CONFIG_APP_WECHAT_VOIP_REMOTE_VIDEO_ENABLE ?
            "WAITING FOR WECHAT VIDEO" : "WECHAT AUDIO CALL",
        40,
        148,
        400,
        lv_color_hex(0xB8C4CF),
        15,
        LV_TEXT_ALIGN_CENTER);

    const display_call_video_overlay_config_t video_overlay = {
        .title = "微信视频",
        .initial_state = "正在呼叫",
        .back_cb = display_wechat_hangup_btn_cb,
        .hangup_cb = display_wechat_hangup_btn_cb,
        .volume_cb = display_wechat_volume_btn_cb,
        .title_label = NULL,
        .state_label = &s_wechat_video_state_label,
        .duration_label = &s_wechat_duration_label,
        .stats_label = NULL,
        .mic_value_label = &s_wechat_mic_value_label,
        .speaker_value_label = &s_wechat_speaker_value_label,
        .overlays = &s_wechat_video_overlays,
    };
    display_create_call_video_overlay(s_wechat_video_panel, &video_overlay);
    display_update_wechat_active_page(&s_last_status);
}

static lv_obj_t *display_create_tirtc_config_field(lv_obj_t *parent,
                                                            lv_coord_t y,
                                                            const char *label,
                                                            const char *value,
                                                            display_tirtc_config_field_t field)
{
    (void)field;

    lv_obj_t *row = display_create_figma_box(parent,
                                             0,
                                             y,
                                             304,
                                             42,
                                             lv_color_hex(0xFFFFFF),
                                             lv_color_hex(0xD5E0EB),
                                             6);

    display_create_figma_text(row,
                              label,
                              10,
                              7,
                              250,
                              lv_color_hex(0x64758A),
                              12,
                              LV_TEXT_ALIGN_LEFT);
    lv_obj_t *value_label = display_create_figma_text(row,
                                                      value,
                                                      10,
                                                      22,
                                                      246,
                                                      lv_color_hex(0x10243E),
                                                      12,
                                                      LV_TEXT_ALIGN_LEFT);
    return value_label;
}

static void display_build_network_test_page(lv_obj_t *screen)
{
    lv_obj_t *summary = NULL;
    lv_obj_t *result = NULL;

    s_network_test_page = lv_obj_create(screen);
    display_prepare_figma_page(s_network_test_page);
    lv_obj_add_flag(s_network_test_page, LV_OBJ_FLAG_HIDDEN);

    (void)display_create_figma_header(s_network_test_page,
                                      "网络测试",
                                      display_system_child_back_btn_cb,
                                      "重测",
                                      lv_color_hex(0x1768B7),
                                      display_network_test_start_btn_cb);

    summary = display_create_figma_box(s_network_test_page,
                                       8,
                                       34,
                                       304,
                                       24,
                                       lv_color_hex(0xE7F1FB),
                                       lv_color_hex(0xD5E0EB),
                                       6);
    s_network_summary_wifi_label = display_create_figma_live_text(summary,
                                                                  "Wi-Fi --",
                                                                  8,
                                                                  4,
                                                                  148,
                                                                  lv_color_hex(0x10243E),
                                                                  LV_TEXT_ALIGN_LEFT);
    s_network_summary_ip_label = display_create_figma_text(summary,
                                                           "IP --",
                                                           116,
                                                           4,
                                                           180,
                                                           lv_color_hex(0x10243E),
                                                           12,
                                                           LV_TEXT_ALIGN_LEFT);

    display_create_check_row(s_network_test_page, 60, "网关", "等待", lv_color_hex(0xFFFFFF), lv_color_hex(0xF59E0B), &s_network_gateway_value_label);
    display_create_check_row(s_network_test_page, 84, "DNS", "等待", lv_color_hex(0xFFFFFF), lv_color_hex(0xF59E0B), &s_network_dns_value_label);
    display_create_check_row(s_network_test_page, 108, "外网", "未测试", lv_color_hex(0xFFFFFF), lv_color_hex(0xF59E0B), &s_network_wan_value_label);
    s_network_service_row =
        display_create_check_row(s_network_test_page, 132, "TiRTC 服务", "--", lv_color_hex(0xFFFFFF), lv_color_hex(0xF59E0B), &s_network_service_value_label);
    display_create_check_row(s_network_test_page, 156, "Jitter", "--", lv_color_hex(0xFFFFFF), lv_color_hex(0x0D8A59), &s_network_jitter_value_label);
    display_create_check_row(s_network_test_page, 180, "丢包", "--", lv_color_hex(0xFFFFFF), lv_color_hex(0x0D8A59), &s_network_loss_value_label);

    result = display_create_figma_box(s_network_test_page,
                                      8,
                                      204,
                                      304,
                                      30,
                                      lv_color_hex(0xFFF2D8),
                                      lv_color_hex(0xF59E0B),
                                      6);
    s_network_result_box = result;
    s_network_result_label = display_create_figma_text(result,
                                                       "基础网络待测",
                                                       10,
                                                       7,
                                                       150,
                                                       lv_color_hex(0xF59E0B),
                                                       12,
                                                       LV_TEXT_ALIGN_LEFT);
    s_network_result_detail_label = display_create_figma_text(result,
                                                              "点击重测",
                                                              174,
                                                              7,
                                                              120,
                                                              lv_color_hex(0x64758A),
                                                              12,
                                                              LV_TEXT_ALIGN_RIGHT);

    display_update_network_test_page(&s_last_status);
}

static void display_build_ai_chat_page(lv_obj_t *screen)
{
    lv_obj_t *avatar_card = NULL;

    s_ai_chat_page = lv_obj_create(screen);
    display_prepare_figma_page(s_ai_chat_page);
    lv_obj_set_style_bg_color(s_ai_chat_page, lv_color_hex(DISPLAY_UI_COLOR_PAGE_BG), 0);
    lv_obj_add_flag(s_ai_chat_page, LV_OBJ_FLAG_HIDDEN);

    (void)display_create_ai_header(s_ai_chat_page,
                                   DISPLAY_AI_APP_TITLE,
                                   display_ai_back_btn_cb,
                                   true,
                                   true);

    avatar_card = display_create_native_box(s_ai_chat_page,
                                            DISPLAY_AI_AVATAR_CARD_X,
                                            DISPLAY_AI_AVATAR_CARD_Y,
                                            DISPLAY_AI_AVATAR_CARD_WIDTH,
                                            DISPLAY_AI_AVATAR_CARD_HEIGHT,
                                            lv_color_hex(DISPLAY_UI_COLOR_SURFACE),
                                            lv_color_hex(DISPLAY_UI_COLOR_BORDER),
                                            8);
    s_ai_avatar_img = lv_img_create(avatar_card);
    if (s_ai_avatar_img != NULL) {
        lv_img_set_src(s_ai_avatar_img,
                       ai_chat_avatar_asset_get(DISPLAY_AI_AVATAR_BUDDY,
                                                AI_CHAT_AVATAR_STATE_RESTING));
        display_obj_set_native_pos(s_ai_avatar_img, DISPLAY_AI_AVATAR_IMG_X, DISPLAY_AI_AVATAR_IMG_Y);
        lv_obj_clear_flag(s_ai_avatar_img, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        s_ai_avatar_last_variant = DISPLAY_AI_AVATAR_BUDDY;
        s_ai_avatar_last_state = AI_CHAT_AVATAR_STATE_RESTING;
    }
    s_ai_avatar_name_label = display_create_ai_static_text(avatar_card,
                                                           DISPLAY_AI_AVATAR_NAME,
                                                           12,
                                                           128,
                                                           108,
                                                           lv_color_hex(DISPLAY_UI_COLOR_TEXT),
                                                           18,
                                                           LV_TEXT_ALIGN_CENTER);
    s_ai_avatar_state_label = display_create_ai_static_text(avatar_card,
                                                            "休息",
                                                            12,
                                                            160,
                                                            108,
                                                            lv_color_hex(DISPLAY_UI_COLOR_TEXT_MUTED),
                                                            14,
                                                            LV_TEXT_ALIGN_CENTER);
    display_create_ai_static_text(avatar_card,
                                  "AI 语音伙伴",
                                  12,
                                  212,
                                  108,
                                  lv_color_hex(DISPLAY_UI_COLOR_TEXT_MUTED),
                                  12,
                                  LV_TEXT_ALIGN_CENTER);

    s_ai_caption_bar = display_create_native_box(s_ai_chat_page,
                                                 DISPLAY_AI_CAPTION_CARD_X,
                                                 DISPLAY_AI_CAPTION_CARD_Y,
                                                 DISPLAY_AI_CAPTION_CARD_WIDTH,
                                                 DISPLAY_AI_CAPTION_CARD_HEIGHT,
                                                 lv_color_hex(DISPLAY_UI_COLOR_SURFACE),
                                                 lv_color_hex(DISPLAY_UI_COLOR_BORDER),
                                                 8);
    display_create_ai_static_text(s_ai_caption_bar,
                                  "对话内容",
                                  16,
                                  16,
                                  286,
                                  lv_color_hex(DISPLAY_UI_COLOR_TEXT),
                                  16,
                                  LV_TEXT_ALIGN_LEFT);
    s_ai_single_caption_label =
        display_create_ai_native_caption_text(s_ai_caption_bar,
                                              "我在，准备好后可以开始新的对话。",
                                              16,
                                              52,
                                              286,
                                              116,
                                              lv_color_hex(DISPLAY_UI_COLOR_TEXT_MUTED));

    s_ai_new_chat_btn = display_create_native_button(s_ai_caption_bar,
                                                     20,
                                                     184,
                                                     278,
                                                     50,
                                                     lv_color_hex(DISPLAY_UI_COLOR_GREEN),
                                                     lv_color_hex(DISPLAY_UI_COLOR_GREEN),
                                                     "",
                                                     lv_color_hex(0xFFFFFF),
                                                     16,
                                                     display_ai_start_new_btn_cb);
    if (s_ai_new_chat_btn != NULL) {
        s_ai_new_chat_btn_label = display_create_ai_static_text(s_ai_new_chat_btn,
                                                                "开始新对话",
                                                                0,
                                                                17,
                                                                278,
                                                                lv_color_hex(0xFFFFFF),
                                                                16,
                                                                LV_TEXT_ALIGN_CENTER);
        if (s_ai_new_chat_btn_label != NULL) {
            lv_obj_clear_flag(s_ai_new_chat_btn_label, LV_OBJ_FLAG_CLICKABLE);
        }
        lv_obj_add_flag(s_ai_new_chat_btn, LV_OBJ_FLAG_HIDDEN);
    }

    display_update_ai_chat_page(&s_last_status);
}

static void display_build_ai_chat_settings_page(lv_obj_t *screen)
{
    lv_obj_t *mic_row = NULL;
    lv_obj_t *speaker_row = NULL;
    lv_obj_t *avatar_row = NULL;
    lv_obj_t *info_panel = NULL;
    lv_obj_t *value_box = NULL;

    s_ai_chat_settings_page = lv_obj_create(screen);
    display_prepare_figma_page(s_ai_chat_settings_page);
    lv_obj_set_style_bg_color(s_ai_chat_settings_page, lv_color_hex(DISPLAY_UI_COLOR_PAGE_BG), 0);
    lv_obj_add_flag(s_ai_chat_settings_page, LV_OBJ_FLAG_HIDDEN);

    (void)display_create_ai_header(s_ai_chat_settings_page,
                                   DISPLAY_AI_SETTINGS_TITLE,
                                   display_ai_settings_back_btn_cb,
                                   false,
                                   false);

    mic_row = display_create_native_box(s_ai_chat_settings_page,
                                        10,
                                        54,
                                        460,
                                        62,
                                        lv_color_hex(DISPLAY_UI_COLOR_SURFACE),
                                        lv_color_hex(DISPLAY_UI_COLOR_BORDER),
                                        8);
    display_create_ai_static_text(mic_row,
                                  "麦克风音量",
                                  18,
                                  21,
                                  180,
                                  lv_color_hex(DISPLAY_UI_COLOR_TEXT),
                                  15,
                                  LV_TEXT_ALIGN_LEFT);
    (void)display_create_ai_setting_button(mic_row,
                                           285,
                                           11,
                                           44,
                                           40,
                                           "-",
                                           DISPLAY_AI_SETTING_MIC_DOWN);
    value_box = display_create_native_box(mic_row,
                                          337,
                                          11,
                                          54,
                                          40,
                                          lv_color_hex(0xE5FAF0),
                                          lv_color_hex(0xE5FAF0),
                                          7);
    s_ai_settings_mic_value_label = display_create_native_text(value_box,
                                                               "80",
                                                               0,
                                                               12,
                                                               54,
                                                               lv_color_hex(DISPLAY_UI_COLOR_GREEN),
                                                               15,
                                                               LV_TEXT_ALIGN_CENTER);
    (void)display_create_ai_setting_button(mic_row,
                                           399,
                                           11,
                                           44,
                                           40,
                                           "+",
                                           DISPLAY_AI_SETTING_MIC_UP);

    speaker_row = display_create_native_box(s_ai_chat_settings_page,
                                            10,
                                            124,
                                            460,
                                            62,
                                            lv_color_hex(DISPLAY_UI_COLOR_SURFACE),
                                            lv_color_hex(DISPLAY_UI_COLOR_BORDER),
                                            8);
    display_create_ai_static_text(speaker_row,
                                  "扬声器音量",
                                  18,
                                  21,
                                  180,
                                  lv_color_hex(DISPLAY_UI_COLOR_TEXT),
                                  15,
                                  LV_TEXT_ALIGN_LEFT);
    (void)display_create_ai_setting_button(speaker_row,
                                           285,
                                           11,
                                           44,
                                           40,
                                           "-",
                                           DISPLAY_AI_SETTING_SPEAKER_DOWN);
    value_box = display_create_native_box(speaker_row,
                                          337,
                                          11,
                                          54,
                                          40,
                                          lv_color_hex(0xE5FAF0),
                                          lv_color_hex(0xE5FAF0),
                                          7);
    s_ai_settings_speaker_value_label = display_create_native_text(value_box,
                                                                   "70",
                                                                   0,
                                                                   12,
                                                                   54,
                                                                   lv_color_hex(DISPLAY_UI_COLOR_GREEN),
                                                                   15,
                                                                   LV_TEXT_ALIGN_CENTER);
    (void)display_create_ai_setting_button(speaker_row,
                                           399,
                                           11,
                                           44,
                                           40,
                                           "+",
                                           DISPLAY_AI_SETTING_SPEAKER_UP);

    avatar_row = display_create_native_box(s_ai_chat_settings_page,
                                           10,
                                           194,
                                           460,
                                           62,
                                           lv_color_hex(DISPLAY_UI_COLOR_SURFACE),
                                           lv_color_hex(DISPLAY_UI_COLOR_BORDER),
                                           8);
    display_create_ai_static_text(avatar_row,
                                  "角色形象",
                                  18,
                                  21,
                                  150,
                                  lv_color_hex(DISPLAY_UI_COLOR_TEXT),
                                  15,
                                  LV_TEXT_ALIGN_LEFT);
    (void)display_create_ai_avatar_choice_button(avatar_row,
                                                 236,
                                                 10,
                                                 DISPLAY_AI_AVATAR_BUDDY,
                                                 DISPLAY_AI_SETTING_AVATAR_BUDDY);
    (void)display_create_ai_avatar_choice_button(avatar_row,
                                                 344,
                                                 10,
                                                 DISPLAY_AI_AVATAR_SPROUT,
                                                 DISPLAY_AI_SETTING_AVATAR_SPROUT);

    info_panel = display_create_native_box(s_ai_chat_settings_page,
                                           10,
                                           264,
                                           460,
                                           40,
                                           lv_color_hex(DISPLAY_UI_COLOR_SURFACE),
                                           lv_color_hex(DISPLAY_UI_COLOR_BORDER),
                                           8);
    display_create_ai_static_text(info_panel,
                                  "默认常听，直接说话；AI 回复时按侧键打断。",
                                  18,
                                  13,
                                  424,
                                  lv_color_hex(DISPLAY_UI_COLOR_TEXT_MUTED),
                                  13,
                                  LV_TEXT_ALIGN_LEFT);

    display_update_ai_chat_settings_page(&s_last_status);
}

static void display_build_tirtc_config_page(lv_obj_t *screen)
{
    lv_obj_t *fields = NULL;

    s_tirtc_config_page = lv_obj_create(screen);
    display_prepare_figma_page(s_tirtc_config_page);
    lv_obj_add_flag(s_tirtc_config_page, LV_OBJ_FLAG_HIDDEN);

    (void)display_create_figma_header(s_tirtc_config_page,
                                      "TiRTC 配置",
                                      display_system_child_back_btn_cb,
                                      NULL,
                                      lv_color_hex(0x000000),
                                      NULL);

    fields = lv_obj_create(s_tirtc_config_page);
    lv_obj_remove_style_all(fields);
    display_obj_set_design_pos(fields, 8, 40);
    display_obj_set_design_size(fields, 304, 154);
    lv_obj_set_style_bg_opa(fields, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(fields, 0, 0);
    lv_obj_set_scroll_dir(fields, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(fields, LV_SCROLLBAR_MODE_OFF);

    s_tirtc_config_value_labels[DISPLAY_TIRTC_CONFIG_FIELD_DEVICE_ID] =
        display_create_tirtc_config_field(fields,
                                          0,
                                          "Device ID",
                                          APP_CONFIG_RTC_DEVICE_ID,
                                          DISPLAY_TIRTC_CONFIG_FIELD_DEVICE_ID);
    s_tirtc_config_value_labels[DISPLAY_TIRTC_CONFIG_FIELD_DEVICE_SECRET] =
        display_create_tirtc_config_field(fields,
                                          46,
                                          "Binding",
                                          "Idle",
                                          DISPLAY_TIRTC_CONFIG_FIELD_DEVICE_SECRET);
    s_tirtc_config_value_labels[DISPLAY_TIRTC_CONFIG_FIELD_TOKEN_SUBJECT] =
        display_create_tirtc_config_field(fields,
                                          92,
                                          "Token Subject",
                                          "Not set",
                                          DISPLAY_TIRTC_CONFIG_FIELD_TOKEN_SUBJECT);
    s_tirtc_config_value_labels[DISPLAY_TIRTC_CONFIG_FIELD_ACCESS_KEY_ID] =
        display_create_tirtc_config_field(fields,
                                          138,
                                          "Token API",
                                          "Service issued",
                                          DISPLAY_TIRTC_CONFIG_FIELD_ACCESS_KEY_ID);
    s_tirtc_config_value_labels[DISPLAY_TIRTC_CONFIG_FIELD_ACCESS_KEY_SECRET] =
        display_create_tirtc_config_field(fields,
                                          184,
                                          "Credential",
                                          "Managed by binding",
                                          DISPLAY_TIRTC_CONFIG_FIELD_ACCESS_KEY_SECRET);

    (void)display_create_figma_button(s_tirtc_config_page,
                                      8,
                                      202,
                                      304,
                                      30,
                                      lv_color_hex(0xFFE7E7),
                                      lv_color_hex(0xF15A5A),
                                      "重置绑定",
                                      lv_color_hex(0xE44747),
                                      14,
                                      display_tirtc_config_scan_btn_cb);

    display_update_tirtc_config_page(&s_last_status);
}

static void display_build_tirtc_config_edit_page(lv_obj_t *screen)
{
    s_tirtc_config_edit_page = lv_obj_create(screen);
    display_prepare_figma_page(s_tirtc_config_edit_page);
    lv_obj_add_flag(s_tirtc_config_edit_page, LV_OBJ_FLAG_HIDDEN);

    (void)display_create_figma_header(s_tirtc_config_edit_page,
                                      "编辑配置",
                                      display_tirtc_config_edit_back_btn_cb,
                                      "保存",
                                      lv_color_hex(0x20BF7A),
                                      display_tirtc_config_edit_save_btn_cb);

    s_tirtc_edit_hint_label = display_create_figma_text(s_tirtc_config_edit_page,
                                                        "Device ID",
                                                        8,
                                                        36,
                                                        196,
                                                        lv_color_hex(0x64758A),
                                                        12,
                                                        LV_TEXT_ALIGN_LEFT);
    s_tirtc_edit_length_label = display_create_figma_text(s_tirtc_config_edit_page,
                                                          "0/127",
                                                          230,
                                                          36,
                                                          82,
                                                          lv_color_hex(0x64758A),
                                                          12,
                                                          LV_TEXT_ALIGN_RIGHT);

    s_tirtc_edit_ta = lv_textarea_create(s_tirtc_config_edit_page);
    display_obj_set_design_pos(s_tirtc_edit_ta, 8, DISPLAY_UUID_INPUT_TOP);
    display_obj_set_design_size(s_tirtc_edit_ta, DISPLAY_UUID_INPUT_WIDTH, DISPLAY_UUID_INPUT_HEIGHT);
    lv_textarea_set_one_line(s_tirtc_edit_ta, true);
    lv_textarea_set_max_length(s_tirtc_edit_ta, DISPLAY_TIRTC_CONFIG_TEXT_MAX - 1U);
    lv_textarea_set_placeholder_text(s_tirtc_edit_ta, "Device ID");
    lv_obj_set_style_radius(s_tirtc_edit_ta, 8, 0);
    lv_obj_set_style_border_width(s_tirtc_edit_ta, 1, 0);
    lv_obj_set_style_border_color(s_tirtc_edit_ta, lv_color_hex(0xD5E0EB), 0);
    lv_obj_set_style_bg_color(s_tirtc_edit_ta, lv_color_hex(0xFFFFFF), 0);
    display_text_set_color(s_tirtc_edit_ta, lv_color_hex(0x10243E), 0);
    lv_obj_set_style_text_font(s_tirtc_edit_ta, display_ascii_font(12), 0);
    lv_obj_set_style_pad_left(s_tirtc_edit_ta, 12, 0);
    lv_obj_set_style_pad_right(s_tirtc_edit_ta, 12, 0);
    lv_obj_add_event_cb(s_tirtc_edit_ta, display_tirtc_edit_textarea_event_cb, LV_EVENT_ALL, NULL);

    s_tirtc_edit_status_label = display_create_figma_text(s_tirtc_config_edit_page,
                                                          "点击保存生效",
                                                          8,
                                                          DISPLAY_UUID_STATUS_TOP,
                                                          DISPLAY_UUID_STATUS_WIDTH,
                                                          lv_color_hex(0x0D8A59),
                                                          12,
                                                          LV_TEXT_ALIGN_LEFT);

    s_tirtc_edit_keyboard = lv_keyboard_create(s_tirtc_config_edit_page);
    display_obj_set_design_pos(s_tirtc_edit_keyboard, 8, DISPLAY_UUID_KEYBOARD_TOP);
    display_obj_set_design_size(s_tirtc_edit_keyboard, DISPLAY_UUID_INPUT_WIDTH, DISPLAY_UUID_KEYBOARD_HEIGHT);
    lv_obj_set_style_bg_opa(s_tirtc_edit_keyboard, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_tirtc_edit_keyboard, 0, 0);
    lv_obj_set_style_border_width(s_tirtc_edit_keyboard, 0, 0);
    lv_obj_set_style_radius(s_tirtc_edit_keyboard, 0, 0);
    lv_obj_set_style_pad_all(s_tirtc_edit_keyboard, 0, LV_PART_ITEMS);
    display_text_set_color(s_tirtc_edit_keyboard, lv_color_hex(0x10243E), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_tirtc_edit_keyboard, lv_color_hex(0xFFFFFF), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_tirtc_edit_keyboard, lv_color_hex(0xD7EAFB), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_radius(s_tirtc_edit_keyboard, 5, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(s_tirtc_edit_keyboard, 0, LV_PART_ITEMS);
    lv_keyboard_set_map(s_tirtc_edit_keyboard, LV_KEYBOARD_MODE_USER_1, (const char **)s_wifi_keyboard_map_lc, s_wifi_keyboard_ctrl_lc_map);
    lv_keyboard_set_map(s_tirtc_edit_keyboard, LV_KEYBOARD_MODE_USER_2, (const char **)s_wifi_keyboard_map_uc, s_wifi_keyboard_ctrl_uc_map);
    lv_keyboard_set_map(s_tirtc_edit_keyboard, LV_KEYBOARD_MODE_USER_3, (const char **)s_wifi_keyboard_map_spec, s_wifi_keyboard_ctrl_spec_map);
    lv_obj_remove_event_cb(s_tirtc_edit_keyboard, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(s_tirtc_edit_keyboard, display_keyboard_value_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_tirtc_edit_keyboard, display_keyboard_draw_part_event_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);
    lv_obj_add_flag(s_tirtc_edit_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_tirtc_edit_keyboard, display_keyboard_event_cb, LV_EVENT_ALL, NULL);
}

static void display_build_ota_page(lv_obj_t *screen)
{
    s_ota_page = lv_obj_create(screen);
    lv_obj_set_size(s_ota_page, DISPLAY_DRIVER_WIDTH, DISPLAY_DRIVER_HEIGHT);
    lv_obj_set_style_bg_opa(s_ota_page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ota_page, 0, 0);
    lv_obj_set_style_pad_all(s_ota_page, 0, 0);
    lv_obj_clear_flag(s_ota_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ota_page, LV_OBJ_FLAG_HIDDEN);

    display_prepare_figma_page(s_ota_page);
    (void)display_create_figma_header(s_ota_page,
                                      "关于 / OTA",
                                      display_system_child_back_btn_cb,
                                      NULL,
                                      lv_color_hex(0x000000),
                                      NULL);

    (void)display_create_info_row(s_ota_page,
                                  40,
                                  "固件版本",
                                  "--",
                                  lv_color_hex(0x10243E),
                                  &s_ota_version_label);
    (void)display_create_info_row(s_ota_page,
                                  82,
                                  "协议版本",
                                  DISPLAY_TIRTC_VERSION_TEXT,
                                  lv_color_hex(0x10243E),
                                  &s_ota_second_value_label);
    s_ota_second_label = lv_obj_get_child(lv_obj_get_parent(s_ota_second_value_label), 0);
    (void)display_create_info_row(s_ota_page,
                                  124,
                                  "OTA 状态",
                                  "可检查",
                                  lv_color_hex(0x0D8A59),
                                  &s_ota_status_label);

    s_ota_start_btn = display_create_figma_button(s_ota_page,
                                                  8,
                                                  166,
                                                  304,
                                                  38,
                                                  lv_color_hex(0x20BF7A),
                                                  lv_color_hex(0x20BF7A),
                                                  "检查更新",
                                                  lv_color_hex(0xFFFFFF),
                                                  14,
                                                  display_ota_start_btn_cb);
    s_ota_start_btn_label = lv_obj_get_child(s_ota_start_btn, 0);

    s_ota_reboot_btn = display_create_figma_button(s_ota_page,
                                                   8,
                                                   166,
                                                   304,
                                                   38,
                                                   lv_color_hex(0x20BF7A),
                                                   lv_color_hex(0x20BF7A),
                                                   "重启生效",
                                                   lv_color_hex(0xFFFFFF),
                                                   14,
                                                   display_ota_reboot_btn_cb);
    s_ota_reboot_btn_label = lv_obj_get_child(s_ota_reboot_btn, 0);

    s_ota_action_panel = display_create_figma_box(s_ota_page,
                                                  8,
                                                  166,
                                                  304,
                                                  58,
                                                  lv_color_hex(0xFFFFFF),
                                                  lv_color_hex(0xD5E0EB),
                                                  6);
    s_ota_progress_title_label = display_create_figma_text(s_ota_action_panel,
                                                           "正在检查更新",
                                                           122,
                                                           20,
                                                           150,
                                                           lv_color_hex(0x10243E),
                                                           12,
                                                           LV_TEXT_ALIGN_LEFT);
    s_ota_progress_percent_label = display_create_figma_text(s_ota_action_panel,
                                                             "0%",
                                                             234,
                                                             7,
                                                             60,
                                                             lv_color_hex(0x64758A),
                                                             12,
                                                             LV_TEXT_ALIGN_RIGHT);
    s_ota_progress_bar = lv_bar_create(s_ota_action_panel);
    display_obj_set_design_pos(s_ota_progress_bar, 10, 28);
    display_obj_set_design_size(s_ota_progress_bar, 284, 8);
    lv_bar_set_range(s_ota_progress_bar, 0, 100);
    lv_obj_set_style_radius(s_ota_progress_bar, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(s_ota_progress_bar, 6, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_ota_progress_bar, lv_color_hex(0xE2EAF1), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ota_progress_bar, lv_color_hex(0x20BF7A), LV_PART_INDICATOR);
    s_ota_progress_hint_label = display_create_figma_text(s_ota_action_panel,
                                                          "升级中请保持供电",
                                                          10,
                                                          38,
                                                          180,
                                                          lv_color_hex(0x64758A),
                                                          12,
                                                          LV_TEXT_ALIGN_LEFT);

    display_update_ota_page(&s_last_status);
}

static void display_build_main_page(lv_obj_t *screen)
{
    lv_obj_t *status_card = NULL;
    lv_obj_t *video_card = NULL;

    s_main_page = lv_obj_create(screen);
    lv_obj_set_size(s_main_page, DISPLAY_DRIVER_WIDTH, DISPLAY_DRIVER_HEIGHT);
    lv_obj_set_style_bg_opa(s_main_page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_main_page, 0, 0);
    lv_obj_set_style_pad_all(s_main_page, 0, 0);
    lv_obj_clear_flag(s_main_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_main_page, LV_OBJ_FLAG_HIDDEN);

    display_prepare_figma_page(s_main_page);
    (void)display_create_figma_header(s_main_page,
                                      "查看",
                                      display_device_back_btn_cb,
                                      NULL,
                                      lv_color_hex(0x20BF7A),
                                      NULL);

    status_card = display_create_native_box(s_main_page,
                                            10,
                                            54,
                                            210,
                                            58,
                                            lv_color_hex(DISPLAY_UI_COLOR_SURFACE),
                                            lv_color_hex(DISPLAY_UI_COLOR_BORDER),
                                            8);
    display_create_device_status_row(status_card,
                                     0,
                                     "连接状态",
                                     "未连接",
                                     lv_color_hex(0xBCCAD8),
                                     &s_device_connection_dot,
                                     &s_device_connection_value_label);
    display_create_device_status_row(status_card,
                                     25,
                                     "开门指示",
                                     "未开门",
                                     lv_color_hex(0xF59E0B),
                                     &s_device_door_dot,
                                     &s_device_door_value_label);

    display_create_device_volume_card(s_main_page,
                                      120,
                                      "接收音量",
                                      "0",
                                      DISPLAY_DEVICE_VOLUME_RECEIVE_DOWN,
                                      DISPLAY_DEVICE_VOLUME_RECEIVE_UP,
                                      DISPLAY_DEVICE_VOLUME_RECEIVE_MUTE,
                                      &s_device_receive_volume_label,
                                      &s_device_receive_mute_label);
    display_create_device_volume_card(s_main_page,
                                      216,
                                      "发送音量",
                                      "0",
                                      DISPLAY_DEVICE_VOLUME_SEND_DOWN,
                                      DISPLAY_DEVICE_VOLUME_SEND_UP,
                                      DISPLAY_DEVICE_VOLUME_SEND_MUTE,
                                      &s_device_send_volume_label,
                                      &s_device_send_mute_label);

    video_card = display_create_native_box(s_main_page,
                                           228,
                                           54,
                                           242,
                                           250,
                                           lv_color_hex(DISPLAY_UI_COLOR_SURFACE),
                                           lv_color_hex(DISPLAY_UI_COLOR_BORDER),
                                           8);
    s_device_media_bitrate_label = display_create_native_text(video_card,
                                                              "--",
                                                              8,
                                                              7,
                                                              70,
                                                              lv_color_hex(DISPLAY_UI_COLOR_BLUE),
                                                              12,
                                                              LV_TEXT_ALIGN_LEFT);
    s_device_media_fps_label = display_create_native_text(video_card,
                                                          "--",
                                                          86,
                                                          7,
                                                          56,
                                                          lv_color_hex(DISPLAY_UI_COLOR_GREEN),
                                                          12,
                                                          LV_TEXT_ALIGN_CENTER);
    s_device_media_resolution_label = display_create_native_text(video_card,
                                                                 "--",
                                                                 148,
                                                                 7,
                                                                 86,
                                                                 lv_color_hex(DISPLAY_UI_COLOR_TEXT),
                                                                 12,
                                                                 LV_TEXT_ALIGN_RIGHT);
    s_device_qr_view = video_card;
    lv_obj_clear_flag(s_device_qr_view, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
#if LV_USE_QRCODE
    s_device_qrcode = lv_img_create(s_device_qr_view);
    lv_obj_set_size(s_device_qrcode,
                    display_native_scale_square(DISPLAY_DEVICE_QR_SIZE),
                    display_native_scale_square(DISPLAY_DEVICE_QR_SIZE));
    display_obj_set_native_pos(s_device_qrcode, 22, 30);
    lv_obj_clear_flag(s_device_qrcode, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
#else
    display_create_figma_text(s_device_qr_view,
                              "QR",
                              5,
                              82,
                              146,
                              lv_color_hex(0x10243E),
                              16,
                              LV_TEXT_ALIGN_CENTER);
#endif
    s_uuid_label = display_create_native_text(s_device_qr_view,
                                              "--",
                                              12,
                                              230,
                                              218,
                                              lv_color_hex(DISPLAY_UI_COLOR_TEXT_MUTED),
                                              10,
                                              LV_TEXT_ALIGN_CENTER);

    display_update_main_page(&s_last_status);

}

static void display_build_uuid_edit_page(lv_obj_t *screen)
{
    s_uuid_edit_page = lv_obj_create(screen);
    lv_obj_set_size(s_uuid_edit_page, DISPLAY_DRIVER_WIDTH, DISPLAY_DRIVER_HEIGHT);
    lv_obj_set_style_bg_opa(s_uuid_edit_page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_uuid_edit_page, 0, 0);
    lv_obj_set_style_pad_all(s_uuid_edit_page, 0, 0);
    lv_obj_clear_flag(s_uuid_edit_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_uuid_edit_page, LV_OBJ_FLAG_HIDDEN);

    display_prepare_figma_page(s_uuid_edit_page);
    (void)display_create_figma_header(s_uuid_edit_page,
                                      "Device ID",
                                      display_uuid_back_btn_cb,
                                      "保存",
                                      lv_color_hex(0x20BF7A),
                                      display_uuid_save_btn_cb);

    s_uuid_edit_hint_label = display_create_figma_text(s_uuid_edit_page,
                                                       "A-Z / 0-9 / 符号",
                                                       DISPLAY_UUID_HINT_LEFT,
                                                       DISPLAY_UUID_HINT_TOP,
                                                       DISPLAY_UUID_HINT_WIDTH,
                                                       lv_color_hex(0x64758A),
                                                       12,
                                                       LV_TEXT_ALIGN_LEFT);
    s_uuid_edit_length_label = display_create_figma_text(s_uuid_edit_page,
                                                         "",
                                                         DISPLAY_DESIGN_WIDTH - DISPLAY_UUID_HINT_LEFT - DISPLAY_UUID_LENGTH_WIDTH,
                                                         DISPLAY_UUID_HINT_TOP,
                                                         DISPLAY_UUID_LENGTH_WIDTH,
                                                         lv_color_hex(0x64758A),
                                                         12,
                                                         LV_TEXT_ALIGN_RIGHT);

    s_uuid_ta = lv_textarea_create(s_uuid_edit_page);
    display_obj_set_design_pos(s_uuid_ta, 8, DISPLAY_UUID_INPUT_TOP);
    display_obj_set_design_size(s_uuid_ta, DISPLAY_UUID_INPUT_WIDTH, DISPLAY_UUID_INPUT_HEIGHT);
    lv_textarea_set_one_line(s_uuid_ta, true);
    lv_textarea_set_max_length(s_uuid_ta, DEVICE_UUID_EDIT_MAX_LEN);
    lv_textarea_set_accepted_chars(s_uuid_ta, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
    lv_textarea_set_placeholder_text(s_uuid_ta, "Device ID");
    lv_obj_set_style_radius(s_uuid_ta, 8, 0);
    lv_obj_set_style_border_width(s_uuid_ta, 1, 0);
    lv_obj_set_style_border_color(s_uuid_ta, lv_color_hex(0xD5E0EB), 0);
    lv_obj_set_style_bg_color(s_uuid_ta, lv_color_hex(0xFFFFFF), 0);
    display_text_set_color(s_uuid_ta, lv_color_hex(0x10243E), 0);
    lv_obj_set_style_text_font(s_uuid_ta, display_ascii_font(12), 0);
    lv_obj_set_style_pad_left(s_uuid_ta, 12, 0);
    lv_obj_set_style_pad_right(s_uuid_ta, 12, 0);

    s_uuid_edit_status_label = display_create_figma_text(s_uuid_edit_page,
                                                         "请输入 4-12 位",
                                                         8,
                                                         DISPLAY_UUID_STATUS_TOP,
                                                         DISPLAY_UUID_STATUS_WIDTH,
                                                         lv_color_hex(0x64758A),
                                                         12,
                                                         LV_TEXT_ALIGN_LEFT);

    s_uuid_keyboard = lv_btnmatrix_create(s_uuid_edit_page);
    display_obj_set_design_pos(s_uuid_keyboard, 8, DISPLAY_UUID_KEYBOARD_TOP);
    display_obj_set_design_size(s_uuid_keyboard, DISPLAY_UUID_INPUT_WIDTH, DISPLAY_UUID_KEYBOARD_HEIGHT);
    lv_btnmatrix_set_map(s_uuid_keyboard, (const char **)s_uuid_keyboard_map);
    lv_obj_set_style_bg_opa(s_uuid_keyboard, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_uuid_keyboard, 0, 0);
    lv_obj_set_style_pad_all(s_uuid_keyboard, 0, 0);
    lv_obj_set_style_pad_row(s_uuid_keyboard, 4, 0);
    lv_obj_set_style_pad_column(s_uuid_keyboard, 4, 0);
    lv_obj_set_style_radius(s_uuid_keyboard, 6, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_uuid_keyboard, lv_color_hex(0xFFFFFF), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_uuid_keyboard, lv_color_hex(0xD7EAFB), LV_PART_ITEMS | LV_STATE_PRESSED);
    display_text_set_color(s_uuid_keyboard, lv_color_hex(0x10243E), LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(s_uuid_keyboard, 0, LV_PART_ITEMS);
    lv_obj_clear_flag(s_uuid_keyboard, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_uuid_keyboard, display_uuid_keyboard_value_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_uuid_keyboard,
                        display_uuid_keyboard_draw_part_event_cb,
                        LV_EVENT_DRAW_PART_BEGIN,
                        NULL);

    display_update_uuid_edit_feedback(NULL, lv_color_hex(0x64758A));
}

static void display_build_wifi_page(lv_obj_t *screen)
{
    lv_obj_t *list_panel = NULL;

    s_wifi_page = lv_obj_create(screen);
    lv_obj_set_size(s_wifi_page, DISPLAY_DRIVER_WIDTH, DISPLAY_DRIVER_HEIGHT);
    lv_obj_set_style_bg_opa(s_wifi_page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_wifi_page, 0, 0);
    lv_obj_set_style_pad_all(s_wifi_page, 0, 0);
    lv_obj_clear_flag(s_wifi_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_wifi_page, LV_OBJ_FLAG_HIDDEN);

    display_prepare_figma_page(s_wifi_page);
    (void)display_create_figma_header(s_wifi_page,
                                      "Wi-Fi 设置",
                                      display_wifi_back_btn_cb,
                                      "刷新",
                                      lv_color_hex(0x1768B7),
                                      display_wifi_refresh_btn_cb);

    s_wifi_connection_state_label = display_create_figma_live_text(s_wifi_page,
                                                                   "未连接 Wi-Fi",
                                                                   8,
                                                                   37,
                                                                   210,
                                                                   lv_color_hex(0x64758A),
                                                                   LV_TEXT_ALIGN_LEFT);
    s_wifi_scan_state_label = display_create_figma_live_text(s_wifi_page,
                                                             "扫描中",
                                                             242,
                                                             37,
                                                             70,
                                                             lv_color_hex(0x1768B7),
                                                             LV_TEXT_ALIGN_RIGHT);
    if (s_wifi_scan_state_label != NULL) {
        lv_obj_add_flag(s_wifi_scan_state_label, LV_OBJ_FLAG_HIDDEN);
    }
    s_wifi_scan_count_label = lv_label_create(s_wifi_page);
    display_obj_set_design_pos(s_wifi_scan_count_label, 242, 37);
    lv_obj_set_width(s_wifi_scan_count_label, display_scale_x(70));
    lv_label_set_long_mode(s_wifi_scan_count_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_wifi_scan_count_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(s_wifi_scan_count_label, display_ascii_font(12), 0);
    display_text_set_color(s_wifi_scan_count_label, lv_color_hex(0x64758A), 0);
    display_text_set(s_wifi_scan_count_label, "0 APs");

    list_panel = display_create_figma_box(s_wifi_page,
                                          8,
                                          58,
                                          304,
                                          174,
                                          lv_color_hex(DISPLAY_UI_COLOR_PAGE_BG),
                                          lv_color_hex(DISPLAY_UI_COLOR_PAGE_BG),
                                          0);
    lv_obj_set_style_border_width(list_panel, 0, 0);

    s_wifi_list = lv_obj_create(list_panel);
    lv_obj_remove_style_all(s_wifi_list);
    lv_obj_set_pos(s_wifi_list, 0, 0);
    display_obj_set_design_size(s_wifi_list, 304, 174);
    lv_obj_set_style_bg_opa(s_wifi_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_wifi_list, 6, 0);
    lv_obj_set_style_pad_row(s_wifi_list, 8, 0);
    lv_obj_set_flex_flow(s_wifi_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_wifi_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(s_wifi_list, LV_DIR_VER);
    lv_obj_clear_flag(s_wifi_list, LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scrollbar_mode(s_wifi_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(s_wifi_list, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(s_wifi_list, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(s_wifi_list, lv_color_hex(DISPLAY_UI_COLOR_BLUE), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_wifi_list, LV_OPA_50, LV_PART_SCROLLBAR);

    for (uint16_t index = 0; index < DISPLAY_WIFI_SCAN_MAX; ++index) {
        lv_obj_t *row = lv_btn_create(s_wifi_list);
        lv_obj_t *label_row = lv_label_create(row);
        lv_obj_t *label_rssi = lv_label_create(row);

        lv_obj_set_width(row, display_scale_x(292));
        display_style_wifi_list_button(row);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(row,
                            display_wifi_ap_select_cb,
                            LV_EVENT_CLICKED,
                            (void *)(uintptr_t)index);
        lv_obj_set_width(label_row, display_scale_x(178));
        lv_label_set_long_mode(label_row, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(label_row, LV_TEXT_ALIGN_LEFT, 0);
        display_text_set_color(label_row, lv_color_hex(0x10243E), 0);
        lv_obj_set_style_text_font(label_row, display_cjk_font(), 0);
        display_text_set(label_row, "");
        lv_obj_align(label_row, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_set_width(label_rssi, display_scale_x(70));
        lv_obj_set_style_text_align(label_rssi, LV_TEXT_ALIGN_RIGHT, 0);
        display_text_set_color(label_rssi, lv_color_hex(0xF59E0B), 0);
        lv_obj_set_style_text_font(label_rssi, display_ascii_font(12), 0);
        display_text_set(label_rssi, "");
        lv_obj_align(label_rssi, LV_ALIGN_RIGHT_MID, 0, 0);

        s_wifi_list_buttons[index] = row;
        s_wifi_list_ssid_labels[index] = label_row;
        s_wifi_list_rssi_labels[index] = label_rssi;
    }

    display_update_wifi_scan_state(&s_last_status);
    display_refresh_wifi_list(&s_last_status);
}

static void display_build_wifi_connect_page(lv_obj_t *screen)
{
    s_wifi_connect_page = lv_obj_create(screen);
    lv_obj_set_size(s_wifi_connect_page, DISPLAY_DRIVER_WIDTH, DISPLAY_DRIVER_HEIGHT);
    lv_obj_set_style_bg_opa(s_wifi_connect_page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_wifi_connect_page, 0, 0);
    lv_obj_set_style_pad_all(s_wifi_connect_page, 0, 0);
    lv_obj_clear_flag(s_wifi_connect_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_wifi_connect_page, LV_OBJ_FLAG_HIDDEN);

    display_prepare_figma_page(s_wifi_connect_page);
    (void)display_create_figma_header(s_wifi_connect_page,
                                      "连接 Wi-Fi",
                                      display_wifi_connect_back_btn_cb,
                                      NULL,
                                      lv_color_hex(0x000000),
                                      NULL);

    s_wifi_connect_hint_label = display_create_figma_live_text(s_wifi_connect_page,
                                                               "输入密码加入",
                                                               DISPLAY_WIFI_CONNECT_HINT_LEFT,
                                                               DISPLAY_WIFI_CONNECT_HINT_TOP,
                                                               DISPLAY_WIFI_CONNECT_HINT_WIDTH,
                                                               lv_color_hex(0x64758A),
                                                               LV_TEXT_ALIGN_LEFT);
    s_wifi_connect_rssi_label = display_create_figma_live_text(s_wifi_connect_page,
                                                               "",
                                                               DISPLAY_DESIGN_WIDTH - DISPLAY_WIFI_CONNECT_HINT_LEFT - DISPLAY_WIFI_CONNECT_RSSI_WIDTH,
                                                               DISPLAY_WIFI_CONNECT_HINT_TOP,
                                                               DISPLAY_WIFI_CONNECT_RSSI_WIDTH,
                                                               lv_color_hex(0xF59E0B),
                                                               LV_TEXT_ALIGN_RIGHT);

    s_password_ta = lv_textarea_create(s_wifi_connect_page);
    display_obj_set_design_pos(s_password_ta, 8, DISPLAY_WIFI_CONNECT_INPUT_TOP);
    lv_obj_set_size(s_password_ta,
                    display_scale_x(DISPLAY_WIFI_CONNECT_INPUT_WIDTH),
                    display_scale_y(DISPLAY_WIFI_CONNECT_INPUT_HEIGHT));
    lv_textarea_set_one_line(s_password_ta, true);
    lv_textarea_set_password_mode(s_password_ta, false);
    lv_textarea_set_placeholder_text(s_password_ta, "Password");
    lv_obj_set_style_radius(s_password_ta, 8, 0);
    lv_obj_set_style_border_width(s_password_ta, 1, 0);
    lv_obj_set_style_border_color(s_password_ta, lv_color_hex(0xD5E0EB), 0);
    lv_obj_set_style_bg_color(s_password_ta, lv_color_hex(0xFFFFFF), 0);
    display_text_set_color(s_password_ta, lv_color_hex(0x10243E), 0);
    lv_obj_set_style_text_font(s_password_ta, display_ascii_font(12), 0);
    lv_obj_set_style_pad_left(s_password_ta, 12, 0);
    lv_obj_set_style_pad_right(s_password_ta, 12, 0);
    lv_obj_add_event_cb(s_password_ta, display_textarea_event_cb, LV_EVENT_ALL, NULL);

    s_wifi_connect_details_label = display_create_figma_live_text(s_wifi_connect_page,
                                                                  "",
                                                                  8,
                                                                  DISPLAY_WIFI_CONNECT_DETAILS_TOP,
                                                                  DISPLAY_WIFI_CONNECT_DETAILS_WIDTH,
                                                                  lv_color_hex(0x64758A),
                                                                  LV_TEXT_ALIGN_LEFT);
    lv_obj_add_flag(s_wifi_connect_details_label, LV_OBJ_FLAG_HIDDEN);

    s_keyboard = lv_btnmatrix_create(s_wifi_connect_page);
    lv_obj_set_style_bg_opa(s_keyboard, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_keyboard, 0, 0);
    lv_obj_set_style_border_width(s_keyboard, 0, 0);
    lv_obj_set_style_radius(s_keyboard, 0, 0);
    lv_obj_set_style_pad_row(s_keyboard, 4, 0);
    lv_obj_set_style_pad_column(s_keyboard, 3, 0);
    lv_obj_set_style_pad_all(s_keyboard, 0, LV_PART_ITEMS);
    display_text_set_color(s_keyboard, lv_color_hex(0x10243E), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_keyboard, lv_color_hex(0xFFFFFF), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_keyboard, lv_color_hex(0xD7EAFB), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_radius(s_keyboard, 5, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(s_keyboard, 0, LV_PART_ITEMS);
    lv_obj_clear_flag(s_keyboard, LV_OBJ_FLAG_SCROLLABLE);
    display_set_wifi_keyboard_mode(LV_KEYBOARD_MODE_USER_1);
    lv_obj_add_event_cb(s_keyboard, display_keyboard_value_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_keyboard, display_keyboard_draw_part_event_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_keyboard, display_keyboard_event_cb, LV_EVENT_ALL, NULL);
    display_layout_wifi_keyboard();
}

static void display_build_ui(void)
{
    lv_obj_t *screen = lv_scr_act();
    display_page_registry_init(&s_page_registry,
                               s_page_entries,
                               sizeof(s_page_entries) / sizeof(s_page_entries[0]));

    lv_obj_set_style_bg_color(screen, lv_color_hex(DISPLAY_UI_COLOR_PAGE_BG), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    display_build_home_page(screen);
    display_build_main_page(screen);

    display_show_home_page();
}

static void display_snapshot_task(void *arg)
{
    (void)arg;

    while (true) {
        display_status_t *ready = NULL;

        if (s_snapshot_scratch_status_ptr != NULL) {
            if (s_snapshot_provider != NULL) {
                s_snapshot_provider(s_snapshot_scratch_status_ptr, s_snapshot_ctx);
            } else {
                memset(s_snapshot_scratch_status_ptr, 0, sizeof(*s_snapshot_scratch_status_ptr));
            }
        }

        if (s_snapshot_mutex != NULL &&
            s_snapshot_scratch_status_ptr != NULL &&
            xSemaphoreTake(s_snapshot_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            ready = s_snapshot_status_ptr;
            s_snapshot_status_ptr = s_snapshot_scratch_status_ptr;
            s_snapshot_scratch_status_ptr = ready;
            s_snapshot_valid = true;
            xSemaphoreGive(s_snapshot_mutex);
        }

        vTaskDelay(pdMS_TO_TICKS(DISPLAY_SNAPSHOT_TASK_PERIOD_MS));
    }
}

static esp_err_t display_start_snapshot_task(void)
{
    if (s_snapshot_task != NULL) {
        return ESP_OK;
    }

    BaseType_t task_ret = xTaskCreateWithCaps(display_snapshot_task,
                                              "display_snapshot",
                                              DISPLAY_SNAPSHOT_TASK_STACK_SIZE,
                                              NULL,
                                              4,
                                              &s_snapshot_task,
                                              APP_TASK_STACK_CAPS_BACKGROUND);
    return task_ret == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

static void display_lvgl_mark_heartbeat(int64_t now_us, bool entering)
{
    taskENTER_CRITICAL(&s_lvgl_watchdog_lock);
    if (entering) {
        s_lvgl_last_refresh_enter_us = now_us;
    } else {
        s_lvgl_last_refresh_exit_us = now_us;
    }
    s_lvgl_last_heartbeat_us = now_us;
    taskEXIT_CRITICAL(&s_lvgl_watchdog_lock);
}

static void display_lvgl_watchdog_snapshot(int64_t *heartbeat_us,
                                           int64_t *enter_us,
                                           int64_t *exit_us)
{
    taskENTER_CRITICAL(&s_lvgl_watchdog_lock);
    if (heartbeat_us != NULL) {
        *heartbeat_us = s_lvgl_last_heartbeat_us;
    }
    if (enter_us != NULL) {
        *enter_us = s_lvgl_last_refresh_enter_us;
    }
    if (exit_us != NULL) {
        *exit_us = s_lvgl_last_refresh_exit_us;
    }
    taskEXIT_CRITICAL(&s_lvgl_watchdog_lock);
}

static const char *display_current_page_name(void)
{
    lv_obj_t *current = display_page_registry_current(&s_page_registry);

    if (current == NULL) {
        return "none";
    }
    for (size_t index = 0; index < sizeof(s_page_entries) / sizeof(s_page_entries[0]); ++index) {
        if (s_page_entries[index].page != NULL &&
            *s_page_entries[index].page == current &&
            s_page_entries[index].name != NULL) {
            return s_page_entries[index].name;
        }
    }
    return "unknown";
}

static void display_get_pending_debug_state(bool *action_pending, bool *capture_pending)
{
    taskENTER_CRITICAL(&s_debug_capture_lock);
    if (action_pending != NULL) {
        *action_pending = s_debug_action_request != NULL;
    }
    if (capture_pending != NULL) {
        *capture_pending = s_debug_capture_request != NULL;
    }
    taskEXIT_CRITICAL(&s_debug_capture_lock);
}

static void display_register_lvgl_task_wdt_once(void)
{
#if CONFIG_ESP_TASK_WDT_EN
    if (!s_lvgl_task_wdt_added) {
        esp_err_t ret = esp_task_wdt_status(NULL);
        if (ret == ESP_OK) {
            s_lvgl_task_wdt_added = true;
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ret = esp_task_wdt_add(NULL);
            if (ret == ESP_OK) {
                s_lvgl_task_wdt_added = true;
                ESP_LOGI(TAG, "LVGL task watchdog registered");
            } else {
                ESP_LOGW(TAG, "LVGL task watchdog register failed: %s", esp_err_to_name(ret));
            }
        } else if (ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "LVGL task watchdog status failed: %s", esp_err_to_name(ret));
        }
    }
    if (s_lvgl_task_wdt_added) {
        (void)esp_task_wdt_reset();
    }
#endif
}

static int64_t display_lvgl_finish_refresh(void)
{
    int64_t now_us = esp_timer_get_time();
    display_lvgl_mark_heartbeat(now_us, false);

    UBaseType_t watermark = uxTaskGetStackHighWaterMark(NULL);
    if ((uint32_t)watermark < DISPLAY_LVGL_STACK_LOW_WATERMARK &&
        now_us - s_lvgl_stack_last_log_us > DISPLAY_LVGL_STACK_LOG_INTERVAL_US) {
        s_lvgl_stack_last_log_us = now_us;
        ESP_LOGW(TAG,
                 "LVGL task stack low: watermark=%u page=%s",
                 (unsigned)watermark,
                 display_current_page_name());
    }

#if CONFIG_ESP_TASK_WDT_EN
    if (s_lvgl_task_wdt_added) {
        (void)esp_task_wdt_reset();
    }
#endif
    return now_us;
}

static void display_ui_watchdog_task(void *arg)
{
    (void)arg;
    int64_t last_log_us = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(DISPLAY_UI_WATCHDOG_PERIOD_MS));

        int64_t now_us = esp_timer_get_time();
        int64_t heartbeat_us = 0;
        int64_t enter_us = 0;
        int64_t exit_us = 0;
        bool action_pending = false;
        bool capture_pending = false;

        display_lvgl_watchdog_snapshot(&heartbeat_us, &enter_us, &exit_us);
        if (heartbeat_us <= 0) {
            continue;
        }

        int64_t age_us = now_us - heartbeat_us;
        if (age_us < DISPLAY_UI_WATCHDOG_STALL_US ||
            now_us - last_log_us < DISPLAY_UI_WATCHDOG_LOG_INTERVAL_US) {
            continue;
        }

        display_get_pending_debug_state(&action_pending, &capture_pending);
        last_log_us = now_us;
        ESP_LOGE(TAG,
                 "display refresh stalled: heartbeat_age=%lldms page=%s last_enter=%lldms last_exit=%lldms pending_action=%d pending_capture=%d internal_free=%u",
                 (long long)(age_us / 1000LL),
                 display_current_page_name(),
                 (long long)((now_us - enter_us) / 1000LL),
                 (long long)((now_us - exit_us) / 1000LL),
                 action_pending ? 1 : 0,
                 capture_pending ? 1 : 0,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
}

static esp_err_t display_start_ui_watchdog_task(void)
{
    if (s_ui_watchdog_task != NULL) {
        return ESP_OK;
    }

    BaseType_t task_ret = xTaskCreateWithCaps(display_ui_watchdog_task,
                                              "ui_watchdog",
                                              DISPLAY_UI_WATCHDOG_TASK_STACK_SIZE,
                                              NULL,
                                              3,
                                              &s_ui_watchdog_task,
                                              APP_TASK_STACK_CAPS_BACKGROUND);
    return task_ret == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

static void display_video_refresh_timer(lv_timer_t *timer)
{
    (void)timer;
    display_update_call_video_frame();
}

static void display_set_video_refresh_enabled(bool enabled)
{
    bool changed = s_video_refresh_enabled != enabled;
    s_video_refresh_enabled = enabled;
    if (s_video_refresh_timer == NULL) {
        return;
    }

    if (!enabled) {
        lv_timer_pause(s_video_refresh_timer);
        return;
    }
    if (changed) {
        lv_timer_resume(s_video_refresh_timer);
        lv_timer_ready(s_video_refresh_timer);
    }
}

static esp_err_t display_start_refresh_timer(void)
{
    if (s_refresh_timer != NULL && s_video_refresh_timer != NULL) {
        return ESP_OK;
    }

    if (s_refresh_timer == NULL) {
        s_refresh_timer = lv_timer_create(display_refresh_timer,
                                          DISPLAY_REFRESH_TASK_PERIOD_MS,
                                          NULL);
    }
    if (s_video_refresh_timer == NULL) {
        s_video_refresh_timer = lv_timer_create(display_video_refresh_timer,
                                                DISPLAY_VIDEO_REFRESH_TASK_PERIOD_MS,
                                                NULL);
        if (s_video_refresh_timer != NULL) {
            display_set_video_refresh_enabled(s_video_refresh_enabled);
        }
    }
    if (s_refresh_timer != NULL && s_video_refresh_timer != NULL) {
        return ESP_OK;
    }
    if (s_refresh_timer != NULL) {
        lv_timer_del(s_refresh_timer);
        s_refresh_timer = NULL;
    }
    if (s_video_refresh_timer != NULL) {
        lv_timer_del(s_video_refresh_timer);
        s_video_refresh_timer = NULL;
    }
    return ESP_ERR_NO_MEM;
}

static void display_read_latest_status(display_status_t *status)
{
    if (status == NULL) {
        return;
    }

    if (s_snapshot_mutex != NULL &&
        xSemaphoreTake(s_snapshot_mutex, 0) == pdTRUE) {
        if (s_snapshot_valid && s_snapshot_status_ptr != NULL) {
            *status = *s_snapshot_status_ptr;
        } else {
            memset(status, 0, sizeof(*status));
        }
        xSemaphoreGive(s_snapshot_mutex);
        return;
    }

    *status = s_last_status;
}

static void display_refresh_timer(lv_timer_t *timer)
{
    int64_t refresh_start_us = esp_timer_get_time();
    display_lvgl_mark_heartbeat(refresh_start_us, true);
    display_register_lvgl_task_wdt_once();
    (void)timer;

    if (display_debug_process_pending_action()) {
        (void)display_lvgl_finish_refresh();
        return;
    }
    if (display_debug_process_pending_capture()) {
        (void)display_lvgl_finish_refresh();
        return;
    }

    display_status_t *status = &s_refresh_status;
    display_status_t *previous_status = &s_refresh_previous_status;
    bool main_page_visible = display_page_is_visible(s_main_page);
    bool system_page_visible = display_page_is_visible(s_system_page);
    bool call_page_visible = display_page_is_visible(s_call_page);
    bool call_list_page_visible = display_page_is_visible(s_call_list_page);
    bool call_active_page_visible = display_page_is_visible(s_call_active_page);
    bool wechat_page_visible = display_page_is_visible(s_wechat_page);
    bool wechat_list_page_visible = display_page_is_visible(s_wechat_list_page);
    bool wechat_active_page_visible = display_page_is_visible(s_wechat_active_page);
    bool network_test_page_visible = display_page_is_visible(s_network_test_page);
    bool tirtc_config_page_visible = display_page_is_visible(s_tirtc_config_page);
    bool ota_page_visible = display_page_is_visible(s_ota_page);
    bool ai_chat_page_visible = display_page_is_visible(s_ai_chat_page);
    bool ai_settings_page_visible = display_page_is_visible(s_ai_chat_settings_page);
    bool wifi_page_visible = display_page_is_visible(s_wifi_page);
    bool wifi_connect_page_visible = display_page_is_visible(s_wifi_connect_page);
    bool wifi_status_changed = false;
    bool wifi_scan_refresh_due = false;
    int64_t now_us = refresh_start_us;
    int64_t status_stage_done_us = refresh_start_us;
    int64_t common_stage_done_us = refresh_start_us;
    int64_t call_stage_done_us = refresh_start_us;
    int64_t page_stage_done_us = refresh_start_us;
    int64_t alert_stage_done_us = refresh_start_us;

    *previous_status = s_last_status;
    display_read_latest_status(status);
    s_last_status = *status;
    if (!display_call_state_keeps_active_page(status->call_state)) {
        s_call_hangup_pending = false;
    }
    if (display_call_state_keeps_active_page(status->call_state) &&
        !call_active_page_visible) {
        display_show_call_active_page();
        call_active_page_visible = true;
        main_page_visible = false;
        call_page_visible = false;
        call_list_page_visible = false;
    }
    if (display_wechat_call_state_opens_active_page(status->wechat_call_state) &&
        !display_wechat_call_state_keeps_active_page(previous_status->wechat_call_state) &&
        !wechat_active_page_visible) {
        display_show_wechat_active_page();
        wechat_active_page_visible = true;
        wechat_page_visible = false;
        wechat_list_page_visible = false;
        ESP_LOGI(TAG,
                 "wechat active page restored: state=%d",
                 (int)status->wechat_call_state);
    }
    if (display_sync_call_contacts_from_status(status)) {
        display_invalidate_call_list_page();
        if (call_list_page_visible) {
            display_show_call_list_page();
        }
    }
    display_update_call_contact_request(status);
    status_stage_done_us = esp_timer_get_time();

    display_update_home_status_bar(status);
    display_update_binding_prompt(status);

    if (main_page_visible) {
        display_update_main_page(status);
    }

    if (call_page_visible) {
        display_update_call_page(status);
    }

    if (system_page_visible &&
        (status->memory_internal_free != previous_status->memory_internal_free ||
         status->memory_internal_largest != previous_status->memory_internal_largest ||
         status->memory_dma_largest != previous_status->memory_dma_largest ||
         status->memory_psram_largest != previous_status->memory_psram_largest ||
         status->memory_health != previous_status->memory_health)) {
        display_update_system_memory(status);
    }
    common_stage_done_us = esp_timer_get_time();

    if (call_active_page_visible) {
        display_update_call_active_page(status);
        if (!display_call_state_keeps_active_page(status->call_state) &&
            refresh_start_us - s_call_active_page_opened_us >
                DISPLAY_CALL_PAGE_TRANSITION_GRACE_US) {
            display_hide_call_hangup_confirm();
            s_call_active_started_us = 0;
            s_call_active_page_opened_us = 0;
            display_show_call_page();
            call_active_page_visible = false;
            call_page_visible = true;
            call_list_page_visible = false;
        }
    }
    call_stage_done_us = esp_timer_get_time();

    if (wechat_page_visible) {
        display_update_wechat_page(status);
    }

    if (wechat_list_page_visible) {
        display_update_wechat_contact_list(status);
    }

    if (wechat_active_page_visible) {
        display_update_wechat_active_page(status);
        if (!display_wechat_call_state_keeps_active_page(status->wechat_call_state) &&
            refresh_start_us - s_wechat_active_page_opened_us >
                DISPLAY_CALL_PAGE_TRANSITION_GRACE_US) {
            s_wechat_active_started_us = 0;
            s_wechat_active_page_opened_us = 0;
            display_show_wechat_page();
            wechat_active_page_visible = false;
            wechat_page_visible = true;
        }
    }

    if (wifi_page_visible || wifi_connect_page_visible || s_wifi_connect_pending || s_last_wifi_scan_request_us > 0) {
        wifi_status_changed = !display_wifi_scan_equals(status, previous_status) ||
                              status->network_connected != previous_status->network_connected ||
                              status->network_connect_failed != previous_status->network_connect_failed ||
                              strcmp(status->network_ssid, previous_status->network_ssid) != 0;

        if (s_last_wifi_scan_request_us > 0) {
            int64_t scan_elapsed_us = now_us - s_last_wifi_scan_request_us;
            wifi_scan_refresh_due = true;
            if ((!status->wifi_scan_in_progress &&
                 scan_elapsed_us > DISPLAY_WIFI_SCAN_REFRESH_GRACE_US) ||
                scan_elapsed_us > DISPLAY_WIFI_SCAN_REFRESH_TIMEOUT_US) {
                s_last_wifi_scan_request_us = 0;
            }
        }

        if (wifi_page_visible && (wifi_status_changed ||
                                  (wifi_scan_refresh_due && !status->wifi_scan_in_progress))) {
            display_update_wifi_scan_state(status);
            display_refresh_wifi_list(status);
        } else if (wifi_page_visible && wifi_scan_refresh_due) {
            display_update_wifi_scan_state(status);
        }

        if (wifi_connect_page_visible || s_wifi_connect_pending) {
            display_update_wifi_connect_feedback(status);
        }
        if (wifi_connect_page_visible && wifi_status_changed) {
            display_update_wifi_connect_details_line(status);
            display_update_wifi_connect_status_line(status);
        }
    }

    if (network_test_page_visible &&
        display_network_test_status_changed(status, previous_status)) {
        display_update_network_test_page(status);
    }
    if (tirtc_config_page_visible) {
        display_update_tirtc_config_page(status);
    }
    if (ota_page_visible || status->ota_running) {
        display_update_ota_page(status);
    }
    if (ai_chat_page_visible) {
        display_update_ai_chat_page(status);
    }
    if (ai_settings_page_visible) {
        display_update_ai_chat_settings_page(status);
    }
    page_stage_done_us = esp_timer_get_time();

    if (status->wechat_incoming_call_pending) {
        display_show_call_alert(true);
    } else if (status->rtc_incoming_call_pending) {
        display_show_call_alert(false);
    } else {
        display_hide_call_alert();
    }
    alert_stage_done_us = esp_timer_get_time();

    int64_t refresh_end_us = display_lvgl_finish_refresh();
    int64_t refresh_elapsed_us = refresh_end_us - refresh_start_us;
    if (refresh_elapsed_us > DISPLAY_REFRESH_SLOW_LOG_US &&
        refresh_start_us - s_refresh_slow_last_log_us > DISPLAY_REFRESH_SLOW_LOG_INTERVAL_US) {
        s_refresh_slow_last_log_us = refresh_start_us;
        ESP_LOGW(TAG,
                 "LVGL refresh slow: elapsed=%lldus stage=status:%lld/common:%lld/call:%lld/pages:%lld/alert:%lld/finish:%lldus "
                 "home=%d main=%d ai=%d wifi=%d ota=%d",
                 (long long)refresh_elapsed_us,
                 (long long)(status_stage_done_us - refresh_start_us),
                 (long long)(common_stage_done_us - status_stage_done_us),
                 (long long)(call_stage_done_us - common_stage_done_us),
                 (long long)(page_stage_done_us - call_stage_done_us),
                 (long long)(alert_stage_done_us - page_stage_done_us),
                 (long long)(refresh_end_us - alert_stage_done_us),
                 display_page_is_visible(s_home_page) ? 1 : 0,
                 main_page_visible ? 1 : 0,
                 ai_chat_page_visible ? 1 : 0,
                 (wifi_page_visible || wifi_connect_page_visible) ? 1 : 0,
                 ota_page_visible ? 1 : 0);
    }
}

esp_err_t display_init(const display_actions_t *actions)
{
    display_driver_handles_t driver_handles = {0};

    if (s_display_initialized) {
        return ESP_OK;
    }

    if (actions != NULL) {
        s_actions = *actions;
    }

    ESP_RETURN_ON_ERROR(display_allocate_status_buffers(), TAG, "display status buffers alloc failed");
    if (s_call_video_composition_pixels == NULL) {
        s_call_video_composition_pixels = app_memory_aligned_calloc_psram(
            DISPLAY_VIDEO_CACHE_LINE_SIZE,
            1U,
            DISPLAY_CALL_VIDEO_FRAME_BYTES,
            MALLOC_CAP_DMA | MALLOC_CAP_CACHE_ALIGNED);
    }
    ESP_RETURN_ON_FALSE(s_call_video_composition_pixels != NULL,
                        ESP_ERR_NO_MEM,
                        TAG,
                        "call video composition buffer alloc failed");
    if (s_call_scan_preview_mutex == NULL) {
        s_call_scan_preview_mutex = xSemaphoreCreateMutexWithCaps(APP_SYNC_CAPS_CONTROL);
    }
    ESP_RETURN_ON_FALSE(s_call_scan_preview_mutex != NULL,
                        ESP_ERR_NO_MEM,
                        TAG,
                        "scan preview mutex alloc failed");
    ESP_RETURN_ON_ERROR(display_driver_init(&driver_handles), TAG, "screen driver init failed");
    s_display = driver_handles.display;
    s_touch_indev = driver_handles.touch_indev;
    if (s_display == NULL || s_touch_indev == NULL) {
        return ESP_FAIL;
    }

    if (!lvgl_port_lock(0)) {
        ESP_LOGW(TAG, "lvgl lock busy during init");
    }
    display_build_ui();
    esp_err_t snapshot_ret = display_start_snapshot_task();
    if (snapshot_ret != ESP_OK) {
        lvgl_port_unlock();
        ESP_LOGE(TAG, "display snapshot task start failed: %s", esp_err_to_name(snapshot_ret));
        return snapshot_ret;
    }
    esp_err_t watchdog_ret = display_start_ui_watchdog_task();
    if (watchdog_ret != ESP_OK) {
        lvgl_port_unlock();
        ESP_LOGE(TAG, "display watchdog task start failed: %s", esp_err_to_name(watchdog_ret));
        return watchdog_ret;
    }
    esp_err_t refresh_ret = display_start_refresh_timer();
    if (refresh_ret != ESP_OK) {
        lvgl_port_unlock();
        ESP_LOGE(TAG, "display refresh timer start failed: %s", esp_err_to_name(refresh_ret));
        return refresh_ret;
    }
    lvgl_port_unlock();

    s_display_initialized = true;
    return ESP_OK;
}

void display_set_snapshot_provider(display_snapshot_cb_t cb, void *ctx)
{
    s_snapshot_provider = cb;
    s_snapshot_ctx = ctx;
}

static lv_obj_t *display_find_tap_target_locked(const lv_point_t *point)
{
    lv_obj_t *target = NULL;

    if (s_display == NULL || point == NULL) {
        return NULL;
    }

    target = lv_indev_search_obj(lv_disp_get_layer_sys(s_display), (lv_point_t *)point);
    if (target == NULL) {
        target = lv_indev_search_obj(lv_disp_get_layer_top(s_display), (lv_point_t *)point);
    }
    if (target == NULL) {
        target = lv_indev_search_obj(lv_disp_get_scr_act(s_display), (lv_point_t *)point);
    }

    return target;
}

static lv_obj_t *display_promote_tap_target_locked(lv_obj_t *target)
{
    while (target != NULL) {
        if (target->spec_attr != NULL && target->spec_attr->event_dsc_cnt > 0) {
            return target;
        }
        target = lv_obj_get_parent(target);
    }

    return NULL;
}

static lv_obj_t *display_find_scroll_target_locked(lv_obj_t *target)
{
    while (target != NULL) {
        if (lv_obj_has_flag(target, LV_OBJ_FLAG_SCROLLABLE)) {
            return target;
        }
        target = lv_obj_get_parent(target);
    }

    return NULL;
}

static uint16_t display_button_matrix_button_at_point_locked(lv_obj_t *button_matrix_obj,
                                                             const lv_point_t *point)
{
    if (button_matrix_obj == NULL || point == NULL) {
        return LV_BTNMATRIX_BTN_NONE;
    }

    lv_obj_update_layout(button_matrix_obj);

    lv_btnmatrix_t *button_matrix = (lv_btnmatrix_t *)button_matrix_obj;
    if (button_matrix->button_areas == NULL) {
        return LV_BTNMATRIX_BTN_NONE;
    }

    lv_area_t matrix_area;
    lv_obj_get_coords(button_matrix_obj, &matrix_area);

    for (uint16_t button_id = 0; button_id < button_matrix->btn_cnt; ++button_id) {
        lv_area_t button_area = button_matrix->button_areas[button_id];
        button_area.x1 += matrix_area.x1;
        button_area.x2 += matrix_area.x1;
        button_area.y1 += matrix_area.y1;
        button_area.y2 += matrix_area.y1;

        if (point->x >= button_area.x1 && point->x <= button_area.x2 &&
            point->y >= button_area.y1 && point->y <= button_area.y2) {
            return button_id;
        }
    }

    return LV_BTNMATRIX_BTN_NONE;
}

static bool display_is_keyboard_object(const lv_obj_t *target)
{
    return target != NULL &&
           (target == s_keyboard ||
            target == s_tirtc_edit_keyboard ||
            target == s_call_add_edit_keyboard ||
            target == s_call_remark_keyboard ||
            target == s_wechat_add_edit_keyboard);
}

static esp_err_t display_dispatch_tap_locked(lv_obj_t *target, const lv_point_t *point)
{
    if (target == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    if (target == s_password_ta ||
        target == s_uuid_ta ||
        target == s_tirtc_edit_ta ||
        target == s_call_add_edit_ta ||
        target == s_call_remark_ta ||
        target == s_wechat_add_edit_ta) {
        return lv_event_send(target, LV_EVENT_FOCUSED, s_touch_indev) == LV_RES_OK ? ESP_OK : ESP_FAIL;
    }

    if (display_is_keyboard_object(target)) {
        uint16_t button_id = display_button_matrix_button_at_point_locked(target, point);
        if (button_id == LV_BTNMATRIX_BTN_NONE) {
            return ESP_ERR_NOT_FOUND;
        }
        lv_btnmatrix_set_selected_btn(target, button_id);
        lv_res_t ret = lv_event_send(target, LV_EVENT_VALUE_CHANGED, s_touch_indev);
        lv_btnmatrix_set_selected_btn(target, LV_BTNMATRIX_BTN_NONE);
        return ret == LV_RES_OK ? ESP_OK : ESP_FAIL;
    }

    /* Match LVGL's physical pointer sequence. Some safety-critical controls,
     * including call hangup, intentionally react on PRESSED rather than
     * CLICKED; emitting only CLICKED made the browser test input disagree with
     * the real touch controller. A handler may delete its target, so validate
     * the object before sending each following event. */
    lv_res_t event_ret = lv_event_send(target, LV_EVENT_PRESSED, s_touch_indev);
    if (!lv_obj_is_valid(target)) {
        return ESP_OK;
    }
    if (event_ret != LV_RES_OK) {
        return ESP_FAIL;
    }
    event_ret = lv_event_send(target, LV_EVENT_RELEASED, s_touch_indev);
    if (!lv_obj_is_valid(target)) {
        return ESP_OK;
    }
    if (event_ret != LV_RES_OK) {
        return ESP_FAIL;
    }
    event_ret = lv_event_send(target, LV_EVENT_CLICKED, s_touch_indev);
    if (!lv_obj_is_valid(target)) {
        return ESP_OK;
    }
    return event_ret == LV_RES_OK ? ESP_OK : ESP_FAIL;
}

static esp_err_t display_debug_tap_in_lvgl(uint16_t x, uint16_t y)
{
    lv_point_t point = {
        .x = (lv_coord_t)x,
        .y = (lv_coord_t)y,
    };
    lv_obj_t *target = display_find_tap_target_locked(&point);
    target = display_promote_tap_target_locked(target);
    s_debug_tap_point = point;
    s_debug_tap_point_valid = true;
    esp_err_t ret = display_dispatch_tap_locked(target, &point);
    s_debug_tap_point_valid = false;
    return ret;
}

static esp_err_t display_debug_scroll_in_lvgl(uint16_t x, uint16_t y, int16_t dx, int16_t dy)
{
    lv_point_t point = {
        .x = (lv_coord_t)x,
        .y = (lv_coord_t)y,
    };
    lv_obj_t *target = display_find_tap_target_locked(&point);

    if (display_page_is_visible(s_home_page) &&
        abs(dx) >= abs(dy) &&
        abs(dx) >= 40) {
        display_home_set_page(dx < 0);
        return ESP_OK;
    }

    lv_obj_t *scroll_target = display_find_scroll_target_locked(target);
    if (scroll_target == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    lv_obj_scroll_by(scroll_target, (lv_coord_t)-dx, (lv_coord_t)-dy, LV_ANIM_OFF);
    lv_obj_update_snap(scroll_target, LV_ANIM_OFF);

    return ESP_OK;
}

static esp_err_t display_capture_bmp_in_lvgl(uint8_t **bmp_data, size_t *bmp_size)
{
    if (bmp_data == NULL || bmp_size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_display_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    *bmp_data = NULL;
    *bmp_size = 0;

    lv_obj_t *screen = lv_scr_act();
    uint32_t snapshot_size = lv_snapshot_buf_size_needed(screen, LV_IMG_CF_TRUE_COLOR);
    if (snapshot_size == 0) {
        return ESP_FAIL;
    }

    uint8_t *snapshot_buf = app_memory_alloc_psram(snapshot_size);
    if (snapshot_buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    lv_img_dsc_t snapshot = {0};
    if (lv_snapshot_take_to_buf(screen,
                                LV_IMG_CF_TRUE_COLOR,
                                &snapshot,
                                snapshot_buf,
                                snapshot_size) != LV_RES_OK) {
        free(snapshot_buf);
        return ESP_FAIL;
    }

    uint32_t width = snapshot.header.w;
    uint32_t height = snapshot.header.h;
    uint32_t row_stride = ((width * 3U) + 3U) & ~3U;
    size_t total_size = 54U + (size_t)row_stride * height;

    uint8_t *buffer = app_memory_alloc_psram(total_size);
    if (buffer == NULL) {
        free(snapshot_buf);
        return ESP_ERR_NO_MEM;
    }

    memset(buffer, 0, total_size);
    buffer[0] = 'B';
    buffer[1] = 'M';
    display_write_u32_le(&buffer[2], (uint32_t)total_size);
    display_write_u32_le(&buffer[10], 54U);
    display_write_u32_le(&buffer[14], 40U);
    display_write_u32_le(&buffer[18], width);
    display_write_u32_le(&buffer[22], height);
    display_write_u16_le(&buffer[26], 1U);
    display_write_u16_le(&buffer[28], 24U);
    display_write_u32_le(&buffer[34], row_stride * height);
    display_write_u32_le(&buffer[38], 2835U);
    display_write_u32_le(&buffer[42], 2835U);

    for (uint32_t y = 0; y < height; ++y) {
        const lv_color_t *src_row = (const lv_color_t *)(snapshot.data + ((size_t)y * width * sizeof(lv_color_t)));
        uint8_t *dst_row = buffer + 54U + ((size_t)(height - 1U - y) * row_stride);

        for (uint32_t x = 0; x < width; ++x) {
            lv_color32_t color32 = {.full = lv_color_to32(src_row[x])};
            dst_row[x * 3U + 0U] = color32.ch.blue;
            dst_row[x * 3U + 1U] = color32.ch.green;
            dst_row[x * 3U + 2U] = color32.ch.red;
        }
    }

    free(snapshot_buf);

    *bmp_data = buffer;
    *bmp_size = total_size;
    return ESP_OK;
}

static bool display_debug_process_pending_capture(void)
{
    display_debug_capture_request_t *request = NULL;
    bool cancelled = false;

    taskENTER_CRITICAL(&s_debug_capture_lock);
    request = s_debug_capture_request;
    s_debug_capture_request = NULL;
    taskEXIT_CRITICAL(&s_debug_capture_lock);

    if (request == NULL) {
        return false;
    }

    request->ret = display_capture_bmp_in_lvgl(request->bmp_data, request->bmp_size);
    taskENTER_CRITICAL(&s_debug_capture_lock);
    cancelled = request->cancelled;
    taskEXIT_CRITICAL(&s_debug_capture_lock);
    if (cancelled) {
        vSemaphoreDeleteWithCaps(request->done);
        free(request);
    } else {
        xSemaphoreGive(request->done);
    }
    return true;
}

static bool display_debug_process_pending_action(void)
{
    display_debug_action_request_t *request = NULL;
    bool cancelled = false;

    taskENTER_CRITICAL(&s_debug_capture_lock);
    request = s_debug_action_request;
    s_debug_action_request = NULL;
    taskEXIT_CRITICAL(&s_debug_capture_lock);

    if (request == NULL) {
        return false;
    }

    switch (request->type) {
    case DISPLAY_DEBUG_ACTION_TAP:
        request->ret = display_debug_tap_in_lvgl(request->x, request->y);
        break;
    case DISPLAY_DEBUG_ACTION_SCROLL:
        request->ret = display_debug_scroll_in_lvgl(request->x, request->y, request->dx, request->dy);
        break;
    default:
        request->ret = ESP_ERR_INVALID_ARG;
        break;
    }

    taskENTER_CRITICAL(&s_debug_capture_lock);
    cancelled = request->cancelled;
    taskEXIT_CRITICAL(&s_debug_capture_lock);
    if (cancelled) {
        vSemaphoreDeleteWithCaps(request->done);
        free(request);
    } else {
        xSemaphoreGive(request->done);
    }
    return true;
}

static esp_err_t display_debug_queue_action(display_debug_action_type_t type,
                                            uint16_t x,
                                            uint16_t y,
                                            int16_t dx,
                                            int16_t dy)
{
    if (!s_display_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (x >= display_driver_width() || y >= display_driver_height()) {
        return ESP_ERR_INVALID_ARG;
    }

    SemaphoreHandle_t done = xSemaphoreCreateBinaryWithCaps(APP_SYNC_CAPS_CONTROL);
    if (done == NULL) {
        return ESP_ERR_NO_MEM;
    }

    display_debug_action_request_t *request = display_calloc_psram(1, sizeof(*request));
    if (request == NULL) {
        vSemaphoreDeleteWithCaps(done);
        return ESP_ERR_NO_MEM;
    }
    request->type = type;
    request->x = x;
    request->y = y;
    request->dx = dx;
    request->dy = dy;
    request->ret = ESP_ERR_TIMEOUT;
    request->done = done;

    bool queued = false;
    taskENTER_CRITICAL(&s_debug_capture_lock);
    if (s_debug_action_request == NULL) {
        s_debug_action_request = request;
        queued = true;
    }
    taskEXIT_CRITICAL(&s_debug_capture_lock);

    if (!queued) {
        vSemaphoreDeleteWithCaps(done);
        free(request);
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(done, pdMS_TO_TICKS(DISPLAY_CAPTURE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        bool free_now = false;
        taskENTER_CRITICAL(&s_debug_capture_lock);
        if (s_debug_action_request == request) {
            s_debug_action_request = NULL;
            free_now = true;
        } else {
            request->cancelled = true;
        }
        taskEXIT_CRITICAL(&s_debug_capture_lock);
        if (free_now) {
            vSemaphoreDeleteWithCaps(done);
            free(request);
        }
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = request->ret;
    vSemaphoreDeleteWithCaps(done);
    free(request);
    return ret;
}

esp_err_t display_capture_bmp(uint8_t **bmp_data, size_t *bmp_size)
{
    if (bmp_data == NULL || bmp_size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_display_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    *bmp_data = NULL;
    *bmp_size = 0;

    SemaphoreHandle_t done = xSemaphoreCreateBinaryWithCaps(APP_SYNC_CAPS_CONTROL);
    if (done == NULL) {
        return ESP_ERR_NO_MEM;
    }

    display_debug_capture_request_t *request = display_calloc_psram(1, sizeof(*request));
    if (request == NULL) {
        vSemaphoreDeleteWithCaps(done);
        return ESP_ERR_NO_MEM;
    }
    request->bmp_data = bmp_data;
    request->bmp_size = bmp_size;
    request->ret = ESP_ERR_TIMEOUT;
    request->done = done;

    bool queued = false;
    taskENTER_CRITICAL(&s_debug_capture_lock);
    if (s_debug_capture_request == NULL) {
        s_debug_capture_request = request;
        queued = true;
    }
    taskEXIT_CRITICAL(&s_debug_capture_lock);

    if (!queued) {
        vSemaphoreDeleteWithCaps(done);
        free(request);
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(done, pdMS_TO_TICKS(DISPLAY_CAPTURE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        bool free_now = false;
        taskENTER_CRITICAL(&s_debug_capture_lock);
        if (s_debug_capture_request == request) {
            s_debug_capture_request = NULL;
            free_now = true;
        } else {
            request->cancelled = true;
        }
        taskEXIT_CRITICAL(&s_debug_capture_lock);
        if (free_now) {
            vSemaphoreDeleteWithCaps(done);
            free(request);
        }
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = request->ret;
    vSemaphoreDeleteWithCaps(done);
    free(request);
    return ret;
}

esp_err_t display_debug_tap(uint16_t x, uint16_t y)
{
    if (!s_display_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (x >= display_driver_width() || y >= display_driver_height()) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lvgl_port_lock(100)) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = display_debug_tap_in_lvgl(x, y);

    lvgl_port_unlock();
    return ret;
}

static void display_open_call_page_async_cb(void *arg)
{
    (void)arg;
    display_show_call_page();
}

static void display_open_home_page_async_cb(void *arg)
{
    (void)arg;
    if (display_page_is_visible(s_ai_chat_page) ||
        display_page_is_visible(s_ai_chat_settings_page)) {
        display_clear_ai_chat_message_view();
    }
    display_show_home_page();
}

static void display_open_device_page_async_cb(void *arg)
{
    (void)arg;
    display_show_main_page();
}

static void display_open_call_active_page_async_cb(void *arg)
{
    (void)arg;
    display_show_call_active_page();
}

static void display_open_wechat_page_async_cb(void *arg)
{
    (void)arg;
    display_show_wechat_page();
}

static void display_open_wechat_active_page_async_cb(void *arg)
{
    (void)arg;
    display_show_wechat_active_page();
}

static void display_open_ai_chat_page_async_cb(void *arg)
{
    (void)arg;
    display_show_ai_chat_page();
}

static void display_open_system_page_async_cb(void *arg)
{
    (void)arg;
    display_show_system_page();
}

static esp_err_t display_open_page_async(lv_async_cb_t callback)
{
    if (!s_display_initialized || callback == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!lvgl_port_lock(100)) {
        return ESP_ERR_TIMEOUT;
    }

    /* lv_async_call mutates LVGL's timer list and must share the LVGL lock. */
    lv_res_t result = lv_async_call(callback, NULL);
    lvgl_port_unlock();
    return result == LV_RES_OK ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t display_open_home_page_async(void)
{
    return display_open_page_async(display_open_home_page_async_cb);
}

esp_err_t display_open_device_page_async(void)
{
    return display_open_page_async(display_open_device_page_async_cb);
}

esp_err_t display_open_call_page_async(void)
{
    return display_open_page_async(display_open_call_page_async_cb);
}

esp_err_t display_open_call_active_page_async(void)
{
    return display_open_page_async(display_open_call_active_page_async_cb);
}

esp_err_t display_open_wechat_page_async(void)
{
    return display_open_page_async(display_open_wechat_page_async_cb);
}

esp_err_t display_open_wechat_active_page_async(void)
{
    return display_open_page_async(display_open_wechat_active_page_async_cb);
}

esp_err_t display_open_ai_chat_page_async(void)
{
    return display_open_page_async(display_open_ai_chat_page_async_cb);
}

esp_err_t display_open_system_page_async(void)
{
    return display_open_page_async(display_open_system_page_async_cb);
}

esp_err_t display_debug_tap_async(uint16_t x, uint16_t y)
{
    return display_debug_queue_action(DISPLAY_DEBUG_ACTION_TAP, x, y, 0, 0);
}

esp_err_t display_debug_scroll_async(uint16_t x, uint16_t y, int16_t dx, int16_t dy)
{
    return display_debug_queue_action(DISPLAY_DEBUG_ACTION_SCROLL, x, y, dx, dy);
}
