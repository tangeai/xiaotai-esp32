// SPDX-License-Identifier: MIT
#pragma once
/* Application "alarm": an alarm clock on a display.
 *
 * What the application is: a clock screen (the theme's CLOCK view: time,
 * date and what rings next), the list of alarms and the form that sets one
 * (time, the days it repeats, on or off), a countdown, its settings (how long
 * it rings, the snooze interval, the time zone when the product offers it)
 * and the ringing screen with "stop" and "snooze". Alarms and countdowns are
 * the clock module's schedules (xiaotai/modules/clock.h): alarms are kept
 * across restarts there, and AT+CLOCK reaches the same entries.
 *
 * Ringing. When an entry is due (XIAOTAI_EVENT_CLOCK_DUE) the application
 * begins its activity "alarm" (screen and speaker), wakes the screen, shows
 * the ringing screen and switches on the feedback state
 * XIAOTAI_APP_ALARM_STATE_RINGING. It rings until the user taps "stop", for
 * at most the setting "alarm.ring_s", or until a session with a higher
 * priority takes the device (an incoming call). "Snooze" stops it and rings
 * again after the setting "alarm.snooze_min" (a countdown of the clock
 * module, labelled with the alarm's time). While another session is running
 * (a call, a conversation with the assistant, a room) a due entry does not
 * interrupt it: the application does not claim the event and the entry does
 * not ring (the clock module still counts it as fired). A second entry due
 * while one rings replaces it on the ringing screen.
 *
 * Several applications (xiaotai/framework.h, "Which application presents an
 * event"): the manifest lists "clock.due" in "handles", and
 * xiaotai_app_alarm_claim() accepts it while the device is idle or already
 * ringing, so the framework brings the application forward; it gives the
 * screen back with xiaotai_app_return() when the ringing ends. The clock
 * screen is a page of the home pager (xiaotai_app_next / _previous).
 *
 * Modules. Required: clock, and the engines ui, feedback, sounds, eventlog,
 * at. Needs a display and a speaker (the manifest's "needs").
 *
 * Tags as in xiaotai/framework.h. Every handler runs on the owner task. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/framework.h"
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------- configuration */

/* Product configuration. Every field: 0 selects the default. */
typedef struct {
    /* Default of "alarm.ring_s": one of 30, 60, 120, 300; default 60. */
    uint16_t ring_s;
    /* Step of the minute field of the alarm form, 1..30; default 5. */
    uint8_t minute_step;
    /* Default of "alarm.snooze_min": one of 5, 10, 15; default 5. */
    uint8_t snooze_min;
} xiaotai_app_alarm_config_t;

/* [glue] Before the application's init. NULL: all defaults.
 * XIAOTAI_ERR_INVALID_ARG for a value that is not one of the choices,
 * XIAOTAI_ERR_INVALID_STATE after init. */
xiaotai_err_t xiaotai_app_alarm_configure(const xiaotai_app_alarm_config_t *config);

/* [owner] Whether this application presents the event (manifest "claims"):
 * XIAOTAI_EVENT_CLOCK_DUE while no activity runs or this application's own
 * ringing is the current one. Changes nothing. */
bool xiaotai_app_alarm_claim(const xiaotai_event_t *event);

/* ------------------------------------------------------------------ settings */

/* How long it rings at most, seconds: 30, 60, 120 or 300. Key "alarm_ring". */
#define XIAOTAI_SETTING_APP_ALARM_RING_S "alarm.ring_s"
/* Snooze interval, minutes: 5, 10 or 15. Key "alarm_snooze". */
#define XIAOTAI_SETTING_APP_ALARM_SNOOZE_MIN "alarm.snooze_min"

/* -------------------------------------------------------------------- pages */

/* Page ids (xiaotai_view_t.id). Appended, never renamed. */
#define XIAOTAI_APP_ALARM_PAGE_HOME "alarm.home"          /* CLOCK: time, date, what rings next */
#define XIAOTAI_APP_ALARM_PAGE_LIST "alarm.list"          /* LIST: one row per alarm */
#define XIAOTAI_APP_ALARM_PAGE_EDIT "alarm.edit"          /* FORM: hour, minute, repeat, on/off */
#define XIAOTAI_APP_ALARM_PAGE_RINGING "alarm.ringing"    /* CLOCK: what rings, "snooze", "stop" */
#define XIAOTAI_APP_ALARM_PAGE_TIMER "alarm.timer"        /* FORM: minutes and start, or time left and cancel */
#define XIAOTAI_APP_ALARM_PAGE_SETTINGS "alarm.settings"  /* FORM: ring length, snooze, time zone */

/* ------------------------------------------------------------------ intents */

/*   alarm.home     args.value 0: the clock screen (brings the application
 *                  forward); 1 / -1: the next / previous application of the
 *                  home pager. Ignored while it rings.
 *   alarm.list     the list of alarms
 *   alarm.edit     args.value: the alarm's id, 0 for a new one
 *   alarm.hour, alarm.minute, alarm.repeat, alarm.enabled
 *                  form fields: args.value the new value (repeat: the index
 *                  of XIAOTAI_APP_ALARM_REPEAT_*)
 *   alarm.save     store the form (xiaotai_schedule_add or _update)
 *   alarm.delete   remove the alarm the form shows
 *   alarm.timer    the countdown page; alarm.timer.minutes args.value
 *                  1..180; alarm.timer.start; alarm.timer.cancel removes
 *                  the running countdowns
 *   alarm.settings the settings page; alarm.setting.ring, .snooze, .zone
 *                  args.value: the index of the chosen option
 *   alarm.back     one page up: form -> list -> clock screen
 *   alarm.stop     stop ringing; alarm.snooze stop and ring again later */
