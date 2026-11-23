// SPDX-License-Identifier: MIT
/* XiaoTai runtime framework core: owner task, lifecycle, intents, events and
 * their presenting application, timers, foreground activity arbitration,
 * quiesce and its guards, the orderly restart, applications and views.
 * Registries are written only during initialization on the owner task and read
 * afterwards. */
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "internal.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/log.h"
#include "xiaotai/port/sys.h"
#include "xiaotai/view.h"

static const char *TAG = "xiaotai_fw";

typedef enum { MSG_INTENT = 0, MSG_EVENT, MSG_RUN, MSG_STOP, MSG_OWNER_TERMINAL } msg_kind_t;

typedef struct {
    msg_kind_t kind;
    union {
        struct {
            uint16_t index;
            bool external;
            xiaotai_args_t args;
            xiaotai_intent_stamp_t stamp;
        } intent;
        struct {
            const char *name;   /* resolved to a registered string */
            xiaotai_args_t args;
            void *data;        /* owned payload copy (BULK) */
            size_t size;
        } event;
        struct {
            int16_t app;       /* application that queued it; -1 otherwise */
            void (*fn)(void *ctx);
            void *ctx;
        } run;
    };
} fw_msg_t;

_Static_assert(sizeof(((fw_msg_t *)0)->intent) <= sizeof(((fw_msg_t *)0)->event),
               "intent metadata must not enlarge owner queue slots");
_Static_assert(sizeof(const xiaotai_intent_prepared_t *) <= sizeof(xiaotai_intent_fn_t),
               "prepared descriptor must fit the existing handler slot");

enum { INTENT_PREPARED = 1u << 1 };

typedef struct {
    const char *name;
    union {
        xiaotai_intent_fn_t handler;
        const xiaotai_intent_prepared_t *prepared;
    };
    void *ctx;
    uint32_t flags;
    int16_t app;               /* registering application; -1: an engine or module */
} intent_t;

typedef xiaotai_subscriber_descriptor_t subscriber_descriptor_t;

#if XIAOTAI_FRAMEWORK_COMPACT_SUBSCRIBERS
_Static_assert(XIAOTAI_FRAMEWORK_MAX_APPLICATIONS <= INT8_MAX + 1,
               "compact subscriber scopes must fit in int8_t");
typedef struct runtime_subscriber {
    subscriber_descriptor_t descriptor;
    struct runtime_subscriber *next;
} runtime_subscriber_t;

#else
typedef struct {
    subscriber_descriptor_t descriptor;
    int16_t app;               /* index of the subscribing application; -1: an engine or module */
} subscriber_t;
#endif

typedef struct {
    const char *activity;
    xiaotai_attachment_t attachment;
    int16_t app;
} attachment_t;

typedef struct {
    const char *name;
    union {
        xiaotai_quiesce_guard_fn_t guard;
        const xiaotai_quiesce_gate_t *gate;
    };
    void *ctx;
} guard_t;

typedef struct {
    uint32_t id;
    xiaotai_activity_t activity;
    uint8_t priority;
    int16_t app;               /* application that began it; -1: an engine or module */
} current_activity_t;

typedef struct {
    uint32_t id;               /* 0: free */
    int64_t due_us;
    xiaotai_timer_fn_t handler;
    void *ctx;
    int16_t app;
} fw_timer_t;

typedef enum {
    RESTART_IDLE = 0,
    RESTART_QUIESCING,         /* waiting for the quiesce guards */
    RESTART_HOLDING,           /* waiting for the restart holds */
    RESTART_DELAY,             /* announced; waiting for the delay */
    RESTART_DONE,              /* the restart function returned (host tests) */
} restart_phase_t;

typedef struct {
    xiaotai_framework_config_t config;
    const xiaotai_module_t *order[XIAOTAI_FRAMEWORK_MAX_MODULES];   /* dependency order */
    size_t order_count;
    intent_t intents[XIAOTAI_FRAMEWORK_MAX_INTENTS];
    _Atomic size_t intent_count;
#if XIAOTAI_FRAMEWORK_COMPACT_SUBSCRIBERS
    const subscriber_descriptor_t *subscribers[XIAOTAI_FRAMEWORK_MAX_SUBSCRIBERS];
    int8_t subscriber_app[XIAOTAI_FRAMEWORK_MAX_SUBSCRIBERS];
    runtime_subscriber_t *runtime_subscriber_first, *runtime_subscriber_last;
#else
    subscriber_t subscribers[XIAOTAI_FRAMEWORK_MAX_SUBSCRIBERS];
#endif
    _Atomic size_t subscriber_count;
    attachment_t attachments[XIAOTAI_FRAMEWORK_MAX_ATTACHMENTS];
    size_t attachment_count;
    guard_t guards[XIAOTAI_FRAMEWORK_MAX_QUIESCE_GUARDS];
    uint8_t guard_count, gate_count; /* Read-only guards grow from the front, gates from the back. */
    current_activity_t current;
    uint32_t next_activity_id;
    uint32_t quiesce_ticket;     /* 0: not quiesced */
    uint32_t next_quiesce_ticket;
    const char *quiesce_reason;
    fw_timer_t timers[XIAOTAI_FRAMEWORK_MAX_TIMERS];
    uint32_t next_timer_id;
    size_t foreground;           /* index into config.applications */
    bool has_foreground, intent_external;
    size_t home;                 /* index of the home application */
    xiaotai_app_entry_t entry;   /* why the foreground application came forward */
    /* Application whose code runs on the owner task now (init, start,
     * foreground, claim, handlers, timers, jobs it queued); -1: none. */
    int16_t running_app;
    bool has_return;             /* the framework brought `brought` forward over `return_to` */
    size_t return_to, brought;
    restart_phase_t restart_phase;
    bool restart_direct;         /* no quiesce or settings flush */
    int64_t restart_due_us;      /* the restart's own timer, outside the table; 0: none */
    int64_t restart_guard_deadline_us;
    char restart_reason[XIAOTAI_ARG_TEXT_MAX];
    uint32_t guard_refusals, restart_forced, restart_inline;
    const char *last_guard;
    xiaotai_view_sink_t view_sink;
    void *view_ctx;
    bool owner_on_caller;
} fw_state_t;

static fw_state_t *s_fw;
static xiaotai_task_t s_owner;
static xiaotai_queue_t s_queue;
static xiaotai_signal_t s_stopped;
static xiaotai_signal_t s_ready;
static atomic_bool s_running;
static atomic_bool s_initializing;
static atomic_bool s_started;
static _Atomic xiaotai_err_t s_storage_boot_error;
static _Atomic uint32_t s_activity_active;
static atomic_bool s_quiesced;
static atomic_bool s_restart_pending;
static _Atomic uint32_t s_restart_holds;
static _Atomic uint32_t s_queue_peak;
static _Atomic uint32_t s_queue_full;
static _Atomic uint32_t s_view_refused;
static xiaotai_err_t s_start_result;
/* Session generations: one counter for the whole boot, never reset. */
static _Atomic uint32_t s_generation;
/* The task that performs the restart; it never returns on a device. */
static xiaotai_task_t s_restart_task;

bool fw_initializing(void) { return atomic_load(&s_initializing); }
bool fw_started(void) { return atomic_load(&s_started); }
bool xiaotai_on_owner(void) { return atomic_load(&s_running) && xiaotai_task_is_current(&s_owner); }
bool xiaotai_framework_owner_on_caller(void) { return xiaotai_on_owner() && s_fw->owner_on_caller; }
bool xiaotai_framework_active(void) { return atomic_load(&s_running); }

int16_t fw_app_current(void) { return xiaotai_on_owner() && s_fw ? s_fw->running_app : -1; }

int16_t fw_app_enter(int16_t app)
{
    const int16_t previous = s_fw->running_app;
    s_fw->running_app = app;
    return previous;
}

void fw_app_leave(int16_t previous) { s_fw->running_app = previous; }

/* In reserve mode, keep occupancy and the diagnostic peak in the existing
 * 32-bit counter: ordinary claims (bits 0..14), one terminal (bit 15),
 * and peak (bits 16..31). Include producers that have not copied their item yet. */
enum { QUEUE_ORDINARY = 0x7fffU, QUEUE_TERMINAL = 0x8000U, QUEUE_PEAK_SHIFT = 16U };

static bool queue_claim(bool terminal)
{
    const uint32_t ordinary_limit = s_fw->config.queue_depth - 1U;
    const uint32_t count = terminal ? QUEUE_TERMINAL : 1U;
    uint32_t state = atomic_load(&s_queue_peak);
    do {
        if (terminal ? (state & QUEUE_TERMINAL) != 0U : (state & QUEUE_ORDINARY) >= ordinary_limit)
            return false;
    } while (!atomic_compare_exchange_weak(&s_queue_peak, &state, state + count));
    return true;
}

