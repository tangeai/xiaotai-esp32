// SPDX-License-Identifier: MIT
#pragma once
/* Shared helpers of the identity host tests. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "identity_internal.h"
#include "xiaotai/framework.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/preferences.h"
#include "xiaotai_kv_host.h"
#include "xiaotai_mem_host.h"

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition);   \
            exit(1);                                                                        \
        }                                                                                   \
    } while (0)

#define WAIT_MS 1000U

static inline void test_reset(void)
{
    xiaotai_framework_reset();
    xiaotai_preferences_reset();
    xiaotai_identity_reset();
}

/* Empty storage, no injected failure, nothing loaded. */
static inline void test_clear(void)
{
    test_reset();
    CHECK(xiaotai_kv_init() == XIAOTAI_OK);
    CHECK(xiaotai_kv_flush(WAIT_MS) == XIAOTAI_OK);
    xiaotai_kv_host_fail(XIAOTAI_OK);
    xiaotai_kv_host_delay(0);
    xiaotai_kv_host_clear();
}

static inline xiaotai_err_t test_init_identity(void)
{
    const uint32_t before = xiaotai_mem_host_allocations();
    const xiaotai_err_t result = xiaotai_engine_identity_init();
    CHECK(xiaotai_mem_host_allocations() == before);
    return result;
}

/* The engine installs its setting store during real framework init. */
static inline xiaotai_identity_state_t test_load(const xiaotai_identity_config_t *config)
{
    static const xiaotai_module_t engine = {.name = "identity", .init = test_init_identity};
    static const xiaotai_module_t *const modules[] = {&engine};
    const xiaotai_framework_config_t framework = {.modules = modules, .module_count = 1};
    test_reset();
    CHECK(xiaotai_identity_configure(config) == XIAOTAI_OK);
    CHECK(xiaotai_framework_start(&framework) == XIAOTAI_OK);
    return xiaotai_identity_state();
}

static inline void test_set_str(const char *space, const char *key, const char *value)
{
    CHECK(xiaotai_kv_set_str(space, key, value, WAIT_MS) == XIAOTAI_OK);
}

/* A text of length characters, all c. */
static inline const char *test_text(char *buffer, size_t length, char c)
{
    memset(buffer, c, length);
    buffer[length] = '\0';
    return buffer;
}
