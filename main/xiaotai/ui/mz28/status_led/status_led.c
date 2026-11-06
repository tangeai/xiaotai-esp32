#include "status_led.h"

#include <stdatomic.h>
#include <string.h>

#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#define STATUS_LED_MAX_PIXELS 16U
#define STATUS_LED_REFRESH_MS 40U
#define STATUS_LED_RMT_RESOLUTION_HZ 10000000U

typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} rgb_t;

static const char *TAG = "status_led";
static rmt_channel_handle_t s_channel;
static rmt_encoder_handle_t s_encoder;
static uint8_t s_frame[STATUS_LED_MAX_PIXELS * 3U];
static uint8_t s_pixel_count;
static uint8_t s_brightness;
static uint32_t s_last_refresh_ms;
static uint32_t s_last_error_log_ms;
static atomic_int s_state;
static atomic_int s_feedback;
static atomic_llong s_feedback_until_ms;

static uint8_t triangle(uint32_t now_ms, uint32_t period_ms,
                        uint8_t low, uint8_t high)
{
    uint32_t half = period_ms / 2U;
    uint32_t position = now_ms % period_ms;
    uint32_t rising = position < half ? position : period_ms - position;
    return (uint8_t)(low + ((uint32_t)(high - low) * rising) / half);
}

static uint8_t scaled(uint8_t channel, uint8_t level)
{
    return (uint8_t)(((uint32_t)channel * level * s_brightness + 32512U) /
                     65025U);
}

static void set_pixel(unsigned index, rgb_t color, uint8_t level)
{
    if (index >= s_pixel_count) {
        return;
    }
    /* MZ28 uses SW2812B-compatible LEDs in GRB wire order. */
    s_frame[index * 3U] = scaled(color.green, level);
    s_frame[index * 3U + 1U] = scaled(color.red, level);
    s_frame[index * 3U + 2U] = scaled(color.blue, level);
}

static void fill(rgb_t color, uint8_t level)
{
    for (unsigned i = 0; i < s_pixel_count; ++i) {
        set_pixel(i, color, level);
    }
}

static void runner(uint32_t now_ms, rgb_t head, rgb_t tail, uint32_t step_ms)
{
    unsigned head_index = (now_ms / step_ms) % s_pixel_count;
    for (unsigned i = 0; i < s_pixel_count; ++i) {
        unsigned distance = (head_index + s_pixel_count - i) % s_pixel_count;
        if (distance == 0U) {
            set_pixel(i, head, 255U);
        } else if (distance == 1U) {
            set_pixel(i, tail, 150U);
        } else if (distance == 2U) {
            set_pixel(i, tail, 55U);
        }
    }
}

static void render_state(status_led_state_t state, uint32_t now_ms)
{
    memset(s_frame, 0, s_pixel_count * 3U);
    switch (state) {
    case STATUS_LED_BOOT:
        runner(now_ms, (rgb_t){80, 210, 255}, (rgb_t){0, 50, 180}, 90U);
        break;
    case STATUS_LED_WIFI_PROVISIONING: {
        uint8_t level = triangle(now_ms, 1200U, 45U, 220U);
        fill((rgb_t){25, 115, 255}, level);
        if (s_pixel_count != 0U) {
            set_pixel((now_ms / 180U) % s_pixel_count,
                      (rgb_t){230, 250, 255}, 255U);
        }
        break;
    }
    case STATUS_LED_BINDING:
        fill((rgb_t){255, 118, 8}, triangle(now_ms, 1800U, 35U, 210U));
        break;
    case STATUS_LED_CONNECTING:
        runner(now_ms, (rgb_t){50, 125, 255}, (rgb_t){20, 30, 170}, 140U);
        break;
    case STATUS_LED_READY:
        fill((rgb_t){0, 210, 155}, triangle(now_ms, 2600U, 28U, 90U));
        break;
    case STATUS_LED_OUTGOING:
        runner(now_ms, (rgb_t){0, 235, 255}, (rgb_t){20, 80, 255}, 75U);
        break;
    case STATUS_LED_INCOMING: {
        bool alternate = ((now_ms / 220U) & 1U) != 0U;
        for (unsigned i = 0; i < s_pixel_count; ++i) {
            if (((i & 1U) != 0U) == alternate) {
                set_pixel(i, (rgb_t){20, 255, 92}, 255U);
            }
        }
        break;
    }
    case STATUS_LED_CALL_CONNECTING:
        runner(now_ms, (rgb_t){255, 215, 35}, (rgb_t){0, 170, 255}, 95U);
        break;
    case STATUS_LED_ACTIVE: {
        uint8_t level = triangle(now_ms, 1400U, 70U, 175U);
        fill((rgb_t){28, 255, 74}, level);
        if (s_pixel_count >= 2U) {
            set_pixel((s_pixel_count - 1U) / 2U,
                      (rgb_t){180, 255, 215}, 210U);
            set_pixel(s_pixel_count / 2U,
                      (rgb_t){180, 255, 215}, 210U);
        }
        break;
    }
    case STATUS_LED_CLOSING:
        fill((rgb_t){255, 48, 22},
             ((now_ms / 110U) & 1U) != 0U ? 170U : 28U);
        break;
    case STATUS_LED_ERROR: {
        uint32_t phase = now_ms % 1300U;
        if (phase < 120U || (phase >= 240U && phase < 360U)) {
            fill((rgb_t){255, 18, 12}, 255U);
        }
        break;
    }
    }
}

