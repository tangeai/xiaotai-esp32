#pragma once

#include "camera_video_source.h"
#include "tirtc_session.h"

esp_err_t rtc_media_bridge_register_external_video_sink(camera_video_source_submit_cb_t cb,
                                                        void *ctx);
void rtc_media_bridge_unregister_external_video_sink(camera_video_source_submit_cb_t cb,
                                                     void *ctx);
const tirtc_session_media_ops_t *rtc_media_bridge_get_ops(void);
void *rtc_media_bridge_get_context(void);
