// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/apps/door.h"
#include "xiaotai/modules/call.h"
#include "xiaotai/view.h"

#define DOOR_ROWS_MAX 8U

typedef enum {
    DOOR_HOME,
    DOOR_DEVICES,
    DOOR_MONITOR,
    DOOR_CALL,
    DOOR_OTHER_CALL,
    DOOR_OTHER_RESULT,
    DOOR_SETUP,
    DOOR_ASSISTANT,
} door_page_t;

typedef enum {DOOR_DIALOG_NONE, DOOR_DIALOG_OPEN, DOOR_DIALOG_RESULT, DOOR_DIALOG_VOLUME} door_dialog_t;

typedef struct {
    xiaotai_err_t (*init)(void);
    bool (*claim)(const xiaotai_event_t *event);
    bool (*lock_configured)(void);
    xiaotai_err_t (*open_lock)(void);
} door_feature_t;

extern const door_feature_t door_with_stream, door_with_stream_video, door_with_video_call, door_with_lock;
typedef struct {
    char id[XIAOTAI_SETTING_TEXT_MAX];
    char label[XIAOTAI_SETTING_TEXT_MAX];
    bool online;
} door_row_t;

typedef struct {
    char peer[XIAOTAI_SETTING_TEXT_MAX];
    door_row_t *rows;
    const char *published_page;
    int64_t last_swipe_us;
    int64_t stream_stop_until_us;
    uint32_t session, classification_session;
    uint32_t stream_generation, stream_stop_timer;
    uint32_t stream_format_noted, call_format_noted, call_picture_generation;
    uint32_t door_calls, yielded;
    uint32_t opens, opens_done;
    uint32_t row_generation, peer_revision;
    uint8_t row_count;
    door_page_t page;
    door_dialog_t overlay;
    bool initialized, foreground, setup, sender;
    bool last_swipe_valid, classified_door;
    bool stream_picture, call_picture;
} door_state_t;

extern xiaotai_app_door_config_t door_config;
extern door_state_t door;

void door_render(void);
void door_page(door_page_t page);
void door_render_home_page(void);
void door_register_view(const xiaotai_view_t *view);
void door_finish_presentation(void);
bool door_lock_configured(void);
xiaotai_err_t door_lock_request(void);
xiaotai_app_door_state_t door_home_state(void);
void door_home_intent(const xiaotai_args_t *args, void *ctx);
void door_devices_intent(const xiaotai_args_t *args, void *ctx);
void door_select_intent(const xiaotai_args_t *args, void *ctx);
void door_back_intent(const xiaotai_args_t *args, void *ctx);
void door_monitor_intent(const xiaotai_args_t *args, void *ctx);

xiaotai_err_t door_init_call(void);
bool door_call_matches(const xiaotai_call_status_t *call);
const xiaotai_call_status_t *door_shown_call(void);
void door_on_call(const xiaotai_event_t *event, void *ctx);
void door_call_render(void);
void door_call_ring_update(void);
void door_call_settings(const xiaotai_event_t *event, void *ctx);
void door_call_reset(void);
void door_volume_commit(void);

xiaotai_err_t door_init_open(void);
void door_overlay_close(void);
void door_overlay_reset(void);

xiaotai_err_t door_init_at(void);
xiaotai_err_t door_fallback_init(void);
void door_fallback_render(void);
void door_fallback_foreground(bool foreground);
void door_fallback_reset(void);
bool door_fallback_busy(void);
void door_fallback_device_ended(const xiaotai_call_status_t *ended);

#define DOOR_FORMAT_ERROR "视频格式不支持"
#define DOOR_CALL_FORMAT_TOAST "door.call_format"
#define DOOR_STREAM_FORMAT_TOAST "door.stream_format"
void door_format_show(uint32_t generation, uint32_t *noted, const char *id);
void door_format_clear(uint32_t generation, uint32_t *noted, const char *id);
