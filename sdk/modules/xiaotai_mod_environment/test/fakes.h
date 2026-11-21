// SPDX-License-Identifier: MIT
#pragma once
/* Fakes for the environment module's host tests: the platform core (gate,
 * drained flag, environment store with scripted results and timing), the
 * network engine's maintenance gate and the preferences service. Controls are
 * set from the test thread and read on the owner or barrier task, so they are
 * atomics. Every call the module makes is appended to one trace, in order. */
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include "xiaotai/port/err.h"

/* ------------------------------------------------------------------- trace */

void fake_trace(const char *word);
/* Copy of the trace: words separated by single spaces. */
void fake_trace_get(char *out, unsigned capacity);
void fake_trace_clear(void);
/* True when the words appear in this order (not necessarily adjacent). */
bool fake_trace_in_order(const char *const *words);
/* How often word appears. */
unsigned fake_trace_count(const char *word);

/* ---------------------------------------------------------------- platform */

typedef struct {
    atomic_int gate_close_result;
    atomic_int gate_open_result_once;  /* returned by the next gate_open, then XIAOTAI_OK */
    atomic_bool gate_closed;
    atomic_uint gate_ticket;
    atomic_bool drained;
    atomic_uint drained_checks;
    /* Store: result handed to done after store_delay_ms (from a timer on the
     * owner task); store_manual keeps done until fake_platform_complete_store. */
    atomic_int store_call_result;      /* returned by the call itself */
    atomic_int store_result;
    atomic_uint store_delay_ms;
    atomic_bool store_manual;
    atomic_uint stores;
    atomic_bool store_was_null;
    char stored[256];
    /* Snapshot pieces. */
    atomic_int boot_error;
    atomic_bool unconfirmed;
    atomic_uint snapshots;
} fake_platform_t;
extern fake_platform_t g_platform;
void fake_platform_reset(void);
/* Test thread only between owner barriers. NULL restores default snapshot fields. */
void fake_platform_saved(const char *saved, const char *next);
/* [owner] Runs the done callback of a manual store with result. */
void fake_platform_complete_store(xiaotai_err_t result);

/* ------------------------------------------------------------------ network */

typedef struct {
    atomic_int begin_result;
    atomic_int restart_result;
    atomic_bool held;
    atomic_bool restart_locked;
    atomic_uint ticket;
    atomic_uint restart_ticket;
    /* Calls xiaotai_environment_cancel() from inside begin (ACQUIRING). */
    atomic_bool cancel_in_begin;
    atomic_int cancel_in_begin_result;
} fake_net_t;
extern fake_net_t g_net;
void fake_net_reset(void);

/* ------------------------------------------------------------- preferences */

#define FAKE_FLUSH_SCRIPT_MAX 8
typedef struct {
    atomic_bool pending;
    /* Results of successive flushes; after the script, flush_result. */
    xiaotai_err_t script[FAKE_FLUSH_SCRIPT_MAX];
    atomic_uint script_length;
    atomic_uint flushes;
    atomic_int flush_result;
    /* Block in flush until fake_preferences_unblock() or the timeout. */
    atomic_bool block;
    atomic_uint last_timeout_ms;
    atomic_bool flushed_on_owner;
} fake_preferences_t;
extern fake_preferences_t g_preferences;
void fake_preferences_reset(void);
void fake_preferences_unblock(void);
/* True once a flush is blocked, within timeout_ms. */
bool fake_preferences_wait_blocked(uint32_t timeout_ms);
