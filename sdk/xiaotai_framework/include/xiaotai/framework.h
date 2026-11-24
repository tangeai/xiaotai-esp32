// SPDX-License-Identifier: MIT
#pragma once
/* XiaoTai runtime framework: what applications and base modules build on.
 *
 * Layers: applications (alarm clock, door station, companion…) use base modules
 * (AI talk, calls, provisioning…); both are optional and selected per product.
 * The framework is the one required part: it registers and starts them, routes
 * intents and events by name, decides which application presents a module
 * event, arbitrates the foreground activity, performs the orderly restart,
 * and holds the registries for settings, commands (AT) and diagnostics.
 *
 * Threading: one owner task runs every handler and callback below, one at a
 * time, so module and application state needs no locks. Other tasks only post
 * (intents, events, commands, xiaotai_run). Posting never blocks: a full queue
 * is XIAOTAI_ERR_NO_MEM and the caller decides whether to try again.
 *
 * Owner task: its stack is XIAOTAI_FRAMEWORK_OWNER_STACK_BYTES
 * (xiaotai/framework_config.h; 24576 bytes in external RAM where the board has
 * it, 6144 bytes otherwise) and its queue holds XIAOTAI_FRAMEWORK_QUEUE_DEPTH
 * messages of about 100 bytes each (BULK memory). Every handler of every
 * engine, module and application shares that stack, so a handler, with what it
 * calls, uses at most XIAOTAI_FRAMEWORK_HANDLER_STACK_BYTES of it: larger
 * buffers (a signal being parsed, a request body being built) come from BULK
 * memory or the component's own state. The "framework" diagnostic section
 * reports the stack size and the queue's peak and overflows.
 *
 * Task tags. They are defined here once and mean the same in every SDK header,
 * the port's included (the port sits below the framework but uses these
 * words; it adds [isr] only):
 *   [glue]   only on the product's main task before the framework starts:
 *            the product glue (xiaotai_selected_configure in
 *            xiaotai/selection.h) and xiaotai_start() itself. Nothing else
 *            runs yet, so no lock is needed. Nothing may be started from
 *            there, with one exception made so that start-up failures are
 *            recorded and answered: the glue opens the event log
 *            (xiaotai_eventlog_init) and then the console (xiaotai_at_open),
 *            which allocate and create the console's task
 *   [board]  only inside xiaotai_hal_init(): the board's Init() creating and
 *            bringing up its parts, before the framework starts
 *   [init]   only from an init() of an engine, module or application (it runs
 *            on the owner task, so [owner] functions may be called there too).
 *            A header that admits fewer callers says so in the same words
 *            ("[init], engines excluded")
 *   [owner]  the framework owner task
 *   [any]    any task; never an interrupt handler. A call that waits says how
 *            long at the function, and never waits for the network
 *   [rt]     also real-time tasks (capture, playout, transport callbacks):
 *            never allocates or logs, it counts, and diagnostics read the
 *            counters. It blocks only where waiting is the real-time task's
 *            own job (its own input, its own pacing, a timeout stated at the
 *            function), never for another component's work
 * Tags combine ("[glue, init]": the glue, or an init()). A header that needs
 * another tag (its own worker, the UI task, the console task) defines it at
 * its top and never restates or narrows one of these in its own words. No
 * SDK function may be called from an interrupt handler unless it says so. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/framework_config.h"
#include "xiaotai/needs.h"
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Stack a handler may use on the owner task, with everything it calls. */
#define XIAOTAI_FRAMEWORK_HANDLER_STACK_BYTES 3072U

/* ---------------------------------------------------------------- arguments */

#define XIAOTAI_ARG_TEXT_MAX 64U

/* Carried by intents and events; copied when posted. */
typedef struct {
    int32_t value;
    char text[XIAOTAI_ARG_TEXT_MAX];
} xiaotai_args_t;

/* -------------------------------------------------------------------- events */

/* System events emitted by the framework. Engines and modules emit their own,
 * named "<component>.<what>" in lower case ("call.incoming", "net.up") and
 * published as XIAOTAI_EVENT_<COMPONENT>_<WHAT> in the component's header. */
#define XIAOTAI_EVENT_STARTED "system.started"
/* args.text: activity name, "" when none; args.value: the activity ID's
 * 32 bits at the transition (0 when none), not the current activity at delivery. */
#define XIAOTAI_EVENT_ACTIVITY "activity.changed"
#define XIAOTAI_EVENT_FOREGROUND "app.foreground"   /* args.text: application name */
/* The system was quiesced (args.value 1) or released (0); args.text: the reason
 * given to xiaotai_quiesce_begin (or to xiaotai_restart_request). */
#define XIAOTAI_EVENT_QUIESCE "system.quiesce"
/* The orderly restart is under way: every restart hold is released and the
 * device restarts in args.value milliseconds. args.text: the reason. */
#define XIAOTAI_EVENT_RESTARTING "system.restarting"
/* A changed setting could not be written to storage: the value in memory is the
 * new one, the stored one is still the old. args.text: the setting's name;
 * args.value: the storage error. Applications show "setting not saved". */
#define XIAOTAI_EVENT_SETTING_NOT_SAVED "setting.not_saved"

typedef struct {
    const char *name;
    xiaotai_args_t args;
    const void *data;   /* optional payload, valid during the handler only */
    size_t size;
} xiaotai_event_t;

typedef void (*xiaotai_event_fn_t)(const xiaotai_event_t *event, void *ctx);

/* [init] Several subscribers per event, called in registration order (but see
 * "Which application presents an event" below for events applications
 * handle). */
xiaotai_err_t xiaotai_event_subscribe(const char *name, xiaotai_event_fn_t handler, void *ctx);
typedef struct {
    const char *name;
    xiaotai_event_fn_t handler;
    void *ctx;
} xiaotai_subscriber_descriptor_t;

#if XIAOTAI_FRAMEWORK_COMPACT_SUBSCRIBERS
/* [init] Register an immutable descriptor owned by the registering unit.
 * The descriptor and borrowed fields must remain valid through reset. */
xiaotai_err_t xiaotai_event_subscribe_static(const xiaotai_subscriber_descriptor_t *descriptor);
#define XIAOTAI_EVENT_SUBSCRIBE_CONST(event_name, callback, context) __extension__ ({ \
    static const xiaotai_subscriber_descriptor_t descriptor = {(event_name), (callback), (context)}; \
    xiaotai_event_subscribe_static(&descriptor); \
})

