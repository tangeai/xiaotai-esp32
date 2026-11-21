// SPDX-License-Identifier: MIT
#pragma once
/* Preferences: where registered settings are kept in persistent storage.
 *
 * A setting is identified by its storage key (xiaotai_setting_t.key in
 * xiaotai/framework.h). By default a value lives in the SDK's own namespace
 * "xt_settings" under that key: integers and booleans as a 4-byte
 * little-endian blob, text as a terminated blob.
 *
 * Deployed devices already hold their users' preferences in other places and
 * shapes (integer entries of any width in a product namespace, an index into a
 * table instead of the value, one byte of a packed record). A product lists
 * those places in a binding table; a bound setting is then read from and
 * written to its old place only, so the device keeps its preferences and can
 * still be rolled back. A new place or shape is one more row (or one more
 * storage kind), never a change to the settings that use it.
 *
 * Reads wait for storage; writes never do: they are queued, repeated writes of
 * one setting collapse into the latest value, and after the configured delay a
 * framework timer on the owner task hands them to the storage port as
 * asynchronous commits (one per namespace). No task of this component waits
 * for a preference write, so a settings change never creates a task: on a
 * board without external RAM a task stack would come from internal RAM in the
 * middle of a real-time session, and a failed creation would lose the
 * setting. The flash work runs on the storage port's worker, which also
 * reports each commit's result.
 *
 * How the framework reaches it: the identity engine's init installs the
 * preferences service as the framework's setting store
 * (xiaotai_setting_store_set in xiaotai/framework.h), before any setting is
 * registered. The store maps load_int to xiaotai_preferences_load_int with the
 * setting's default, load_text, store_int, store_text and flush to the
 * functions of the same name here; the listener slot is taken by the store,
 * which passes failures on with xiaotai_setting_store_result(key, err). The
 * framework does not link this component.
 *
 * Task tags: [glue], [init], [owner], [any] as in xiaotai/framework.h;
 * [blocking] any task that may wait, not a real-time task; [storage] the
 * storage port's worker (callbacks only, see xiaotai/port/kv.h). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/port/err.h"
#include "xiaotai/port/kv.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Longest text value, with its terminator. Covers the longest text deployed
 * firmware stores as a preference: a 64-character device id. */
#define XIAOTAI_PREFERENCE_TEXT_MAX 65U
/* Largest packed record. */
#define XIAOTAI_PREFERENCE_RECORD_MAX 32U
/* version_offset of a record without a version byte. */
#define XIAOTAI_PREFERENCE_NO_VERSION 0xFFU

/* ----------------------------------------------------------------- bindings */

/* Storage kinds. INT covers every integer entry type of the storage port
 * (xiaotai_kv_int_t: 8 to 64 bits, signed or not), so an entry of another
 * width is a binding row, not a change here. A further shape (a text table, a
 * bit field) is one more value here; no binding of another kind changes. */
typedef enum {
    XIAOTAI_PREFERENCE_INT = 0, /* integer entry of the binding's int_type */
    XIAOTAI_PREFERENCE_STRING,  /* string entry (text settings) */
    XIAOTAI_PREFERENCE_FIELD,   /* one byte of a packed record (see below) */
} xiaotai_preference_storage_t;

/* A fixed-size blob entry shared by several settings. It is read once, checked
 * as a whole (exact size, version within version_min..version_max, every
 * bound field in range, every before_raw byte as required) and rewritten as a
 * whole whenever one of its fields changes; bytes no binding covers are
 * written as 0. Rewriting needs a known record: it is written only when it
 * was read and passed the check, or was absent (NOT_FOUND).
 *   - A record that fails the check (corruption, or a newer version left by
 *     firmware the device was rolled back from) is never written during this
 *     boot: its fields report "not stored" when read, and storing any of them
 *     returns XIAOTAI_ERR_INVALID_STATE and is counted (refused). Rewriting it
 *     would replace data this firmware cannot see with defaults.
 *   - A record whose read failed in storage (timeout, I/O) is in the same
 *     state until a later load of one of its fields reads it successfully. */
typedef struct {
    const char *space;        /* namespace, at most 15 characters */
    const char *key;          /* entry, at most 15 characters */
    uint8_t size;             /* exact size, 1..XIAOTAI_PREFERENCE_RECORD_MAX */
    uint8_t version_offset;   /* byte holding the version, or XIAOTAI_PREFERENCE_NO_VERSION */
    uint8_t version_min;      /* versions accepted when read (inclusive) */
    uint8_t version_max;
    uint8_t version_write;    /* version always written */
    bool elide_unchanged;     /* Keep a known equal record (including absent logical defaults). */
    bool debounce_as_record; /* An accepted edit restarts the delay for this record's waiting fields. */
} xiaotai_preference_record_t;

