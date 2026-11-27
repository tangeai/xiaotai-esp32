// SPDX-License-Identifier: MIT
/* The same public headers seen by a build whose Kconfig changed every option:
 * on ESP-IDF sdkconfig.h defines these symbols, on the host a test or a
 * product variant passes them on the compiler command line. Defined here,
 * before the first include, which is what both amount to. */
#define CONFIG_XIAOTAI_AT_SOURCES_MAX 5
#define CONFIG_XIAOTAI_AT_LOCAL_MAX 40
#define CONFIG_XIAOTAI_AT_VERB_SETS_MAX 96
#define CONFIG_XIAOTAI_AT_RESTART_COMMAND 0
#define CONFIG_XIAOTAI_AT_RESTART_FORCE 0
#define CONFIG_XIAOTAI_AT_SETTING_WRITE 0
#define CONFIG_XIAOTAI_AT_INTENT_COMMAND 0

#include "xiaotai/at.h"

#include "at_config_test.h"

void at_config_overridden(at_config_values_t *values)
{
    values->sources = XIAOTAI_AT_SOURCES_MAX;
    values->local = XIAOTAI_AT_LOCAL_MAX;
    values->verb_sets = XIAOTAI_AT_VERB_SETS_MAX;
    values->restart = XIAOTAI_AT_RESTART_COMMAND;
    values->restart_force = XIAOTAI_AT_RESTART_FORCE;
    values->setting_write = XIAOTAI_AT_SETTING_WRITE;
    values->intent = XIAOTAI_AT_INTENT_COMMAND;
}
