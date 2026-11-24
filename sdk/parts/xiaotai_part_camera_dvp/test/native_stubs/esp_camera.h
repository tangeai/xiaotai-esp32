// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>
#include "esp_err.h"

typedef enum { FRAMESIZE_QQVGA = 1, FRAMESIZE_HQVGA, FRAMESIZE_QVGA } framesize_t;
typedef enum { PIXFORMAT_YUV422 = 1, PIXFORMAT_JPEG } pixformat_t;
enum { CAMERA_FB_IN_PSRAM = 1, CAMERA_GRAB_LATEST };
typedef struct {
    int pin_pwdn, pin_reset, pin_xclk, xclk_freq_hz, ledc_timer, ledc_channel;
    int pin_sccb_sda, pin_sccb_scl, sccb_i2c_port;
    int pin_d0, pin_d1, pin_d2, pin_d3, pin_d4, pin_d5, pin_d6, pin_d7;
    int pin_vsync, pin_href, pin_pclk;
    pixformat_t pixel_format;
    framesize_t frame_size;
    int jpeg_quality, fb_count, fb_location, grab_mode;
} camera_config_t;
typedef struct sensor sensor_t;
struct sensor {
    struct { uint16_t PID; } id;
    struct { framesize_t framesize; } status;
    pixformat_t pixformat;
    int (*set_hmirror)(sensor_t *, int);
    int (*set_vflip)(sensor_t *, int);
};
typedef struct { size_t width, height; } resolution_info_t;
extern const resolution_info_t resolution[];
typedef struct {
    uint8_t *buf;
    size_t len;
    size_t width, height;
    pixformat_t format;
    struct timeval timestamp;
} camera_fb_t;

esp_err_t esp_camera_init(const camera_config_t *config);
esp_err_t esp_camera_deinit(void);
sensor_t *esp_camera_sensor_get(void);
bool esp_camera_get_psram_mode(void);
bool esp_camera_available_frames(void);
camera_fb_t *esp_camera_fb_get(void);
void esp_camera_fb_return(camera_fb_t *frame);
