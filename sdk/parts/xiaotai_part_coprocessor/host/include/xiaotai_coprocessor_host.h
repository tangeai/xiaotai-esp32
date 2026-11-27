// SPDX-License-Identifier: MIT
#pragma once
/* Test controls of the host fake behind the coprocessor part: a scripted
 * coprocessor on a scripted link. Tests of this part and of the boards and
 * engines built on it set what the coprocessor answers, run the part's hooks,
 * then read the counts and the journal: one line per link call, in order,
 * that other fakes of the same test may add their own lines to.
 *
 * Journal lines (each followed by '\n'):
 *   wait <timeout_ms> | start <timeout_ms> | stop | chip | facts | version |
 *   app | probe | begin | write +<offset> <bytes> | end | activate |
 *   command <channel_hex> <word> <wait_for_reply>
 * where <offset> counts from xiaotai_coprocessor_host_t.image (or is -1 when
 * the written data does not lie inside it).
 *
 * Single-threaded: the test drives the part from one thread. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XIAOTAI_COPROCESSOR_HOST_JOURNAL_BYTES 8192U

typedef struct {
    /* ---- what the link and the coprocessor answer */
    xiaotai_err_t wait_result, start_result, stop_result;
    uint16_t chip;
    uint32_t firmware, capabilities, ext_capabilities;   /* handshake facts */
    bool rpc_version_advertised, bus_buffer_config;
    xiaotai_err_t version_result;
    uint32_t major, minor, patch;
    xiaotai_err_t app_result;
    uint32_t app_magic;
    char project[32], version[32], platform[32];        /* copied as is, terminators included */
    uint8_t elf_sha256[32];
    xiaotai_err_t probe_result;
    bool probe_matched;
    xiaotai_err_t begin_result, end_result, activate_result;
    uint32_t fail_write;            /* 1-based write that fails with write_error; 0: none */
    xiaotai_err_t write_error;
    const uint8_t *image;           /* base that write offsets are reported against */
    xiaotai_err_t command_result;
    const char *command_reply;

    /* ---- what happened */
    unsigned waits, starts, stops, versions, apps, probes, begins, writes, ends, activations, commands;
    size_t written;                 /* bytes of the writes that succeeded */
    size_t largest_write;
    bool contiguous;                /* every write continued exactly where the last good one ended */
    uint32_t last_timeout_ms;       /* of the last wait or start */
} xiaotai_coprocessor_host_t;

/* The fake's state; tests write the answers and read the counts. */
xiaotai_coprocessor_host_t *xiaotai_coprocessor_host(void);
/* All answers XIAOTAI_OK with zero values, counts and journal cleared,
 * contiguous true. */
void xiaotai_coprocessor_host_reset(void);
/* Appends one line (printf format, no line end) to the journal. */
void xiaotai_coprocessor_host_note(const char *format, ...) __attribute__((format(printf, 1, 2)));
/* The journal so far; truncated lines are dropped once it is full. */
const char *xiaotai_coprocessor_host_journal(void);
/* Empties the journal and keeps everything else. */
void xiaotai_coprocessor_host_journal_clear(void);

#ifdef __cplusplus
}
#endif
