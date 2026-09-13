#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "tiRTC.h"

#define AI_CHAT_VIDEO_STREAM_ID 0U

typedef struct {
    bool active;
    uint32_t queued_frames;
    uint32_t queue_failures;
} ai_chat_video_stats_t;

typedef bool (*ai_chat_video_session_valid_cb_t)(tirtc_conn_t conn, void *ctx);

/* Owns only the AI Chat camera-to-TiRTC route. The AI Chat service owns the
 * session/connection lifecycle and calls start/stop after start_session. */
esp_err_t ai_chat_video_init(void);
esp_err_t ai_chat_video_start(tirtc_conn_t conn,
                              ai_chat_video_session_valid_cb_t session_valid,
                              void *session_ctx);
void ai_chat_video_stop(tirtc_conn_t conn);
void ai_chat_video_get_stats(ai_chat_video_stats_t *stats);
