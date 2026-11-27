// SPDX-License-Identifier: MIT
/* mod_environment_optional: a product without the environment module.
 *
 * The modules that veto a switch (binding, recognition) do it with a framework
 * quiesce guard, so they neither include nor depend on this module:
 *   - this file is compiled with the include directories of binding and
 *     recognition and of everything they publicly depend on, and the
 *     environment header must not be reachable from there (the file itself
 *     includes neither header: a test, like its component, uses only what its
 *     manifest declares);
 *   - their manifests, CMake files and public headers name neither
 *     "environment" as a dependency, xiaotai_mod_environment nor the
 *     environment header;
 *   - the framework runs without the module: no AT+EP intents exist, and a
 *     guard written the way those modules write theirs still vetoes the
 *     reason XIAOTAI_QUIESCE_ENVIRONMENT and lets every other reason pass. */
#include <stdio.h>
#include <string.h>
#include "xiaotai/framework.h"
#include "xiaotai/port/os.h"

#if !__has_include("xiaotai/modules/binding.h") || !__has_include("xiaotai/modules/recognition.h")
#error "the include directories of binding and recognition are missing"
#endif
#if __has_include("xiaotai/modules/environment.h")
#error "binding or recognition pulls in the environment module"
#endif

#ifndef XT_MODULES_DIR
#error "XT_MODULES_DIR is the sdk/modules directory"
#endif

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                                     \
        }                                                                                 \
    } while (0)

static long read_file(const char *relative, char *out, size_t capacity)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", XT_MODULES_DIR, relative);
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    const size_t length = fread(out, 1, capacity - 1, file);
    fclose(file);
    out[length] = '\0';
    return (long)length;
}

static int check_component(const char *name, const char *header)
{
    static char text[65536];
    char relative[160];
    snprintf(relative, sizeof(relative), "%s/module.json", name);
    CHECK(read_file(relative, text, sizeof(text)) > 0);
    CHECK(strstr(text, "\"environment\"") == NULL);
    snprintf(relative, sizeof(relative), "%s/CMakeLists.txt", name);
    CHECK(read_file(relative, text, sizeof(text)) > 0);
    CHECK(strstr(text, "xiaotai_mod_environment") == NULL);
    snprintf(relative, sizeof(relative), "%s/include/xiaotai/modules/%s", name, header);
    CHECK(read_file(relative, text, sizeof(text)) > 0);
    CHECK(strstr(text, "modules/environment.h") == NULL);
    return 0;
}

static int s_vetoes;
/* The rule of binding.h / recognition.h: refuse only this reason, compared
 * with strcmp against the framework's macro. */
static xiaotai_err_t guard(const char *reason, void *ctx)
{
    (void)ctx;
    if (strcmp(reason, XIAOTAI_QUIESCE_ENVIRONMENT) != 0) return XIAOTAI_OK;
    ++s_vetoes;
    return XIAOTAI_ERR_BUSY;
}
static xiaotai_err_t guarded_init(void) { return xiaotai_quiesce_guard_register("guarded", guard, NULL); }
static const xiaotai_module_t GUARDED = {.name = "guarded", .init = guarded_init};
static const xiaotai_module_t *const MODULES[] = {&GUARDED};

typedef struct {
    const char *reason;
    xiaotai_err_t result;
} quiesce_call_t;
static xiaotai_signal_t s_sync;
static void quiesce_once(void *ctx)
{
    quiesce_call_t *call = ctx;
    uint32_t ticket = 0;
    call->result = xiaotai_quiesce_begin(call->reason, &ticket);
    if (call->result == XIAOTAI_OK) xiaotai_quiesce_end(ticket);
    xiaotai_signal_give(&s_sync);
}
static xiaotai_err_t quiesce(const char *reason)
{
    quiesce_call_t call = {.reason = reason, .result = 12345};
    if (xiaotai_run(quiesce_once, &call) != XIAOTAI_OK) return 12345;
    (void)xiaotai_signal_take(&s_sync, 3000);
    return call.result;
}

int main(void)
{
    if (check_component("xiaotai_mod_binding", "binding.h") ||
        check_component("xiaotai_mod_recognition", "recognition.h")) {
        return 1;
    }

    xiaotai_signal_init(&s_sync);
    const xiaotai_framework_config_t config = {.modules = MODULES, .module_count = 1, .available_needs = UINT32_MAX};
    CHECK(xiaotai_framework_start(&config) == XIAOTAI_OK);
    CHECK(!xiaotai_module_present("environment"));
    CHECK(!xiaotai_intent_exists("environment.reset") && !xiaotai_intent_exists("environment.cancel"));
    CHECK(quiesce(XIAOTAI_QUIESCE_ENVIRONMENT) == XIAOTAI_ERR_BUSY && s_vetoes == 1);
    CHECK(quiesce(XIAOTAI_QUIESCE_PROVISIONING) == XIAOTAI_OK && s_vetoes == 1);
    xiaotai_framework_reset();
    printf("PASS: mod_environment_optional\n");
    return 0;
}
