// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/apps/companion.h"
#include "xiaotai/view.h"
#include "xiaotai/view_config.h"

#define COMPANION_STACK_MAX 6U
#define COMPANION_PAGES_MAX 24U
#define COMPANION_ENTRY_INTENT "companion.entry"
#define COMPANION_SETUP_BACK 1
#define COMPANION_HOME_START_INTENT "companion.home.start"
#define COMPANION_PHONE_INTENT "companion.contact.first"

typedef struct {
    const char *id;
    const char *title;
    const char *(*entry_title)(int32_t arg);
    bool (*arg_valid)(int32_t arg);
    void (*build)(xiaotai_view_t *view, int32_t arg);
    void (*entered)(void);
    const char *icon;     /* XIAOTAI_ICON_* shown beside the page's menu entry; NULL: none */
} companion_page_t;

typedef enum {
    COMPANION_HOME_IDLE, COMPANION_HOME_AI_CONNECT, COMPANION_HOME_AI_STOP,
    COMPANION_HOME_AI_ACTIVE, COMPANION_HOME_STREAM_AUDIO, COMPANION_HOME_STREAM_VIEW,
    COMPANION_HOME_STREAM_CAMERA_START, COMPANION_HOME_STREAM_CAMERA_ON, COMPANION_HOME_STREAM_CAMERA_ERROR,
    COMPANION_HOME_RINGING, COMPANION_HOME_CALL_CONNECT, COMPANION_HOME_CALL,
    COMPANION_HOME_ROOM_CONNECT, COMPANION_HOME_ROOM,
} companion_home_mode_t;

typedef struct {
    companion_home_mode_t mode;
    xiaotai_talk_phase_t phase;
    xiaotai_speaker_t speaker;
    const char *caption;
    const char *emotion;
} companion_home_session_t;

typedef xiaotai_err_t (*companion_local_room_hold_fn_t)(uint32_t ticket, uint32_t generation, bool pressed);
typedef xiaotai_err_t (*companion_local_room_refresh_fn_t)(uint32_t generation, uint32_t ticket);

typedef struct {
    const char *name;
    xiaotai_err_t (*init)(void);
    bool (*claim)(const xiaotai_event_t *event);
    void (*decorate_view)(xiaotai_view_t *view);
    /* NOT_SUPPORTED means let another part handle the main action. */
    xiaotai_err_t (*main)(int32_t action);
    xiaotai_err_t (*end)(uint32_t session);
    void (*foreground)(bool foreground);
    void (*reset)(void);
    /* Optional presentation notification (e.g. wake a selected UI). */
    void (*user_activity)(void);
    bool (*screen_awake)(void);
    xiaotai_err_t (*screen_sleep)(void);
    void (*sleep_timeout)(uint32_t timeout_ms);
    void (*brightness)(uint8_t percent);
    xiaotai_err_t (*show_wechat_code)(void);
    xiaotai_err_t (*dial_contact)(const char *kind, const char *id, bool video);
    bool (*contact_video_available)(void);
    bool (*home_session)(companion_home_session_t *session);
    /* Optional setup status contributed by a selected session adapter. */
    bool (*service_preparing)(void);
    void (*page_visible)(const char *id);
    bool (*defer_offline_setup)(void);
    bool (*room_member_name)(const char *device_id, char *out, size_t capacity);
    xiaotai_err_t (*register_local_room_hold)(const char *intent, companion_local_room_hold_fn_t fn,
                                              companion_local_room_refresh_fn_t refresh);
    /* NOT_SUPPORTED means this part does not route that session action. */
    xiaotai_err_t (*session)(const xiaotai_args_t *args);
} xiaotai_app_companion_part_t;

extern const xiaotai_app_companion_part_t *const xiaotai_app_companion_parts[];
extern const xiaotai_app_companion_style_t *const companion_builtins[];
const xiaotai_interaction_profile_t *companion_interaction_profile(const xiaotai_app_companion_style_t *style,
                                                                 bool main_button_on_press);

typedef struct {
    const companion_page_t *page;
    int32_t arg;
    uint16_t focus;
    uint16_t focus_on_return;
} companion_location_t;

typedef struct {
    xiaotai_view_t view;
#if XIAOTAI_UI_OWNER_FRAME_WORKSPACE
    union {
        struct {
            xiaotai_view_item_t items[XIAOTAI_VIEW_ITEMS_MAX];
            xiaotai_view_action_t actions[XIAOTAI_VIEW_ACTIONS_MAX];
            xiaotai_view_field_t fields[XIAOTAI_VIEW_FIELDS_MAX];
            xiaotai_view_indicator_t indicators[2];
        };
        uint8_t owner_frame[1024];
    };
#else
    xiaotai_view_item_t items[XIAOTAI_VIEW_ITEMS_MAX];
    xiaotai_view_action_t actions[XIAOTAI_VIEW_ACTIONS_MAX];
    xiaotai_view_field_t fields[XIAOTAI_VIEW_FIELDS_MAX];
    xiaotai_view_indicator_t indicators[2];
#endif
    char details[1536];
} companion_view_buffer_t;

typedef struct {
    xiaotai_app_companion_config_t config;
    const xiaotai_app_companion_style_t *style;
    companion_view_buffer_t *buffer;
    const companion_page_t *pages[COMPANION_PAGES_MAX];
    companion_location_t stack[COMPANION_STACK_MAX];
    unsigned page_count, depth;
    uint32_t epoch, confirm_generation;
    char confirm_id[17];
    void (*confirm_resolved)(bool);
    const char *confirm_title, *confirm_text, *confirm_choice_label;
    xiaotai_app_companion_status_t status;
    bool initialized, has_display, code_visible, confirm_enabled;
#if XIAOTAI_UI_OWNER_FRAME_WORKSPACE
    bool building_view;
#endif
} companion_t;

