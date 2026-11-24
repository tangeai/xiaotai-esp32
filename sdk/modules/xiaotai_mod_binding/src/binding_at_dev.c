// SPDX-License-Identifier: MIT
#include "xiaotai/modules/binding_config.h"

#if XIAOTAI_BINDING_DEV_COMMANDS
#include "binding_internal.h"
#include "xiaotai/at_command.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#include <stdatomic.h>
#include <string.h>

#define STORE_REPLY_TIMEOUT_MS 10000U

/* The console owns one reference after the stream hand-over; an admitted
 * identity transaction owns another. A console timeout never cancels an
 * already-admitted persistent write or strands the restart hold. */
typedef struct {
    xiaotai_signal_t complete;
    atomic_uint refs;
    xiaotai_err_t result;
} at_store_t;

static void xiaotai_binding_at_dev_unref(at_store_t *state)
{
    if (atomic_fetch_sub(&state->refs, 1U) != 1U) return;
    xiaotai_signal_deinit(&state->complete);
    xiaotai_mem_free(state);
}

static bool xiaotai_binding_at_dev_next(void *ctx, const xiaotai_reply_t *reply)
{
    at_store_t *state = ctx;
    if (!xiaotai_signal_take(&state->complete, STORE_REPLY_TIMEOUT_MS)) {
        (void)xiaotai_at_fail(reply, XIAOTAI_ERR_TIMEOUT, "STORE_PENDING");
    } else if (state->result != XIAOTAI_OK) {
        (void)xiaotai_at_fail(reply, state->result, "STORE");
    }
    return false;
}

static void xiaotai_binding_at_dev_release(void *ctx)
{
    xiaotai_restart_release();
    xiaotai_binding_at_dev_unref(ctx);
}

static void xiaotai_binding_at_dev_done(xiaotai_err_t result, void *ctx)
{
    at_store_t *state = ctx;
    state->result = result;
    xiaotai_signal_give(&state->complete);
    xiaotai_binding_at_dev_unref(state);
}

static xiaotai_err_t xiaotai_binding_at_dev_start(const xiaotai_identity_t *identity, const xiaotai_reply_t *reply)
{
    xiaotai_err_t err = binding_mutation_ready();
    if (err != XIAOTAI_OK) return err;
    at_store_t *state = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*state));
    if (!state) return XIAOTAI_ERR_NO_MEM;
    err = xiaotai_signal_init(&state->complete);
    if (err != XIAOTAI_OK) {
        xiaotai_mem_free(state);
        return err;
    }
    atomic_init(&state->refs, 2U);
    xiaotai_restart_hold();
    const xiaotai_at_stream_t stream = {
        .next = xiaotai_binding_at_dev_next, .release = xiaotai_binding_at_dev_release, .ctx = state,
    };
    err = xiaotai_at_stream(reply, &stream);
    if (err != XIAOTAI_OK) {
        xiaotai_restart_release();
        xiaotai_signal_deinit(&state->complete);
        xiaotai_mem_free(state);
        return err;
    }
    err = binding_mutate(identity, xiaotai_binding_at_dev_done, state);
    if (err != XIAOTAI_OK) xiaotai_binding_at_dev_unref(state);
    return err;
}

static xiaotai_err_t xiaotai_binding_at_dev_set(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    if (!xiaotai_at_is_id(argv[0], XIAOTAI_IDENTITY_DEVICE_ID_MAX) ||
        (argc == 3 && !xiaotai_at_is_id(argv[2], XIAOTAI_IDENTITY_CLIENT_ID_MAX))) return XIAOTAI_ERR_INVALID_ARG;
    xiaotai_identity_t identity = {0};
    size_t length;
    const xiaotai_err_t decoded = xiaotai_at_hex_decode(argv[1], (uint8_t *)identity.device_key,
                                                        XIAOTAI_IDENTITY_DEVICE_KEY_MAX, &length);
    xiaotai_err_t result = XIAOTAI_ERR_INVALID_ARG;
    /* Storage and signing use a terminated byte string, not a binary blob. */
    if (decoded == XIAOTAI_OK && !memchr(identity.device_key, 0, length)) {
        memcpy(identity.device_id, argv[0], strlen(argv[0]));
        if (argc == 3) memcpy(identity.client_id, argv[2], strlen(argv[2]));
        result = xiaotai_binding_at_dev_start(&identity, reply);
    }
    xiaotai_identity_wipe(&identity, sizeof(identity));
    return result;
}

static xiaotai_err_t xiaotai_binding_at_dev_clear(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc; (void)argv;
    return xiaotai_binding_at_dev_start(NULL, reply);
}

xiaotai_err_t xiaotai_binding_at_dev_register(void)
{
    static const xiaotai_at_verb_t verbs[] = {
        {.word = "SET", .min_args = 2, .max_args = 3, .run = xiaotai_binding_at_dev_set,
         .usage = "<device-id> <key-hex> [<client-id>]"},
        {.word = "CLEAR", .run = xiaotai_binding_at_dev_clear},
    };
    return xiaotai_at_verbs_register("AT+BIND", verbs, sizeof(verbs) / sizeof(verbs[0]));
}
#endif
