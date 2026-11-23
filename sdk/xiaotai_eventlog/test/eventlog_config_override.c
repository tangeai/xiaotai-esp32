// SPDX-License-Identifier: MIT
/* The same public header seen by a build whose Kconfig changed every option:
 * on ESP-IDF sdkconfig.h defines these symbols, on the host a test or a
 * product variant passes them on the compiler command line. Defined here,
 * before the first include, which is what both amount to. */
#define CONFIG_XIAOTAI_EVENTLOG_SINKS_MAX 6
#define CONFIG_XIAOTAI_EVENTLOG_TOPICS_MAX 32

#include "xiaotai/eventlog.h"

#include "eventlog_config_test.h"

void eventlog_config_overridden(eventlog_config_values_t *values)
{
    values->sinks = XIAOTAI_EVENTLOG_SINKS_MAX;
    values->topics = XIAOTAI_EVENTLOG_TOPICS_MAX;
}