extern companion_t companion;

/* Paging steps closer together than this are one tap. */
#define COMPANION_PAGER_GUARD_US 350000
/* foreground(true): whether the pager brought the companion forward. */
void companion_pager_entered(bool paged);

/* HOME has the clock screen: the style offers it and the product did not turn it off. */
static inline bool companion_clock_home(void)
{
    return (companion.style->policy & XIAOTAI_APP_COMPANION_CLOCK_HOME) && !companion.config.clock_home_off;
}

xiaotai_err_t companion_pages_init(void);
xiaotai_err_t companion_page_register(const companion_page_t *page);
void companion_render(void);
void companion_presentation_changed(void);
bool companion_finish_presentation(void);
void companion_refuse(const char *target, xiaotai_err_t error);
bool companion_reset_home(int32_t screen);
bool companion_on_page(const char *id);
bool companion_page_exists(const char *id);
xiaotai_err_t companion_show_page(const char *id, int32_t value);
xiaotai_err_t companion_show_page_from_item(const char *id, int32_t value, uint16_t source_focus);
/* Foreground owner only. OK submits the overlay; it does not attest to display or scanning. */
xiaotai_err_t companion_show_wechat_code(void);
void companion_user_activity(void);
bool companion_screen_awake(void);
uint32_t companion_toast_ms(void);
void companion_refuse_text(const char *target, xiaotai_err_t error, const char *text);
void companion_report_refusal(const char *target, xiaotai_err_t error);
void companion_invalidate_entries(void);
void companion_back_page(void);
bool companion_pop_call_page(void);
bool companion_remove_page(const char *id);
/* The overlay borrows static text until it is cancelled or resolved. */
bool companion_confirm_open(const char *title, const char *text, void (*resolved)(bool));
bool companion_confirm_open_choice(const char *title, const char *text, const char *accept_label,
                                   void (*resolved)(bool));
void companion_confirm_enabled(void (*resolved)(bool), bool enabled);
void companion_confirm_cancel(void (*resolved)(bool));
void companion_session_context(const char *context, uint32_t identity);
void companion_session_refresh_context(void);
xiaotai_err_t companion_session_init(void);
xiaotai_err_t companion_at_init(void);
void companion_session_reset(void);
void companion_session_dispatch(const xiaotai_args_t *args);
bool companion_session_pending(const char *action, uint32_t revision);
void companion_session_accepted(const char *action, uint32_t revision);
xiaotai_err_t companion_session_result(const char *text, bool failed);
xiaotai_err_t companion_session_result_with_ack(const char *text, bool failed, void (*ack)(uint32_t), uint32_t token);
xiaotai_err_t companion_session_dismiss_result(void);
void companion_session_result_left(void);
xiaotai_err_t companion_setup_init(void);
void companion_setup_refresh(void);
void companion_setup_visible(void);
void companion_setup_reset(void);
const companion_page_t *companion_setup_page(void);
bool companion_room_maintenance(void);
bool companion_room_navigation_here(void);
bool companion_setup_can_retry(void);
bool companion_setup_cancel_from_key(void);
bool companion_setup_claim(const xiaotai_event_t *event);
xiaotai_err_t companion_home_init(void);
void companion_home_reset(void);
void companion_home_visible(void);
bool companion_page_may_sleep(void);
void companion_home_screen_dark(void);
void companion_home_status(xiaotai_view_t *view, int32_t screen);
uint8_t companion_home_voice_level(void);
void companion_home_ack(void);
void companion_home_interaction(void);
void companion_home_notice(const char *text, uint32_t ms);
void companion_home_session_notice(const char *text, uint32_t ms);
void companion_home_clear_session_notice(void);
bool companion_home_network_up(void);
xiaotai_err_t companion_settings_init(void);
void companion_settings_visible(void);
void companion_settings_reprovision_cancelled(void);
void companion_settings_reset(void);
const char *companion_save_failure_home(void);
void companion_settings_screen_changed(bool awake);
void companion_settings_code_visible(bool visible);
xiaotai_err_t companion_aux_init(void);
const char *companion_emotion_label(size_t index);
void companion_aux_visible(void);
void companion_aux_reset(void);
xiaotai_err_t companion_audio_init(void);
void companion_audio_leave(void);
void companion_audio_reset(void);
xiaotai_err_t companion_feedback_init(void);
void companion_feedback_reset(void);
void companion_feedback_visible(void);
bool companion_feedback_audio_error(void);
typedef enum {
    COMPANION_FEEDBACK_IDLE, COMPANION_FEEDBACK_INCOMING, COMPANION_FEEDBACK_DIALING,
    COMPANION_FEEDBACK_CONNECTING, COMPANION_FEEDBACK_CALL, COMPANION_FEEDBACK_TALK,
    COMPANION_FEEDBACK_ACTIVE, COMPANION_FEEDBACK_FAILED,
} companion_feedback_phase_t;
void companion_feedback_session(companion_feedback_phase_t phase, uint32_t generation);
void companion_feedback_room_attempt(bool started, bool failed, uint32_t activity);
void companion_feedback_wake_ack(uint32_t generation, uint32_t valid_ms, bool male);
void companion_feedback_wake_ack_at_admission(uint32_t valid_ms, bool male);
void companion_feedback_cancel_wake_ack(bool male);