static bool render_feedback(uint32_t now_ms)
{
    if ((int64_t)now_ms >= atomic_load_explicit(&s_feedback_until_ms,
                                                memory_order_acquire)) {
        return false;
    }
    status_led_feedback_t feedback =
        (status_led_feedback_t)atomic_load_explicit(&s_feedback,
                                                    memory_order_relaxed);
    memset(s_frame, 0, s_pixel_count * 3U);
    switch (feedback) {
    case STATUS_LED_FEEDBACK_TAP:
        fill((rgb_t){110, 245, 255}, 210U);
        break;
    case STATUS_LED_FEEDBACK_DOUBLE_TAP:
        for (unsigned i = 0; i < s_pixel_count; ++i) {
            if (((i + now_ms / 70U) & 1U) != 0U) {
                set_pixel(i, (rgb_t){255, 45, 20}, 255U);
            }
        }
        break;
    case STATUS_LED_FEEDBACK_SUCCESS:
        fill((rgb_t){210, 255, 225}, 255U);
        break;
    case STATUS_LED_FEEDBACK_WARNING:
        fill((rgb_t){255, 138, 0},
             ((now_ms / 80U) & 1U) != 0U ? 255U : 55U);
        break;
    }
    return true;
}

esp_err_t status_led_init(const status_led_config_t *config)
{
    if (config == NULL || config->data_gpio < 0 ||
        config->data_gpio >= GPIO_NUM_MAX || config->pixel_count == 0U ||
        config->pixel_count > STATUS_LED_MAX_PIXELS ||
        config->brightness == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_channel != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    rmt_tx_channel_config_t channel = {
        .gpio_num = config->data_gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = STATUS_LED_RMT_RESOLUTION_HZ,
        .mem_block_symbols = 48,
        .trans_queue_depth = 1,
    };
    esp_err_t err = rmt_new_tx_channel(&channel, &s_channel);
    if (err != ESP_OK) {
        return err;
    }
    rmt_bytes_encoder_config_t encoder = {
        .bit0 = {.level0 = 1, .duration0 = 3,
                 .level1 = 0, .duration1 = 9},
        .bit1 = {.level0 = 1, .duration0 = 9,
                 .level1 = 0, .duration1 = 3},
        .flags.msb_first = 1,
    };
    err = rmt_new_bytes_encoder(&encoder, &s_encoder);
    if (err == ESP_OK) {
        err = rmt_enable(s_channel);
    }
    if (err != ESP_OK) {
        if (s_encoder != NULL) {
            rmt_del_encoder(s_encoder);
            s_encoder = NULL;
        }
        rmt_del_channel(s_channel);
        s_channel = NULL;
        return err;
    }
    s_pixel_count = config->pixel_count;
    s_brightness = config->brightness;
    atomic_store(&s_state, STATUS_LED_BOOT);
    atomic_store(&s_feedback_until_ms, 0);
    ESP_LOGI(TAG, "ready: gpio=%d pixels=%u brightness=%u/255",
             config->data_gpio, config->pixel_count, config->brightness);
    return ESP_OK;
}

void status_led_set_state(status_led_state_t state)
{
    if (state < STATUS_LED_BOOT || state > STATUS_LED_ERROR) {
        return;
    }
    int previous = atomic_exchange_explicit(&s_state, state,
                                            memory_order_acq_rel);
    if (previous != state) {
        ESP_LOGI(TAG, "state: %d -> %d", previous, state);
    }
}

void status_led_feedback(status_led_feedback_t feedback)
{
    static const uint16_t durations_ms[] = {140U, 320U, 420U, 360U};
    if (feedback < STATUS_LED_FEEDBACK_TAP ||
        feedback > STATUS_LED_FEEDBACK_WARNING) {
        return;
    }
    int64_t now_ms = esp_timer_get_time() / 1000;
    atomic_store_explicit(&s_feedback, feedback, memory_order_relaxed);
    atomic_store_explicit(&s_feedback_until_ms,
                          now_ms + durations_ms[feedback],
                          memory_order_release);
}

void status_led_tick(uint32_t now_ms)
{
    if (s_channel == NULL ||
        (uint32_t)(now_ms - s_last_refresh_ms) < STATUS_LED_REFRESH_MS) {
        return;
    }
    s_last_refresh_ms = now_ms;
    if (!render_feedback(now_ms)) {
        render_state((status_led_state_t)atomic_load_explicit(
                         &s_state, memory_order_acquire),
                     now_ms);
    }
    rmt_transmit_config_t transmit = {
        .loop_count = 0,
        .flags.eot_level = 0,
    };
    esp_err_t err = rmt_transmit(s_channel, s_encoder, s_frame,
                                 s_pixel_count * 3U, &transmit);
    if (err == ESP_OK) {
        err = rmt_tx_wait_all_done(s_channel, pdMS_TO_TICKS(10));
    }
    if (err != ESP_OK &&
        (s_last_error_log_ms == 0U ||
         (uint32_t)(now_ms - s_last_error_log_ms) >= 1000U)) {
        s_last_error_log_ms = now_ms;
        ESP_LOGW(TAG, "refresh failed: %s", esp_err_to_name(err));
    }
}
