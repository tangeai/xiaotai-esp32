// SPDX-License-Identifier: MIT
#pragma once
/* The option values one translation unit sees, so the test can compare a unit
 * built with the defaults against units built with options changed. */
typedef struct {
    long sources;
    long local;
    long verb_sets;
    long restart;
    long restart_force;
    long setting_write;
    long intent;
} at_config_values_t;

/* at_config_override.c: built with every CONFIG_XIAOTAI_AT_* symbol set, the
 * guarded commands off. */
void at_config_overridden(at_config_values_t *values);

/* at_config_force_only.c: AT+RESTART off but AT+RESTART FORCE on, which
 * Kconfig cannot produce ("depends on") and a hand-written command line can. */
void at_config_force_only(at_config_values_t *values);