/* [init] Append a descriptor from the configured const table. Its ID is a
 * table index, not an activation slot. Repeated IDs remain separate listeners,
 * in registration order and with the current application's scope. */
xiaotai_err_t xiaotai_event_subscribe_compiled(uint16_t descriptor_id);
#else
static inline xiaotai_err_t xiaotai_event_subscribe_static(const xiaotai_subscriber_descriptor_t *descriptor)
{
    return descriptor ? xiaotai_event_subscribe(descriptor->name, descriptor->handler, descriptor->ctx) :
                        XIAOTAI_ERR_INVALID_ARG;
}
#define XIAOTAI_EVENT_SUBSCRIBE_CONST(event_name, callback, context) \
    xiaotai_event_subscribe((event_name), (callback), (context))
#endif
/* [any] Queued. data (up to XIAOTAI_EVENT_DATA_MAX) is copied and freed after dispatch.
 * Emitting an event nobody subscribes to is not an error. */
#define XIAOTAI_EVENT_DATA_MAX 4096U
xiaotai_err_t xiaotai_event_emit(const char *name, const xiaotai_args_t *args, const void *data, size_t size);
/* [owner] Like xiaotai_event_emit, but can use the product's reserved slot.
 * Keeps FIFO order and the same payload copy. Only for a conversation's
 * terminal state, once per transition; not a general queue bypass. */
xiaotai_err_t xiaotai_event_emit_owner_terminal(const char *name, const xiaotai_args_t *args,
                                              const void *data, size_t size);

/* ---------------------------------------------------- modules and applications */

/* Priority of an activity (see "activities" below); a table ends with a NULL
 * activity. */
typedef struct {
    const char *activity;
    uint8_t priority;
} xiaotai_priority_t;

/* Hardware needs (XIAOTAI_NEED_* in xiaotai/needs.h) are checked against the
 * board before anything is initialized.
 *
 * A base module: a capability with an API of its own (xiaotai/modules/<name>.h),
 * events and settings; no business flow, no screens. Engines (media, transport,
 * platform, ...) share this lifecycle: the build generates one table from the
 * manifests, engines first, then modules, each after what it depends on.
 *
 * Order at start, all on the owner task and without anything in between:
 *   1. init() of every engine, then every module, then every application;
 *   2. start() of every engine, then every module, then every application;
 *   3. the home application comes to the foreground and
 *      XIAOTAI_EVENT_STARTED is emitted.
 * The owner task dispatches no event, intent, timer or xiaotai_run() job
 * before step 3 has finished. An engine's start() may therefore reserve what
 * must be taken before any link exists (contiguous internal memory, buffers
 * sized for the declared needs): nothing that is reached through the owner
 * task (network up -> platform online -> RTC stack -> links) can run earlier.
 * For this to hold:
 *   - init() and start() neither wait for the network nor start a protocol
 *     stack themselves; work they begin on their own tasks reaches other
 *     components only through owner-task events;
 *   - no start() begins hardware bring-up that allocates internal or DMA
 *     memory on another task and returns before it is done (a radio or
 *     coprocessor driver started in the background). Such bring-up begins on
 *     XIAOTAI_EVENT_STARTED, so every reservation made in a start() is taken
 *     before it, whatever the order of the engines. Exception: preparation
 *     that a start() runs on the engine's own task and waits for before it
 *     returns counts as a reservation made inside that start() and does not
 *     wait for XIAOTAI_EVENT_STARTED (the video engine has its camera task
 *     call the camera part's prepare(), waits for it, then reserves the
 *     encoder: all of it before any link exists).
 * xiaotai_module_present() answers from the first init() on.
 *
 * A start failure (a missing dependency, an unmet need, an init() error)
 * stops the owner task and forgets every registration made so far; the
 * modules that were initialized are not told (the device restarts or stays
 * down, see xiaotai_start() in xiaotai/xiaotai.h). */
typedef struct {
    const char *name;                 /* "ai_talk", "call", "provisioning", … */
    const char *const *depends;       /* engines and modules it calls; NULL-terminated or NULL */
    uint32_t needs;                   /* XIAOTAI_NEED_* */
    xiaotai_err_t (*init)(void);      /* [init] register intents, events, settings, commands; allocate */
    void (*start)(void);              /* [owner] after every module and application is initialized */
    /* The activities this module begins, with their default priorities (from
     * the manifest's "activities"); NULL-terminated or NULL. The product's
     * table (xiaotai_framework_config_t.priorities) overrides them by name. */
    const xiaotai_priority_t *activities;
} xiaotai_module_t;

/* Why an application came to the foreground (xiaotai_app_entry()). */
typedef enum {
    XIAOTAI_APP_ENTRY_START = 0,  /* the home application at start */
    XIAOTAI_APP_ENTRY_SWITCH,     /* xiaotai_app_switch() */
    XIAOTAI_APP_ENTRY_NEXT,       /* xiaotai_app_next(): paging forward */
    XIAOTAI_APP_ENTRY_PREVIOUS,   /* xiaotai_app_previous(): paging back */
    XIAOTAI_APP_ENTRY_PRESENT,    /* brought forward to present an event */
    XIAOTAI_APP_ENTRY_RETURN,     /* xiaotai_app_return() after presenting */
} xiaotai_app_entry_t;

/* An application: a product's business (alarm clock, door station, …). It uses
 * modules through their APIs and events and describes screens with views.
 *
 * Which application presents an event. A product may hold several
 * applications; one is in the foreground (the only one that shows views and
 * whose interaction profile applies). An application lists in `handles` the
 * module events it presents ("call.incoming", "ai_talk.state", ...). Before
 * such an event is dispatched the framework chooses its presenter: the first
 * application, in the product's order, that lists the event and whose claim()
 * accepts it (no claim(): it accepts every event it lists); when none does,
 * the foreground application. A presenter that is not in the foreground is
 * brought there first (its foreground(true) runs before the event's
 * handlers), and the event reaches the subscriptions of the presenter only,
 * besides those of engines and modules. Events no application lists reach
 * every subscriber, background applications included. The application that
 * was brought forward goes back with xiaotai_app_return() when it has
 * finished presenting. Applications never name each other: the product's
 * order and the claims decide.
 *
 * Views belong to the foreground application. The framework knows which
 * application's code it is running (init, start, foreground, its event and
 * intent handlers, its timers and the xiaotai_run() jobs it queued, its
 * commands, its claim): xiaotai_view_show() from a background application
 * is dropped and counted (view_refused= in the "framework" diagnostics).
 * Engines and modules never call it.
 *
 * Activities. An application may begin activities of its own (an alarm
 * ringing) with xiaotai_activity_begin(), like a module; it declares them in
 * `activities` with their default priorities, merged with the modules'
 * defaults (the same name must have the same default everywhere). */