static void queue_unclaim(bool terminal)
{
    atomic_fetch_sub(&s_queue_peak, terminal ? QUEUE_TERMINAL : 1U);
}

/* Real-time callers come through here (xiaotai_run): no wait, no allocation,
 * no log; a full queue is counted. */
static xiaotai_err_t post(const fw_msg_t *message)
{
    if (!atomic_load(&s_running)) return XIAOTAI_ERR_INVALID_STATE;
    const bool reserve = s_fw->config.reserve_owner_terminal;
    const bool terminal = message->kind == MSG_OWNER_TERMINAL;
    if (reserve && !queue_claim(terminal)) {
        atomic_fetch_add(&s_queue_full, 1U);
        return XIAOTAI_ERR_NO_MEM;
    }
    if (!xiaotai_queue_send(&s_queue, message, 0)) {
        if (reserve) queue_unclaim(terminal);
        atomic_fetch_add(&s_queue_full, 1U);
        return XIAOTAI_ERR_NO_MEM;
    }
    const uint32_t used = (uint32_t)xiaotai_queue_count(&s_queue);
    uint32_t state = atomic_load(&s_queue_peak);
    while (used > (reserve ? state >> QUEUE_PEAK_SHIFT : state)) {
        const uint32_t next = reserve ? (state & UINT16_MAX) | (used << QUEUE_PEAK_SHIFT) : used;
        if (atomic_compare_exchange_weak(&s_queue_peak, &state, next)) break;
    }
    return XIAOTAI_OK;
}

uint32_t xiaotai_generation_next(void)
{
    uint32_t value;
    do {
        value = atomic_fetch_add(&s_generation, 1U) + 1U;
    } while (value == 0);
    return value;
}

static bool listed(const char *const *names, const char *name)
{
    for (const char *const *entry = names; entry && *entry; ++entry) {
        if (strcmp(*entry, name) == 0) return true;
    }
    return false;
}

/* --------------------------------------------------------------- timers */

xiaotai_err_t xiaotai_timer_start(uint32_t delay_ms, xiaotai_timer_fn_t handler, void *ctx, uint32_t *id)
{
    if (!handler || !id || delay_ms > XIAOTAI_TIMER_DELAY_MAX_MS) return XIAOTAI_ERR_INVALID_ARG;
    if (!s_fw || !xiaotai_on_owner()) return XIAOTAI_ERR_INVALID_STATE;
    for (size_t i = 0; i < XIAOTAI_FRAMEWORK_MAX_TIMERS; ++i) {
        fw_timer_t *timer = &s_fw->timers[i];
        if (timer->id) continue;
        if (++s_fw->next_timer_id == 0) s_fw->next_timer_id = 1;
        *timer = (fw_timer_t){.id = s_fw->next_timer_id, .due_us = xiaotai_time_us() + (int64_t)delay_ms * 1000,
                              .handler = handler, .ctx = ctx, .app = s_fw->running_app};
        *id = timer->id;
        return XIAOTAI_OK;
    }
    return XIAOTAI_ERR_NO_MEM;
}

void xiaotai_timer_cancel(uint32_t id)
{
    if (!s_fw || id == 0 || !xiaotai_on_owner()) return;
    for (size_t i = 0; i < XIAOTAI_FRAMEWORK_MAX_TIMERS; ++i) {
        if (s_fw->timers[i].id == id) s_fw->timers[i].id = 0;
    }
}

static void restart_step(void);

/* Runs every timer that was due when the pass began, earliest first. A timer a
 * handler starts is due after the pass began, so a pass always ends. The
 * restart has its own timer, so a full table can never hold it back. */
static void timers_run(void)
{
    const int64_t now = xiaotai_time_us();
    if (s_fw->restart_due_us && s_fw->restart_due_us <= now) {
        s_fw->restart_due_us = 0;
        restart_step();
    }
    for (size_t pass = 0; pass < XIAOTAI_FRAMEWORK_MAX_TIMERS; ++pass) {
        fw_timer_t *next = NULL;
        for (size_t i = 0; i < XIAOTAI_FRAMEWORK_MAX_TIMERS; ++i) {
            fw_timer_t *timer = &s_fw->timers[i];
            if (timer->id && timer->due_us <= now && (!next || timer->due_us < next->due_us)) next = timer;
        }
        if (!next) return;
        const fw_timer_t due = *next;
        next->id = 0;
        const int16_t previous = fw_app_enter(due.app);
        due.handler(due.ctx);
        fw_app_leave(previous);
    }
}

/* How long the owner may sleep before the next timer is due. */
static uint32_t timers_wait_ms(void)
{
    int64_t earliest = s_fw->restart_due_us ? s_fw->restart_due_us : INT64_MAX;
    for (size_t i = 0; i < XIAOTAI_FRAMEWORK_MAX_TIMERS; ++i) {
        if (s_fw->timers[i].id && s_fw->timers[i].due_us < earliest) earliest = s_fw->timers[i].due_us;
    }
    if (earliest == INT64_MAX) return XIAOTAI_WAIT_FOREVER;
    const int64_t remaining_us = earliest - xiaotai_time_us();
    if (remaining_us <= 0) return 0;
    return (uint32_t)((remaining_us + 999) / 1000);
}

/* ------------------------------------------------------------- modules */

static const xiaotai_module_t *find_module(const char *name)
{
    for (size_t i = 0; i < s_fw->config.module_count; ++i) {
        if (strcmp(s_fw->config.modules[i]->name, name) == 0) return s_fw->config.modules[i];
    }
    return NULL;
}

bool xiaotai_module_present(const char *name)
{
    if (!s_fw || !name) return false;
    for (size_t i = 0; i < s_fw->order_count; ++i) {
        if (strcmp(s_fw->order[i]->name, name) == 0) return true;
    }
    return false;
}

static size_t module_index(const xiaotai_module_t *module)
{
    for (size_t i = 0; i < s_fw->config.module_count; ++i) {
        if (s_fw->config.modules[i] == module) return i;
    }
    return SIZE_MAX;
}

/* Depth-first: place a module after its dependencies. marks: 0 new, 1 visiting, 2 placed. */
static xiaotai_err_t visit(const xiaotai_module_t *module, uint8_t *marks)
{
    const size_t index = module_index(module);
    if (marks[index] == 2) return XIAOTAI_OK;
    if (marks[index] == 1) {
        XIAOTAI_LOGE(TAG, "module dependency cycle at %s", module->name);
        return XIAOTAI_ERR_INVALID_STATE;
    }
    marks[index] = 1;
    for (const char *const *dep = module->depends; dep && *dep; ++dep) {
        const xiaotai_module_t *required = find_module(*dep);
        if (!required) {
            XIAOTAI_LOGE(TAG, "module %s needs module %s, which is not in this build", module->name, *dep);
            return XIAOTAI_ERR_NOT_FOUND;
        }
        const xiaotai_err_t err = visit(required, marks);
        if (err != XIAOTAI_OK) return err;
    }
    marks[index] = 2;
    s_fw->order[s_fw->order_count++] = module;
    return XIAOTAI_OK;
}

static xiaotai_err_t check_needs(const char *kind, const char *name, uint32_t needs)
{
    const uint32_t missing = needs & ~s_fw->config.available_needs;
    if (missing) {
        XIAOTAI_LOGE(TAG, "%s %s needs hardware this board lacks (0x%lx)", kind, name, (unsigned long)missing);
        return XIAOTAI_ERR_NOT_SUPPORTED;
    }
    return XIAOTAI_OK;
}

static xiaotai_err_t resolve(void)
{
    const xiaotai_framework_config_t *config = &s_fw->config;
    if (config->module_count > XIAOTAI_FRAMEWORK_MAX_MODULES ||
        config->application_count > XIAOTAI_FRAMEWORK_MAX_APPLICATIONS) {
        XIAOTAI_LOGE(TAG, "%u modules and %u applications exceed the build's capacity",
                     (unsigned)config->module_count, (unsigned)config->application_count);
        return XIAOTAI_ERR_INVALID_SIZE;
    }
    for (size_t a = 0; a < config->application_count; ++a) {
        const xiaotai_application_t *app = config->applications[a];
        xiaotai_err_t err = check_needs("application", app->name, app->needs);
        if (err != XIAOTAI_OK) return err;
        for (const char *const *name = app->modules; name && *name; ++name) {
            if (!find_module(*name)) {
                XIAOTAI_LOGE(TAG, "application %s needs module %s, which is not in this build", app->name, *name);
                return XIAOTAI_ERR_NOT_FOUND;
            }
        }
    }
    uint8_t marks[XIAOTAI_FRAMEWORK_MAX_MODULES] = {0};
    for (size_t i = 0; i < config->module_count; ++i) {
        xiaotai_err_t err = check_needs("module", config->modules[i]->name, config->modules[i]->needs);
        if (err == XIAOTAI_OK) err = visit(config->modules[i], marks);
        if (err != XIAOTAI_OK) return err;
    }
    return XIAOTAI_OK;
}

