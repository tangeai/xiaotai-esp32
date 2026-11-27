// SPDX-License-Identifier: MIT
#pragma once
/* Feedback engine: presents state on what the board has besides a screen:
 * lights, a vibrator, the speaker. It is table driven. An application
 * registers a table of
 *   states   named conditions that last ("provisioning", "call.incoming").
 *            The application switches them on and off; the engine shows the
 *            light pattern of the active state with the highest priority and
 *            loops the sound of the active state with the highest priority
 *            that has one (a ring).
 *   effects  named one-shots ("tap", "warning", "call.ended"): a short light
 *            pattern that covers the state pattern for its duration, a
 *            vibration, a sound played once. Fired by name, or by a framework
 *            event bound in the row.
 * The engine knows no application, module or board: what a state means and
 * when it holds is decided by the application; which outputs exist comes from
 * the board's output parts (by role). It drives only outputs nobody owns and
 * only the kinds it renders: when start resolves the roles it takes a part
 * whose role family no component claimed (xiaotai_hal_output_owner() is
 * NULL; the lock module claims "door", xiaotai/hal.h) and whose kind is
 * XIAOTAI_OUTPUT_LIGHT, _RGB_LIGHT or _VIBRATOR. A claimed output, and a
 * relay or any other kind it does not render, is never driven, even when a
 * row names its role: the row's entry resolves to nothing, as for a role the
 * board lacks, and is counted (skipped_outputs). Sounds are
 * names in the sound index (xiaotai/sounds.h) and are played through a player
 * interface, by default the media engine's sound playback when the product
 * has the media engine (an optional dependency): without it there is no
 * default player, and rows with sounds drive only their lights and vibrators.
 *
 * Sound rules (one speaker, shared with sessions)
 * - One sound plays at a time; one more waits. A one-shot of lower rank than
 *   the one-shot that plays or waits is dropped. Otherwise it replaces the
 *   waiting one, and never the playing one, unless it says
 *   XIAOTAI_FEEDBACK_INTERRUPT: then it stops the playing one and starts.
 * - A one-shot stops the state loop for its duration; the loop resumes after.
 * - While the player reports the output busy (a session owns it) a one-shot
 *   is dropped, unless its row says XIAOTAI_FEEDBACK_WAIT: then it waits for
 *   the output, until replaced, cancelled or expired. A state loop that
 *   cannot play is tried again when the output becomes free.
 * - Groups (an application's numbering, 0: none) switch sounds as a set:
 *   a disabled group plays nothing (a voice-guidance switch).
 * Lights and vibration never wait: they render from the current table state.
 *
 * Threading: functions are [any] unless tagged; they take a short lock and
 * never block on playback or on an output part. Sound decisions are made on
 * the framework owner task (calls from other tasks are queued to it), which
 * is also where the player is called, completion callbacks run and the sound
 * timers (loop gaps, retries, expiry) run as owner-task timers
 * (xiaotai_timer_start). Light and vibration frames are produced only when
 * some table row resolves to a light or vibrator part it drives, and only
 * while something is shown: by the engine's own low-priority task
 * (XIAOTAI_FEEDBACK_TASK, the default with external RAM; a part write may
 * block up to 20 ms, which then delays nothing else), or by an owner-task
 * timer (the default without external RAM: no task stack, the writes run on
 * the owner task). A board without lights and vibrators has neither. Nothing
 * here may be called from an ISR or a real-time task. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/feedback_config.h"
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------- lifecycle */

/* Engine "feedback" (engine.json; the media engine, which owns the speaker,
 * is an optional dependency). init creates the registries, installs the
 * default player when the media engine is built in, and registers the command
 * AT+FEEDBACK (state; STATES, SET, FIRE, CANCEL, GROUP) and the diagnostics
 * provider "feedback"; start resolves the output roles of every table against
 * the board's unclaimed outputs (claims are complete once every init() ran)
 * and, when at least one role resolved to a light or vibrator,
 * starts the frame producer (the engine's task or the owner-task timer, see
 * XIAOTAI_FEEDBACK_TASK). */
xiaotai_err_t xiaotai_engine_feedback_init(void);
void xiaotai_engine_feedback_start(void);
/* Host tests: stop the task, forget tables, states and the player. */
void xiaotai_feedback_reset(void);

/* ------------------------------------------------------------------ lights */