typedef struct {
    const char *name;                       /* "companion", "door", "alarm", … */
    const char *const *modules;             /* required modules; NULL-terminated or NULL */
    const char *const *optional_modules;    /* used when the product includes them */
    uint32_t needs;
    xiaotai_err_t (*init)(void);            /* [init] */
    void (*start)(void);                    /* [owner] */
    void (*foreground)(bool foreground);    /* [owner] becomes or stops being the shown application */
    /* Module events this application presents; NULL-terminated or NULL. */
    const char *const *handles;
    /* [owner] Optional: whether this application presents this event (for
     * example only calls from the stored door station). Answers from memory
     * and changes nothing; called before any handler of the event runs. */
    bool (*claim)(const xiaotai_event_t *event);
    /* The activities this application begins, with their default priorities
     * (the manifest's "activities"); NULL-terminated or NULL. Same rules as
     * xiaotai_module_t.activities. */
    const xiaotai_priority_t *activities;
    /* [owner] Optional relative Home-page action (direction is -1 or +1).
     * The app decides whether its current page permits it. */
    xiaotai_err_t (*page_step)(int direction);
} xiaotai_application_t;

/* [any] True when the named engine or module is part of this build. Valid from
 * the first init() on (the order table is built before any init runs). */
bool xiaotai_module_present(const char *name);

/* ------------------------------------------------------------------- intents */

/* What a user or another part asked for ("talk.toggle", "call.accept", …).
 * Exactly one handler per intent; a product without the module has no intent. */
typedef void (*xiaotai_intent_fn_t)(const xiaotai_args_t *args, void *ctx);

typedef struct {
    uint32_t scope, revision;
} xiaotai_intent_stamp_t;

typedef struct {
    /* [caller] Validate the copied arguments and capture context before
     * posting. An error admits no job. Do not run owner actions here. */
    xiaotai_err_t (*prepare)(const xiaotai_args_t *args, xiaotai_intent_stamp_t *stamp, void *ctx);
    /* [owner] Recheck the captured context before taking action. */
    void (*dispatch)(const xiaotai_args_t *args, const xiaotai_intent_stamp_t *stamp, void *ctx);
} xiaotai_intent_prepared_t;

enum {
    /* The intent may come from the device itself only (inputs, touch,
     * applications, modules, the public API), never from an open channel such
     * as the console. Required for every intent whose handler erases the
     * identity (xiaotai_identity_erase), forgets network credentials
     * (xiaotai_net_forget*) or starts provisioning over (FORGET):
     * scripts/check_sdk_layers.py refuses such a handler registered without it. */
    XIAOTAI_INTENT_LOCAL = 1u << 0,
};

/* Shared intent names. At most one application handles each; they take no
 * arguments. Whoever offers the entry (a launcher, a menu key, another
 * application's page) shows it only when xiaotai_intent_exists() is true. */
#define XIAOTAI_INTENT_SYSTEM_MENU "system.menu"         /* the product's system menu */
#define XIAOTAI_INTENT_SYSTEM_NETWORK "system.network"   /* the network and provisioning page */

xiaotai_err_t xiaotai_intent_handle(const char *name, xiaotai_intent_fn_t handler, void *ctx); /* [init] */
/* [init] As xiaotai_intent_handle with XIAOTAI_INTENT_* flags. */
xiaotai_err_t xiaotai_intent_handle_ex(const char *name, xiaotai_intent_fn_t handler, void *ctx, uint32_t flags);
/* [init] Register a prepared intent. Its descriptor must live until reset.
 * Preparation runs on the posting task, using the same copied arguments
 * that the owner receives, and preserves the normal dispatch source. */
xiaotai_err_t xiaotai_intent_handle_prepared(const char *name, const xiaotai_intent_prepared_t *handler,
                                            void *ctx, uint32_t flags);
/* [any] Queued to the owner task. XIAOTAI_ERR_NOT_FOUND when nothing handles it. */
xiaotai_err_t xiaotai_intent_post(const char *name, const xiaotai_args_t *args);
/* [any] As xiaotai_intent_post, for a request that arrived over an open
 * channel (AT+INTENT): XIAOTAI_ERR_NOT_ALLOWED for an XIAOTAI_INTENT_LOCAL
 * intent, which is not queued. */
xiaotai_err_t xiaotai_intent_post_external(const char *name, const xiaotai_args_t *args);
/* [any] True on the owner task while dispatching an intent posted with
 * xiaotai_intent_post_external(). Argument contents never set the source.
 * An intent or job posted by that handler has its own source when dispatched. */
bool xiaotai_intent_from_external(void);
/* [any] */
bool xiaotai_intent_exists(const char *name);

/* [any, rt] Run fn(ctx) on the owner task; how engines and real-time tasks
 * hand results to modules. Zero wait and no allocation, whichever task
 * calls: XIAOTAI_ERR_NO_MEM when the owner's queue is full (nothing was
 * queued; the caller may try again later, see its own header for when),
 * XIAOTAI_ERR_INVALID_STATE before the framework started. Jobs run in the
 * order they were queued, interleaved with events and intents. */
xiaotai_err_t xiaotai_run(void (*fn)(void *ctx), void *ctx);
/* [any, rt] */
bool xiaotai_on_owner(void);

/* --------------------------------------------------------------- generations */

/* [any, rt] The next session generation: a non-zero uint32_t, unique in this
 * boot until the counter wraps (0 is skipped). One counter serves the whole
 * device, so a link's generation (xiaotai/link.h) and the generation of a
 * media session that has no link (a recording for sound recognition) are never
 * equal, and one number names a session on every engine. Lock-free; valid
 * before the framework starts. The binding generation (xiaotai/identity.h) and
 * the network address generation (xiaotai/net.h) are counters of their own and
 * are never mixed with this one. */
uint32_t xiaotai_generation_next(void);

