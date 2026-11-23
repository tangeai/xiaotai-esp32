// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/apps/door.h"
#include "xiaotai/modules/call.h"
#include "xiaotai/view.h"

#define DOOR_ROWS_MAX 16U
/* Rendered rows carry DOOR_ROW_TOKEN_BASE + generation * DOOR_ROWS_MAX + index. */
#define DOOR_ROW_TOKEN_BASE (UINT32_C(1) << 30)

typedef enum {
    DOOR_HOME,
    DOOR_DEVICES,
    DOOR_MONITOR,
    DOOR_CALL,
    DOOR_OTHER_CALL,
    DOOR_OTHER_RESULT,
    DOOR_SETUP,
    DOOR_ASSISTANT,
    DOOR_DIRECTORY,     /* door station: whom a visitor can call */
} door_page_t;

typedef enum {DOOR_DIALOG_NONE, DOOR_DIALOG_OPEN, DOOR_DIALOG_RESULT, DOOR_DIALOG_VOLUME, DOOR_DIALOG_PENDING} door_dialog_t;

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
    bool wechat;        /* a WeChat contact (directory); otherwise a device */
} door_row_t;

typedef struct {
    char peer[XIAOTAI_SETTING_TEXT_MAX];
    door_row_t *rows;
    const char *published_page;
    int64_t last_swipe_us;
    int64_t stream_stop_until_us;
    uint32_t session, classification_session;
    /* Door station: the device call and the WeChat call it dialled itself. */
    uint32_t dialed_session, wechat_session;
    uint32_t stream_generation, stream_stop_timer;
    uint32_t stream_format_noted, call_format_noted, call_picture_generation;
    uint32_t door_calls, yielded;
    uint32_t opens, opens_done, opens_received;
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

static inline void door_rows_invalidate(void)
{
    door.row_generation = (door.row_generation + 1U) % (DOOR_ROW_TOKEN_BASE / DOOR_ROWS_MAX);
}
/* The row a tap names: a current index, or a token of the rendered list (stale tokens: -1). */
static inline int door_row_from_value(int32_t value)
{
    if (value < 0) return -1;
    uint32_t row = (uint32_t)value;
    if (row >= DOOR_ROW_TOKEN_BASE) {
        const uint32_t token = row - DOOR_ROW_TOKEN_BASE;
        if (token / DOOR_ROWS_MAX != door.row_generation) return -1;
        row = token % DOOR_ROWS_MAX;
    }
    return row < door.row_count ? (int)row : -1;
}

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
bool door_busy(void);
int door_contact_kind(const char *name);

/* door_station.c: the door station's directory and the calls it dials. */
xiaotai_err_t door_init_station(void);
void door_render_directory(void);
bool door_station_claim(const xiaotai_event_t *event);
bool door_station_video_link(uint32_t link);
bool door_station_wechat(uint32_t session);
void door_station_render_wechat(const void *wechat_status);
void door_station_wechat_ended(const void *wechat_status);

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
void door_on_message(const xiaotai_event_t *event, void *ctx);
void door_open_call_ended(void);
void door_ring_intent(const xiaotai_args_t *args, void *ctx);

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