typedef struct {
    uint8_t red, green, blue;
} xiaotai_rgb_t;

/* A light part has `pixels` lamps in a row (1 for a single lamp). Patterns
 * are defined on that row. Single-channel parts (a lamp of one colour, a
 * vibrator) take the brightest channel of the lamp's colour as their level.
 * Levels are 0..255 and multiply the colour (per channel: color * level /
 * 255, rounded); the part applies its own brightness limit.
 *
 * Time: every pattern is a pure function of t, the port's monotonic clock in
 * 32-bit milliseconds (it wraps), not of when the state or effect began.
 * Pattern times are used exactly as written (70, 75, 95, 110 ms keep their
 * tempo); only the output is sampled, once every XIAOTAI_FEEDBACK_FRAME_MS.
 * With p = period_ms, lamp i (0 at the left) and n lamps:
 *   OFF
 *   SOLID      color at level_max
 *   BREATHE    color at a linear triangle: with h = p / 2 and q = t % p,
 *              r = q < h ? q : p - q, level = level_min + (level_max -
 *              level_min) * r / h (integer division)
 *   BLINK      color at level_max while (t / p) is odd, level_min while even
 *   CHASE      head h = (t / p) % n in color at 255; the lamps at h - 1 and
 *              h - 2 (wrapping) in accent at 150 and 55; the rest off
 *   ALTERNATE  color at level_max on lamps with (i + t / p) even; the rest off
 *   FLASHES    with q = t % p and a = accent_ms: color at level_max while
 *              q < 2 * a * count and (q / a) is even (`count` flashes a long,
 *              a apart, repeated every p); off otherwise
 * Accent on BREATHE and SOLID (accent_mode), drawn over the base:
 *   DOT        lamp (t / accent_ms) % n in accent at accent_level
 *   CENTER     with n >= 2, lamp (n - 1) / 2 and lamp n / 2 (one lamp for odd
 *              n) in accent at accent_level; a single lamp shows no accent
 * accent_level 0 means 255: an accent that should not show is ACCENT_NONE. */
typedef enum {
    XIAOTAI_LIGHT_OFF = 0,
    XIAOTAI_LIGHT_SOLID,
    XIAOTAI_LIGHT_BREATHE,
    XIAOTAI_LIGHT_BLINK,
    XIAOTAI_LIGHT_CHASE,
    XIAOTAI_LIGHT_ALTERNATE,
    XIAOTAI_LIGHT_FLASHES,
} xiaotai_light_kind_t;

typedef enum {
    XIAOTAI_LIGHT_ACCENT_NONE = 0,
    XIAOTAI_LIGHT_ACCENT_DOT,
    XIAOTAI_LIGHT_ACCENT_CENTER,
} xiaotai_light_accent_t;

typedef struct {
    uint8_t kind;              /* xiaotai_light_kind_t */
    uint8_t accent_mode;       /* xiaotai_light_accent_t */
    uint8_t level_min, level_max;
    uint8_t accent_level;
    uint8_t count;             /* FLASHES */
    uint16_t period_ms;
    uint16_t accent_ms;
    xiaotai_rgb_t color;
    xiaotai_rgb_t accent;
} xiaotai_light_pattern_t;

/* Output period of lights and vibration: a frame is computed from t and
 * written at most this often. Pattern times are not rounded to it. */
#define XIAOTAI_FEEDBACK_FRAME_MS 40U
/* Lamps rendered per light part; longer rows repeat the pattern. */
#define XIAOTAI_FEEDBACK_PIXELS_MAX 16U

/* --------------------------------------------------------------- vibration */

typedef struct {
    uint8_t strength;          /* 0: none; 1..255 */
    uint8_t repeat;            /* pulses; 0 with a state: repeat while the state is shown */
    uint16_t on_ms, off_ms;
} xiaotai_vibration_t;

/* ------------------------------------------------------------------ sounds */

/* xiaotai_feedback_sound_t.flags */
enum {
    /* One-shot: wait for the output instead of being dropped when busy. */
    XIAOTAI_FEEDBACK_WAIT = 1u << 0,
    /* One-shot: stop the one-shot that is playing (not only replace the
     * waiting one) when this is accepted. */
    XIAOTAI_FEEDBACK_INTERRUPT = 1u << 1,
};