/* -------------------------------------------------------------------- timers */

/* One-shot timers on the owner task: a pending state that gives up after
 * 1500 ms, a status text shown for 3200 ms, an idle expression after 45 s.
 * The handler runs on the owner task like every other handler, never earlier
 * than delay_ms after the call and as soon after as the owner is free; a
 * handler may start timers, its own again included. */
typedef void (*xiaotai_timer_fn_t)(void *ctx);

/* Longest delay: about 24 days. */
#define XIAOTAI_TIMER_DELAY_MAX_MS 0x7FFFFFFFU

/* [owner] *id (required) is never 0. XIAOTAI_ERR_INVALID_ARG: no handler, no
 * id, or a delay above XIAOTAI_TIMER_DELAY_MAX_MS; XIAOTAI_ERR_INVALID_STATE:
 * not on the owner task; XIAOTAI_ERR_NO_MEM: every timer slot is in use
 * (XIAOTAI_FRAMEWORK_MAX_TIMERS). */
xiaotai_err_t xiaotai_timer_start(uint32_t delay_ms, xiaotai_timer_fn_t handler, void *ctx, uint32_t *id);
/* [owner] After it returns the handler of this timer does not run. A timer
 * that already ran, an unknown id and 0 are ignored. */
void xiaotai_timer_cancel(uint32_t id);

/* --------------------------------------------------------------- activities */

/* The foreground activity owns the shared device: audio, the link, the camera
 * and the screen's focus. Modules begin one when a session starts (AI talk,
 * call, room, stream, recognition); an application may begin its own (an
 * alarm ringing). Whether a new activity preempts the current one is decided
 * per pair:
 *   1. a product rule for the pair (xiaotai_preemption_t) decides when there
 *      is one, whatever the priorities say;
 *   2. otherwise the higher priority preempts; an equal or lower request gets
 *      XIAOTAI_ERR_BUSY, and the module answers its peer in its own way (a
 *      call replies "busy").
 * Priorities come from the product's table, else from the manifests of the
 * engines, modules and applications (xiaotai_module_t.activities,
 * xiaotai_application_t.activities), else 1. Rules express what a single ranking
 * cannot: "AI does not preempt recognition although it preempts the room"
 * (rank AI = recognition above room, plus a rule "recognition does not
 * preempt room"), or a cycle such as AI over room over stream over AI. */
enum {
    XIAOTAI_RESOURCE_AUDIO_IN = 1u << 0,
    XIAOTAI_RESOURCE_AUDIO_OUT = 1u << 1,
    XIAOTAI_RESOURCE_LINK = 1u << 2,
    XIAOTAI_RESOURCE_CAMERA = 1u << 3,
    XIAOTAI_RESOURCE_SCREEN = 1u << 4,
};

typedef enum {
    XIAOTAI_REVOKE_PREEMPTED = 0, /* a higher-priority activity began */
    XIAOTAI_REVOKE_SYSTEM,        /* the system was quiesced (maintenance, restart) */
} xiaotai_revoke_t;

typedef struct {
    const char *name;          /* priority table key: "call", "ai", "room", "stream", "recognition" */
    uint32_t resources;        /* XIAOTAI_RESOURCE_* */
    /* [owner] The activity is gone; release its session. Not called for
     * xiaotai_activity_end. Runs after the attachments' end(). */
    void (*revoked)(uint32_t id, xiaotai_revoke_t reason, void *ctx);
    void *ctx;
    /* The session's generation (the link's where the session has one); 0
     * while not known yet: xiaotai_activity_set_generation() gives it later. */
    uint32_t generation;
} xiaotai_activity_t;

/* One pair the priorities do not decide: when `activity` begins while `over`
 * is current, it preempts (true) or gets XIAOTAI_ERR_BUSY (false). A table
 * ends with a NULL activity. */
typedef struct {
    const char *activity;
    const char *over;
    bool preempts;
} xiaotai_preemption_t;

/* [owner] XIAOTAI_ERR_BUSY when the current activity is not preempted (see
 * above), and for every request while the system is quiesced. */
xiaotai_err_t xiaotai_activity_begin(const xiaotai_activity_t *activity, uint32_t *id);
void xiaotai_activity_end(uint32_t id);                                                  /* [owner] */
/* [owner] Current activity id (0: none) and its name. */
uint32_t xiaotai_activity_current(const char **name);
/* [any, rt] Atomic presence snapshot for admission decisions. Stays active
 * across preemption and through the previous activity's cleanup callbacks. */
bool xiaotai_activity_active(void);
/* [owner] The host (the module that began the activity) sets the activity's
 * generation to its session's generation (non-zero) once attachments may use
 * the session, and back to 0 when they no longer may; attachments hear every
 * change through session(), and session(id, 0) means stop. Each host states
 * its timing in its own header (AI talk: when the handshake passes, 0 at
 * ENDING; call and WeChat: when ACTIVE, 0 when the call ends). Ignored for an
 * id that is not current and for an unchanged value. */
void xiaotai_activity_set_generation(uint32_t id, uint32_t generation);
/* [owner] Generation of the current activity with this id; 0 when it is not
 * current or has none yet. */
uint32_t xiaotai_activity_generation(uint32_t id);

/* Session extensions: what rides on another activity (AI vision on AI talk,
 * video on a call or on a web stream). One contract for every host and every
 * extension:
 *   - lifecycle: the extension attaches by activity name and hears begin(),
 *     end() and session() with the activity's generation; it uses the
 *     resources of the activity it rides on and is not arbitrated
 *     separately. On a revocation every attachment's end() runs before the
 *     activity's revoked().
 *   - contributions: what the extension adds to what the host builds for
 *     its sessions (the video part of a link profile, the video members of a
 *     device-profile scene), as typed records under a slot name. A slot is
 *     defined by the header of the engine whose structure it fills, with
 *     the record's type (xiaotai/link.h: XIAOTAI_LINK_SLOT_VIDEO;
 *     xiaotai/platform.h: XIAOTAI_PLATFORM_SLOT_SCENE); the framework knows
 *     neither slots nor types. A record also names the host scene it is for
 *     (the profile scene of the host's sessions: "call", "voip", "stream",
 *     "ai"), or NULL for every scene of that activity.
 * A host collects the contributions of its activity and scene with
 * xiaotai_activity_contributions() when it builds a link profile or writes
 * its scene; a host with no contribution has no video. So a new host or a
 * new extension adds no API to the other side. */
