// SPDX-License-Identifier: MIT
/* A command line that turns AT+RESTART off and leaves AT+RESTART FORCE set.
 * Kconfig never produces it (FORCE depends on AT+RESTART); the header must
 * still not publish a FORCE spelling without the command it belongs to. */
#define CONFIG_XIAOTAI_AT_RESTART_COMMAND 0
#define CONFIG_XIAOTAI_AT_RESTART_FORCE 1

#include "xiaotai/at.h"

#include "at_config_test.h"

void at_config_force_only(at_config_values_t *values)
{
    values->sources = XIAOTAI_AT_SOURCES_MAX;
    values->local = XIAOTAI_AT_LOCAL_MAX;
    values->verb_sets = XIAOTAI_AT_VERB_SETS_MAX;
    values->restart = XIAOTAI_AT_RESTART_COMMAND;
    values->restart_force = XIAOTAI_AT_RESTART_FORCE;
    values->setting_write = XIAOTAI_AT_SETTING_WRITE;
    values->intent = XIAOTAI_AT_INTENT_COMMAND;
}