typedef struct {
    const char *name;          /* sound index name ("ring.incoming"); NULL: no sound */
    uint16_t gap_ms;           /* state loops: silence between repeats */
    uint8_t group;             /* 0: not switched */
    uint8_t flags;             /* XIAOTAI_FEEDBACK_* */
    uint8_t rank;              /* one-shots: lower than the one playing or waiting is dropped */
} xiaotai_feedback_sound_t;

/* ------------------------------------------------------------------ tables */

/* One more output a row drives, addressed by the part's role: a second light,
 * a second vibrator (only unclaimed parts of the kinds the engine renders,
 * see the top of this header). The pattern
 * is the same generator as a light's; a single-channel part takes the
 * brightest channel (so a vibrator pulse train is FLASHES or BLINK in white).
 * Per part the same rules apply as to a row's light: the highest-priority
 * active state that names the part wins, an effect covers it for its
 * duration, a role the board lacks is a no-op.
 *
 * Extension: an output kind that needs more than a level over time (a piezo
 * tone) appends a member here (0 meaning "not used") and the part an optional
 * function; existing tables stay valid as written. */
typedef struct {
    const char *role;                  /* output part role; NULL matches no part */
    xiaotai_light_pattern_t pattern;
} xiaotai_feedback_output_t;

/* Outputs of one row besides its light and vibration. */
#define XIAOTAI_FEEDBACK_OUTPUTS_MAX 4U

typedef struct {
    const char *name;                  /* unique over all tables */
    uint8_t priority;                  /* higher wins; equal: the earlier row */
    const char *light;                 /* role of the light part ("status"); NULL: the first light it drives */
    xiaotai_light_pattern_t pattern;   /* kind OFF with a sound: a sound-only state */
    xiaotai_vibration_t vibration;     /* on the first vibrator it drives */
    xiaotai_feedback_sound_t sound;    /* looped while this is the winning sound state */

    /* Members below default to "absent" when the structure is zeroed. */
    const xiaotai_feedback_output_t *outputs;   /* further outputs by role */
    size_t output_count;                        /* at most XIAOTAI_FEEDBACK_OUTPUTS_MAX */
} xiaotai_feedback_state_t;

typedef struct {
    const char *name;                  /* unique over all tables */
    /* Framework event that fires the effect by itself, with default options;
     * NULL: only xiaotai_feedback_fire(). */
    const char *event;
    const char *light;
    xiaotai_light_pattern_t pattern;   /* covers the state pattern on that light */
    uint16_t duration_ms;              /* of the light pattern and the outputs; 0: none of them */
    xiaotai_vibration_t vibration;     /* on the first vibrator it drives */
    xiaotai_feedback_sound_t sound;    /* played once */

    /* Members below default to "absent" when the structure is zeroed. */
    const xiaotai_feedback_output_t *outputs;   /* further outputs by role, each covered for duration_ms */
    size_t output_count;                        /* at most XIAOTAI_FEEDBACK_OUTPUTS_MAX */
} xiaotai_feedback_effect_t;

typedef struct {
    const xiaotai_feedback_state_t *states;
    size_t state_count;
    const xiaotai_feedback_effect_t *effects;
    size_t effect_count;
} xiaotai_feedback_table_t;

/* [init] Add an application's table. The table and everything it points to
 * are kept by pointer (constant data). Several tables may be registered; a
 * role the board lacks makes that row's light or vibration a no-op, which is
 * how one table serves boards with and without the part.
 * XIAOTAI_ERR_INVALID_ARG: a row without a name, or more than
 * XIAOTAI_FEEDBACK_OUTPUTS_MAX outputs; XIAOTAI_ERR_INVALID_STATE: a
 * name registered before, or registration after start; XIAOTAI_ERR_NO_MEM:
 * more than XIAOTAI_FEEDBACK_TABLES_MAX tables or XIAOTAI_FEEDBACK_STATES_MAX
 * states in total (xiaotai/feedback_config.h). */
xiaotai_err_t xiaotai_feedback_register(const xiaotai_feedback_table_t *table);

/* ------------------------------------------------------------------ states */

/* Switch a state on or off. Setting what is already set does nothing.
 * XIAOTAI_ERR_NOT_FOUND: no such state. */
xiaotai_err_t xiaotai_feedback_set(const char *state, bool active);
bool xiaotai_feedback_is_set(const char *state);
/* Name of the state whose pattern the first light shows; NULL: none. */
const char *xiaotai_feedback_shown(void);