typedef struct {
    const char *slot;    /* XIAOTAI_*_SLOT_* of the header that defines `data` */
    const char *scene;   /* the host scene it is for; NULL: every scene */
    const void *data;    /* the record of the slot's type; static storage */
} xiaotai_contribution_t;

typedef struct {
    void (*begin)(uint32_t id, uint32_t generation, void *ctx);   /* [owner] the named activity began */
    void (*end)(uint32_t id, uint32_t generation, void *ctx);     /* [owner] it ended or was revoked */
    /* [owner] Optional: the activity's generation changed (see above). */
    void (*session)(uint32_t id, uint32_t generation, void *ctx);
    void *ctx;
    /* Optional: the extension's contributions, ended by an entry with a NULL
     * slot, or NULL. Static storage: the framework keeps the pointer. */
    const xiaotai_contribution_t *contributions;
} xiaotai_attachment_t;
xiaotai_err_t xiaotai_activity_attach(const char *activity, const xiaotai_attachment_t *attachment); /* [init] */

/* [owner] The contributions to `slot` for `scene` of every attachment of
 * `activity`, in attachment order: those that name this scene and those that
 * name none. Copies up to capacity data pointers into out (out may be NULL
 * with capacity 0) and returns how many there are, which may exceed
 * capacity. Answers from the registrations made at init: valid from start()
 * on, whether or not the activity is current. */
size_t xiaotai_activity_contributions(const char *activity, const char *slot, const char *scene,
                                      const void **out, size_t capacity);

/* ------------------------------------------------------------------- quiesce */

/* Quiescing winds the device down for an operation that cannot share it: a
 * coprocessor firmware update, switching the service environment, starting
 * over with provisioning, an orderly restart. While the system is quiesced
 *   - the current activity has been revoked with XIAOTAI_REVOKE_SYSTEM (its
 *     module closed its media session and its link in the revoked callback),
 *   - xiaotai_activity_begin answers XIAOTAI_ERR_BUSY, so modules refuse
 *     incoming sessions the way they refuse them while another one is active,
 *   - xiaotai_setting_set_int and _text answer XIAOTAI_ERR_BUSY (writes
 *     accepted before run to their end),
 *   - XIAOTAI_EVENT_QUIESCE has told every subscriber (the wake word stops
 *     listening, the screen shows the maintenance page).
 * The framework knows activities only. Whoever quiesces also takes what the
 * engines offer for their own state where the operation needs it: the network
 * engine's maintenance gate, the platform's gate and its drained check, a
 * transport hold. One holder at a time.
 *
 * Quiesce guards: a module whose background work must not be cut (a room
 * reservation being released, a reconciliation request in flight) registers
 * a guard. Every guard is asked, in registration order, before the system is
 * quiesced; the first refusal is returned and nothing changes. */

/* Quiesce reasons: fixed words, compared by guards with strcmp(). A component
 * with another kind of operation adds its own word in its own header. The
 * orderly restart quiesces, or labels a quiesce it takes over, with the
 * reason given to xiaotai_restart_request ("network", "intent", or the
 * caller's own word above). */
#define XIAOTAI_QUIESCE_COPROCESSOR "coprocessor"     /* coprocessor firmware update or rollback */
#define XIAOTAI_QUIESCE_ENVIRONMENT "environment"     /* switching the service environment */
#define XIAOTAI_QUIESCE_PROVISIONING "provisioning"   /* starting over with provisioning */
#define XIAOTAI_QUIESCE_BINDING "binding"             /* a binding change or local reset */

/* [owner] Answers from memory: no waiting, no storage, no request; changes
 * nothing. XIAOTAI_OK agrees; any other result refuses (normally
 * XIAOTAI_ERR_BUSY). reason is the one given to xiaotai_quiesce_begin. */
typedef xiaotai_err_t (*xiaotai_quiesce_guard_fn_t)(const char *reason, void *ctx);

/* [init] name: a fixed word for diagnostics (static storage).
 * XIAOTAI_ERR_NO_MEM beyond XIAOTAI_FRAMEWORK_MAX_QUIESCE_GUARDS. */
xiaotai_err_t xiaotai_quiesce_guard_register(const char *name, xiaotai_quiesce_guard_fn_t guard, void *ctx);

/* [init] Registers a reversible admission gate for explicit quiesce_begin.
 * The owner runs acquire after every read-only guard agreed. An acquire
 * must be nonblocking and leave no change if it fails. Release earlier
 * successful gates in reverse order on refusal or quiesce_end; a restart
 * which takes over an existing quiesce keeps the gates closed. Both
 * callbacks receive the same reason; do not reenter quiesce operations.
 * Uses the same registration capacity as read-only guards. Forced restart
 * without a supplied quiesce ticket still uses only read-only guards. */
typedef struct {
    xiaotai_quiesce_guard_fn_t acquire;
    void (*release)(const char *reason, void *ctx);
    /* Optional nonblocking observation after acquisition. *ready starts true;
     * set false when an observation is pending. An error or pending result
     * leaves gate ownership unchanged. Do not reenter quiesce. */
    xiaotai_err_t (*check)(const char *reason, void *ctx, bool *ready);
} xiaotai_quiesce_gate_t;
/* gate is a static descriptor, referenced for the lifetime of the framework. */
xiaotai_err_t xiaotai_quiesce_gate_register(const char *name, const xiaotai_quiesce_gate_t *gate, void *ctx);

/* [owner] reason is a fixed word for diagnostics, the guards and the event
 * (XIAOTAI_QUIESCE_* above); static storage. *ticket
 * (required) is never 0. XIAOTAI_ERR_BUSY when the system is already
 * quiesced or a restart is pending; a guard's refusal is returned as the
 * guard gave it. */
xiaotai_err_t xiaotai_quiesce_begin(const char *reason, uint32_t *ticket);
/* [owner] Check active gates without changing ownership. On success, ready
 * reports whether they have all confirmed. A nonnull ready is required.
 * Invalid, stale, or restart-owned tickets return XIAOTAI_ERR_INVALID_STATE. */
xiaotai_err_t xiaotai_quiesce_check(uint32_t ticket, bool *ready);
/* [owner] Activities may begin again. A ticket that is not the holder's is
 * ignored (also after a restart took the quiesce over). */
