// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_runtime_init.py
// Replaces legacy: tests/test_native_profiles.py
// Replaces legacy: tests/test_boot_contract.py
/* xiaotai_start(): the product glue runs first, the board second, the
 * framework last, and a failure of an earlier step leaves the later ones
 * untouched; a board whose parts give other needs than its manifest declares
 * is refused; a second start returns the first result. The test plays the
 * things a product build supplies: the generated selection
 * (xiaotai/selection.h), and the board's entry points (defined here, so the
 * hardware abstraction's own board view is not linked and no board class is
 * needed). Every configuration halts on failure: the default restart would
 * end the test process. */
#include <stdio.h>
#include <string.h>
#include "xiaotai/hal.h"
#include "xiaotai/selection.h"
#include "xiaotai/xiaotai.h"

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition);   \
            return 1;                                                                       \
        }                                                                                   \
    } while (0)

static char s_trace[128];
static void trace(const char *text)
{
    strncat(s_trace, text, sizeof(s_trace) - strlen(s_trace) - 1);
    strncat(s_trace, " ", sizeof(s_trace) - strlen(s_trace) - 1);
}

/* ------------------------------------------------------------------- board */

static xiaotai_err_t s_board_result;
static uint32_t s_board_needs;

xiaotai_err_t xiaotai_hal_init(void)
{
    trace("board");
    return s_board_result;
}

uint32_t xiaotai_hal_needs(void) { return s_board_needs; }

void xiaotai_hal_identity(xiaotai_board_identity_t *identity)
{
    *identity = (xiaotai_board_identity_t){.model = "acme-test", .revision = "1"};
}

/* --------------------------------------------------------------- selection */

static xiaotai_err_t s_configure_result;
static xiaotai_err_t s_module_result;
static const char *s_theme, *s_discovery_url;

xiaotai_err_t xiaotai_selected_configure(const xiaotai_config_t *config)
{
    trace("configure");
    s_theme = config->theme;
    s_discovery_url = config->discovery_url;
    return s_configure_result;
}

static xiaotai_err_t engine_init(void) { trace("engine"); return XIAOTAI_OK; }
static xiaotai_err_t module_init(void) { trace("module"); return s_module_result; }
static int s_home_shown, s_clock_shown;
static void home_foreground(bool shown) { s_home_shown += shown ? 1 : -1; }
static void clock_foreground(bool shown) { s_clock_shown += shown ? 1 : -1; }

static const char *const MODULE_DEPENDS[] = {"engine", NULL};
static const xiaotai_module_t ENGINE = {.name = "engine", .init = engine_init};
static const xiaotai_module_t MODULE = {.name = "module", .depends = MODULE_DEPENDS,
                                        .needs = XIAOTAI_NEED_CAMERA, .init = module_init};
static const xiaotai_application_t CLOCK = {.name = "alarm", .foreground = clock_foreground};
static const xiaotai_application_t HOME = {.name = "companion", .foreground = home_foreground};

const xiaotai_module_t *const xiaotai_selected_modules[] = {&ENGINE, &MODULE, NULL};
const size_t xiaotai_selected_module_count = 2;
const xiaotai_application_t *const xiaotai_selected_applications[] = {&CLOCK, &HOME, NULL};
const size_t xiaotai_selected_application_count = 2;
const xiaotai_config_t xiaotai_product_config = {.home = "companion", .start_failure_restart_ms = XIAOTAI_START_HALT};
/* What the test's board manifest declares; the board's parts below vary. */
const uint32_t xiaotai_board_declared_needs = XIAOTAI_NEED_AUDIO_OUT | XIAOTAI_NEED_CAMERA;

int main(void)
{
    const xiaotai_config_t config = {.home = "companion", .theme = "lcd320",
                                     .discovery_url = "http://example.invalid/services",
                                     .start_failure_restart_ms = XIAOTAI_START_HALT};

    /* The glue refuses: the board is never touched; a second start returns
     * the first result without running anything again. */
    s_configure_result = XIAOTAI_ERR_INVALID_ARG;
    CHECK(xiaotai_start(&config) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(strcmp(s_trace, "configure ") == 0);
    s_configure_result = XIAOTAI_OK;
    CHECK(xiaotai_start(&config) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(strcmp(s_trace, "configure ") == 0);
    xiaotai_reset();

    /* The board fails: nothing is initialized. */
    s_trace[0] = '\0';
    s_board_result = XIAOTAI_ERR_TIMEOUT;
    CHECK(xiaotai_start(&config) == XIAOTAI_ERR_TIMEOUT);
    CHECK(strcmp(s_trace, "configure board ") == 0);
    CHECK(!xiaotai_module_present("engine"));
    xiaotai_reset();

    /* The board's parts do not give what its manifest declares: refused
     * before the framework. */
    s_trace[0] = '\0';
    s_board_result = XIAOTAI_OK;
    s_board_needs = XIAOTAI_NEED_AUDIO_OUT;
    CHECK(xiaotai_start(&config) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(strcmp(s_trace, "configure board ") == 0 && !xiaotai_module_present("engine"));
    xiaotai_reset();

    /* The services handoff fails after successful glue and board startup. */
    s_trace[0] = '\0';
    s_board_needs = xiaotai_board_declared_needs;
    s_module_result = XIAOTAI_ERR_NO_MEM;
    CHECK(xiaotai_start(&config) == XIAOTAI_ERR_NO_MEM);
    CHECK(strcmp(s_trace, "configure board engine module ") == 0 && !s_home_shown);
    CHECK(!xiaotai_module_present("engine") && !xiaotai_module_present("module"));
    s_module_result = XIAOTAI_OK;
    CHECK(xiaotai_start(&config) == XIAOTAI_ERR_NO_MEM);
    CHECK(strcmp(s_trace, "configure board engine module ") == 0 && !s_home_shown);
    CHECK(!xiaotai_module_present("engine") && !xiaotai_module_present("module"));
    xiaotai_reset();

    /* A product starts: glue, board, engines before modules, the home application shown. */
    s_trace[0] = '\0';
    CHECK(xiaotai_start(&config) == XIAOTAI_OK);
    CHECK(strcmp(s_trace, "configure board engine module ") == 0);
    CHECK(s_theme && strcmp(s_theme, "lcd320") == 0 && s_discovery_url == config.discovery_url);
    CHECK(xiaotai_module_present("module") && s_home_shown == 1 && s_clock_shown == 0);
    /* Started once: the second call repeats nothing. */
    CHECK(xiaotai_start(&config) == XIAOTAI_OK);
    CHECK(strcmp(s_trace, "configure board engine module ") == 0);
    xiaotai_reset();

    /* The generated product configuration. */
    s_trace[0] = '\0';
    s_home_shown = s_clock_shown = 0;
    CHECK(xiaotai_start(&xiaotai_product_config) == XIAOTAI_OK);
    CHECK(s_theme == NULL && s_discovery_url == NULL && s_home_shown == 1 && s_clock_shown == 0);
    xiaotai_reset();

    /* No configuration at all: defaults, the first application is shown. */
    s_home_shown = s_clock_shown = 0;
    CHECK(xiaotai_start(NULL) == XIAOTAI_OK);
    CHECK(s_clock_shown == 1 && s_home_shown == 0);
    xiaotai_reset();

    printf("PASS: xiaotai_start order, failures of each step, declared needs, one start, defaults\n");
    return 0;
}