/* --------------------------------------------------------- applications */

static void app_foreground(size_t index, bool shown)
{
    const xiaotai_application_t *app = s_fw->config.applications[index];
    if (!app->foreground) return;
    const int16_t previous = fw_app_enter((int16_t)index);
    app->foreground(shown);
    fw_app_leave(previous);
}

static void set_foreground(size_t index, xiaotai_app_entry_t entry)
{
    const xiaotai_framework_config_t *config = &s_fw->config;
    if (s_fw->has_foreground && s_fw->foreground == index) return;
    if (s_fw->has_foreground) app_foreground(s_fw->foreground, false);
    s_fw->foreground = index;
    s_fw->has_foreground = true;
    s_fw->entry = entry;
    app_foreground(index, true);
    xiaotai_args_t args = {0};
    strncpy(args.text, config->applications[index]->name, sizeof(args.text) - 1);
    (void)xiaotai_event_emit(XIAOTAI_EVENT_FOREGROUND, &args, NULL, 0);
}

/* A switch the user or an application asked for replaces the framework's own. */
static void switch_to(size_t index, xiaotai_app_entry_t entry)
{
    s_fw->has_return = false;
    set_foreground(index, entry);
}

xiaotai_err_t xiaotai_app_switch(const char *name)
{
    if (!s_fw || !name) return XIAOTAI_ERR_INVALID_ARG;
    if (!xiaotai_on_owner()) return XIAOTAI_ERR_INVALID_STATE;
    for (size_t i = 0; i < s_fw->config.application_count; ++i) {
        if (strcmp(s_fw->config.applications[i]->name, name) == 0) {
            switch_to(i, XIAOTAI_APP_ENTRY_SWITCH);
            return XIAOTAI_OK;
        }
    }
    return XIAOTAI_ERR_NOT_FOUND;
}