void xiaotai_quiesce_end(uint32_t ticket);
/* [any] */
bool xiaotai_quiesced(void);
/* [owner] True only for the current quiesce ticket and the specified reason.
 * False off owner, with no ticket, for NULL/empty, or for a different reason. */
bool xiaotai_quiesced_for(const char *reason);

/* ------------------------------------------------------------------- restart */

/* The orderly restart, for every product whatever it selects. Whoever needs
 * the device restarted (stored credentials, a finished maintenance, a local
 * reset, AT+RESTART) asks; the framework
 *   1. quiesces the system under the given reason, as soon as every quiesce
 *      guard agrees (after XIAOTAI_FRAMEWORK_RESTART_GUARD_WAIT_MS it
 *      quiesces without them), or takes over the caller's quiesce;
 *   2. waits until no restart hold is left (no time limit: a hold that is
 *      never released keeps the device running);
 *   3. emits XIAOTAI_EVENT_RESTARTING and waits
 *      XIAOTAI_FRAMEWORK_RESTART_DELAY_MS;
 *   4. flushes the setting store on the owner task (at most 3000 ms; the
 *      framework's own store waits with xiaotai_kv_flush() for the storage
 *      port's queued writes) and then calls the system port's
 *      xiaotai_restart() on a short task of its own whose stack is internal
 *      RAM (XIAOTAI_FRAMEWORK_RESTART_STACK_BYTES): on boards with a radio
 *      coprocessor the restart path shuts the coprocessor link down, which
 *      deployed firmware never runs on a stack in external RAM. When that
 *      task cannot be created the owner task calls it (counted,
 *      restart_inline= in the diagnostics). That fallback is a last resort
 *      that breaks the internal-stack rule of xiaotai_restart()
 *      (xiaotai/port/sys.h) where the owner stack lives in external RAM; it
 *      is taken only because staying up after a requested restart is worse.
 *      Without external RAM the owner stack is internal and the rule holds.
 *      Components that queue storage writes of their own hold the restart
 *      until they are written.
 * A restart that must not wait (the coprocessor is already restarting) calls
 * xiaotai_restart() itself, from a task with an internal stack. The intent
 * XIAOTAI_INTENT_SYSTEM_RESTART does step 1 to 4 with args.text as the reason
 * ("intent" when empty). */
#define XIAOTAI_INTENT_SYSTEM_RESTART "system.restart"

/* [owner] reason: a fixed word for diagnostics and the events (copied).
 * quiesce_ticket: 0, or the caller's own quiesce ticket, which the restart
 * then takes over without releasing it, so nothing can begin between the
 * caller's work and the restart. XIAOTAI_ERR_BUSY: the system is quiesced by
 * another holder. A restart already pending: XIAOTAI_OK, the first reason
 * stays. XIAOTAI_ERR_INVALID_STATE: not on the owner task, or before the
 * framework started. */
xiaotai_err_t xiaotai_restart_request(const char *reason, uint32_t quiesce_ticket);
/* [owner] Direct restart for a local product flow that has already committed
 * its own data. It does not consult quiesce guards, revoke the current
 * activity, or flush unrelated settings. It still observes restart holds,
 * announces XIAOTAI_EVENT_RESTARTING and waits the configured delay before
 * using the same restart task as the orderly request. An existing quiesce
 * refuses it with XIAOTAI_ERR_BUSY; a pending restart keeps its first policy
 * and reason. No intent or console command selects this path. */
xiaotai_err_t xiaotai_restart_request_direct(const char *reason);
/* [any] Restart holds: whoever has something to finish before a restart (a
 * prompt, a confirmation sound, a setting being written) takes a hold when it
 * starts and releases it when done, on every path. Holds are counted; a
 * release without a hold is ignored. */
void xiaotai_restart_hold(void);
void xiaotai_restart_release(void);
/* [any] A restart was requested and has not happened yet. */
bool xiaotai_restart_pending(void);

/* ------------------------------------------------------------ applications */

/* [owner] Brings the named application to the foreground: the previous one's
 * foreground(false) and this one's foreground(true) have both run when it
 * returns. Background applications keep receiving the events they subscribe
 * to (except events an application presents, see xiaotai_application_t).
 * Forgets where xiaotai_app_return() would go. Switching to the application
 * already in the foreground runs nothing. */
xiaotai_err_t xiaotai_app_switch(const char *name);
/* [owner] The next or previous application in the product's order, wrapping
 * around (a launcher's paging). XIAOTAI_ERR_NOT_FOUND with fewer than two
 * applications. Forgets where xiaotai_app_return() would go. The application
 * paged to often has its own arrows where the finger still is: it ignores
 * its own paging controls for a moment after it came forward this way, so
 * one tap is one step. */
xiaotai_err_t xiaotai_app_next(void);
xiaotai_err_t xiaotai_app_previous(void);
/* [owner] Ask the shown application to step within its home pages (direction
 * -1 or +1), without an implicit app switch or change to a pending return.
 * The app decides which views permit paging and supplies the result. No
 * callback: NOT_SUPPORTED. No foreground, off owner or quiesced: INVALID_STATE. */
xiaotai_err_t xiaotai_app_page_step(int direction);
/* [owner] The application the framework brought forward to present an event
 * has finished: the one that was in the foreground before comes back.
 * XIAOTAI_ERR_INVALID_STATE when the framework brought nothing forward, or
 * the foreground changed since by a switch. */
xiaotai_err_t xiaotai_app_return(void);
const char *xiaotai_app_foreground(void);           /* [owner] */
/* [owner] Why the foreground application came forward the last time: read in
 * its foreground(true) (a paged-to application opens on its first screen
 * after NEXT and on its last after PREVIOUS). */
xiaotai_app_entry_t xiaotai_app_entry(void);
/* [any] The product's home application (xiaotai_framework_config_t.home, else
 * the first application), NULL without applications. Valid from the first
 * init() on. */
const char *xiaotai_app_home(void);
/* [any] How many applications the product has: with two or more, the home
 * pager (xiaotai_app_next / _previous) leads somewhere, so an application
 * shows its paging controls. 0 before the framework started. */
size_t xiaotai_app_count(void);

/* ------------------------------------------------------------------ settings */

typedef enum {
    XIAOTAI_SETTING_INT = 0,
    XIAOTAI_SETTING_BOOL,
    XIAOTAI_SETTING_TEXT,
} xiaotai_setting_kind_t;

