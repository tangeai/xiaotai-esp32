// SPDX-License-Identifier: MIT
#pragma once
/* Interaction engine: polls the board's input parts in one place (its task,
 * or an owner-task timer, see Threading below), recognizes
 * gestures with the timing an application's interaction profile gives, and
 * posts the intent the profile names for (role, gesture, context). The same
 * application thus runs with one key, three keys or a knob by changing a
 * table, and a new input is a new row.
 *
 * Inputs and gestures
 * - A part that reports edges (XIAOTAI_INPUT_PRESSED and _RELEASED in
 *   desc.events) debounces by itself, with the time its board gives it, and
 *   never reports a key that was held when it opened until that key was
 *   released once (xiaotai/hal.h). The edges are timed here: clicks are
 *   counted until the multi-click window closes, a long press fires once
 *   while still held. A RELEASED without a PRESSED before it is ignored.
 * - A part that reports finished gestures (a click from a power-management
 *   chip, a knob step) is passed through: CLICK counts as one click resolved
 *   at once, DOUBLE_CLICK as two, LONG_PRESS as a long press.
 * - A part that reports XIAOTAI_INPUT_DATA (a card reader, a voice chip)
 *   produces XIAOTAI_GESTURE_DATA with the payload as the intent's text.
 * - A XIAOTAI_INPUT_CONTACT part (a door contact) holds a state; it is not a
 *   key. Its PRESSED and RELEASED are not timed into gestures: each becomes
 *   XIAOTAI_GESTURE_ACTIVE or _INACTIVE at once, the state its first poll
 *   reports after open included (a RELEASED at rest is not an orphan
 *   release). They emit no XIAOTAI_EVENT_INPUT_BEGAN and
 *   are never dropped as stale (a door that opened stays open whatever the
 *   screen shows); while matching is suspended (xiaotai_interaction_enable)
 *   the last change of each contact is kept and matched on resume.
 * - A XIAOTAI_INPUT_SLIDER part (a slider, a potentiometer, a threshold dial)
 *   has a position, not presses. Its XIAOTAI_INPUT_LEVEL is not timed into
 *   gestures: it becomes XIAOTAI_GESTURE_LEVEL, posted with args.value = the
 *   level read_level() gives (desc.level_min..level_max), the level its first
 *   poll reports after open included. Like a contact it emits no
 *   XIAOTAI_EVENT_INPUT_BEGAN, is never dropped as stale, and while matching
 *   is suspended only its last level is kept and matched on resume. A level
 *   is matched at most once per XIAOTAI_INTERACTION_LEVEL_MS per input: a
 *   control moved continuously reports a new level every poll, and the owner
 *   queue must not fill with intermediate positions. Levels that arrive
 *   within that interval replace each other, and the last one is matched when
 *   it ends, so the final position always arrives. A failed read_level()
 *   matches nothing and is counted in data_errors.
 * - Pointer input on a display is not handled here: the UI system turns it
 *   into the intents of the view.
 *
 * Staleness: a gesture belongs to what the user saw when it began. The engine
 * emits XIAOTAI_EVENT_INPUT_BEGAN at the first press of a gesture, so the
 * application can note the state the user reacted to, and it drops gestures
 * in flight when asked (xiaotai_interaction_invalidate, or the profile's
 * flags). Both events are queued before the intent, in order.
 *
 * Threading: the poll task only reads parts, runs the recognizer (no
 * allocation, no logging: it counts) and posts. Rules are evaluated on the
 * poll task against the context last set. Where the build option
 * XIAOTAI_INTERACTION_TASK is 0 (the default without external RAM) there is
 * no poll task: an owner-task timer runs the same poll step on the same
 * schedule (xiaotai/interaction_config.h has the rule and its cost), and
 * "the poll task" below means that timer. Intents and events are queued to
 * the owner task in both forms, so their order is the same. Tags as in
 * xiaotai/framework.h. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/interaction_config.h"
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------- lifecycle */