static xiaotai_err_t step_application(bool forward)
{
    if (!s_fw || !xiaotai_on_owner()) return XIAOTAI_ERR_INVALID_STATE;
    const size_t count = s_fw->config.application_count;
    if (count < 2 || !s_fw->has_foreground) return XIAOTAI_ERR_NOT_FOUND;
    switch_to((s_fw->foreground + (forward ? 1 : count - 1)) % count,
              forward ? XIAOTAI_APP_ENTRY_NEXT : XIAOTAI_APP_ENTRY_PREVIOUS);
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_app_next(void) { return step_application(true); }
xiaotai_err_t xiaotai_app_previous(void) { return step_application(false); }

xiaotai_err_t xiaotai_app_page_step(int direction)
{
    if (direction != -1 && direction != 1) return XIAOTAI_ERR_INVALID_ARG;
    if (!xiaotai_on_owner() || !s_fw || !s_fw->has_foreground || xiaotai_quiesced())
        return XIAOTAI_ERR_INVALID_STATE;
    const size_t index = s_fw->foreground;
    const xiaotai_application_t *app = s_fw->config.applications[index];
    if (!app->page_step) return XIAOTAI_ERR_NOT_SUPPORTED;
    const int16_t previous = fw_app_enter((int16_t)index);
    const xiaotai_err_t result = app->page_step(direction);
    fw_app_leave(previous);
    return result;
}

xiaotai_err_t xiaotai_app_return(void)
{
    if (!s_fw || !xiaotai_on_owner()) return XIAOTAI_ERR_INVALID_STATE;
    if (!s_fw->has_return || s_fw->foreground != s_fw->brought) return XIAOTAI_ERR_INVALID_STATE;
    switch_to(s_fw->return_to, XIAOTAI_APP_ENTRY_RETURN);
    return XIAOTAI_OK;
}

const char *xiaotai_app_foreground(void)
{
    return s_fw && s_fw->has_foreground ? s_fw->config.applications[s_fw->foreground]->name : NULL;
}

xiaotai_app_entry_t xiaotai_app_entry(void) { return s_fw ? s_fw->entry : XIAOTAI_APP_ENTRY_START; }

const char *xiaotai_app_home(void)
{
    return s_fw && s_fw->config.application_count ? s_fw->config.applications[s_fw->home]->name : NULL;
}

size_t xiaotai_app_count(void) { return s_fw ? s_fw->config.application_count : 0; }

static bool presented(const char *name)
{
    for (size_t i = 0; i < s_fw->config.application_count; ++i) {
        if (listed(s_fw->config.applications[i]->handles, name)) return true;
    }
    return false;
}

/* The first application in the product's order that lists the event and
 * claims it; otherwise the foreground one. */
static bool app_claims(size_t index, const xiaotai_event_t *event)
{
    const xiaotai_application_t *app = s_fw->config.applications[index];
    if (!listed(app->handles, event->name)) return false;
    if (!app->claim) return true;
    const int16_t previous = fw_app_enter((int16_t)index);
    const bool claimed = app->claim(event);
    fw_app_leave(previous);
    return claimed;
}

static size_t presenter(const xiaotai_event_t *event)
{
    for (size_t i = 0; i < s_fw->config.application_count; ++i) {
        if (app_claims(i, event)) return i;
    }
    return s_fw->foreground;
}

static void bring_forward(size_t index)
{
    if (s_fw->foreground == index) return;
    if (!s_fw->has_return) {
        s_fw->return_to = s_fw->foreground;
        s_fw->has_return = true;
    }
    s_fw->brought = index;
    set_foreground(index, XIAOTAI_APP_ENTRY_PRESENT);
}

/* ------------------------------------------------------------- restart */

static void restart_arm(uint32_t delay_ms)
{
    s_fw->restart_due_us = xiaotai_time_us() + (int64_t)delay_ms * 1000;
}

static void quiesce_take(const char *reason);
static xiaotai_err_t ask_guards(const char *reason);

/* The restart function is read once, on the owner task, before the restart
 * task exists; the state is not touched afterwards. */
static void (*s_restart_fn)(void);

static void restart_now(void)
{
    if (s_restart_fn) {
        s_restart_fn();
    } else {
        xiaotai_restart();
    }
}

/* Internal stack: see step 4 in xiaotai/framework.h. On a device it does not
 * return; a host test's restart function does, and the task ends. */
static void restart_task(void *arg)
{
    (void)arg;
    restart_now();
    xiaotai_task_exit();
}

static void restart_step(void)
{
    switch (s_fw->restart_phase) {
    case RESTART_QUIESCING:
        if (!s_fw->quiesce_ticket) {
            if (ask_guards(s_fw->restart_reason) != XIAOTAI_OK) {
                if (xiaotai_time_us() < s_fw->restart_guard_deadline_us) {
                    restart_arm(FW_RESTART_POLL_MS);
                    return;
                }
                ++s_fw->restart_forced;
                XIAOTAI_LOGW(TAG, "restart: guard %s still refuses; quiescing without it",
                             s_fw->last_guard ? s_fw->last_guard : "?");
            }
            quiesce_take(s_fw->restart_reason);
        }
        s_fw->restart_phase = RESTART_HOLDING;
        /* fall through */
    case RESTART_HOLDING:
        if (atomic_load(&s_restart_holds)) {
            restart_arm(FW_RESTART_POLL_MS);
            return;
        }
        {
            xiaotai_args_t args = {.value = (int32_t)s_fw->config.restart_delay_ms};
            memcpy(args.text, s_fw->restart_reason, sizeof(args.text));
            (void)xiaotai_event_emit(XIAOTAI_EVENT_RESTARTING, &args, NULL, 0);
        }
        XIAOTAI_LOGI(TAG, "restart (%s) in %lu ms", s_fw->restart_reason, (unsigned long)s_fw->config.restart_delay_ms);
        s_fw->restart_phase = RESTART_DELAY;
        restart_arm(s_fw->config.restart_delay_ms);
        return;
    case RESTART_DELAY: {
        s_fw->restart_phase = RESTART_DONE;
        if (!s_fw->restart_direct && fw_settings_flush(FW_RESTART_FLUSH_MS) != XIAOTAI_OK) {
            XIAOTAI_LOGW(TAG, "restart: settings not flushed");
        }
        s_restart_fn = s_fw->config.restart;
        const xiaotai_task_config_t task = {
            .name = "xt_restart", .entry = restart_task, .stack_bytes = XIAOTAI_FRAMEWORK_RESTART_STACK_BYTES,
            .stack = XIAOTAI_MEM_INTERNAL, .priority = XIAOTAI_FRAMEWORK_OWNER_PRIORITY, .core = XIAOTAI_CORE_ANY,
        };
        if (xiaotai_task_create(&task, &s_restart_task) != XIAOTAI_OK) {
            ++s_fw->restart_inline;
            XIAOTAI_LOGW(TAG, "restart: no internal stack for the restart task; restarting on the owner task");
            restart_now();
        }
        return;
    }
    default:
        return;
    }
}

static void restart_schedule(const char *reason, bool direct)
{
    s_fw->restart_direct = direct;
    memset(s_fw->restart_reason, 0, sizeof(s_fw->restart_reason));
    strncpy(s_fw->restart_reason, reason && *reason ? reason : "restart", sizeof(s_fw->restart_reason) - 1);
    if (s_fw->quiesce_ticket) s_fw->quiesce_reason = s_fw->restart_reason;
    atomic_store(&s_restart_pending, true);
    XIAOTAI_LOGI(TAG, "restart requested: %s%s", s_fw->restart_reason, direct ? " (direct)" : "");
    restart_arm(0);
}

xiaotai_err_t xiaotai_restart_request(const char *reason, uint32_t quiesce_ticket)
{
    if (!s_fw || !xiaotai_on_owner() || !fw_started()) return XIAOTAI_ERR_INVALID_STATE;
    if (atomic_load(&s_restart_pending)) return XIAOTAI_OK;
    if (s_fw->quiesce_ticket) {
        if (quiesce_ticket == 0 || quiesce_ticket != s_fw->quiesce_ticket) return XIAOTAI_ERR_BUSY;
        /* A new ticket: the former holder's xiaotai_quiesce_end() no longer
         * matches, so nothing can begin before the restart. */
        if (++s_fw->next_quiesce_ticket == 0) s_fw->next_quiesce_ticket = 1;
        s_fw->quiesce_ticket = s_fw->next_quiesce_ticket;
        s_fw->restart_phase = RESTART_HOLDING;
    } else {
        s_fw->restart_phase = RESTART_QUIESCING;
        s_fw->restart_guard_deadline_us = xiaotai_time_us() + (int64_t)XIAOTAI_FRAMEWORK_RESTART_GUARD_WAIT_MS * 1000;
    }
    restart_schedule(reason, false);
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_restart_request_direct(const char *reason)
{
    if (!s_fw || !xiaotai_on_owner() || !fw_started()) return XIAOTAI_ERR_INVALID_STATE;
    if (atomic_load(&s_restart_pending)) return XIAOTAI_OK;
    if (s_fw->quiesce_ticket) return XIAOTAI_ERR_BUSY;
    s_fw->restart_phase = RESTART_HOLDING;
    restart_schedule(reason, true);
    return XIAOTAI_OK;
}

void xiaotai_restart_hold(void) { atomic_fetch_add(&s_restart_holds, 1U); }

void xiaotai_restart_release(void)
{
    uint32_t holds = atomic_load(&s_restart_holds);
    while (holds && !atomic_compare_exchange_weak(&s_restart_holds, &holds, holds - 1U)) {
    }
}

bool xiaotai_restart_pending(void) { return atomic_load(&s_restart_pending); }

static void on_restart_intent(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    const xiaotai_err_t err = xiaotai_restart_request(args->text[0] ? args->text : "intent", 0);
    if (err != XIAOTAI_OK) {
        XIAOTAI_LOGW(TAG, "restart refused: quiesced for %s", s_fw->quiesce_reason ? s_fw->quiesce_reason : "?");
    }
}

/* ---------------------------------------------------------- diagnostics */

/* Diagnostic values: '-' stands for an empty one, so every line splits into
 * key=value words. */
static const char *word(const char *value) { return value && *value ? value : "-"; }

static void diag_framework(const xiaotai_reply_t *out)
{
    char line[224];
    const uint32_t state = atomic_load(&s_queue_peak);
    const uint32_t peak = s_fw->config.reserve_owner_terminal ? state >> QUEUE_PEAK_SHIFT : state;
    snprintf(line, sizeof(line),
             "owner_stack=%lu queue_depth=%lu queue_peak=%lu queue_full=%lu foreground=%s activity=%s "
             "view_refused=%lu",
             (unsigned long)s_fw->config.owner_stack_bytes, (unsigned long)s_fw->config.queue_depth,
             (unsigned long)peak, (unsigned long)atomic_load(&s_queue_full),
             word(xiaotai_app_foreground()), word(s_fw->current.id ? s_fw->current.activity.name : NULL),
             (unsigned long)atomic_load(&s_view_refused));
    out->write(out->ctx, line);
    snprintf(line, sizeof(line),
             "+FRAMEWORK-RESTART: quiesced=%d reason=%s restart=%d holds=%lu guard_refusals=%lu last_guard=%s "
             "restart_forced=%lu restart_inline=%lu settings_refused=%lu",
             s_fw->quiesce_ticket ? 1 : 0, word(s_fw->quiesce_ticket ? s_fw->quiesce_reason : NULL),
             atomic_load(&s_restart_pending) ? 1 : 0, (unsigned long)atomic_load(&s_restart_holds),
             (unsigned long)s_fw->guard_refusals, word(s_fw->last_guard), (unsigned long)s_fw->restart_forced,
             (unsigned long)s_fw->restart_inline, (unsigned long)fw_settings_refused());
    out->write(out->ctx, line);
}

/* Most tasks listed; the copy lives in BULK memory for the report only, so
 * the handler stays inside its stack budget. */
#define DIAG_TASKS_MAX 48U

static void diag_tasks(const xiaotai_reply_t *out)
{
    char line[96];
    size_t count = 0;
    xiaotai_task_info_t *tasks = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, DIAG_TASKS_MAX, sizeof(*tasks));
    const xiaotai_err_t err = tasks ? xiaotai_task_list(tasks, DIAG_TASKS_MAX, &count) : XIAOTAI_ERR_NO_MEM;
    if (err != XIAOTAI_OK) count = 0;
    snprintf(line, sizeof(line), "supported=%d count=%lu%s", err == XIAOTAI_ERR_NOT_SUPPORTED ? 0 : 1,
             (unsigned long)count, err == XIAOTAI_ERR_NO_MEM ? " error=no_mem" : "");
    out->write(out->ctx, line);
    for (size_t i = 0; i < count && i < DIAG_TASKS_MAX; ++i) {
        snprintf(line, sizeof(line), "+TASK: name=%s stack_min_free=%lu priority=%u core=%d", word(tasks[i].name),
                 (unsigned long)tasks[i].stack_min_free, tasks[i].priority, tasks[i].core);
        out->write(out->ctx, line);
    }
    xiaotai_mem_free(tasks);
}

/* ------------------------------------------------------------ lifecycle */

xiaotai_err_t xiaotai_storage_boot_error(void) { return atomic_load(&s_storage_boot_error); }

static xiaotai_err_t initialize(void)
{
    xiaotai_err_t local_error = XIAOTAI_OK;
    xiaotai_err_t err = xiaotai_kv_init_boot(s_fw->config.local_on_storage_boot_failure ? &local_error : NULL);
    if (err != XIAOTAI_OK) {
        XIAOTAI_LOGE(TAG, "storage init failed: %d", err);
        if (local_error == XIAOTAI_OK) return err;
        atomic_store(&s_storage_boot_error, local_error);
        XIAOTAI_LOGW(TAG, "starting local read-only");
    }
    if ((err = resolve()) != XIAOTAI_OK) return err;
    const xiaotai_framework_config_t *config = &s_fw->config;
    for (size_t i = 0; config->home && i < config->application_count; ++i) {
        if (strcmp(config->applications[i]->name, config->home) == 0) s_fw->home = i;
    }
    atomic_store(&s_initializing, true);
    err = xiaotai_intent_handle(XIAOTAI_INTENT_SYSTEM_RESTART, on_restart_intent, NULL);
    if (err == XIAOTAI_OK) err = fw_diag_add("framework", diag_framework);
    if (err == XIAOTAI_OK) err = fw_diag_add("tasks", diag_tasks);
    for (size_t i = 0; i < s_fw->order_count && err == XIAOTAI_OK; ++i) {
        if (s_fw->order[i]->init) err = s_fw->order[i]->init();
        if (err != XIAOTAI_OK) XIAOTAI_LOGE(TAG, "module %s init failed: %d", s_fw->order[i]->name, err);
    }
    for (size_t i = 0; i < config->application_count && err == XIAOTAI_OK; ++i) {
        const int16_t previous = fw_app_enter((int16_t)i);
        if (config->applications[i]->init) err = config->applications[i]->init();
        fw_app_leave(previous);
        if (err != XIAOTAI_OK) XIAOTAI_LOGE(TAG, "application %s init failed: %d", config->applications[i]->name, err);
    }
    atomic_store(&s_initializing, false);
    if (err != XIAOTAI_OK) return err;
    atomic_store(&s_started, true);
    for (size_t i = 0; i < s_fw->order_count; ++i) {
        if (s_fw->order[i]->start) s_fw->order[i]->start();
    }
    for (size_t i = 0; i < config->application_count; ++i) {
        if (!config->applications[i]->start) continue;
        const int16_t previous = fw_app_enter((int16_t)i);
        config->applications[i]->start();
        fw_app_leave(previous);
    }
    if (config->application_count) set_foreground(s_fw->home, XIAOTAI_APP_ENTRY_START);
    (void)xiaotai_event_emit(XIAOTAI_EVENT_STARTED, NULL, NULL, 0);
    return XIAOTAI_OK;
}

static inline const subscriber_descriptor_t *subscriber_at(size_t i, int16_t *app)
{
#if XIAOTAI_FRAMEWORK_COMPACT_SUBSCRIBERS
    if (app) *app = s_fw->subscriber_app[i];
    return s_fw->subscribers[i];
#else
    const subscriber_t *entry = &s_fw->subscribers[i];
    if (app) *app = entry->app;
    return &entry->descriptor;
#endif
}

static void dispatch_event(const fw_msg_t *message)
{
    const xiaotai_event_t event = {
        .name = message->event.name, .args = message->event.args,
        .data = message->event.data, .size = message->event.size,
    };
    /* An event an application presents goes to one application only. */
    const bool routed = s_fw->has_foreground && presented(event.name);
    size_t target = 0;
    if (routed) {
        target = presenter(&event);
        bring_forward(target);
    }
    const size_t count = atomic_load(&s_fw->subscriber_count);
    for (size_t i = 0; i < count; ++i) {
        int16_t app;
        const subscriber_descriptor_t *subscriber = subscriber_at(i, &app);
        if (strcmp(subscriber->name, event.name) != 0) continue;
        if (routed && app >= 0 && (size_t)app != target) continue;
        const int16_t previous = fw_app_enter(app);
        subscriber->handler(&event, subscriber->ctx);
        fw_app_leave(previous);
    }
    xiaotai_mem_free(message->event.data);
}

static void owner_loop(void)
{
    for (;;) {
        fw_msg_t message;
        /* Due timers run between messages, so a busy queue cannot starve them
         * and they cannot starve the queue. */
        timers_run();
        if (!xiaotai_queue_receive(&s_queue, &message, timers_wait_ms())) continue;
        if (message.kind == MSG_STOP) break;
        if (s_fw->config.reserve_owner_terminal) queue_unclaim(message.kind == MSG_OWNER_TERMINAL);
        switch (message.kind) {
        case MSG_INTENT: {
            const intent_t *intent = &s_fw->intents[message.intent.index];
            const int16_t previous = fw_app_enter(intent->app);
            const bool previous_external = s_fw->intent_external;
            s_fw->intent_external = message.intent.external;
            if (intent->flags & INTENT_PREPARED)
                intent->prepared->dispatch(&message.intent.args, &message.intent.stamp, intent->ctx);
            else intent->handler(&message.intent.args, intent->ctx);
            s_fw->intent_external = previous_external;
            fw_app_leave(previous);
            break;
        }
        case MSG_EVENT:
        case MSG_OWNER_TERMINAL:
            dispatch_event(&message);
            break;
        case MSG_RUN: {
            const int16_t previous = fw_app_enter(message.run.app);
            message.run.fn(message.run.ctx);
            fw_app_leave(previous);
            break;
        }
        default:
            break;
        }
    }
}

static void owner(void *arg)
{
    (void)arg;
    /* ESP task creation may run the owner before returning its handle. */
    (void)xiaotai_task_wait_notify(XIAOTAI_WAIT_FOREVER);
    s_start_result = initialize();
    xiaotai_signal_give(&s_ready);
    if (s_start_result == XIAOTAI_OK) owner_loop();
    xiaotai_signal_give(&s_stopped);
    xiaotai_task_exit();
}

/* Releases everything a start created; the owner task has stopped. */
static void teardown(void)
{
    if (atomic_load(&s_running)) {
        atomic_store(&s_running, false);
        fw_msg_t leftover;
        while (xiaotai_queue_receive(&s_queue, &leftover, 0)) {
            if (leftover.kind == MSG_EVENT || leftover.kind == MSG_OWNER_TERMINAL)
                xiaotai_mem_free(leftover.event.data);
        }
        xiaotai_queue_deinit(&s_queue);
        if (!s_fw->owner_on_caller) xiaotai_signal_deinit(&s_ready);
        xiaotai_signal_deinit(&s_stopped);
    }
    fw_settings_reset();
    fw_commands_reset();
#if XIAOTAI_FRAMEWORK_COMPACT_SUBSCRIBERS
    runtime_subscriber_t *entry = s_fw ? s_fw->runtime_subscriber_first : NULL;
    while (entry) {
        runtime_subscriber_t *next = entry->next;
        xiaotai_mem_free(entry);
        entry = next;
    }
#endif
    xiaotai_mem_free(s_fw);
    s_fw = NULL;
    atomic_store(&s_started, false);
    atomic_store(&s_storage_boot_error, XIAOTAI_OK);
    atomic_store(&s_activity_active, 0U);
    atomic_store(&s_initializing, false);
    atomic_store(&s_quiesced, false);
    atomic_store(&s_restart_pending, false);
    atomic_store(&s_restart_holds, 0U);
    atomic_store(&s_queue_peak, 0U);
    atomic_store(&s_queue_full, 0U);
    atomic_store(&s_view_refused, 0U);
}

static xiaotai_err_t prepare_start(const xiaotai_framework_config_t *config, bool owner_on_caller)
{
    if (!config || (config->module_count && !config->modules) ||
        (config->application_count && !config->applications)) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
#if XIAOTAI_FRAMEWORK_COMPACT_SUBSCRIBERS
    if (config->compiled_subscriber_count && !config->compiled_subscribers) return XIAOTAI_ERR_INVALID_ARG;
#endif
    if (s_fw) return XIAOTAI_ERR_INVALID_STATE;
    s_fw = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*s_fw));
    if (!s_fw) return XIAOTAI_ERR_NO_MEM;
    s_fw->config = *config;
    s_fw->owner_on_caller = owner_on_caller;
    s_fw->running_app = -1;
    if (!s_fw->config.owner_stack_bytes) s_fw->config.owner_stack_bytes = XIAOTAI_FRAMEWORK_OWNER_STACK_BYTES;
    if (!s_fw->config.queue_depth) s_fw->config.queue_depth = XIAOTAI_FRAMEWORK_QUEUE_DEPTH;
    if (!s_fw->config.restart_delay_ms) s_fw->config.restart_delay_ms = XIAOTAI_FRAMEWORK_RESTART_DELAY_MS;
    xiaotai_err_t err = XIAOTAI_ERR_INVALID_SIZE;
    if (s_fw->config.reserve_owner_terminal &&
        (s_fw->config.queue_depth < 2U || s_fw->config.queue_depth > QUEUE_TERMINAL)) goto release_state;
    err = xiaotai_queue_init(&s_queue, s_fw->config.queue_depth, sizeof(fw_msg_t), XIAOTAI_MEM_BULK);
    if (err != XIAOTAI_OK) goto release_state;
    if (!owner_on_caller) {
        err = xiaotai_signal_init(&s_ready);
        if (err != XIAOTAI_OK) goto release_queue;
    }
    err = xiaotai_signal_init(&s_stopped);
    if (err != XIAOTAI_OK) goto release_ready;
    return XIAOTAI_OK;