/* Longest text value with its terminator: a 64-character device id fits. It
 * equals XIAOTAI_PREFERENCE_TEXT_MAX (xiaotai/preferences.h). */
#define XIAOTAI_SETTING_TEXT_MAX 65U

/* Registered settings persist automatically and appear in settings screens and
 * AT (AT+SET). Where a value is kept is the setting store's decision: every
 * product selects the identity engine, whose preferences service installs
 * itself as the store at its init (xiaotai/preferences.h: the SDK's own
 * namespace under the setting's key, or the place a product's binding table
 * names, so deployed devices keep the preferences they already hold). Without
 * a store (host tests, a build without the identity engine) the framework
 * keeps values itself in the namespace "xt_settings" under the key. A write
 * that fails later is announced with XIAOTAI_EVENT_SETTING_NOT_SAVED. */
typedef struct {
    const char *name;            /* "audio.volume" */
    const char *key;             /* storage key, at most 15 characters, unique */
    xiaotai_setting_kind_t kind;
    int32_t min, max, default_value;
    const char *default_text;
    void (*changed)(const char *name, void *ctx); /* [owner] after a change */
    void *ctx;
} xiaotai_setting_t;

/* Where settings are kept; self is passed back as the first argument. */
typedef struct {
    /* [init] Stored value of an integer or boolean setting; any error keeps
     * the default. default_value lets a store that writes packed records fill
     * a field nobody changed with its default, never with 0. */
    xiaotai_err_t (*load_int)(void *self, const char *key, int32_t default_value, int32_t *value);
    /* [init] Stored text, terminated; any error keeps the default. */
    xiaotai_err_t (*load_text)(void *self, const char *key, char *out, size_t capacity);
    /* [owner] Accept a new value and return at once; a later failure is
     * reported with xiaotai_setting_store_result(). */
    xiaotai_err_t (*store_int)(void *self, const char *key, int32_t value);
    xiaotai_err_t (*store_text)(void *self, const char *key, const char *value);
    /* [owner] Optional: write what was accepted, at most timeout_ms (before a
     * restart). */
    xiaotai_err_t (*flush)(void *self, uint32_t timeout_ms);
    void *self;
} xiaotai_setting_store_t;

/* [init] Installs the store (copied) before the first setting is registered:
 * XIAOTAI_ERR_INVALID_STATE afterwards or for a second store, so no setting is
 * ever read from one place and written to another. */
xiaotai_err_t xiaotai_setting_store_set(const xiaotai_setting_store_t *store);
/* [any] A write the store accepted failed (key: the setting's storage key):
 * emits XIAOTAI_EVENT_SETTING_NOT_SAVED. Never blocks; usable from the
 * store's worker. */
void xiaotai_setting_store_result(const char *key, xiaotai_err_t result);
/* [owner] Capture the revision during a store call; zero if untracked. */
uint32_t xiaotai_setting_store_revision(const char *key);
/* [any] As above, with the revision saved at admission. The failure remains
 * observable even when a later value replaced it. */
void xiaotai_setting_store_result_at(const char *key, xiaotai_err_t result, uint32_t revision);
/* [owner] Whether a NOT_SAVED event still describes the setting's value.
 * Unversioned store notifications retain their original behavior. */
bool xiaotai_setting_failure_current(const xiaotai_event_t *event);

xiaotai_err_t xiaotai_setting_register(const xiaotai_setting_t *setting); /* [init] loads the stored value */
/* [init] Register a numeric setting with an owner-side admission callback.
 * Called after range, maintenance and no-op checks, before updating the
 * value or its revision. The callback must not block or submit settings.
 * Returning false consumes the request without a write or changed() call;
 * the registering module reports its refusal through its own event. */
typedef bool (*xiaotai_setting_accept_int_fn_t)(int32_t value, void *ctx);
xiaotai_err_t xiaotai_setting_register_int_admission(const xiaotai_setting_t *setting,
                                                      xiaotai_setting_accept_int_fn_t accept);
/* [init] Registers a text setting with a per-setting byte limit, excluding
 * the terminator. max_bytes may be zero (only the empty value) and must be
 * less than XIAOTAI_SETTING_TEXT_MAX. Wrong kind or limit: INVALID_ARG;
 * oversize default: INVALID_SIZE. An oversize stored value leaves the default
 * in use without touching the store. */
xiaotai_err_t xiaotai_setting_register_text(const xiaotai_setting_t *setting, size_t max_bytes);
xiaotai_err_t xiaotai_setting_get_int(const char *name, int32_t *value);   /* [any] */
xiaotai_err_t xiaotai_setting_get_text(const char *name, char *out, size_t capacity); /* [any] */
/* [owner] Validates, checks admission if registered, hands the value to the
 * store, then calls changed(). A consumed request still returns XIAOTAI_OK.
 * XIAOTAI_ERR_BUSY while the system is quiesced (the value is unchanged and
 * the refusal counted), so nothing new is written between a maintenance
 * decision and the restart. */
xiaotai_err_t xiaotai_setting_set_int(const char *name, int32_t value);
/* An oversize text value returns INVALID_SIZE without changing memory,
 * writing to the store or calling changed(). */
xiaotai_err_t xiaotai_setting_set_text(const char *name, const char *value);
/* [any] Registered settings, in registration order; registration ends with
 * the last init(). */
size_t xiaotai_setting_count(void);
const xiaotai_setting_t *xiaotai_setting_at(size_t index);

/* ------------------------------------------------- commands and diagnostics */

typedef struct {
    void (*write)(void *ctx, const char *text);
    void *ctx;
} xiaotai_reply_t;

/* Longest command line with its terminator (the console's default line limit:
 * a 256-byte text as hexadecimal plus the command), and most words in one
 * line, the command name included. */
#define XIAOTAI_COMMAND_LINE_MAX 768U
#define XIAOTAI_COMMAND_ARGS_MAX 12U

/* argv[0] is the command name. Runs on the owner task. */
typedef xiaotai_err_t (*xiaotai_command_fn_t)(int argc, const char *const *argv, const xiaotai_reply_t *reply);
/* [init] name is the first word of the line ("AT+CALL"). help is the argument
 * synopsis ("<device-id>", "0|1"; NULL: none): the console prints it verbatim
 * after the name. Both have static storage. */