typedef struct {
    /* The setting's storage key (xiaotai_setting_t.key). */
    const char *setting;
    xiaotai_preference_storage_t storage;
    /* INT, STRING: the entry. NULL for FIELD. */
    const char *space;
    const char *key;
    /* INT: the entry's type, as deployed firmware wrote it (an entry is read
     * with its own type only, see xiaotai/port/kv.h). Left 0 it is
     * XIAOTAI_KV_U8. Ignored for STRING and FIELD. */
    xiaotai_kv_int_t int_type;
    /* FIELD: the record and the byte in it. NULL / 0 otherwise. */
    const xiaotai_preference_record_t *record;
    uint8_t offset;
    /* FIELD in a versioned record: the first record version in which this byte
     * carries the setting. In older records the byte must equal before_raw
     * (else the record is invalid) and the setting reads as before_value.
     * 0: the byte carries the setting in every version. */
    uint8_t since_version;
    uint8_t before_raw;
    int32_t before_value;
    /* Integers (INT, FIELD): stored numbers accepted when read, inclusive. A
     * stored number outside the range is ignored (the setting keeps its
     * default) and counted. The range lies within the entry type's range
     * (FIELD: 0..255) and, without a value table, within int32_t, since the
     * stored number is then the setting's value; otherwise configuring fails
     * with XIAOTAI_ERR_INVALID_ARG. A U64 entry above INT64_MAX is therefore
     * never accepted. */
    int64_t raw_min;
    int64_t raw_max;
    /* Integers, optional: the stored number is an index into this table and the
     * setting's value is values[index] (value_count entries; raw_min/raw_max
     * are then 0 and value_count - 1). Storing picks the entry nearest to the
     * value, the lower index on a tie. The table's order is storage format:
     * entries are appended, never reordered. NULL: the stored number is the value. */
    const int32_t *values;
    size_t value_count;
    /* Member of the product's optional native typed snapshot. The marked
     * keys share one namespace; a change resaves the complete group.
     * This group has one latest-value slot and its own admission timeout. */
    bool snapshot;
} xiaotai_preference_binding_t;

typedef struct {
    /* Product binding table and its length; static storage. NULL / 0: every
     * setting uses the SDK's own namespace. A setting key may appear once. */
    const xiaotai_preference_binding_t *bindings;
    size_t binding_count;
    /* Delay between a change and its write, so a value dragged through many
     * steps is written once. Negative: write at once. 0: the default, 1000. */
    int32_t write_delay_ms;
    /* Requires a nonempty table of individual INT or STRING bindings,
     * without snapshots. Maintenance reports each failed commit until a
     * later commit of the same setting succeeds. It also reports a failed
     * startup storage job or namespace open until restart; a single key
     * that cannot be read still keeps its default. Ordinary flush, storage
     * layout and the write schedule are unchanged. */
    bool retain_maintenance_failures;
    /* Report a malformed packed entry as INVALID_STATE instead of NOT_FOUND.
     * Its fields still use their defaults, and the original bytes stay intact. */
    bool report_invalid_packed_load;
} xiaotai_preferences_config_t;

typedef struct {
    uint32_t loads;          /* values read from storage */
    uint32_t invalid;        /* stored values or records ignored as malformed or out of range */
    uint32_t refused;        /* stores refused because their record is not known to be valid */
    uint32_t stores;         /* changes accepted */
    uint32_t coalesced;      /* changes replaced by a newer one before being written */
    uint32_t writes;         /* entries handed to the storage port */
    uint32_t write_failures;
    /* A due write postponed: the storage port had no free job slot, or no
     * framework timer was free to schedule it. Retried after the write delay
     * (a missing timer: at the next store or flush). */
    uint32_t deferred;
    uint32_t pending;        /* changes accepted and not yet handed to the storage port */
} xiaotai_preferences_stats_t;

/* [storage] One accepted change reached storage (XIAOTAI_OK) or failed to. For a
 * packed record it is called once per field that was waiting. setting is the
 * storage key. Runs on the storage port's worker when its commit ended: no
 * allocation is needed, return quickly, call no function of this header and
 * no storage function, hand work to another task. An equal packed record
 * with elide_unchanged enabled submits no job and gets no write callback. */
typedef void (*xiaotai_preferences_written_fn_t)(const char *setting, xiaotai_err_t result, void *ctx);

/* ---------------------------------------------------------------- lifecycle */

/* [glue] Product configuration. XIAOTAI_ERR_INVALID_ARG for a table that is
 * inconsistent (a duplicate setting key, a field outside its record, a bad
 * namespace or entry name, an empty value table, an unknown int_type, a raw
 * range outside what the entry or the setting can hold);
 * XIAOTAI_ERR_INVALID_STATE after the first load or store. The table is not
 * copied. */
xiaotai_err_t xiaotai_preferences_configure(const xiaotai_preferences_config_t *config);

/* [init] The one listener for write results. The setting store the identity
 * engine installs takes it at init and turns a failure into "setting not
 * saved" (xiaotai_setting_store_result); host tests without that engine may
 * set their own. NULL removes it. XIAOTAI_ERR_INVALID_STATE when another
 * listener is set. */