release_ready:
    if (!owner_on_caller) xiaotai_signal_deinit(&s_ready);
release_queue:
    xiaotai_queue_deinit(&s_queue);
release_state:
    teardown();
    return err;
}

xiaotai_err_t xiaotai_framework_start(const xiaotai_framework_config_t *config)
{
    xiaotai_err_t err = prepare_start(config, false);
    if (err != XIAOTAI_OK) return err;
    const xiaotai_task_config_t task = {
        .name = "xiaotai", .entry = owner, .stack_bytes = s_fw->config.owner_stack_bytes,
        .stack = XIAOTAI_MEM_BULK, .priority = XIAOTAI_FRAMEWORK_OWNER_PRIORITY, .core = XIAOTAI_CORE_ANY,
    };
    err = xiaotai_task_create(&task, &s_owner);
    if (err != XIAOTAI_OK) {
        xiaotai_signal_deinit(&s_stopped);
        xiaotai_signal_deinit(&s_ready);
        xiaotai_queue_deinit(&s_queue);
        teardown();
        return err;
    }
    atomic_store(&s_running, true);
    xiaotai_task_notify(&s_owner);
    if (!xiaotai_signal_take(&s_ready, FW_START_WAIT_MS)) return XIAOTAI_ERR_TIMEOUT;
    if (s_start_result != XIAOTAI_OK) {
        (void)xiaotai_signal_take(&s_stopped, XIAOTAI_WAIT_FOREVER);
        teardown();
    }
    return s_start_result;
}

