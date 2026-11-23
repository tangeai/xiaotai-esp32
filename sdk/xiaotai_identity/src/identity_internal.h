// SPDX-License-Identifier: MIT
#pragma once
/* Internal interface of the identity component: the configuration in effect,
 * the storage layouts (reading, classifying, the operations of a save and an
 * erase) and the state transitions the storage jobs apply. Shared by the
 * component's sources and its host tests; not part of the public API. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/identity.h"
#include "xiaotai/port/kv.h"

/* Namespace deployed firmware uses for the separate-entry layout. */
#define XIAOTAI_IDENTITY_STRINGS_SPACE "tirtc_cfg"
/* Entry names of the separate-entry layout; "endpoint" is obsolete and only
 * ever removed. */
#define XIAOTAI_IDENTITY_KEY_DEVICE_ID "device_id"
#define XIAOTAI_IDENTITY_KEY_SECRET "secret"
#define XIAOTAI_IDENTITY_KEY_CLIENT_ID "client_id"
#define XIAOTAI_IDENTITY_KEY_ENDPOINT "endpoint"
/* The one entry of the record layout and its exact size:
 * char device_id[65]; char device_key[257]. */
#define XIAOTAI_IDENTITY_KEY_RECORD "identity"
#define XIAOTAI_IDENTITY_RECORD_ID_BYTES (XIAOTAI_IDENTITY_DEVICE_ID_MAX + 1U)
#define XIAOTAI_IDENTITY_RECORD_KEY_BYTES (XIAOTAI_IDENTITY_DEVICE_KEY_MAX + 1U)
#define XIAOTAI_IDENTITY_RECORD_BYTES (XIAOTAI_IDENTITY_RECORD_ID_BYTES + XIAOTAI_IDENTITY_RECORD_KEY_BYTES)
/* "<prefix>-" plus 12 hex digits must fit the client id. */
#define XIAOTAI_IDENTITY_PREFIX_MAX (XIAOTAI_IDENTITY_CLIENT_ID_MAX - 13U)
/* Most storage operations of one save or erase (separate-entry layout). */
#define XIAOTAI_IDENTITY_OPS_MAX 4U

xiaotai_err_t identity_install_setting_store(void);

/* Configuration in effect: the product's, validated, with defaults filled. */
typedef struct {
    xiaotai_identity_layout_t layout;
    char space[XIAOTAI_KV_NAME_MAX + 1U];
    xiaotai_fingerprint_format_t fingerprint;
    const char *client_prefix; /* NULL: the chip name of the system port */
    bool storage_error_unbound;
} xiaotai_identity_settings_t;

/* ------------------------------------------------------------------ layouts */

/* Separate-entry layout: the state the three entry reads give. Each result is
 * what xiaotai_kv_get_str returned for that entry; the client id result is
 * ignored unless both required entries were read and are in range (the reader
 * then does not read it). identity holds the texts that were read and is
 * wiped unless the state is VALID.
 *   both required entries absent                   ABSENT
 *   both in range, client id absent or in range    VALID
 *   anything else that is absent, empty or too long (XIAOTAI_ERR_INVALID_SIZE)
 *                                                  INCOMPLETE
 * Any other result is a storage failure, XIAOTAI_FAIL included (a stored
 * string without its terminator cannot come from the storage itself): it is
 * returned, the first in entry order, and *state is UNREADABLE. Otherwise
 * XIAOTAI_OK. */
xiaotai_err_t xiaotai_identity_classify_strings(xiaotai_err_t device_id, xiaotai_err_t secret,
                                                xiaotai_err_t client_id, xiaotai_identity_t *identity,
                                                xiaotai_identity_state_t *state);

/* Record layout: the state one blob read gives (result and size as
 * xiaotai_kv_get returned them). Absent: ABSENT. Larger than the record
 * (XIAOTAI_ERR_INVALID_SIZE), another size, a field without its terminator or
 * an empty field: UNREADABLE with XIAOTAI_OK, a shape this firmware must not
 * overwrite. Any other failure is returned with *state UNREADABLE. identity is
 * filled only for VALID (client id empty) and wiped otherwise. */
