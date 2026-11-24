// SPDX-License-Identifier: MIT
#pragma once
/* Internal interface of the coprocessor part: the smallest actions the
 * platform does on the host link to the coprocessor. Every decision (rules,
 * markers, order, progress) is portable code in src/; an implementation only
 * moves bytes and reports what the link said.
 *
 *   esp-idf/coprocessor_link.c   the host-link component (work package A15)
 *   host/coprocessor_link.c      a scripted fake coprocessor for host tests
 *
 * All functions run on the network engine's worker (the part's hooks are
 * [engine]); none is called from an interrupt or a real-time task. They block
 * for as long as the link needs (RPC round trips), which is why the engine
 * runs the hooks on a worker of its own. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Width of the text fields of an application descriptor, terminator
 * included when the firmware sent one. */
#define XIAOTAI_COPROCESSOR_APP_TEXT 32U
#define XIAOTAI_COPROCESSOR_DIGEST_BYTES 32U

/* Version of the host-link protocol the coprocessor firmware reports. */
typedef struct {
    uint32_t major, minor, patch;
} xiaotai_coprocessor_version_t;

/* Application descriptor of the firmware the coprocessor runs, as received:
 * the text fields need not be terminated (callers check). magic is 0 from
 * firmware that sends only the strings. */
typedef struct {
    uint32_t magic;
    char project[XIAOTAI_COPROCESSOR_APP_TEXT];
    char version[XIAOTAI_COPROCESSOR_APP_TEXT];
    char platform[XIAOTAI_COPROCESSOR_APP_TEXT];   /* SDK version it was built with */
    uint8_t elf_sha256[XIAOTAI_COPROCESSOR_DIGEST_BYTES];
} xiaotai_coprocessor_app_t;

/* What the link handshake itself announced, before any request: the facts
 * that tell the pre-versioned protocol apart (rule 9 of coprocessor.h). */
typedef struct {
    uint32_t firmware;               /* firmware word of the handshake; 0: not sent */
    uint32_t capabilities;
    uint32_t ext_capabilities;
    bool rpc_version_advertised;     /* the peer announced its request protocol version */
    bool bus_buffer_config;          /* the peer sent a bus buffer configuration */
} xiaotai_coprocessor_link_facts_t;

/* Waits until the link the system started at boot is ready.
 * XIAOTAI_ERR_TIMEOUT when it is not within timeout_ms. */
xiaotai_err_t xiaotai_coprocessor_link_wait(uint32_t timeout_ms);
/* Starts the link and connects to the coprocessor (manual_link boards).
 * Any failure is reported as an error (XIAOTAI_FAIL when the link has no
 * code of its own); timeout_ms bounds the wait where the link takes one. */
xiaotai_err_t xiaotai_coprocessor_link_start(uint32_t timeout_ms);
/* Stops a link started by xiaotai_coprocessor_link_start, through the link
 * component's own lifecycle, so that a failed start is not bypassed. */
xiaotai_err_t xiaotai_coprocessor_link_stop(void);

/* Chip id the coprocessor announced in the handshake; 0 when none. */
uint16_t xiaotai_coprocessor_link_chip(void);
void xiaotai_coprocessor_link_facts(xiaotai_coprocessor_link_facts_t *facts);

/* Requests. An error is the request's own (timeout, no answer, refusal). */
xiaotai_err_t xiaotai_coprocessor_link_version(xiaotai_coprocessor_version_t *version);
xiaotai_err_t xiaotai_coprocessor_link_app(xiaotai_coprocessor_app_t *app);
/* The request that only the pre-versioned protocol answers in a known way
 * before Wi-Fi is initialised. *matched: the answer was that known one. An
 * error (no answer, timeout) is never a match. */
xiaotai_err_t xiaotai_coprocessor_link_probe_unversioned(bool *matched);

/* Firmware transfer into the coprocessor's inactive slot. A write that fails
 * may still have been appended by the coprocessor. With the pre-versioned
 * protocol end also activates. */
xiaotai_err_t xiaotai_coprocessor_link_ota_begin(void);
xiaotai_err_t xiaotai_coprocessor_link_ota_write(const uint8_t *data, size_t size);
xiaotai_err_t xiaotai_coprocessor_link_ota_end(void);
xiaotai_err_t xiaotai_coprocessor_link_ota_activate(void);

/* Sends one command on the configured peer-data channel, without trailing
 * NUL. Ordinary commands request the text reply; restart commands rely on
 * send status, because the old peer is about to stop serving the link. */
xiaotai_err_t xiaotai_coprocessor_link_command(uint32_t channel, const char *command, bool wait_for_reply,
                                              char *reply, size_t capacity);

#ifdef __cplusplus
}
#endif
