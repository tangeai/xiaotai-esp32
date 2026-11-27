// SPDX-License-Identifier: MIT
#pragma once

#include "door_rooms.h"
#include "xiaotai/apps/door.h"
#include "xiaotai/modules/call.h"
#include "xiaotai/modules/contacts.h"
#include "xiaotai/view.h"

#define DOOR_ROWS_MAX 32U
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
    DOOR_MODE,          /* the choice of mode */
    DOOR_SETTINGS,
    DOOR_LOG,           /* indoor station: recent door calls */
} door_page_t;

typedef enum {
    DOOR_DIALOG_NONE, DOOR_DIALOG_OPEN, DOOR_DIALOG_RESULT, DOOR_DIALOG_VOLUME, DOOR_DIALOG_PENDING, DOOR_DIALOG_ASK,
} door_dialog_t;

typedef enum { DOOR_LOG_MISSED, DOOR_LOG_ANSWERED, DOOR_LOG_REJECTED, DOOR_LOG_OPENED } door_log_kind_t;

typedef struct {
    xiaotai_err_t (*init)(void);
    bool (*claim)(const xiaotai_event_t *event);
    bool (*lock_configured)(void);
    xiaotai_err_t (*open_lock)(void);
    bool (*camera)(void);
    bool (*shows_video)(void);
    bool (*wechat_video)(void);
} door_feature_t;

extern const door_feature_t door_with_stream, door_with_stream_video, door_with_video_call, door_with_lock;
typedef struct {
    char id[XIAOTAI_SETTING_TEXT_MAX];
    char label[XIAOTAI_SETTING_TEXT_MAX];
    char room[DOOR_ROOM_SIZE];      /* access machine: the household's room, "" without one */
    bool online;
    bool wechat;        /* a WeChat contact (directory); otherwise a device. Of a household: one of them */
} door_row_t;

typedef char door_peer_id_t[XIAOTAI_SETTING_TEXT_MAX];

/* Whom a hunt calls. */
typedef struct {
    char id[XIAOTAI_SETTING_TEXT_MAX];
    bool wechat;
} door_target_t;
#define DOOR_TARGETS_MAX 6U

