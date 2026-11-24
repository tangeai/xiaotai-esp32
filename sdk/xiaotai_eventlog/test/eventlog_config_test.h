// SPDX-License-Identifier: MIT
#pragma once
/* The capacities one translation unit sees, so the test can compare a unit
 * built with the defaults against one built with every option changed. */
typedef struct {
    long sinks;
    long topics;
} eventlog_config_values_t;

/* eventlog_config_override.c: built with every CONFIG_XIAOTAI_EVENTLOG_*
 * symbol set. */
void eventlog_config_overridden(eventlog_config_values_t *values);
