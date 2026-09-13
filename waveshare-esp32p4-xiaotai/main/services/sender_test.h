#pragma once

#include <stdbool.h>

#include "esp_err.h"

#define SENDER_TEST_STATUS_MAX 96

typedef enum {
    SENDER_TEST_MODE_NONE = 0,
    SENDER_TEST_MODE_VIDEO,
    SENDER_TEST_MODE_AUDIO,
} sender_test_mode_t;

typedef struct {
    bool running;
    bool spiffs_ready;
    char status[SENDER_TEST_STATUS_MAX];
} sender_test_snapshot_t;

esp_err_t sender_test_init(void);
esp_err_t sender_test_start(sender_test_mode_t mode);
void sender_test_stop(void);
bool sender_test_is_mode_active(sender_test_mode_t mode);
void sender_test_request_video_restart(void);
void sender_test_request_audio_restart(void);
void sender_test_get_snapshot(sender_test_snapshot_t *snapshot);
