#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "tiRTC.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool running;
    bool uplink_enabled;
    uint64_t tx_frames;
    uint64_t tx_bytes;
    uint32_t dropped_frames;
    uint32_t tx_failures;
    esp_err_t last_error;
} wechat_voip_media_stats_t;

typedef struct {
    esp_err_t (*prepare)(bool local_video_enabled,
                         bool remote_video_enabled,
                         void *ctx);
    void (*release)(void *ctx);
} wechat_voip_media_lifecycle_t;

esp_err_t wechat_voip_media_configure_lifecycle(const wechat_voip_media_lifecycle_t *lifecycle,
                                                void *ctx);
esp_err_t wechat_voip_media_prepare(bool local_video_enabled, bool remote_video_enabled);
esp_err_t wechat_voip_media_start(tirtc_conn_t conn);
void wechat_voip_media_stop(tirtc_conn_t conn);
esp_err_t wechat_voip_media_stop_wait(tirtc_conn_t conn, uint32_t timeout_ms);
bool wechat_voip_media_is_running(void);
esp_err_t wechat_voip_media_set_uplink_enabled(bool enabled);
void wechat_voip_media_get_stats(wechat_voip_media_stats_t *stats);

#ifdef __cplusplus
}
#endif