typedef struct {
    char peers[XIAOTAI_APP_DOOR_PEERS_MAX][XIAOTAI_SETTING_TEXT_MAX];   /* peer_count of them, the first one first */
    door_row_t *rows;
    xiaotai_view_item_t *items;     /* DOOR_ROWS_MAX, for the page being shown */
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
    uint32_t looks, missed;
    uint32_t row_generation, peer_revision;
    uint8_t row_count, peer_count;
    door_page_t page;
    door_dialog_t overlay;
    char called[XIAOTAI_SETTING_TEXT_MAX];  /* whom this call was dialled for, "": by the peer's name */
    char learnt[XIAOTAI_SETTING_TEXT_MAX + 48]; /* a door station learnt: said once a door page is in front */
    bool initialized, foreground, setup, chosen;
    bool sender, access;    /* a door station; of a building */
    bool bell_indoor;       /* the indoor station of a doorbell: the simple one */
    uint8_t shown;          /* mode_pages: the mode of the page in front + 1 until it settles; 0: the stored one */
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
/* The page of a running door call, the look's included. */
bool door_on_call_page(void);
void door_home_intent(const xiaotai_args_t *args, void *ctx);
void door_devices_intent(const xiaotai_args_t *args, void *ctx);
void door_select_intent(const xiaotai_args_t *args, void *ctx);
void door_back_intent(const xiaotai_args_t *args, void *ctx);
bool door_busy(void);
void door_idle_restart(void);       /* somebody is at the device */
void door_idle_reset(void);
int door_contact_kind(const char *name);
const char *door_pager_icon(int direction);
/* Whether this display draws the door's own home and mode pages. */
bool door_home_template(void);
/* Whether this display draws the pages that fill it (templates_station.c). */
bool door_station_template(void);
/* A list page: the door's own template, or the display's list. */
void door_show_list(const xiaotai_view_t *view);
/* A picture of this device can be sent. */
bool door_has_camera(void);
/* A picture of the other end can be shown on this device. */
bool door_shows_video(void);
bool door_wechat_video(void);

/* door_peers.c: the counterparts. */
xiaotai_err_t door_peers_register(void (*changed)(const char *name, void *ctx));
bool door_peers_setting(const char *name);
void door_peers_read(void);
int door_peer_slot(const char *id);
xiaotai_err_t door_peer_put_first(const char *id);
xiaotai_err_t door_peer_remove(const char *id);
/* First contact pairs the side that was not set up (XIAOTAI_APP_DOOR_MESSAGE_HELLO, LOOK). */
void door_peer_learn_indoor(const char *id);
bool door_peer_learn_station(const xiaotai_call_status_t *call, uint8_t mode);
void door_peer_learnt_say(void);
/* This call stays with whoever presents it, whatever is learnt during it. */
void door_call_pass(uint32_t session);
/* XIAOTAI_EVENT_ACTIVITY: whether the running call ended something else (then the station was not at rest). */
void door_call_activity(const xiaotai_event_t *event, void *ctx);
bool door_peer_contact(const char *id, xiaotai_contact_t *contact);
bool door_peer_online(const char *id);
void door_device_label(const char *id, const char *name, char *out, size_t size);
void door_peer_label(const char *id, char *out, size_t size);
/* The counterpart settings as stored, without gaps or repetitions. */
void door_peers_stored(door_peer_id_t *out);
/* The list changed without a setting: the shown call's classification, the settings again, the page. */
void door_peers_changed(void);
void door_read_settings(void);
xiaotai_app_door_mode_t door_mode(void);        /* of the page in front */

/* door_pages.c: every mode a page of its own (mode_pages). */
xiaotai_err_t door_pages_init(void);
void door_pages_reset(void);
bool door_pages_on(void);
void door_pages_load(void);
const door_peer_id_t *door_pages_list(xiaotai_app_door_mode_t mode);     /* NULL: the settings */
xiaotai_err_t door_pages_store(door_peer_id_t *list);
void door_pages_enter(xiaotai_app_entry_t entry);   /* on the way to its home page */
void door_pages_leave(void);        /* to the background: a page not settled goes */
void door_pages_saved(void);        /* the stored mode was set: the page follows it */
void door_pages_commit(void);       /* the page in front is used: it is the stored mode */
bool door_pages_step(int direction);    /* false: paging leads out of the door */

/* Calls a device, with video when this product has it and `voice` is false. */
xiaotai_err_t door_dial(const char *id, bool voice, uint32_t *session);
/* A device without video refuses a video call by itself, faster than anybody
 * could: it is called again without. Seconds later somebody declined. */
bool door_refused_video(const xiaotai_call_status_t *ended);

/* door_mode.c: choice of mode, settings, recent calls. */
xiaotai_err_t door_init_mode(void);
bool door_mode_known(void);
void door_mode_render(void);
void door_settings_render(void);
void door_log_render(void);
void door_log_add(door_log_kind_t kind, const char *id);
void door_mode_reset(void);

/* door_look.c: looking at the door, both ends. */
xiaotai_err_t door_init_look(void);
bool door_look_is(uint32_t session);        /* indoor station: the look's call */
bool door_look_running(void);
bool door_look_seen_talking(void);
xiaotai_err_t door_init_key(void);
bool door_look_seen(uint32_t session);      /* door station: the call it answered by itself */
bool door_look_answer(const xiaotai_call_status_t *incoming);
/* Indoor station: the visitor's call shown at once, on the door's home page. */
bool door_look_take(const xiaotai_call_status_t *incoming);
/* Door station: a call it dialled that the indoor station looks at before it answers. */
bool door_look_viewed(uint32_t session);
void door_look_connected(uint32_t session);
bool door_look_ended(uint32_t session, const xiaotai_call_status_t *ended);
bool door_look_ring(void);
bool door_look_ringing(void);
bool door_look_rung(void);
void door_look_visitor(void);               /* the call being answered takes over the visitor's */
void door_look_talks(uint32_t session);     /* an ordinary call to the door station: it is told that we talk */
void door_look_message(const xiaotai_call_message_t *message);
void door_look_render(void);
void door_look_station_render(void);
void door_look_reset(void);

/* door_station.c: the door station's directory and the calls it dials. */
xiaotai_err_t door_init_station(void);
void door_station_reset(void);
void door_render_directory(void);
bool door_station_claim(const xiaotai_event_t *event);
bool door_station_video_link(uint32_t link);
bool door_station_wechat(uint32_t session);
void door_station_render_wechat(const void *wechat_status);
void door_station_wechat_ended(const void *wechat_status);
/* Calling the chosen indoor stations one after the other. */
void door_hunt_start(uint32_t delay_ms, const char *except);
void door_hunt_targets(const door_target_t *targets, uint8_t count, const char *label, const char *room);
/* What the visitor is told when the hunt is over, "" the usual. */
const char *door_hunt_farewell(void);
bool door_hunt_active(void);
void door_hunt_cancel(void);
bool door_hunt_ended(const xiaotai_call_status_t *ended, bool answered);   /* true: the next one is called */
/* Ends the call it dialled as unanswered: the next one is called, or the visitor told. */
void door_hunt_give_way(uint32_t session);
void door_hunt_render(void);          /* nothing is dialled at this moment */
void door_hunt_caption(char *out, size_t size);                           /* "", or which of how many */

xiaotai_err_t door_init_call(void);
bool door_call_matches(const xiaotai_call_status_t *call);
bool door_call_looks_at_home(const xiaotai_call_status_t *snapshot);
const xiaotai_call_status_t *door_shown_call(void);
void door_on_call(const xiaotai_event_t *event, void *ctx);
void door_call_render(void);
void door_call_ring_update(void);
void door_call_settings(const xiaotai_event_t *event, void *ctx);
void door_call_reset(void);
void door_volume_commit(void);

xiaotai_err_t door_init_open(void);
xiaotai_err_t door_init_sounds(void);
void door_sound_opened(void);
void door_sound_key(void);

/* door_access.c: the access machine's keypad and households. */
xiaotai_err_t door_init_access(void);
void door_access_reset(void);
void door_access_clear(void);           /* what was typed goes */
void door_access_home_render(const xiaotai_view_indicator_t *network);
void door_access_list(bool people);     /* the door numbers, or (people) the directory below them */
bool door_access_back(void);            /* from the directory to the door numbers; false: nothing to do */
void door_access_list_render(void);
void door_access_call_row(const door_row_t *row);
bool door_access_call(const char *typed);       /* on its home screen; true: it calls, or asks which */
const char *door_title(void);
void door_overlay_close(void);
void door_overlay_drop(void);       /* the same, the page is drawn by the caller */
void door_overlay_reset(void);
void door_on_message(const xiaotai_event_t *event, void *ctx);
void door_open_call_ended(uint32_t session);
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
/* Whether this display draws the door's own wide templates. */
bool door_own_template(void);