xiaotai_err_t xiaotai_framework_run_on_caller(const xiaotai_framework_config_t *config)
{
    xiaotai_task_t caller = {0};
    xiaotai_err_t err = xiaotai_task_borrow_current(&caller);
    if (err != XIAOTAI_OK) return err;
    err = prepare_start(config, true);
    if (err != XIAOTAI_OK) return err;
    s_owner = caller;
    atomic_store(&s_running, true);
    unsigned priority = 0;
    err = xiaotai_task_set_current_priority(XIAOTAI_FRAMEWORK_OWNER_PRIORITY, &priority);
    if (err != XIAOTAI_OK) {
        teardown();
        return err;
    }
    err = initialize();
    if (err != XIAOTAI_OK) {
        teardown();
        (void)xiaotai_task_set_current_priority(priority, NULL);
        return err;
    }
    owner_loop();
    (void)xiaotai_task_set_current_priority(priority, NULL);
    xiaotai_signal_give(&s_stopped);
    return XIAOTAI_OK;
}

void xiaotai_framework_reset(void)
{
    if (atomic_load(&s_running)) {
        const fw_msg_t stop = {.kind = MSG_STOP};
        while (!xiaotai_queue_send(&s_queue, &stop, 100)) {
        }
        (void)xiaotai_signal_take(&s_stopped, XIAOTAI_WAIT_FOREVER);
    }
    teardown();
}

xiaotai_err_t xiaotai_run(void (*fn)(void *ctx), void *ctx)
{
    if (!fn) return XIAOTAI_ERR_INVALID_ARG;
    /* A job an application queues on the owner task runs as that application. */
    const fw_msg_t message = {.kind = MSG_RUN, .run = {.app = fw_app_current(), .fn = fn, .ctx = ctx}};
    return post(&message);
}

/* -------------------------------------------------------------- intents */

static xiaotai_err_t register_intent(intent_t intent)
{
    if (!fw_initializing()) return XIAOTAI_ERR_INVALID_STATE;
    const size_t count = atomic_load(&s_fw->intent_count);
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(s_fw->intents[i].name, intent.name) == 0) {
            XIAOTAI_LOGE(TAG, "intent %s already has a handler", intent.name);
            return XIAOTAI_ERR_INVALID_STATE;
        }
    }
    if (count >= XIAOTAI_FRAMEWORK_MAX_INTENTS) return XIAOTAI_ERR_NO_MEM;
    intent.app = s_fw->running_app;
    s_fw->intents[count] = intent;
    atomic_store(&s_fw->intent_count, count + 1);
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_intent_handle_ex(const char *name, xiaotai_intent_fn_t handler, void *ctx, uint32_t flags)
{
    if (!name || !handler || (flags & ~(uint32_t)XIAOTAI_INTENT_LOCAL)) return XIAOTAI_ERR_INVALID_ARG;
    return register_intent((intent_t){.name = name, .handler = handler, .ctx = ctx, .flags = flags});
}

xiaotai_err_t xiaotai_intent_handle_prepared(const char *name, const xiaotai_intent_prepared_t *handler,
                                            void *ctx, uint32_t flags)
{
    if (!name || !handler || !handler->prepare || !handler->dispatch ||
        (flags & ~(uint32_t)XIAOTAI_INTENT_LOCAL)) return XIAOTAI_ERR_INVALID_ARG;
    return register_intent((intent_t){.name = name, .prepared = handler, .ctx = ctx, .flags = flags | INTENT_PREPARED});
}

xiaotai_err_t xiaotai_intent_handle(const char *name, xiaotai_intent_fn_t handler, void *ctx)
{
    return xiaotai_intent_handle_ex(name, handler, ctx, 0);
}

static int find_intent(const char *name)
{
    if (!s_fw || !name) return -1;
    const size_t count = atomic_load(&s_fw->intent_count);
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(s_fw->intents[i].name, name) == 0) return (int)i;
    }
    return -1;
}

bool xiaotai_intent_exists(const char *name) { return find_intent(name) >= 0; }

static xiaotai_err_t intent_post(const char *name, const xiaotai_args_t *args, bool external)
{
    if (!fw_started()) return XIAOTAI_ERR_INVALID_STATE;
    const int index = find_intent(name);
    if (index < 0) return XIAOTAI_ERR_NOT_FOUND;
    const intent_t *intent = &s_fw->intents[index];
    if (external && (intent->flags & XIAOTAI_INTENT_LOCAL)) return XIAOTAI_ERR_NOT_ALLOWED;
    fw_msg_t message = {.kind = MSG_INTENT, .intent = {.index = (uint16_t)index, .external = external}};
    if (args) message.intent.args = *args;
    if (intent->flags & INTENT_PREPARED) {
        const xiaotai_err_t err = intent->prepared->prepare(&message.intent.args, &message.intent.stamp, intent->ctx);
        if (err != XIAOTAI_OK) return err;
    }
    return post(&message);
}

xiaotai_err_t xiaotai_intent_post(const char *name, const xiaotai_args_t *args)
{
    return intent_post(name, args, false);
}

xiaotai_err_t xiaotai_intent_post_external(const char *name, const xiaotai_args_t *args)
{
    return intent_post(name, args, true);
}

bool xiaotai_intent_from_external(void)
{
    return xiaotai_on_owner() && s_fw && s_fw->intent_external;
}

/* --------------------------------------------------------------- events */

#if XIAOTAI_FRAMEWORK_COMPACT_SUBSCRIBERS
static void activate_subscriber(size_t position, const subscriber_descriptor_t *descriptor)
{
    s_fw->subscribers[position] = descriptor;
    s_fw->subscriber_app[position] = (int8_t)s_fw->running_app;
    atomic_store(&s_fw->subscriber_count, position + 1);
}

xiaotai_err_t xiaotai_event_subscribe_compiled(uint16_t descriptor_id)
{
    if (!fw_initializing() || !xiaotai_on_owner()) return XIAOTAI_ERR_INVALID_STATE;
    if (descriptor_id >= s_fw->config.compiled_subscriber_count) return XIAOTAI_ERR_INVALID_ARG;
    return xiaotai_event_subscribe_static(&s_fw->config.compiled_subscribers[descriptor_id]);
}

xiaotai_err_t xiaotai_event_subscribe_static(const xiaotai_subscriber_descriptor_t *descriptor)
{
    if (!descriptor || !descriptor->name || !descriptor->handler) return XIAOTAI_ERR_INVALID_ARG;
    if (!fw_initializing() || !xiaotai_on_owner()) return XIAOTAI_ERR_INVALID_STATE;
    const size_t count = atomic_load(&s_fw->subscriber_count);
    if (count >= XIAOTAI_FRAMEWORK_MAX_SUBSCRIBERS) return XIAOTAI_ERR_NO_MEM;
    activate_subscriber(count, descriptor);
    return XIAOTAI_OK;
}
#endif

xiaotai_err_t xiaotai_event_subscribe(const char *name, xiaotai_event_fn_t handler, void *ctx)
{
    if (!name || !handler) return XIAOTAI_ERR_INVALID_ARG;
    if (!fw_initializing()) return XIAOTAI_ERR_INVALID_STATE;
#if XIAOTAI_FRAMEWORK_COMPACT_SUBSCRIBERS
    if (!xiaotai_on_owner()) return XIAOTAI_ERR_INVALID_STATE;
#endif
    const size_t count = atomic_load(&s_fw->subscriber_count);
    if (count >= XIAOTAI_FRAMEWORK_MAX_SUBSCRIBERS) return XIAOTAI_ERR_NO_MEM;
    const subscriber_descriptor_t descriptor = {.name = name, .handler = handler, .ctx = ctx};
#if XIAOTAI_FRAMEWORK_COMPACT_SUBSCRIBERS
    runtime_subscriber_t *entry = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, sizeof(*entry));
    if (!entry) return XIAOTAI_ERR_NO_MEM;
    *entry = (runtime_subscriber_t){.descriptor = descriptor};
    if (s_fw->runtime_subscriber_last) s_fw->runtime_subscriber_last->next = entry;
    else s_fw->runtime_subscriber_first = entry;
    s_fw->runtime_subscriber_last = entry;
    activate_subscriber(count, &entry->descriptor);