/* Engine "interaction" (engine.json). init creates the registry, the command
 * AT+KEY (injection, state), the diagnostics provider "interaction" and
 * subscribes to XIAOTAI_EVENT_FOREGROUND. start opens every input part of the
 * board (up to XIAOTAI_INTERACTION_INPUTS_MAX) when at least one profile is
 * registered, since a layer pushed later may name any role, and starts the
 * poll task (or arms the poll timer, XIAOTAI_INTERACTION_TASK); with no
 * profile or no input part neither exists. Inputs
 * work from then on, whether or not the
 * device is online: what a gesture may do in a given situation is decided by
 * the handler of its intent. */
xiaotai_err_t xiaotai_engine_interaction_init(void);
void xiaotai_engine_interaction_start(void);
/* Host tests: stop the task and forget profiles. */
void xiaotai_interaction_reset(void);

/* ---------------------------------------------------------------- gestures */

typedef enum {
    XIAOTAI_GESTURE_CLICK = 0,    /* `count` clicks: 1 single, 2 double, 4 quadruple, ... */
    XIAOTAI_GESTURE_LONG_PRESS,   /* held for long_press_ms; fires once, while still held */
    XIAOTAI_GESTURE_PRESS,        /* stable press (for hold-to-talk keys) */
    XIAOTAI_GESTURE_RELEASE,      /* stable release after a PRESS that fired a rule */
    XIAOTAI_GESTURE_TURN_UP,      /* one encoder step */
    XIAOTAI_GESTURE_TURN_DOWN,
    XIAOTAI_GESTURE_DATA,         /* a payload arrived; posted with args.text = the payload, cut to
                                     XIAOTAI_ARG_TEXT_MAX - 1 bytes at a character boundary */
    XIAOTAI_GESTURE_ACTIVE,       /* a contact became active (the door opened) */
    XIAOTAI_GESTURE_INACTIVE,     /* a contact returned to rest */
    XIAOTAI_GESTURE_LEVEL,        /* a slider's level changed; posted with args.value = the level
                                     (the rule's value is not used) */
} xiaotai_gesture_t;

/* Shortest interval between two LEVEL gestures of one input. */
#define XIAOTAI_INTERACTION_LEVEL_MS 100U

/* One row: when the input with `role` produces `gesture` in `context`, post
 * `intent`. Rows are tried in order; the first whose role, gesture, count and
 * context all match decides, also when its intent is NULL (a gesture that
 * deliberately does nothing here). */
typedef struct {
    const char *role;       /* input part role ("main", "volume_up"); NULL: any input */
    uint8_t gesture;        /* xiaotai_gesture_t */
    uint8_t count;          /* CLICK: the click count; otherwise 0 */
    const char *context;    /* NULL: any; otherwise must equal the current context */
    /* Posted with args.value = value (XIAOTAI_GESTURE_LEVEL: the level);
     * NULL: nothing. args.text is empty except for XIAOTAI_GESTURE_DATA. */
    const char *intent;
    int32_t value;
    /* Optional event emitted when the row decided, whether or not it has an
     * intent ("input.tap", "input.blocked"); applications bind feedback
     * effects to it. args.text: the role. */
    const char *event;
} xiaotai_interaction_rule_t;

/* xiaotai_interaction_profile_t.flags */
enum {
    /* Drop gestures in flight when the display's page changes
     * (XIAOTAI_EVENT_VIEW_CHANGED): a click meant for the old page must not
     * act on the new one. */
    XIAOTAI_INTERACTION_VOID_ON_VIEW = 1u << 0,
    /* Drop gestures in flight when the context changes. */
    XIAOTAI_INTERACTION_VOID_ON_CONTEXT = 1u << 1,
};

