// SPDX-License-Identifier: MIT
/* The app threshold of xiaotai/port/log.h and the per-module thresholds: the
 * same on every platform. All static: a line costs one atomic load, or one
 * short table walk while a module threshold is set, and never allocates. */
#include <stdatomic.h>
#include <string.h>

#include "xiaotai/port/log.h"

typedef struct {
    /* name[XIAOTAI_LOG_MODULE_NAME_MAX] stays zero, so a reader racing with a
     * rewrite never runs past the slot. */
    char name[XIAOTAI_LOG_MODULE_NAME_MAX + 1U];
    atomic_uchar level;                  /* XIAOTAI_LOG_LEVEL_INHERIT: the slot is free */
} log_module_t;

static atomic_uint s_level = XIAOTAI_LOG_LEVEL_INFO;
static log_module_t s_modules[XIAOTAI_LOG_MODULES_MAX];
/* Slots used at least once. Zero until the first module threshold, which is
 * what keeps the common path to one load. */
static atomic_uint s_used;

bool xiaotai_log_enabled(uint8_t level, const char *tag)
{
    const unsigned used = atomic_load_explicit(&s_used, memory_order_acquire);
    for (unsigned i = 0; tag && i < used; ++i) {
        const uint8_t own = atomic_load_explicit(&s_modules[i].level, memory_order_acquire);
        if (own != XIAOTAI_LOG_LEVEL_INHERIT && strcmp(s_modules[i].name, tag) == 0) return level <= own;
    }
    return level <= atomic_load_explicit(&s_level, memory_order_relaxed);
}

xiaotai_err_t xiaotai_log_set_level(uint8_t level)
{
    if (level > XIAOTAI_LOG_LEVEL_DEBUG) return XIAOTAI_ERR_INVALID_ARG;
    atomic_store(&s_level, level);
    return XIAOTAI_OK;
}

uint8_t xiaotai_log_level(void) { return (uint8_t)atomic_load(&s_level); }
uint8_t xiaotai_log_level_max(void) { return (uint8_t)XIAOTAI_LOG_LEVEL_CEILING; }

static bool name_valid(const char *tag)
{
    size_t length = 0;
    for (; tag && tag[length]; ++length) {
        const char c = tag[length];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
        if (!ok || length >= XIAOTAI_LOG_MODULE_NAME_MAX) return false;
    }
    return length > 0;
}

/* Writers are the console task only, so they need no lock among themselves;
 * readers are every task that logs. A free slot's name is rewritten while its
 * level says "free", and the level is published last. */
xiaotai_err_t xiaotai_log_set_module_level(const char *tag, uint8_t level)
{
    if (!name_valid(tag) || (level > XIAOTAI_LOG_LEVEL_DEBUG && level != XIAOTAI_LOG_LEVEL_INHERIT)) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    const unsigned used = atomic_load(&s_used);
    log_module_t *slot = NULL;
    for (unsigned i = 0; i < used; ++i) {
        const bool taken = atomic_load(&s_modules[i].level) != XIAOTAI_LOG_LEVEL_INHERIT;
        if (taken && strcmp(s_modules[i].name, tag) == 0) {
            atomic_store(&s_modules[i].level, level);
            return XIAOTAI_OK;
        }
        if (!taken && !slot) slot = &s_modules[i];
    }
    if (level == XIAOTAI_LOG_LEVEL_INHERIT) return XIAOTAI_OK;
    const bool fresh = !slot;
    if (fresh) {
        if (used >= XIAOTAI_LOG_MODULES_MAX) return XIAOTAI_ERR_NO_MEM;
        slot = &s_modules[used];
        atomic_store(&slot->level, XIAOTAI_LOG_LEVEL_INHERIT);
    }
    memset(slot->name, 0, sizeof(slot->name));
    memcpy(slot->name, tag, strlen(tag));
    atomic_store_explicit(&slot->level, level, memory_order_release);
    if (fresh) atomic_store_explicit(&s_used, used + 1U, memory_order_release);
    return XIAOTAI_OK;
}

bool xiaotai_log_module_at(size_t index, const char **tag, uint8_t *level)
{
    const unsigned used = atomic_load(&s_used);
    for (unsigned i = 0; i < used; ++i) {
        const uint8_t own = atomic_load(&s_modules[i].level);
        if (own == XIAOTAI_LOG_LEVEL_INHERIT) continue;
        if (index--) continue;
        if (tag) *tag = s_modules[i].name;
        if (level) *level = own;
        return true;
    }
    return false;
}
