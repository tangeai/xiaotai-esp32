// SPDX-License-Identifier: MIT
/* Fake platform core for the environment module's host tests: only the calls
 * the module makes. normalize follows the platform's rules closely enough for
 * the tests (scheme, no userinfo or blanks, trailing slashes removed, length limit, http
 * written back for https); the platform's own tests cover the real rules. */
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "fakes.h"
#include "xiaotai/framework.h"
#include "xiaotai/platform.h"

fake_platform_t g_platform;

static pthread_mutex_t s_trace_lock = PTHREAD_MUTEX_INITIALIZER;
static char s_trace[2048];

void fake_trace(const char *word)
{
    pthread_mutex_lock(&s_trace_lock);
    if (s_trace[0]) strncat(s_trace, " ", sizeof(s_trace) - strlen(s_trace) - 1);
    strncat(s_trace, word, sizeof(s_trace) - strlen(s_trace) - 1);
    pthread_mutex_unlock(&s_trace_lock);
}

void fake_trace_get(char *out, unsigned capacity)
{
    pthread_mutex_lock(&s_trace_lock);
    snprintf(out, capacity, "%s", s_trace);
    pthread_mutex_unlock(&s_trace_lock);
}

void fake_trace_clear(void)
{
    pthread_mutex_lock(&s_trace_lock);
    s_trace[0] = '\0';
    pthread_mutex_unlock(&s_trace_lock);
}

/* Whole-word search from position `from`; returns the index after the match
 * or -1. */
static long find_word(const char *text, const char *word, long from)
{
    const size_t length = strlen(word);
    for (const char *at = strstr(text + from, word); at; at = strstr(at + 1, word)) {
        const bool starts = at == text || at[-1] == ' ';
        const bool ends = at[length] == '\0' || at[length] == ' ';
        if (starts && ends) return (long)(at - text) + (long)length;
    }
    return -1;
}

bool fake_trace_in_order(const char *const *words)
{
    char trace[sizeof(s_trace)];
    fake_trace_get(trace, sizeof(trace));
    long position = 0;
    for (; *words; ++words) {
        position = find_word(trace, *words, position);
        if (position < 0) return false;
    }
    return true;
}

unsigned fake_trace_count(const char *word)
{
    char trace[sizeof(s_trace)];
    fake_trace_get(trace, sizeof(trace));
    unsigned count = 0;
    for (long position = find_word(trace, word, 0); position >= 0; position = find_word(trace, word, position)) {
        ++count;
    }
    return count;
}

/* ------------------------------------------------------------ platform core */

static xiaotai_platform_done_fn_t s_done;
static void *s_done_ctx;
static uint32_t s_store_timer;
static char s_snapshot_saved[XIAOTAI_PLATFORM_URL_MAX], s_snapshot_next[XIAOTAI_PLATFORM_URL_MAX];
static bool s_snapshot_override;

void fake_platform_reset(void)
{
    memset(&g_platform, 0, sizeof(g_platform));
    atomic_store(&g_platform.drained, true);
    s_done = NULL;
    s_done_ctx = NULL;
    s_store_timer = 0;
    s_snapshot_override = false;
}

void fake_platform_saved(const char *saved, const char *next)
{
    s_snapshot_override = saved && next;
    if (s_snapshot_override) {
        (void)snprintf(s_snapshot_saved, sizeof(s_snapshot_saved), "%s", saved);
        (void)snprintf(s_snapshot_next, sizeof(s_snapshot_next), "%s", next);
    }
}

