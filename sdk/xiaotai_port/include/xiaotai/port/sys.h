// SPDX-License-Identifier: MIT
#pragma once
/* System facts and actions, the two digests SDK code needs from the
 * platform's cryptography (hardware-accelerated where the chip has it), and
 * the port's own diagnostics.
 *
 * Callers, with the tags defined once in xiaotai/framework.h; nothing here
 * may be called from an interrupt handler. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *version;     /* firmware version (PROJECT_VER) */
    const char *platform;    /* e.g. "ESP-IDF v6.0.2" or "host" */
    const char *chip;        /* e.g. "esp32s3" */
    unsigned cores;
    unsigned chip_revision;
    char elf_sha256[65];     /* hex, empty when unknown */
} xiaotai_firmware_t;

/* [any] */
void xiaotai_firmware(xiaotai_firmware_t *info);
/* [any, rt] Uniform random 32 bits from the platform's hardware or OS source. */
uint32_t xiaotai_random(void);
/* [any task whose stack is internal RAM] Restarts at once. Callers that own
 * queued storage writes flush them first (xiaotai_kv_flush). The rule is the
 * one placement that is safe on every board, not a measured failure: deployed
 * firmware handed its environment-switch restart to an internal-stack task on
 * a board with a coprocessor, while another coprocessor board restarted from
 * an external-RAM stack after a coprocessor update. The framework's orderly
 * restart uses a short task of its own, the network engine its worker. */
void xiaotai_restart(void) __attribute__((noreturn));

#define XIAOTAI_SHA256_BYTES 32U

/* [any task that may block; not a real-time task] SHA-256 of size bytes. Long
 * inputs yield to other tasks between blocks. On failure out is all zero. */
xiaotai_err_t xiaotai_sha256(const void *data, size_t size, uint8_t out[XIAOTAI_SHA256_BYTES]);
/* [any task that may block; not a real-time task] HMAC-SHA256. The key is used
 * for this call only and no copy outlives it. XIAOTAI_ERR_INVALID_ARG for a
 * NULL or empty key or a NULL out; on any failure out is all zero. */
xiaotai_err_t xiaotai_hmac_sha256(const void *key, size_t key_size, const void *data, size_t size,
                                  uint8_t out[XIAOTAI_SHA256_BYTES]);

/* --------------------------------------------------------------- diagnostics */

/* Text sink of the port's reports: one line per call, without a line end, in
 * the "name=value name=value" form. The text is valid only during the call.
 * Same shape as xiaotai_hal_write_fn_t (xiaotai/hal.h), so the console engine
 * prints both as diagnostic sections the same way. */
typedef void (*xiaotai_port_write_fn_t)(void *ctx, const char *line);

/* [any; not a real-time task] Chip-level diagnostics the platform keeps below
 * every component: for example the USB console's connection monitor (its
 * disconnect decisions with their SOF and tick-interval evidence) and the
 * cache counters of chips that have them. Reads counters only; changes
 * nothing and never blocks for long. A platform without such facts writes no
 * line. The console engine shows it as the diagnostic section "port". */
void xiaotai_port_report(xiaotai_port_write_fn_t write, void *ctx);

/* [any; not a real-time task] Runs one named diagnostic action of a
 * diagnostic build (for example "usb_tick": stall the tick interrupt once to
 * prove the USB monitor survives it) and writes its result lines.
 * XIAOTAI_ERR_NOT_FOUND: no such probe in this build (product builds have
 * none). XIAOTAI_ERR_INVALID_STATE: the probe cannot run now (a one-shot probe
 * already ran, the device it needs is absent). xiaotai_port_probes lists the
 * names; probes may disturb timing on purpose and are never run by software
 * on its own, only on an explicit request. */
xiaotai_err_t xiaotai_port_probe(const char *name, xiaotai_port_write_fn_t write, void *ctx);
/* [any] The probe names of this build, NULL-terminated; an empty list in
 * product builds. Static storage. */
const char *const *xiaotai_port_probes(void);

#define XIAOTAI_TASK_NAME_MAX 16U

typedef struct {
    char name[XIAOTAI_TASK_NAME_MAX];   /* cut to fit */
    uint32_t stack_min_free;            /* lowest free stack since the task started, bytes */
    unsigned priority;
    int core;                           /* XIAOTAI_CORE_ANY when not pinned */
} xiaotai_task_info_t;

/* [any; not a real-time task] Every task of the system, platform tasks
 * included, in no particular order. Copies up to capacity records and sets
 * *count to the number of tasks (which may exceed capacity). Briefly stops
 * the scheduler on platforms that need it to walk the task list, so it is a
 * diagnostics call. XIAOTAI_ERR_NOT_SUPPORTED: the platform's configuration
 * keeps no task list (*count 0). */
xiaotai_err_t xiaotai_task_list(xiaotai_task_info_t *tasks, size_t capacity, size_t *count);

#ifdef __cplusplus
}
#endif
