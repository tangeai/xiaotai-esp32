// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "xiaotai/port/err.h"

#define XIAOTAI_RECOGNITION_JOURNAL_ID_CAP 65u
#define XIAOTAI_RECOGNITION_JOURNAL_ITEMS_MAX 8u
#define XIAOTAI_RECOGNITION_JOURNAL_REVISION_MAX INT64_C(9007199254740991)
/* ESP_ERR_INVALID_RESPONSE, not yet named by the shared err.h. */
#define XIAOTAI_RECOGNITION_JOURNAL_INVALID_RESPONSE ((xiaotai_err_t)0x108)

typedef struct {
    char device_id[XIAOTAI_RECOGNITION_JOURNAL_ID_CAP];
    char request_id[XIAOTAI_RECOGNITION_JOURNAL_ID_CAP];
    uint64_t config_revision;
    uint32_t seconds;
    uint32_t scope;
} xiaotai_recognition_journal_record_t;

/* Values 1..3 are part of the deployed v3 format. */
typedef enum {
    XIAOTAI_RECOGNITION_JOURNAL_UNTOUCHED = 0,
    XIAOTAI_RECOGNITION_JOURNAL_PLAY_STARTED = 1,
    XIAOTAI_RECOGNITION_JOURNAL_PLAYED = 2,
    XIAOTAI_RECOGNITION_JOURNAL_DISMISSED = 3,
} xiaotai_recognition_journal_item_state_t;

typedef enum {
    XIAOTAI_RECOGNITION_JOURNAL_UNREAD,
    XIAOTAI_RECOGNITION_JOURNAL_READING,
    XIAOTAI_RECOGNITION_JOURNAL_ABSENT,
    XIAOTAI_RECOGNITION_JOURNAL_PRESENT,
    XIAOTAI_RECOGNITION_JOURNAL_ERROR,
} xiaotai_recognition_journal_state_t;

typedef struct {
    char id[XIAOTAI_RECOGNITION_JOURNAL_ID_CAP];
    xiaotai_recognition_journal_item_state_t state;
} xiaotai_recognition_journal_item_t;

typedef struct {
    xiaotai_recognition_journal_state_t state;
    xiaotai_err_t error; /* valid in ERROR */
    unsigned version;    /* valid in PRESENT; version 4 is a submit conflict */
    xiaotai_recognition_journal_record_t record; /* valid in PRESENT */
    uint8_t item_count;
    xiaotai_recognition_journal_item_t items[XIAOTAI_RECOGNITION_JOURNAL_ITEMS_MAX];
} xiaotai_recognition_journal_mirror_t;

bool rc_record_matches(const xiaotai_recognition_journal_mirror_t *mirror,
                       const xiaotai_recognition_journal_record_t *record);
xiaotai_recognition_journal_item_state_t rc_item_consumption(
    const xiaotai_recognition_journal_mirror_t *mirror, const char *item_id);

typedef enum {
    XIAOTAI_RECOGNITION_JOURNAL_LOAD,
    XIAOTAI_RECOGNITION_JOURNAL_PREPARE,
    XIAOTAI_RECOGNITION_JOURNAL_MARK_CONFLICT,
    XIAOTAI_RECOGNITION_JOURNAL_ITEM_READ,
    XIAOTAI_RECOGNITION_JOURNAL_ITEM_TRANSITION,
    XIAOTAI_RECOGNITION_JOURNAL_CLEAR,
    XIAOTAI_RECOGNITION_JOURNAL_CLEAR_CONFLICT,
    XIAOTAI_RECOGNITION_JOURNAL_ADVANCE_SCOPE,
} xiaotai_recognition_journal_operation_t;

typedef void (*xiaotai_recognition_journal_done_t)(xiaotai_err_t result,
    xiaotai_recognition_journal_item_state_t item, const xiaotai_recognition_journal_mirror_t *snapshot,
    void *ctx);

typedef struct {
    xiaotai_recognition_journal_operation_t operation;
    /* PREPARE ignores record.scope and obtains it from storage. On a
     * successful PREPARE the mirror contains the record to submit. */
    xiaotai_recognition_journal_record_t record;
    char item_id[XIAOTAI_RECOGNITION_JOURNAL_ID_CAP];
    xiaotai_recognition_journal_item_state_t expected_item;
    xiaotai_recognition_journal_item_state_t next_item;
    xiaotai_recognition_journal_done_t done;
    void *ctx;
} xiaotai_recognition_journal_job_t;

/* Start once for the product lifetime, after the storage port is ready.
 * The owner submits the boot LOAD explicitly. Initially the mirror is UNREAD. */
xiaotai_err_t xiaotai_recognition_journal_start(void);

/* [owner] Copies the job, including all IDs; never waits for storage.
 * OK promises one callback on the owner after the mirror has been updated.
 * Its snapshot reflects that job, even if another LOAD is queued. The pointer
 * stays valid only for the callback.
 * LOAD of an absent record returns NOT_FOUND. ITEM_READ reports the item
 * in done() on OK; other operations leave that parameter UNTOUCHED.
 *
 * CLEAR compares durable identity, not a stale caller's conflict flag.
 * CLEAR_CONFLICT is only for an authoritative rejected-without-task decision
 * and refuses a record that has since left v4. Both are idempotent if absent.
 * ADVANCE_SCOPE does not load pending. A confirmed last job advancing past
 * the mirror's known previous-scope record publishes ABSENT; otherwise UNREAD.
 * Storage failures may have persisted; the mirror becomes ERROR; load again. */
xiaotai_err_t xiaotai_recognition_journal_submit(const xiaotai_recognition_journal_job_t *job);

/* [owner] Read only, pointer valid until the next owner action. For environment
 * admission the caller must require ABSENT AND !busy, never just ABSENT. */
const xiaotai_recognition_journal_mirror_t *xiaotai_recognition_journal_mirror(void);
bool xiaotai_recognition_journal_busy(void);

#ifdef XIAOTAI_RECOGNITION_JOURNAL_TEST
/* Host fixture only. The owner must have completed every job and cannot submit
 * again before a subsequent start. Must not be called from the owner. */
void xiaotai_recognition_journal_host_stop(void);
#endif