xiaotai_err_t xiaotai_platform_gate_close(uint32_t *ticket)
{
    fake_trace("gate_close");
    const xiaotai_err_t result = atomic_load(&g_platform.gate_close_result);
    if (result != XIAOTAI_OK) return result;
    if (atomic_load(&g_platform.gate_closed)) return XIAOTAI_ERR_PLATFORM_GATED;
    atomic_store(&g_platform.gate_closed, true);
    *ticket = atomic_fetch_add(&g_platform.gate_ticket, 1U) + 101U;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_platform_gate_close_binding_idle(uint32_t *ticket)
{
    return xiaotai_platform_gate_close(ticket);
}

bool xiaotai_platform_gate_drained(uint32_t ticket)
{
    (void)ticket;
    atomic_fetch_add(&g_platform.drained_checks, 1U);
    return atomic_load(&g_platform.gate_closed) && atomic_load(&g_platform.drained);
}

xiaotai_err_t xiaotai_platform_gate_open(uint32_t ticket)
{
    (void)ticket;
    fake_trace("gate_open");
    const xiaotai_err_t result = atomic_exchange(&g_platform.gate_open_result_once, XIAOTAI_OK);
    if (result != XIAOTAI_OK) return result;
    if (!atomic_load(&g_platform.gate_closed)) return XIAOTAI_ERR_INVALID_ARG;
    atomic_store(&g_platform.gate_closed, false);
    return XIAOTAI_OK;
}

void xiaotai_platform_environment(xiaotai_platform_environment_t *environment)
{
    atomic_fetch_add(&g_platform.snapshots, 1U);
    memset(environment, 0, sizeof(*environment));
    environment->boot_error = atomic_load(&g_platform.boot_error);
    if (environment->boot_error == XIAOTAI_OK) {
        snprintf(environment->active, sizeof(environment->active), "http://active.example.test/services");
    }
    snprintf(environment->saved, sizeof(environment->saved), "%s", s_snapshot_override ? s_snapshot_saved : "");
    snprintf(environment->next, sizeof(environment->next), "%s", s_snapshot_override ? s_snapshot_next :
             "http://builtin.example.test/services");
    environment->unconfirmed = atomic_load(&g_platform.unconfirmed);
}

xiaotai_err_t xiaotai_platform_environment_normalize(const char *base, char *out, size_t capacity)
{
    const bool https = strncmp(base, "https://", 8) == 0;
    if (!https && strncmp(base, "http://", 7) != 0) return XIAOTAI_ERR_INVALID_ARG;
    const char *rest = base + (https ? 8 : 7);
    if (memchr(rest, '@', strcspn(rest, "/"))) return XIAOTAI_ERR_INVALID_ARG;
    size_t length = strlen(rest);
    while (length && rest[length - 1] == '/') --length;
    if (!length) return XIAOTAI_ERR_INVALID_ARG;
    for (size_t i = 0; i < length; ++i) {
        if (rest[i] <= ' ' || rest[i] == '?' || rest[i] == '#' || rest[i] == '\\') return XIAOTAI_ERR_INVALID_ARG;
    }
    if (7 + length > XIAOTAI_PLATFORM_BASE_MAX) return XIAOTAI_ERR_INVALID_ARG;
    if (out) {
        if (capacity < 7 + length + 1) return XIAOTAI_ERR_INVALID_ARG;
        snprintf(out, capacity, "http://%.*s", (int)length, rest);
    }
    return XIAOTAI_OK;
}

static void store_finish(xiaotai_err_t result)
{
    xiaotai_platform_done_fn_t done = s_done;
    void *ctx = s_done_ctx;
    s_done = NULL;
    /* As the platform: any failure after the write reached storage leaves
     * the stored value unknown until a confirmed store or a restart. */
    atomic_store(&g_platform.unconfirmed, result != XIAOTAI_OK);
    if (done) done(result, ctx);
}

static void store_timer(void *ctx)
{
    (void)ctx;
    s_store_timer = 0;
    store_finish(atomic_load(&g_platform.store_result));
}

void fake_platform_complete_store(xiaotai_err_t result) { store_finish(result); }

xiaotai_err_t xiaotai_platform_environment_store(const char *base, xiaotai_platform_done_fn_t done, void *ctx)
{
    char word[300];
    snprintf(word, sizeof(word), "store(%s)", base ? base : "NULL");
    fake_trace(word);
    atomic_fetch_add(&g_platform.stores, 1U);
    atomic_store(&g_platform.store_was_null, base == NULL);
    snprintf(g_platform.stored, sizeof(g_platform.stored), "%s", base ? base : "");
    const xiaotai_err_t call = atomic_load(&g_platform.store_call_result);
    if (call != XIAOTAI_OK) return call;
    s_done = done;
    s_done_ctx = ctx;
    if (atomic_load(&g_platform.store_manual)) return XIAOTAI_OK;
    return xiaotai_timer_start(atomic_load(&g_platform.store_delay_ms), store_timer, NULL, &s_store_timer);
}