/* ----------------------------------------------------------------- effects */

/* Result of a one-shot, delivered once on the owner task:
 *   XIAOTAI_OK                 the sound played to its end (or the effect has no sound)
 *   XIAOTAI_ERR_INVALID_STATE  dropped: output busy, group disabled, outranked, replaced,
 *                              cancelled, cut by a session, or its generation is not open
 *   XIAOTAI_ERR_TIMEOUT        expired before it could start
 *   XIAOTAI_ERR_NOT_FOUND      the sound index has no such name
 *   other                      the player's error */
typedef void (*xiaotai_feedback_done_fn_t)(xiaotai_err_t result, void *ctx);

/* Sounds of one piece, the row's own included (a prompt and the six digits of
 * a code are seven). */
#define XIAOTAI_FEEDBACK_SEQUENCE_MAX 8U

typedef struct {
    /* Sounds appended to the row's sound and played as one piece without a
     * gap (a prompt followed by the digits of a code). The array is borrowed
     * for the call; the names have static lifetime (sound index names). At
     * most XIAOTAI_FEEDBACK_SEQUENCE_MAX - 1 names. */
    const char *const *then;
    size_t then_count;
    /* Media session generation the sound belongs to (0: none). The sound is
     * played only while that generation is the open media session and is
     * dropped (counted) otherwise, so a prompt for a session that ended, or
     * for one that never opened, is never heard. */
    uint32_t generation;
    /* The sound must start within this time or is dropped (0: no limit). */
    uint32_t valid_ms;
    /* Play although the row's group is disabled (an explicit replay). */
    bool force;
    xiaotai_feedback_done_fn_t done;   /* may be NULL */
    void *ctx;
} xiaotai_feedback_options_t;

/* Fire an effect. options NULL: defaults. Light and vibration start at once;
 * the sound follows the rules at the top. The return value tells whether the
 * effect was accepted; the fate of an accepted sound arrives through done.
 * XIAOTAI_ERR_NOT_FOUND: no such effect; XIAOTAI_ERR_INVALID_ARG: too many
 * `then` names; XIAOTAI_ERR_NO_MEM: called off the owner task and the queue
 * to it is full (nothing ran). On the owner task, XIAOTAI_ERR_INVALID_STATE:
 * the sound was dropped at once (light and vibration still ran; done is not
 * called). Called from another task the sound decision is queued: the call
 * returns XIAOTAI_OK and a drop is reported through done. */
xiaotai_err_t xiaotai_feedback_fire(const char *effect, const xiaotai_feedback_options_t *options);

/* Drop the waiting one-shot if it is this effect; with stop_playing also stop
 * it when it plays. effect NULL: any. Its done receives
 * XIAOTAI_ERR_INVALID_STATE. XIAOTAI_OK: ran on the owner, was queued, or no
 * registered effect matches. XIAOTAI_ERR_NO_MEM: the owner queue is full;
 * nothing ran, retry from the caller when it can accept work again.
 * XIAOTAI_ERR_INVALID_STATE: feedback is not running.
 * XIAOTAI_ERR_INVALID_ARG: effect is an empty string. */
xiaotai_err_t xiaotai_feedback_cancel(const char *effect, bool stop_playing);
/* Drop the waiting one-shot if it belongs to the group (and stop it when it
 * plays, with stop_playing). Same admission results as cancel. Group 0
 * matches ungrouped sounds; above XIAOTAI_FEEDBACK_GROUPS_MAX is invalid. */
xiaotai_err_t xiaotai_feedback_cancel_group(uint8_t group, bool stop_playing);
/* Enable or disable a group (all enabled at start). Disabling drops the
 * waiting sound of the group and stops the playing one, loops included.
 * group 1..XIAOTAI_FEEDBACK_GROUPS_MAX. */
#define XIAOTAI_FEEDBACK_GROUPS_MAX 8U
xiaotai_err_t xiaotai_feedback_group_enable(uint8_t group, bool enabled);
bool xiaotai_feedback_group_enabled(uint8_t group);

/* True while a one-shot sound waits or plays (callers that must not restart
 * before a prompt is out). */
bool xiaotai_feedback_sound_pending(void);

/* ------------------------------------------------------------------ player */