xiaotai_err_t xiaotai_command_register(const char *name, const char *help, xiaotai_command_fn_t handler);
/* For a dispatcher sharing one command between components: capture the
 * contributor's scope in init, then run just its handler on the owner in that
 * scope. The scope must come from this boot's registration. */
typedef struct {
    uint32_t generation;
    int16_t application;
} xiaotai_command_scope_t;
xiaotai_err_t xiaotai_command_scope_capture(xiaotai_command_scope_t *scope); /* [init] */
xiaotai_err_t xiaotai_command_invoke(xiaotai_command_scope_t scope, xiaotai_command_fn_t handler,
                                    int argc, const char *const *argv, const xiaotai_reply_t *reply); /* [owner] */
/* [any, not owner] Split line into words at spaces and tabs (nothing else
 * separates words, and quotes are not interpreted: a comma or a URL is part of
 * its word), run the command on the owner task, wait up to timeout_ms.
 * XIAOTAI_ERR_NOT_FOUND for an unknown command; XIAOTAI_ERR_INVALID_SIZE for a
 * line of XIAOTAI_COMMAND_LINE_MAX characters or more; XIAOTAI_ERR_INVALID_ARG
 * for an empty line or more than XIAOTAI_COMMAND_ARGS_MAX words. */
xiaotai_err_t xiaotai_command_run(const char *line, const xiaotai_reply_t *reply, uint32_t timeout_ms);
/* [any] Registered commands, in registration order; registration ends with
 * the last init(). */
size_t xiaotai_command_count(void);
void xiaotai_command_at(size_t index, const char **name, const char **help);

typedef void (*xiaotai_diag_fn_t)(const xiaotai_reply_t *out);
/* [init] The framework registers its own sections first:
 *   "framework"  one line "owner_stack= queue_depth= queue_peak= queue_full=
 *                foreground= activity= view_refused=" and one record
 *                "+FRAMEWORK-RESTART: quiesced= reason= restart= holds=
 *                guard_refusals= last_guard= restart_forced= restart_inline=
 *                settings_refused="; '-' stands for an empty value
 *   "tasks"      "supported=<0|1> count=<n>", then one record per task
 *                "+TASK: name= stack_min_free= priority= core=" from the
 *                system port's xiaotai_task_list() (supported=0 where the
 *                platform keeps no task list) */
xiaotai_err_t xiaotai_diag_register(const char *name, xiaotai_diag_fn_t report);
/* [owner] One provider by name, or all of them for NULL. */
void xiaotai_diag_report(const char *name, const xiaotai_reply_t *out);
/* [owner] Visits each matching section (all for NULL) in registration order.
 * Both arguments are borrowed; the visitor may run the provider synchronously. */
typedef void (*xiaotai_diag_visit_fn_t)(const char *name, xiaotai_diag_fn_t report, void *ctx);
void xiaotai_diag_visit_sections(const char *name, xiaotai_diag_visit_fn_t visit, void *ctx);

/* -------------------------------------------------------------------- start */

typedef struct {
    const xiaotai_module_t *const *modules;            /* this build's modules */
    size_t module_count;
    const xiaotai_application_t *const *applications;  /* this build's applications, in the product's order */
    size_t application_count;
    const char *home;                    /* application shown first; NULL: the first one */
    /* Activity priorities; they override the modules' defaults by name.
     * NULL: the modules' defaults only. */
    const xiaotai_priority_t *priorities;
    const xiaotai_preemption_t *preemptions;   /* NULL: none */
    /* XIAOTAI_NEED_* the board provides: xiaotai_hal_needs(), read after
     * xiaotai_hal_init() succeeded. The framework itself never touches the
     * board, so host tests pass any value; xiaotai_start() (xiaotai/xiaotai.h)
     * initializes the board first and refuses to start when that fails. */
    uint32_t available_needs;
    /* 0: the build options (xiaotai/framework_config.h). */
    uint32_t owner_stack_bytes;
    uint32_t queue_depth;
    uint32_t restart_delay_ms;
    /* NULL: xiaotai_restart() of the system port. Host tests pass their own. */
    void (*restart)(void);
    /* Local read-only startup on a recoverable cold storage backend error.
     * Default false: a storage init failure aborts startup. */
    bool local_on_storage_boot_failure;
    /* Reserve one existing queue slot for terminal owner events (depth 2..32768). */
    bool reserve_owner_terminal;
#if XIAOTAI_FRAMEWORK_COMPACT_SUBSCRIBERS
    /* The table has static lifetime. Entries are inactive until registered. */
    /* Immutable entries and borrowed names/contexts must survive until reset. */
    const xiaotai_subscriber_descriptor_t *compiled_subscribers;
    uint16_t compiled_subscriber_count;
#endif
} xiaotai_framework_config_t;

/* [glue] Called once, by xiaotai_start() (or a host test) on its own task,
 * never on the owner task. Initializes storage, then modules in dependency
 * order, then applications, on the owner task; starts them; shows the home
 * application. Returns the first failure (a missing dependency, an unmet
 * need, an init error), after which nothing of the framework runs, every
 * allocation it made is released and a new start may be made.
 * XIAOTAI_ERR_INVALID_STATE while the framework runs. */
xiaotai_err_t xiaotai_framework_start(const xiaotai_framework_config_t *config);
/* [glue] Same startup sequence, but the current task is the owner. No task or
 * stack is created, and a successful device start does not return. Bootstrap
 * errors still return; a host reset can stop the loop without deleting the
 * calling task. The caller must have the configured owner stack available. */
xiaotai_err_t xiaotai_framework_run_on_caller(const xiaotai_framework_config_t *config);
/* [owner] True on a borrowed owner, including during its initialization. */
bool xiaotai_framework_owner_on_caller(void);

/* [any] A framework owner still owns the runtime, even if the caller's
 * startup wait timed out. It is unsafe to take over its HAL parts. */
bool xiaotai_framework_active(void);
/* [any] Original storage error during a local read-only boot, or XIAOTAI_OK.
 * Published before any module initializes and immutable for the boot. */
xiaotai_err_t xiaotai_storage_boot_error(void);
/* [any] Host tests only, never on the owner task and not during bootstrap.
 * Stop the owner loop and forget every registration. A borrowed caller can
 * be reset after startup has succeeded, even though its call has not returned. */
void xiaotai_framework_reset(void);

#ifdef __cplusplus
}
#endif
