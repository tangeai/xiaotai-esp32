// SPDX-License-Identifier: MIT
#include <stdatomic.h>
#include "internal.h"
#include "xiaotai/port/os.h"
#include "xiaotai/port/sys.h"

#if XIAOTAI_AT_RESTART_COMMAND
typedef enum { IDLE, AWAIT_REPLY, CANCELLED, ACKNOWLEDGED } restart_state_t;
static _Atomic restart_state_t s_restart;
static xiaotai_task_t s_worker;
static bool s_direct, s_idle_only;
static uint16_t s_delay_ms;

static void restart_worker(void *arg)
{
    const uint32_t delay_ms = (uint32_t)(uintptr_t)arg;
    (void)xiaotai_task_wait_notify(XIAOTAI_WAIT_FOREVER);
    if (atomic_load(&s_restart) == ACKNOWLEDGED) {
        xiaotai_delay_ms(delay_ms);
        atomic_store(&s_restart, IDLE);
        xiaotai_restart();
    }
    atomic_store(&s_restart, IDLE);
    xiaotai_task_exit();
}

xiaotai_err_t xiaotai_at_guarded_restart_schedule(const xiaotai_reply_t *reply)
{
    if (!xiaotai_at_reply_is_console(reply)) return XIAOTAI_ERR_NOT_ALLOWED;
    if (s_direct && (xiaotai_quiesced() || xiaotai_restart_pending())) return XIAOTAI_ERR_INVALID_STATE;
    restart_state_t idle = IDLE;
    if (!atomic_compare_exchange_strong(&s_restart, &idle, AWAIT_REPLY)) return XIAOTAI_ERR_INVALID_STATE;
    const xiaotai_task_config_t worker = {.name = "at-rescue", .entry = restart_worker,
        .arg = (void *)(uintptr_t)s_delay_ms,
        .stack_bytes = XIAOTAI_FRAMEWORK_RESTART_STACK_BYTES, .stack = XIAOTAI_MEM_INTERNAL,
        .priority = 2, .core = XIAOTAI_CORE_ANY};
    const xiaotai_err_t err = xiaotai_task_create(&worker, &s_worker);
    if (err != XIAOTAI_OK) atomic_store(&s_restart, IDLE);
    return err;
}

void xiaotai_at_guarded_restart_reply(bool acknowledged)
{
    restart_state_t waiting = AWAIT_REPLY;
    if (atomic_compare_exchange_strong(&s_restart, &waiting, acknowledged ? ACKNOWLEDGED : CANCELLED))
        xiaotai_task_notify(&s_worker);
}

static xiaotai_err_t xiaotai_at_guarded_restart_run(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    if (s_idle_only && xiaotai_activity_active()) return XIAOTAI_ERR_INVALID_STATE;
    if (!s_direct) {
        const xiaotai_args_t args = {.text = "console"};
        const xiaotai_err_t err = xiaotai_intent_post(XIAOTAI_AT_INTENT_RESTART, &args);
        if (err != XIAOTAI_ERR_INVALID_STATE) return err;
    }
    return xiaotai_at_guarded_restart_schedule(reply);
}

xiaotai_err_t xiaotai_at_guarded_restart_register(const xiaotai_at_config_t *config)
{
    static const xiaotai_at_verb_t verbs[] = {
        {.run = xiaotai_at_guarded_restart_run},
#if XIAOTAI_AT_RESTART_FORCE
        {.word = "FORCE", .run = xiaotai_at_guarded_force_run},
#endif
    };
    s_direct = config->restart_direct_when_running;
    s_idle_only = config->restart_idle_only;
    s_delay_ms = config->restart_delay_ms ? config->restart_delay_ms : 200;
    return at_local_verbs_register("AT+RESTART", verbs, sizeof(verbs) / sizeof(*verbs));
}
#endif