/* How the engine reaches the speaker. The default player is the media
 * engine's sound playback (xiaotai/media.h) when the product has the media
 * engine, none otherwise; a product may replace it and tests install fakes.
 * Every function is called on the owner task and must not block. Sounds the
 * engine does not own (a module's own announcement played straight through
 * the media engine) reach it only as XIAOTAI_ERR_BUSY from play(): its rank
 * rule does not see them. */

/* repeat value of a state loop: until stopped. */
#define XIAOTAI_FEEDBACK_FOREVER UINT16_MAX

typedef struct {
    const char *const *names;  /* sound index names played back to back; borrowed for the call */
    size_t count;              /* 1..XIAOTAI_FEEDBACK_SEQUENCE_MAX */
    uint16_t repeat;           /* extra passes after the first; XIAOTAI_FEEDBACK_FOREVER for a state loop */
    uint16_t gap_ms;           /* silence between passes */
    uint32_t generation;       /* media session generation the sound belongs to; 0: none */
    uint32_t token;            /* identifies the request; never 0 */
} xiaotai_feedback_play_t;

typedef struct {
    /* [owner] Start the request. XIAOTAI_ERR_BUSY: a session owns the output
     * or a sound the engine does not own plays with precedence;
     * XIAOTAI_ERR_NOT_FOUND: unknown name; XIAOTAI_ERR_INVALID_STATE:
     * `generation` is not the open session, or the speaker is muted;
     * XIAOTAI_ERR_NOT_SUPPORTED: no output, or no decoder for the sound. On
     * XIAOTAI_OK the player later calls xiaotai_feedback_player_done()
     * exactly once for the token; on an error it never does. */
    xiaotai_err_t (*play)(void *ctx, const xiaotai_feedback_play_t *request);
    /* [owner] Stop the token's sound at the next block boundary; done follows
     * with XIAOTAI_ERR_INVALID_STATE. Unknown tokens are ignored. */
    void (*stop)(void *ctx, uint32_t token);
    /* [owner] True while a one-shot could not start (a session owns the
     * output). */
    bool (*busy)(void *ctx);
    void *ctx;
} xiaotai_feedback_player_t;

/* [init] Replace the player. The structure is kept by pointer. NULL: no
 * sounds (rows with sounds complete with XIAOTAI_ERR_NOT_SUPPORTED). */
void xiaotai_feedback_set_player(const xiaotai_feedback_player_t *player);
/* [any, not an ISR, not a real-time task] The player finished a token.
 * XIAOTAI_OK: played to the end (every pass); XIAOTAI_ERR_INVALID_STATE:
 * stopped or cut by a session. Queued to the owner task when called from
 * another one. A token that is no longer current is counted and ignored. */
void xiaotai_feedback_player_done(uint32_t token, xiaotai_err_t result);
/* [any] The output became free: a waiting one-shot or a state loop may start
 * now. Players call it when a session releases the output; without it the
 * engine retries every XIAOTAI_FEEDBACK_RETRY_MS while something waits. */
void xiaotai_feedback_player_idle(void);
#define XIAOTAI_FEEDBACK_RETRY_MS 100U

/* ------------------------------------------------------------- diagnostics */

typedef struct {
    uint32_t fired;            /* effects accepted */
    uint32_t played;           /* one-shot sounds completed */
    uint32_t dropped_busy;
    uint32_t dropped_group;
    uint32_t dropped_expired;
    uint32_t dropped_rank;     /* one-shots of lower rank than the one playing or waiting */
    uint32_t dropped_stale;    /* one-shots whose generation was not the open session */
    uint32_t replaced;         /* waiting sounds replaced by a newer one */
    uint32_t cancelled;
    uint32_t stale_tokens;     /* player_done for a token no longer current */
    uint32_t loops;            /* state loop repetitions started */
    uint32_t player_errors;
    uint32_t light_errors;     /* output part write failures */
    uint32_t late_frames;      /* light frames more than one period late */

    /* Members below were appended; older readers ignore them. */
    /* Row entries (light, vibration, outputs) whose role named a part the
     * engine does not drive: claimed by another component, or of a kind it
     * does not render (a relay). Counted once per entry at start. */
    uint32_t skipped_outputs;
} xiaotai_feedback_stats_t;

void xiaotai_feedback_stats(xiaotai_feedback_stats_t *out);

#ifdef __cplusplus
}
#endif