#else
    s_fw->subscribers[count] = (subscriber_t){.descriptor = descriptor, .app = s_fw->running_app};
    atomic_store(&s_fw->subscriber_count, count + 1);
#endif
    return XIAOTAI_OK;
}

static xiaotai_err_t emit_event(const char *name, const xiaotai_args_t *args, const void *data,
                                size_t size, bool optional, bool owner_terminal)
{
    if (!name || (size && !data)) return XIAOTAI_ERR_INVALID_ARG;
    if (size > XIAOTAI_EVENT_DATA_MAX) return XIAOTAI_ERR_INVALID_SIZE;
    if (!s_fw || !atomic_load(&s_running)) return XIAOTAI_ERR_INVALID_STATE;
    /* Resolve to the registered name, so the caller's string need not outlive the call. */
    const char *registered = NULL;
    const size_t count = atomic_load(&s_fw->subscriber_count);
    for (size_t i = 0; i < count && !registered; ++i) {
        const char *candidate = subscriber_at(i, NULL)->name;
        if (strcmp(candidate, name) == 0) registered = candidate;
    }
    if (!registered) return XIAOTAI_OK;
    const msg_kind_t kind = owner_terminal && s_fw->config.reserve_owner_terminal ? MSG_OWNER_TERMINAL : MSG_EVENT;
    fw_msg_t message = {.kind = kind, .event = {.name = registered, .size = size}};
    if (args) message.event.args = *args;
    if (size) {
        message.event.data = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, size);
        if (message.event.data) memcpy(message.event.data, data, size);
        else if (optional) message.event.size = 0;
        else return XIAOTAI_ERR_NO_MEM;
    }
    const xiaotai_err_t err = post(&message);
    if (err != XIAOTAI_OK) xiaotai_mem_free(message.event.data);
    return err;
}

xiaotai_err_t xiaotai_event_emit(const char *name, const xiaotai_args_t *args, const void *data, size_t size)
{
    return emit_event(name, args, data, size, false, false);
}

xiaotai_err_t xiaotai_event_emit_owner_terminal(const char *name, const xiaotai_args_t *args,
                                              const void *data, size_t size)
{
    if (!xiaotai_on_owner()) return XIAOTAI_ERR_INVALID_STATE;
    return emit_event(name, args, data, size, false, true);
}

xiaotai_err_t fw_event_emit_optional(const char *name, const xiaotai_args_t *args, const void *data, size_t size)
{
    return emit_event(name, args, data, size, true, false);
}

/* ----------------------------------------------------------- activities */

static const xiaotai_priority_t *find_priority(const xiaotai_priority_t *table, const char *name)
{
    for (const xiaotai_priority_t *entry = table; entry && entry->activity; ++entry) {
        if (strcmp(entry->activity, name) == 0) return entry;
    }
    return NULL;
}

/* The product's table first, then the defaults the engines, modules and
 * applications declare (the build checked that they agree), then 1. */
static uint8_t priority_of(const char *name)
{
    const xiaotai_priority_t *entry = find_priority(s_fw->config.priorities, name);
    for (size_t i = 0; !entry && i < s_fw->order_count; ++i) entry = find_priority(s_fw->order[i]->activities, name);
    for (size_t i = 0; !entry && i < s_fw->config.application_count; ++i) {
        entry = find_priority(s_fw->config.applications[i]->activities, name);
    }
    return entry ? entry->priority : 1;
}

static bool preempts(const char *activity, uint8_t priority, const current_activity_t *current)
{
    for (const xiaotai_preemption_t *rule = s_fw->config.preemptions; rule && rule->activity; ++rule) {
        if (rule->over && strcmp(rule->activity, activity) == 0 && strcmp(rule->over, current->activity.name) == 0) {
            return rule->preempts;
        }
    }
    return priority > current->priority;
}

typedef enum { NOTIFY_BEGIN, NOTIFY_END, NOTIFY_SESSION } notify_t;

static void attachments_notify(const char *activity, uint32_t id, uint32_t generation, notify_t what)
{
    for (size_t i = 0; i < s_fw->attachment_count; ++i) {
        const xiaotai_attachment_t *attachment = &s_fw->attachments[i].attachment;
        if (strcmp(s_fw->attachments[i].activity, activity) != 0) continue;
        void (*fn)(uint32_t, uint32_t, void *) = what == NOTIFY_BEGIN ? attachment->begin
                                                 : what == NOTIFY_END ? attachment->end : attachment->session;
        if (!fn) continue;
        const int16_t previous = fw_app_enter(s_fw->attachments[i].app);
        fn(id, generation, attachment->ctx);
        fw_app_leave(previous);
    }
}

size_t xiaotai_activity_contributions(const char *activity, const char *slot, const char *scene,
                                      const void **out, size_t capacity)
{
    if (!s_fw || !activity || !slot) return 0;
    size_t found = 0;
    for (size_t i = 0; i < s_fw->attachment_count; ++i) {
        if (strcmp(s_fw->attachments[i].activity, activity) != 0) continue;
        for (const xiaotai_contribution_t *entry = s_fw->attachments[i].attachment.contributions; entry && entry->slot;
             ++entry) {
            if (strcmp(entry->slot, slot) != 0) continue;
            if (entry->scene && (!scene || strcmp(entry->scene, scene) != 0)) continue;
            if (out && found < capacity) out[found] = entry->data;
            ++found;
        }
    }
    return found;
}

static void announce(const char *name)
{
    xiaotai_args_t args = {0};
    if (name) {
        strncpy(args.text, name, sizeof(args.text) - 1);
        memcpy(&args.value, &s_fw->current.id, sizeof(args.value));
    }
    (void)xiaotai_event_emit(XIAOTAI_EVENT_ACTIVITY, &args, NULL, 0);
}

/* The attachments end first, then the activity's own module hears it. */
static void revoke_current(xiaotai_revoke_t reason)
{
    const current_activity_t revoked = s_fw->current;
    memset(&s_fw->current, 0, sizeof(s_fw->current));
    attachments_notify(revoked.activity.name, revoked.id, revoked.activity.generation, NOTIFY_END);
    if (!revoked.activity.revoked) return;
    const int16_t previous = fw_app_enter(revoked.app);
    revoked.activity.revoked(revoked.id, reason, revoked.activity.ctx);
    fw_app_leave(previous);
}

xiaotai_err_t xiaotai_activity_begin(const xiaotai_activity_t *activity, uint32_t *id)
{
    if (!activity || !activity->name || !id) return XIAOTAI_ERR_INVALID_ARG;
    if (!xiaotai_on_owner()) return XIAOTAI_ERR_INVALID_STATE;
    if (s_fw->quiesce_ticket || atomic_load(&s_restart_pending)) return XIAOTAI_ERR_BUSY;
    const uint8_t priority = priority_of(activity->name);
    current_activity_t *current = &s_fw->current;
    if (current->id) {
        if (!preempts(activity->name, priority, current)) return XIAOTAI_ERR_BUSY;
        const char *previous = current->activity.name;
        revoke_current(XIAOTAI_REVOKE_PREEMPTED);
        XIAOTAI_LOGI(TAG, "%s preempts %s", activity->name, previous);
    }
    if (++s_fw->next_activity_id == 0) s_fw->next_activity_id = 1;
    atomic_store(&s_activity_active, 1U);
    current->id = s_fw->next_activity_id;
    current->activity = *activity;
    current->priority = priority;
    current->app = s_fw->running_app;
    *id = current->id;
    attachments_notify(activity->name, current->id, activity->generation, NOTIFY_BEGIN);
    announce(activity->name);
    return XIAOTAI_OK;
}

void xiaotai_activity_end(uint32_t id)
{
    if (!s_fw || !xiaotai_on_owner() || id == 0 || s_fw->current.id != id) return;
    const current_activity_t ended = s_fw->current;
    memset(&s_fw->current, 0, sizeof(s_fw->current));
    attachments_notify(ended.activity.name, ended.id, ended.activity.generation, NOTIFY_END);
    atomic_store(&s_activity_active, s_fw->current.id != 0);
    announce(NULL);
}

bool xiaotai_activity_active(void) { return atomic_load(&s_activity_active) != 0; }