#define XIAOTAI_INTENT_APP_ALARM_HOME "alarm.home"
#define XIAOTAI_INTENT_APP_ALARM_LIST "alarm.list"
#define XIAOTAI_INTENT_APP_ALARM_EDIT "alarm.edit"
#define XIAOTAI_INTENT_APP_ALARM_HOUR "alarm.hour"
#define XIAOTAI_INTENT_APP_ALARM_MINUTE "alarm.minute"
#define XIAOTAI_INTENT_APP_ALARM_REPEAT "alarm.repeat"
#define XIAOTAI_INTENT_APP_ALARM_ENABLED "alarm.enabled"
#define XIAOTAI_INTENT_APP_ALARM_SAVE "alarm.save"
#define XIAOTAI_INTENT_APP_ALARM_DELETE "alarm.delete"
#define XIAOTAI_INTENT_APP_ALARM_TIMER "alarm.timer"
#define XIAOTAI_INTENT_APP_ALARM_TIMER_MINUTES "alarm.timer.minutes"
#define XIAOTAI_INTENT_APP_ALARM_TIMER_START "alarm.timer.start"
#define XIAOTAI_INTENT_APP_ALARM_TIMER_CANCEL "alarm.timer.cancel"
#define XIAOTAI_INTENT_APP_ALARM_SETTINGS "alarm.settings"
#define XIAOTAI_INTENT_APP_ALARM_SETTING_RING "alarm.setting.ring"
#define XIAOTAI_INTENT_APP_ALARM_SETTING_SNOOZE "alarm.setting.snooze"
#define XIAOTAI_INTENT_APP_ALARM_SETTING_ZONE "alarm.setting.zone"
#define XIAOTAI_INTENT_APP_ALARM_BACK "alarm.back"
#define XIAOTAI_INTENT_APP_ALARM_STOP "alarm.stop"
#define XIAOTAI_INTENT_APP_ALARM_SNOOZE "alarm.snooze"

/* Choices of the repeat field, in this order. */
typedef enum {
    XIAOTAI_APP_ALARM_REPEAT_ONCE = 0,
    XIAOTAI_APP_ALARM_REPEAT_DAILY,
    XIAOTAI_APP_ALARM_REPEAT_WORKDAYS,   /* Monday .. Friday */
    XIAOTAI_APP_ALARM_REPEAT_WEEKEND,
    XIAOTAI_APP_ALARM_REPEAT_COUNT,
} xiaotai_app_alarm_repeat_t;

/* ------------------------------------------------------------------- events */

/* Ringing began or ended. args.value 1 began, 0 ended; args.text: the
 * entry's label. */
#define XIAOTAI_EVENT_APP_ALARM_RINGING "alarm.ringing"

/* Feedback state switched on while it rings (sound "ring.incoming"). */
#define XIAOTAI_APP_ALARM_STATE_RINGING "alarm.ringing"

/* Activity name and its default priority (manifest "activities"): every
 * session of the modules ranks higher, so they take the device from it. */
#define XIAOTAI_APP_ALARM_ACTIVITY "alarm"

/* -------------------------------------------------------------- diagnostics */

typedef struct {
    bool foreground;
    bool ringing;
    uint8_t ringing_id;        /* the entry that rings, 0 when none */
    char page[24];             /* the page shown, "" in the background */
    uint32_t rings;            /* ringing began */
    uint32_t stops;            /* stopped by the user */
    uint32_t snoozes;          /* snoozed by the user */
    uint32_t timeouts;         /* stopped after the ring length */
    uint32_t revoked;          /* taken over by another session, or the application left the screen */
    uint32_t refused;          /* the activity was refused when it was claimed */
    uint32_t saves, save_failures;
} xiaotai_app_alarm_status_t;

/* [owner] */
void xiaotai_app_alarm_status(xiaotai_app_alarm_status_t *status);

/* ---------------------------------------------------------------- lifecycle */

/* Called through the generated tables. init registers the two settings, the
 * feedback table, the intents, the subscriptions and AT+ALARM:
 *   AT+ALARM   +ALARM: foreground=<0|1> page=<id|-> ringing=<0|1> id=<n>
 *              rings=<n> stops=<n> snoozes=<n> timeouts=<n> revoked=<n>
 *              refused=<n> saves=<n> save_failures=<n> */
xiaotai_err_t xiaotai_app_alarm_init(void);
void xiaotai_app_alarm_start(void);
void xiaotai_app_alarm_foreground(bool foreground);
xiaotai_err_t xiaotai_app_alarm_page_step(int direction);

/* Host tests: forget state and configuration. */
void xiaotai_app_alarm_reset(void);

#ifdef __cplusplus
}
#endif
