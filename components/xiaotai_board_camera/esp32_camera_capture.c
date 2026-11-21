#include "board_camera.h"
#include "esp32_camera_board.h"
#include "esp_log.h"
#include "sdkconfig.h"

#if CONFIG_CAMERA_PSRAM_DMA || !CONFIG_SCCB_HARDWARE_I2C_DRIVER_NEW
#error "S3 viewing requires internal DMA staging and the shared new I2C driver"
#endif

static const char *TAG = "board_camera";
static bool s_initialized, s_ready;
static camera_fb_t *s_fb;
static xiaotai_camera_frame_t *s_loan;

esp_err_t xiaotai_esp32_camera_open(const xiaotai_esp32_camera_board_t *board,
                                  unsigned width, unsigned height)
{
    if (s_initialized) return ESP_ERR_INVALID_STATE;
    framesize_t size;
    if (width == 160 && height == 120) size = FRAMESIZE_QQVGA;
    else if (width == 240 && height == 176) size = FRAMESIZE_HQVGA;
    else if (width == 320 && height == 240) size = FRAMESIZE_QVGA;
    else return ESP_ERR_NOT_SUPPORTED;
    if (size > board->max_frame_size) return ESP_ERR_NOT_SUPPORTED;

    camera_config_t cfg = board->wiring;
    cfg.pixel_format = PIXFORMAT_YUV422;
    cfg.frame_size = size;
    cfg.jpeg_quality = 12; /* Raw capture; software encoder controls JPEG quality. */
    cfg.fb_count = 2;
    cfg.fb_location = CAMERA_FB_IN_PSRAM;
    cfg.grab_mode = CAMERA_GRAB_LATEST;
    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) return err; /* Driver releases partial init on failure. */
    s_initialized = true;

    sensor_t *sensor = esp_camera_sensor_get();
    const bool psram_dma = esp_camera_get_psram_mode();
    if (!sensor || sensor->id.PID != board->sensor_pid || psram_dma) {
        ESP_LOGE(TAG, "camera contract failed: pid=%x psram_dma=%d",
                 sensor ? sensor->id.PID : 0, psram_dma);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (sensor->set_hmirror(sensor, 0) != 0 || sensor->set_vflip(sensor, 0) != 0)
        return ESP_FAIL;
    s_ready = true;
    return ESP_OK;
}

esp_err_t xiaotai_board_camera_close(void)
{
    if (s_loan) return ESP_ERR_INVALID_STATE;
    if (!s_initialized) return ESP_OK;
    s_initialized = s_ready = false;
    /* SCCB SDA=-1 borrows the board's I2C bus; deinit must not delete it. */
    return esp_camera_deinit();
}

esp_err_t xiaotai_board_camera_borrow(xiaotai_camera_frame_t *frame)
{
    if (!frame) return ESP_ERR_INVALID_ARG;
    if (!s_ready || s_loan) return ESP_ERR_INVALID_STATE;
    if (!esp_camera_available_frames()) return ESP_ERR_NOT_FOUND;

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) return ESP_FAIL;
    *frame = (xiaotai_camera_frame_t){
        .data = fb->buf, .size = fb->len, .width = fb->width, .height = fb->height,
        .captured_us = (int64_t)fb->timestamp.tv_sec * 1000000 + fb->timestamp.tv_usec,
        .format = fb->format == PIXFORMAT_YUV422 ? XIAOTAI_CAMERA_YUYV422 : XIAOTAI_CAMERA_UNKNOWN,
    };
    s_fb = fb;
    s_loan = frame;
    return ESP_OK;
}

esp_err_t xiaotai_board_camera_release(xiaotai_camera_frame_t *frame)
{
    if (!frame) return ESP_ERR_INVALID_ARG;
    if (frame != s_loan) return ESP_ERR_INVALID_STATE;
    esp_camera_fb_return(s_fb);
    s_fb = NULL;
    s_loan = NULL;
    *frame = (xiaotai_camera_frame_t){0};
    return ESP_OK;
}