xiaotai_err_t xiaotai_identity_classify_record(xiaotai_err_t result, const uint8_t *record, size_t size,
                                               xiaotai_identity_t *identity, xiaotai_identity_state_t *state);

/* Reads the identity in the configured layout with the storage port's
 * synchronous calls, each waiting at most XIAOTAI_KV_WAIT_MAX_MS, and
 * classifies it as above. The separate-entry layout reads device_id, then
 * secret, then client_id only when both are in range, and stops at the first
 * storage failure. [any task that may wait for storage] */
xiaotai_err_t xiaotai_identity_layout_read(const xiaotai_identity_settings_t *settings, xiaotai_identity_t *identity,
                                           xiaotai_identity_state_t *state);

/* Operations of a save in the configured layout; returns their count.
 * Separate entries: SET_STR device_id, SET_STR secret, SET_STR client_id,
 * ERASE endpoint, in this order, so that every prefix a power loss can leave
 * reads as INCOMPLETE or as the new identity. Record: one SET of the 322-byte
 * record built in record (zero after each terminator). The operations point
 * into identity and record, which the caller keeps until the storage port
 * copied them. identity must be valid (xiaotai_identity_valid). */
size_t xiaotai_identity_layout_save_ops(const xiaotai_identity_settings_t *settings, const xiaotai_identity_t *identity,
                                        uint8_t record[XIAOTAI_IDENTITY_RECORD_BYTES],
                                        xiaotai_kv_op_t ops[XIAOTAI_IDENTITY_OPS_MAX]);

/* Operations of an erase; returns their count. Separate entries: ERASE of the
 * three entries and of "endpoint". Record: ERASE of the record entry only (its
 * namespace holds other owners' data). */
size_t xiaotai_identity_layout_erase_ops(const xiaotai_identity_settings_t *settings,
                                         xiaotai_kv_op_t ops[XIAOTAI_IDENTITY_OPS_MAX]);

/* -------------------------------------------------------------------- state */

/* Configuration in effect (defaults until xiaotai_identity_configure). */
const xiaotai_identity_settings_t *xiaotai_identity_settings(void);

typedef enum {
    IDENTITY_LOADED, IDENTITY_LOAD_FAILED, IDENTITY_SAVED, IDENTITY_SAVE_FAILED, IDENTITY_ERASED, IDENTITY_REFUSED,
} identity_job_stat_t;

void identity_count_job(identity_job_stat_t outcome);
xiaotai_err_t identity_jobs_init(void);
void identity_jobs_silence(void); /* host reset; no concurrent submissions */

/* True once the engine init has run. */
bool xiaotai_identity_ready(void);

/* What a finished storage job changes. */
typedef enum {
    /* A save reported XIAOTAI_OK: the loaded identity becomes this one (VALID).
     * The generation advances when the device id or key differs from the
     * identity loaded before, or none was loaded. */
    XIAOTAI_IDENTITY_APPLY_SAVE = 0,
    /* A reload read storage: identity and state replace the loaded ones. The
     * generation advances when a VALID identity is replaced by one with
     * another device id or key, or by a state that is not VALID. */
    XIAOTAI_IDENTITY_APPLY_RELOAD,
    /* An erase reported XIAOTAI_OK: ABSENT, and the generation advances. */
    XIAOTAI_IDENTITY_APPLY_ERASE,
} xiaotai_identity_apply_t;

/* Applies a finished job under the state lock; identity is ignored for an
 * erase and for a reload whose state is not VALID. Returns true when the
 * generation advanced. Never touches storage. [any] */
bool xiaotai_identity_apply(xiaotai_identity_apply_t kind, const xiaotai_identity_t *identity,
                            xiaotai_identity_state_t state);

/* Copies the stored client id of the loaded identity ("" unless VALID). */
void xiaotai_identity_stored_client_id(char out[XIAOTAI_IDENTITY_CLIENT_ID_MAX + 1U]);

/* The generation after current: current + 1, skipping 0 on wrap-around. */
uint32_t xiaotai_identity_generation_next(uint32_t current);
