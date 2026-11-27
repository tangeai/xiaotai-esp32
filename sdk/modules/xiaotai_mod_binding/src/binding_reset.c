// SPDX-License-Identifier: MIT
#include "binding_internal.h"
#include "xiaotai/port/log.h"

static struct {
    uint32_t ticket;
    bool erasing, direct;
    binding_mutation_fn_t done;
    void *ctx;
} mutation;

static void forgotten(xiaotai_err_t result)
{
    binding_count(result == XIAOTAI_OK ? BINDING_STAT_FORGETS : BINDING_STAT_FORGET_FAILURES);
    const xiaotai_args_t args = {.value = result};
    const xiaotai_err_t queued = xiaotai_event_emit(XIAOTAI_EVENT_BINDING_FORGOTTEN, &args, NULL, 0);
    if (queued != XIAOTAI_OK) XIAOTAI_LOGW(BINDING_LOG_SOURCE, "forget result event could not be queued: 0x%x", (unsigned)queued);
}

static void saved(xiaotai_err_t result)
{
    const uint32_t ticket = mutation.ticket;
    const bool erase = mutation.erasing;
    const bool direct = mutation.direct;
    binding_mutation_fn_t done = mutation.done;
    void *ctx = mutation.ctx;
    mutation.ticket = 0;
    mutation.direct = false;
    mutation.done = NULL;
    mutation.ctx = NULL;
    if (erase) forgotten(result);
    if (result == XIAOTAI_OK) {
        /* Hand the original ticket over. Ending it and then requesting a
         * restart would let a new activity start with the wrong identity. */
        result = direct ? xiaotai_restart_request_direct(XIAOTAI_QUIESCE_BINDING)
                        : xiaotai_restart_request(XIAOTAI_QUIESCE_BINDING, ticket);
        if (result != XIAOTAI_OK) {
            XIAOTAI_LOGE(BINDING_LOG_SOURCE, "identity committed but restart was refused: 0x%x", (unsigned)result);
            if (direct) xiaotai_identity_publish_restart_erase();
        }
    }
    if (result != XIAOTAI_OK && ticket) xiaotai_quiesce_end(ticket);
    if (done) done(result, ctx);
}

static xiaotai_err_t mutate(const xiaotai_identity_t *identity, bool direct, binding_mutation_fn_t done, void *ctx)
{
    xiaotai_err_t err = binding_mutation_ready();
    if (err != XIAOTAI_OK) return err;
    if (identity && !xiaotai_identity_valid(identity)) return XIAOTAI_ERR_INVALID_ARG;

    /* A local input has no console to poll; reserve the same guaranteed
     * storage-result carrier MB-1 uses before preempting or writing anything. */
    void *storage_ctx = NULL;
    err = binding_storage_begin(saved, &storage_ctx);
    if (err != XIAOTAI_OK) return err;
    uint32_t ticket = 0;
    if (!direct) err = xiaotai_quiesce_begin(XIAOTAI_QUIESCE_BINDING, &ticket);
    if (err == XIAOTAI_OK) err = binding_attempt_abort();
    if (err == XIAOTAI_OK) {
        mutation.ticket = ticket;
        mutation.direct = direct;
        mutation.erasing = !identity;
        mutation.done = done;
        mutation.ctx = ctx;
        if (identity) err = xiaotai_identity_save(identity, binding_storage_done, storage_ctx);
        else if (direct) err = xiaotai_identity_erase_for_restart(binding_storage_done, storage_ctx);
        else err = xiaotai_identity_erase(binding_storage_done, storage_ctx);
        if (err == XIAOTAI_OK) return XIAOTAI_OK;
        if (!identity) forgotten(err);
        mutation.ticket = 0;
        mutation.direct = false;
        mutation.done = NULL;
        mutation.ctx = NULL;
    }
    binding_storage_cancel();
    if (ticket) xiaotai_quiesce_end(ticket);
    return err;
}

xiaotai_err_t binding_mutate(const xiaotai_identity_t *identity, binding_mutation_fn_t done, void *ctx)
{
    return mutate(identity, false, done, ctx);
}

xiaotai_err_t xiaotai_binding_forget(void) { return binding_mutate(NULL, NULL, NULL); }

xiaotai_err_t xiaotai_binding_forget_local(bool direct, xiaotai_binding_forget_done_fn_t done, void *ctx)
{
    if (xiaotai_intent_from_external()) return XIAOTAI_ERR_NOT_ALLOWED;
    return mutate(NULL, direct, done, ctx);
}

bool binding_direct_reset_pending(void) { return mutation.direct; }

static void local_forget(const xiaotai_args_t *args, void *ctx)
{
    (void)args; (void)ctx;
    const xiaotai_err_t result = xiaotai_binding_forget();
    if (result != XIAOTAI_OK) XIAOTAI_LOGI(BINDING_LOG_SOURCE, "local forget refused: 0x%x", (unsigned)result);
}

xiaotai_err_t binding_reset_register(void)
{
    return xiaotai_intent_handle_ex(XIAOTAI_INTENT_BINDING_FORGET, local_forget, NULL, XIAOTAI_INTENT_LOCAL);
}