/* Which profiles apply. Every application registers its own profile (named
 * by `application`); the engine follows XIAOTAI_EVENT_FOREGROUND, so only the
 * rules of the application in the foreground are consulted. A gesture is
 * matched against, in this order, and the first matching rule decides:
 *   1. layers (xiaotai_interaction_push), the newest first: what a module or
 *      an activity needs for a while ("any key stops the ring");
 *   2. the profiles of the foreground application, in registration order;
 *   3. global profiles (application NULL), in registration order.
 * The set in force is taken at the first press of a gesture (for a contact
 * or a slider, when the change is matched). Pushing or
 * popping a layer and a change of foreground application drop gestures in
 * flight (counted in invalidated), so a click begun for one owner never acts
 * for another.
 *
 * Timing 0 selects the default. A gesture takes multi_click_ms, long_press_ms
 * and flags from the first profile of the set, in the order above, that has
 * a rule for the input's role (or a rule with role NULL). Clicks resolve as
 * follows: when the count reaches the largest CLICK count any rule of the set
 * has for that role, the gesture fires at that release; otherwise it fires
 * when multi_click_ms passed after the last release without a new press. A
 * count no rule names fires nothing. A long press discards clicks counted
 * before it, and its release is not a click. */
typedef struct {
    const char *name;                         /* "one-key-menu"; for diagnostics, at most 15 characters */
    const xiaotai_interaction_rule_t *rules;
    size_t rule_count;
    uint16_t multi_click_ms;                  /* default 350 */
    uint16_t long_press_ms;                   /* default 1200 */
    uint32_t flags;                           /* XIAOTAI_INTERACTION_* */

    /* Members below default to "absent" when the structure is zeroed. */

    /* Manifest name of the application the profile belongs to
     * ("companion"): its rules apply only while that application is in the
     * foreground. NULL: a global profile, consulted after the foreground
     * application's (a product-wide reset key). Ignored for layers. */
    const char *application;
} xiaotai_interaction_profile_t;

#define XIAOTAI_INTERACTION_MULTI_CLICK_MS 350U
#define XIAOTAI_INTERACTION_LONG_PRESS_MS 1200U

/* [init] Add a profile. Kept by pointer with everything it points to
 * (constant data). Several applications may each register one or more.
 * XIAOTAI_ERR_INVALID_ARG: no rules, a CLICK rule with count 0, or
 * multi_click_ms not below long_press_ms; XIAOTAI_ERR_INVALID_STATE: after
 * start; XIAOTAI_ERR_NO_MEM: more than XIAOTAI_INTERACTION_PROFILES_MAX
 * (xiaotai/interaction_config.h). */
xiaotai_err_t xiaotai_interaction_register(const xiaotai_interaction_profile_t *profile);

/* ------------------------------------------------------------------ layers */

/* [any] Put a profile above every registered one until it is popped: a module
 * or activity that owns the inputs for a while (a ring that any key stops, a
 * maintenance step that takes every key). A rule with intent NULL in a layer
 * swallows that gesture; a gesture no layer rule matches goes on to the
 * foreground application. The profile is kept by pointer until popped.
 * *id (required) is never 0. XIAOTAI_ERR_INVALID_ARG: as for register, or no
 * id; XIAOTAI_ERR_NO_MEM: XIAOTAI_INTERACTION_LAYERS_MAX layers are pushed
 * (xiaotai/interaction_config.h). */
xiaotai_err_t xiaotai_interaction_push(const xiaotai_interaction_profile_t *layer, uint32_t *id);
/* [any] Remove a layer. An unknown id and 0 are ignored. */
void xiaotai_interaction_pop(uint32_t id);

/* ----------------------------------------------------------------- context */

/* [any] The situation rules are matched against ("idle", "ringing", "call").
 * NULL or "": none (only rules with context NULL match). The string is
 * copied, at most XIAOTAI_INTERACTION_CONTEXT_MAX - 1 bytes. Applications set
 * it from their event handlers; a gesture resolves against the context at the
 * moment it fires. There is one context: it is the foreground application's,
 * which sets it in its foreground(true) handler (the engine does not reset it
 * on a change of foreground, because that handler runs before the event is
 * dispatched). Layers and global profiles match the same context. */
#define XIAOTAI_INTERACTION_CONTEXT_MAX 24U
void xiaotai_interaction_set_context(const char *context);

/* [any] Drop every gesture in flight: click counts are cleared and a press
 * that is down stays consumed until released. Counted. */
void xiaotai_interaction_invalidate(void);