uint32_t xiaotai_activity_current(const char **name)
{
    if (!s_fw) return 0;
    if (name) *name = s_fw->current.id ? s_fw->current.activity.name : NULL;
    return s_fw->current.id;
}

void xiaotai_activity_set_generation(uint32_t id, uint32_t generation)
{
    if (!s_fw || !xiaotai_on_owner() || id == 0 || s_fw->current.id != id) return;
    if (s_fw->current.activity.generation == generation) return;
    s_fw->current.activity.generation = generation;
    attachments_notify(s_fw->current.activity.name, id, generation, NOTIFY_SESSION);
}

uint32_t xiaotai_activity_generation(uint32_t id)
{
    return s_fw && id && s_fw->current.id == id ? s_fw->current.activity.generation : 0;
}

xiaotai_err_t xiaotai_activity_attach(const char *activity, const xiaotai_attachment_t *attachment)
{
    if (!activity || !attachment) return XIAOTAI_ERR_INVALID_ARG;
    if (!fw_initializing()) return XIAOTAI_ERR_INVALID_STATE;
    if (s_fw->attachment_count >= XIAOTAI_FRAMEWORK_MAX_ATTACHMENTS) return XIAOTAI_ERR_NO_MEM;
    s_fw->attachments[s_fw->attachment_count++] = (attachment_t){.activity = activity, .attachment = *attachment,
                                                                         .app = s_fw->running_app};
    return XIAOTAI_OK;
}

/* -------------------------------------------------------------- quiesce */

static void announce_quiesce(bool quiesced, const char *reason)
{
    xiaotai_args_t args = {.value = quiesced ? 1 : 0};
    if (reason) strncpy(args.text, reason, sizeof(args.text) - 1);
    (void)xiaotai_event_emit(XIAOTAI_EVENT_QUIESCE, &args, NULL, 0);
}

static xiaotai_err_t guard_room(void)
{
    if (!fw_initializing()) return XIAOTAI_ERR_INVALID_STATE;
    return s_fw->guard_count + s_fw->gate_count < XIAOTAI_FRAMEWORK_MAX_QUIESCE_GUARDS ?
           XIAOTAI_OK : XIAOTAI_ERR_NO_MEM;
}

xiaotai_err_t xiaotai_quiesce_guard_register(const char *name, xiaotai_quiesce_guard_fn_t guard, void *ctx)
{
    if (!name || !guard) return XIAOTAI_ERR_INVALID_ARG;
    const xiaotai_err_t err = guard_room();
    if (err == XIAOTAI_OK) s_fw->guards[s_fw->guard_count++] = (guard_t){.name = name, .guard = guard, .ctx = ctx};
    return err;
}

static size_t gate_index(size_t i) { return XIAOTAI_FRAMEWORK_MAX_QUIESCE_GUARDS - 1U - i; }

xiaotai_err_t xiaotai_quiesce_gate_register(const char *name, const xiaotai_quiesce_gate_t *gate, void *ctx)
{
    if (!name || !gate || !gate->acquire || !gate->release) return XIAOTAI_ERR_INVALID_ARG;
    const xiaotai_err_t err = guard_room();
    if (err == XIAOTAI_OK) s_fw->guards[gate_index(s_fw->gate_count++)] = (guard_t){.name = name, .gate = gate, .ctx = ctx};
    return err;
}

static void release_gates(size_t before, const char *reason)
{
    while (before) {
        const guard_t *guard = &s_fw->guards[gate_index(--before)];
        guard->gate->release(reason, guard->ctx);
    }
}

static xiaotai_err_t ask_guards(const char *reason)
{
    for (size_t i = 0; i < s_fw->guard_count; ++i) {
        const guard_t *guard = &s_fw->guards[i];
        const xiaotai_err_t err = guard->guard(reason, guard->ctx);
        if (err != XIAOTAI_OK) {
            ++s_fw->guard_refusals;
            s_fw->last_guard = guard->name;
            return err;
        }
    }
    return XIAOTAI_OK;
}

static xiaotai_err_t acquire_gates(const char *reason)
{
    for (size_t i = 0; i < s_fw->gate_count; ++i) {
        const guard_t *guard = &s_fw->guards[gate_index(i)];
        const xiaotai_err_t err = guard->gate->acquire(reason, guard->ctx);
        if (err != XIAOTAI_OK) {
            ++s_fw->guard_refusals;
            s_fw->last_guard = guard->name;
            release_gates(i, reason);
            return err;
        }
    }
    return XIAOTAI_OK;
}

/* Closed before the revocation, so a handler that tries to begin another
 * activity from its revoked callback is refused. */
static void quiesce_take(const char *reason)
{
    if (++s_fw->next_quiesce_ticket == 0) s_fw->next_quiesce_ticket = 1;
    s_fw->quiesce_ticket = s_fw->next_quiesce_ticket;
    s_fw->quiesce_reason = reason;
    atomic_store(&s_quiesced, true);
    if (s_fw->current.id) {
        revoke_current(XIAOTAI_REVOKE_SYSTEM);
        atomic_store(&s_activity_active, 0U);
        announce(NULL);
    }
    XIAOTAI_LOGI(TAG, "quiesced: %s", reason ? reason : "");
    announce_quiesce(true, reason);
}

xiaotai_err_t xiaotai_quiesce_begin(const char *reason, uint32_t *ticket)
{
    if (!ticket) return XIAOTAI_ERR_INVALID_ARG;
    if (!s_fw || !xiaotai_on_owner()) return XIAOTAI_ERR_INVALID_STATE;
    if (s_fw->quiesce_ticket || atomic_load(&s_restart_pending)) return XIAOTAI_ERR_BUSY;
    const xiaotai_err_t err = ask_guards(reason ? reason : "");
    if (err != XIAOTAI_OK) return err;
    const xiaotai_err_t gate = acquire_gates(reason ? reason : "");
    if (gate != XIAOTAI_OK) return gate;
    quiesce_take(reason);
    *ticket = s_fw->quiesce_ticket;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_quiesce_check(uint32_t ticket, bool *ready)
{
    if (!ready) return XIAOTAI_ERR_INVALID_ARG;
    *ready = false;
    if (!s_fw || !xiaotai_on_owner() || !ticket || s_fw->quiesce_ticket != ticket ||
        atomic_load(&s_restart_pending)) return XIAOTAI_ERR_INVALID_STATE;
    const char *reason = s_fw->quiesce_reason;
    bool all_ready = true;
    for (size_t i = 0; i < s_fw->gate_count; ++i) {
        const guard_t *guard = &s_fw->guards[gate_index(i)];
        if (!guard->gate->check) continue;
        bool confirmed = true;
        const xiaotai_err_t err = guard->gate->check(reason ? reason : "", guard->ctx, &confirmed);
        if (err != XIAOTAI_OK) return err;
        all_ready = all_ready && confirmed;
    }
    *ready = all_ready;
    return XIAOTAI_OK;
}

void xiaotai_quiesce_end(uint32_t ticket)
{
    if (!s_fw || !xiaotai_on_owner() || ticket == 0 || s_fw->quiesce_ticket != ticket) return;
    if (atomic_load(&s_restart_pending)) return;   /* the restart holds the quiesce to the end */
    const char *reason = s_fw->quiesce_reason;
    s_fw->quiesce_ticket = 0;
    s_fw->quiesce_reason = NULL;
    atomic_store(&s_quiesced, false);
    release_gates(s_fw->gate_count, reason ? reason : "");
    announce_quiesce(false, NULL);
}

bool xiaotai_quiesced(void) { return atomic_load(&s_quiesced); }

bool xiaotai_quiesced_for(const char *reason)
{
    return reason && *reason && xiaotai_on_owner() && s_fw && s_fw->quiesce_ticket && s_fw->quiesce_reason &&
        strcmp(s_fw->quiesce_reason, reason) == 0;
}

/* ---------------------------------------------------------------- views */

void xiaotai_view_set_sink(xiaotai_view_sink_t sink, void *ctx)
{
    if (!s_fw) return;
    s_fw->view_sink = sink;
    s_fw->view_ctx = ctx;
}

/* Only the foreground application draws: a view from the code of another
 * application is dropped and counted. Before the home application is shown
 * (an init or start), the home application counts as the foreground one. */
void xiaotai_view_show(const xiaotai_view_t *view)
{
    if (!s_fw || !view) return;
    const int16_t app = fw_app_current();
    const size_t shown = s_fw->has_foreground ? s_fw->foreground : s_fw->home;
    if (app >= 0 && (size_t)app != shown) {
        atomic_fetch_add(&s_view_refused, 1U);
        return;
    }
    if (s_fw->view_sink) s_fw->view_sink(view, s_fw->view_ctx);
}