xiaotai_err_t xiaotai_preferences_set_listener(xiaotai_preferences_written_fn_t written, void *ctx);

/* ------------------------------------------------------------------ reading */

/* [blocking] Stored value of the integer or boolean setting with this storage
 * key. XIAOTAI_ERR_NOT_FOUND when nothing usable is stored (absent, malformed,
 * out of range): the caller keeps its default. With report_invalid_packed_load,
 * a malformed packed entry returns XIAOTAI_ERR_INVALID_STATE instead.
 * Storage failures are returned
 * as they are (XIAOTAI_ERR_TIMEOUT, ...): the caller also keeps its default,
 * and a later store overwrites. Waits for storage at most 1000 ms per entry;
 * a packed record is read once and then served from memory.
 * default_value is the setting's default. For a FIELD setting it is kept in
 * memory, never written by this call, whatever the result: when the record is
 * rewritten for another field, a field that was not stored and not changed is
 * written as its default (as a raw byte, through the binding's value table),
 * never as 0. Ignored for other kinds. */
xiaotai_err_t xiaotai_preferences_load_int(const char *setting, int32_t default_value, int32_t *value);

/* [any] The configured packed-load diagnostic, separate from boot storage
 * errors and from a failed write. OK when no opted-in record was malformed. */
xiaotai_err_t xiaotai_preferences_packed_load_error(void);

/* [blocking] Stored value of a text setting, terminated. An empty, unterminated
 * or oversized stored text counts as malformed (XIAOTAI_ERR_NOT_FOUND).
 * XIAOTAI_ERR_INVALID_SIZE when capacity is too small. */
xiaotai_err_t xiaotai_preferences_load_text(const char *setting, char *out, size_t capacity);

/* ------------------------------------------------------------------ writing */

/* [owner] Remember the new value and write it after the configured delay
 * (one framework timer, armed for the earliest due change). Returns at once
 * and never waits, allocates or creates a task; XIAOTAI_OK means accepted,
 * and a later failure to write is counted and reported to the listener. value
 * is the setting's value, not the stored number. XIAOTAI_ERR_NO_MEM when more
 * distinct settings are waiting than the queue holds (16).
 * XIAOTAI_ERR_INVALID_STATE off the owner task, and for a FIELD setting whose
 * record is not known to be valid (see xiaotai_preference_record_t); nothing
 * is queued and the record stays as stored. */
xiaotai_err_t xiaotai_preferences_store_int(const char *setting, int32_t value);

/* [owner] As above for text; value is copied. Up to four text slots are shared
 * by waiting and in-flight changes; a new text slot beyond that returns
 * XIAOTAI_ERR_NO_MEM. XIAOTAI_ERR_INVALID_SIZE when the value,
 * with its terminator, exceeds XIAOTAI_PREFERENCE_TEXT_MAX. Storing "" removes
 * the entry of a normal bound STRING setting. Native snapshot groups keep
 * the old typed empty string. */
xiaotai_err_t xiaotai_preferences_store_text(const char *setting, const char *value);

/* [any] True while accepted changes have not reached the storage port, or the
 * storage port has not finished writing them. Reads one counter in memory: no
 * lock that storage work holds, no wait, no allocation; cheap enough to poll
 * from the owner task every few tens of milliseconds. */
bool xiaotai_preferences_pending(void);

/* [blocking] Write every accepted change now and wait until storage has
 * finished them, at most timeout_ms: hands every waiting change to the
 * storage port on the calling task (again while the port has no free job
 * slot), apart from packed records elided as unchanged, and then waits
 * with xiaotai_kv_flush. Used before framework restart.
 * XIAOTAI_ERR_TIMEOUT when still pending (nothing is cancelled); otherwise the
 * first write failure since the previous flush, or XIAOTAI_OK. */
xiaotai_err_t xiaotai_preferences_flush(uint32_t timeout_ms);

/* [blocking] Drain preferences before maintenance. A configured snapshot's
 * latest failure remains until a complete snapshot succeeds. With retention
 * enabled for individual typed bindings, each failure remains until the
 * corresponding setting is saved. */
xiaotai_err_t xiaotai_preferences_maintenance_flush(uint32_t timeout_ms);

/* [owner, quiesced] Check an existing packed record's durable outcome.
 * BUSY means outstanding storage, FAIL an uncommitted packed image.
 * With retry, admit one explicit retry of such an image without applying
 * settings again. OK then means accepted (or already clean), not durable:
 * check again without retry before releasing the maintenance hold. */
xiaotai_err_t xiaotai_preferences_packed_maintenance(bool retry);

/* -------------------------------------------------------------- diagnostics */

/* [any] */
void xiaotai_preferences_stats(xiaotai_preferences_stats_t *stats);

/* Host tests: drop configuration, cached records and queued changes. */
void xiaotai_preferences_reset(void);

#ifdef __cplusplus
}
#endif