/* [any] Suspend rule matching (inputs are still polled, so a held key is not
 * mistaken for a new press on resume). Used while the device must not react
 * (a maintenance step). */
void xiaotai_interaction_enable(bool enabled);

/* ------------------------------------------------------------------ events */

/* The first press of a gesture arrived. args.text: the role;
 * args.value: a gesture number (never 0, increasing, wraps past 0) that
 * XIAOTAI_EVENT_INPUT_REJECTED repeats. */
#define XIAOTAI_EVENT_INPUT_BEGAN "input.began"
/* A rule's intent could not be posted (nothing handles it, or the queue was
 * full). args.text: the intent name; args.value: the gesture number. */
#define XIAOTAI_EVENT_INPUT_REJECTED "input.rejected"

/* --------------------------------------------------------------- injection */

/* [any] Drive an input as if it had been operated: the poll task feeds the
 * edges of a synthetic key into the same recognizer, so timing, staleness and
 * rules are exercised as with the real key (test benches, the AT+KEY
 * command). clicks > 0: that many 100 ms presses 80 ms apart; clicks 0: one
 * press of hold_ms.
 * XIAOTAI_ERR_NOT_FOUND: no polled input has the role;
 * XIAOTAI_ERR_INVALID_STATE: the real input is down, a gesture of that input
 * is still resolving (clicks counted and the multi-click window open: an
 * injected click would join the real one into a double click), or an earlier
 * injection still runs; XIAOTAI_ERR_INVALID_ARG: clicks 0 and hold_ms 0, or
 * clicks above 8. Debouncing belongs to the part and is not seen here. On a
 * CONTACT input only clicks 0 is accepted: ACTIVE, then INACTIVE hold_ms
 * later (XIAOTAI_ERR_INVALID_STATE while the real contact is active). A
 * SLIDER input takes no injection: XIAOTAI_ERR_NOT_SUPPORTED. */
xiaotai_err_t xiaotai_interaction_inject(const char *role, uint8_t clicks, uint32_t hold_ms);

/* ------------------------------------------------------------- diagnostics */

typedef struct {
    uint32_t polls;
    uint32_t late_polls;       /* polls more than XIAOTAI_INTERACTION_LATE_MS after the previous one,
                                  with or without the poll task (not every owner-timer reschedule) */
    uint32_t max_gap_ms;
    uint32_t gestures;         /* gestures that reached rule matching */
    uint32_t posted;
    uint32_t unmatched;        /* gestures no rule named */
    uint32_t rejected;         /* posts that failed */
    uint32_t invalidated;      /* gestures dropped in flight */
    uint32_t orphan_releases;  /* RELEASED without a PRESSED before it */
    uint32_t data_errors;      /* XIAOTAI_INPUT_DATA whose read(), or LEVEL whose read_level(), failed */
    uint32_t injected;
} xiaotai_interaction_stats_t;

#define XIAOTAI_INTERACTION_LATE_MS 40U

void xiaotai_interaction_stats(xiaotai_interaction_stats_t *out);   /* [any] */

typedef struct {
    char role[16];
    char profile[16];          /* profile giving its timing now; "" when no rule names its role */
    bool edge_reporting;       /* the part reports XIAOTAI_INPUT_PRESSED and _RELEASED */
    bool down;                 /* pressed, by the last edge (real or injected) */
    bool injected;             /* an injection runs on this input */
    bool consumed;             /* the press that is down will produce nothing (long press fired, or dropped) */
    uint8_t clicks;            /* counted in the gesture in flight */

    /* Members below were appended; older readers ignore them. */
    int32_t level;             /* SLIDER: the last level read; otherwise 0 */
} xiaotai_interaction_input_t;

/* Input parts polled at most: XIAOTAI_INTERACTION_INPUTS_MAX
 * (xiaotai/interaction_config.h); parts beyond are left closed and counted in
 * the diagnostics provider. */

/* [any] State of polled input `index`; XIAOTAI_ERR_NOT_FOUND past the end. */
xiaotai_err_t xiaotai_interaction_input(size_t index, xiaotai_interaction_input_t *out);

#ifdef __cplusplus
}
#endif
