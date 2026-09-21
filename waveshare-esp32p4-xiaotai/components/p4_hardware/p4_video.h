#pragma once
#include "esp_err.h"
#include "starter_tirtc.h"
#include "lvgl.h"
esp_err_t p4_video_init(void);
void p4_video_set_session(starter_tirtc_mode_t mode, uint32_t generation, bool video);
void p4_video_poll(void);
esp_err_t p4_video_set_camera_enabled(uint32_t generation, bool enabled);
void p4_video_key_frame(void);
uint32_t p4_video_sent(void);
void p4_video_submit(uint32_t generation, const starter_tirtc_frame_t *frame, const void *data);
/* Called only on the LVGL task. Owns a copied canvas, not a borrowed frame. */
void p4_video_ui_reset(void);
void p4_video_ui_tick(lv_obj_t *screen, bool video_visible);
