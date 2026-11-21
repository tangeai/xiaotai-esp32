// SPDX-License-Identifier: MIT
#ifndef XIAOTAI_COMPANION_CALL_UI_H
#define XIAOTAI_COMPANION_CALL_UI_H

#include "companion_internal.h"

typedef enum {
    COMPANION_CALL_IDLE, COMPANION_CALL_OUTGOING, COMPANION_CALL_INCOMING,
    COMPANION_CALL_CONNECTING, COMPANION_CALL_ACTIVE,
} companion_call_phase_t;

typedef struct {
    uint32_t session, link, result_token;
    companion_call_phase_t phase;
    bool outgoing, muted, video, releasing, failed;
    int64_t active_since_us;
    xiaotai_err_t error;
    const char *result;
    char peer[65];
    char peer_id[65]; /* Interaction identity, independent of the visible name. */
} companion_call_snapshot_t;

typedef struct {
    bool wechat;
    void (*read)(companion_call_snapshot_t *out);
    xiaotai_err_t (*accept)(uint32_t session);
    xiaotai_err_t (*reject)(uint32_t session);
    xiaotai_err_t (*hangup)(uint32_t session);
    xiaotai_err_t (*mute)(uint32_t session, bool muted);
    xiaotai_err_t (*save_mute)(uint32_t session, bool muted);
    void (*result_dismissed)(uint32_t session);
} companion_call_ops_t;

typedef struct {
    bool active, enabled, remote;
} companion_call_camera_t;

typedef struct {
    companion_call_camera_t (*read)(uint32_t link);
    xiaotai_err_t (*set)(uint32_t link, bool enabled);
} companion_call_video_ops_t;

xiaotai_err_t companion_call_ui_init(void);
void companion_call_ui_reset(void);
void companion_call_ui_event(const companion_call_ops_t *ops, uint32_t reported);
void companion_call_ui_foreground(const companion_call_ops_t *ops, bool visible);
bool companion_call_ui_home(const companion_call_ops_t *ops, companion_home_session_t *out);
xiaotai_err_t companion_call_ui_main(const companion_call_ops_t *ops, int32_t action);
xiaotai_err_t companion_call_ui_end(const companion_call_ops_t *ops, uint32_t generation);
xiaotai_err_t companion_call_ui_session(const companion_call_ops_t *ops, const xiaotai_args_t *args);
void companion_call_ui_set_video(const companion_call_video_ops_t *ops);
typedef enum {
    COMPANION_VIDEO_UNCHANGED,
    COMPANION_VIDEO_UNSUPPORTED,
    COMPANION_VIDEO_RECOVERED,
} companion_video_format_t;
void companion_call_ui_video_event(uint32_t link, companion_video_format_t format);

#endif
