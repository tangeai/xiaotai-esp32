// SPDX-License-Identifier: MIT
#pragma once
/* Base module "recognition": sound recognition.
 *
 * The device records a few seconds of clean microphone audio, submits it to
 * the recognition service under a request id it made itself, and later holds
 * the task's result: text per item and, where the service produced it, speech
 * that the device plays once. This module owns that life cycle: admission by
 * the service's policy, the recording, the submission, the query by request
 * id, the notifications, the order in which speech may be played and what was
 * played, and the user's decisions (play, dismiss, archive). It offers a typed
 * API, events and intents; it shows no view and decides no business flow.
 *
 * Playback. When speech plays is the application's decision: it asks with
 * xiaotai_recognition_resume() (or the intent recognition.resume), typically
 * on XIAOTAI_EVENT_RECOGNITION_RESULT. A product may configure autoplay
 * instead (xiaotai_recognition_config_t). Speech plays at XIAOTAI_PLAY_URGENT
 * (xiaotai/media.h): an ordinary prompt cannot replace it and is refused while
 * it plays; a session, a recording, the speaker mute or a revoked activity
 * cut it. Speech whose playback began is never played a second time, so a cut
 * item is lost to the device (its text stays).
 *
 * One task at a time. A request id is written to persistent storage before it
 * is submitted and stays there until the task is archived, so a restart or an
 * uncertain upload is resolved by asking the service about that id, never by
 * recording again. While such a record exists nothing new is recorded and the
 * service environment must not be switched.
 *
 * Foreground: reading the policy, recording, submitting, downloading speech
 * and playing it hold the foreground activity "recognition"
 * (xiaotai/framework.h). Queries and policy refreshes run in the background,
 * only while no activity holds the device, and give way when one begins. A
 * revoked activity cancels the operation; what an upload that may have reached
 * the service left behind is recovered by a query.
 *
 * Identities. token names one operation of this module (a session generation,
 * xiaotai_generation_next()); a cancel for another token does nothing.
 * Replies and notifications carry the binding generation (xiaotai/identity.h)
 * and are dropped when it changed. The persistent record carries a binding
 * scope of its own, a stored counter that advances before every binding
 * attempt, so a record of an earlier binding is never queried, shown or
 * overwritten.
 *
 * Tags are those of xiaotai/framework.h: [init], [owner], [any], [rt].
 * Functions tagged [any] that start something validate their arguments, queue
 * the request to the owner task and return: XIAOTAI_OK means accepted, never
 * done. The receipt (xiaotai_recognition_status_t.decision_*) and the events
 * tell what became of it. They return XIAOTAI_ERR_NO_MEM when the owner's
 * queue is full and XIAOTAI_ERR_INVALID_STATE before the module started. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------- limits */

/* Request and item ids: 1..64 characters of [A-Za-z0-9_-]; size with terminator. */
#define XIAOTAI_RECOGNITION_ID_MAX 65U
/* Items of one task. */
#define XIAOTAI_RECOGNITION_ITEMS_MAX 8U
/* Longest recording. */
#define XIAOTAI_RECOGNITION_SECONDS_MAX 60U
/* Largest revision the service uses (2^53 - 1): exact in every JSON reader. */
#define XIAOTAI_RECOGNITION_REVISION_MAX 9007199254740991ULL
/* Longest summary of an item, in bytes with terminator (500 characters). */
#define XIAOTAI_RECOGNITION_SUMMARY_MAX 2001U

/* ------------------------------------------------------------ configuration */

/* What a product chooses. A zeroed struct selects every default. */
typedef struct {
    /* true: once a held result has speech ready, play it by itself, in item
     * order, whenever the device is free (no foreground activity), online and
     * the speaker is not muted, and playback is not suspended. false (the
     * default): speech does not start on result arrival. */
    bool autoplay;
    /* Preserve the deployed interaction: a successful dismiss also resumes
     * this task, including a dismiss of an already consumed item. The
     * default stays paused until xiaotai_recognition_resume() asks. */
    bool resume_after_dismiss;
    /* Preserve the deployed query deadlines and HTTP receive error mapping. */
    bool query_legacy_http;
    /* Request four shared HTTP slots for the legacy WAV download admission. */
    bool legacy_four_http_slots;
} xiaotai_recognition_config_t;

/* [glue] From the product glue, before the module's init. The struct is
 * copied. NULL: all defaults. XIAOTAI_ERR_INVALID_STATE once the module has
 * been initialized. */
xiaotai_err_t xiaotai_recognition_configure(const xiaotai_recognition_config_t *config);

/* -------------------------------------------------------------------- state */

/* The values are part of the diagnostics vocabulary (AT+RECOGNITION): new
 * phases are appended, existing ones never change. */
typedef enum {
    XIAOTAI_RECOGNITION_IDLE = 0,
    XIAOTAI_RECOGNITION_CONFIG,      /* foreground: reading the policy before a recording */
    XIAOTAI_RECOGNITION_RECORDING,   /* foreground */
    XIAOTAI_RECOGNITION_UPLOAD,      /* foreground: submitting the recording */
    XIAOTAI_RECOGNITION_CANCELLING,  /* giving back what a cancelled operation borrowed */
    XIAOTAI_RECOGNITION_ACCEPTED,    /* the service took the recording; its result is awaited */
    XIAOTAI_RECOGNITION_RECOVERY,    /* the outcome is uncertain or needs a decision: query, then archive */
    XIAOTAI_RECOGNITION_FAILED,      /* the operation failed and left nothing behind */
    XIAOTAI_RECOGNITION_CANCELLED,
    XIAOTAI_RECOGNITION_QUERY,       /* background: asking the service about the stored request id */
    XIAOTAI_RECOGNITION_RESULT,      /* a task snapshot is held */
    XIAOTAI_RECOGNITION_REFRESH,     /* background: reading the policy again */
    XIAOTAI_RECOGNITION_DOWNLOAD,    /* foreground: fetching the speech of one item */
    XIAOTAI_RECOGNITION_PLAYBACK,    /* foreground: playing it */
} xiaotai_recognition_phase_t;

/* What was asked for. The values are part of the diagnostics vocabulary. */
typedef enum {
    XIAOTAI_RECOGNITION_ACTION_NONE = 0,
    XIAOTAI_RECOGNITION_ACTION_RESUME,
    XIAOTAI_RECOGNITION_ACTION_DISMISS,
    XIAOTAI_RECOGNITION_ACTION_ARCHIVE,
    XIAOTAI_RECOGNITION_ACTION_RECORD,
    XIAOTAI_RECOGNITION_ACTION_CANCEL,
    XIAOTAI_RECOGNITION_ACTION_QUERY,
} xiaotai_recognition_action_t;

/* State of a task as the service reports it. */
typedef enum {
    XIAOTAI_RECOGNITION_TASK_QUEUED = 0,
    XIAOTAI_RECOGNITION_TASK_PROCESSING,
    XIAOTAI_RECOGNITION_TASK_SUCCEEDED,
    XIAOTAI_RECOGNITION_TASK_PARTIAL_FAILED,
    XIAOTAI_RECOGNITION_TASK_FAILED,
    XIAOTAI_RECOGNITION_TASK_UNKNOWN,
    XIAOTAI_RECOGNITION_TASK_CANCELLED,
} xiaotai_recognition_task_state_t;

/* State of one item of a task. */
typedef enum {
    XIAOTAI_RECOGNITION_ITEM_QUEUED = 0,
    XIAOTAI_RECOGNITION_ITEM_PROCESSING,
    XIAOTAI_RECOGNITION_ITEM_SUCCEEDED,
    XIAOTAI_RECOGNITION_ITEM_FAILED,
    XIAOTAI_RECOGNITION_ITEM_UNKNOWN,
    XIAOTAI_RECOGNITION_ITEM_CANCELLED,
} xiaotai_recognition_item_state_t;

typedef enum {
    XIAOTAI_RECOGNITION_OUTPUT_TEXT = 0,
    XIAOTAI_RECOGNITION_OUTPUT_AUDIO,
    XIAOTAI_RECOGNITION_OUTPUT_TEXT_AUDIO,
} xiaotai_recognition_output_t;

typedef enum {
    XIAOTAI_RECOGNITION_SPEECH_PENDING = 0,
    XIAOTAI_RECOGNITION_SPEECH_SYNTHESIZING,
    XIAOTAI_RECOGNITION_SPEECH_READY,
    XIAOTAI_RECOGNITION_SPEECH_FAILED,
    XIAOTAI_RECOGNITION_SPEECH_UNKNOWN,
    XIAOTAI_RECOGNITION_SPEECH_SKIPPED,
} xiaotai_recognition_speech_t;

/* What the device did with the speech of an item. Kept in persistent storage
 * with these values; an item without a stored state is UNTOUCHED. The only
 * steps are UNTOUCHED -> PLAY_STARTED or DISMISSED, and PLAY_STARTED -> PLAYED
 * or DISMISSED: speech whose playback began is never played a second time. */
typedef enum {
    XIAOTAI_RECOGNITION_UNTOUCHED = 0,
    XIAOTAI_RECOGNITION_PLAY_STARTED = 1,
    XIAOTAI_RECOGNITION_PLAYED = 2,
    XIAOTAI_RECOGNITION_DISMISSED = 3,
} xiaotai_recognition_consumption_t;

/* A snapshot published by the owner task; consistent in itself. */
typedef struct {
    xiaotai_recognition_phase_t phase;
    /* Names the running operation; 0 when none runs. What
     * xiaotai_recognition_cancel() takes. */
    uint32_t token;
    /* Recording progress, in samples at the capture rate. */
    uint32_t samples, target_samples;
    /* Why the last operation ended as it did, with the HTTP status and the
     * service's business code that came with it (0 when none). */
    xiaotai_err_t error;
    int http_status;
    int32_t code;
    /* Something borrowed (an HTTP exchange in flight, the recording buffer,
     * the speech file) has not come back yet: nothing new starts until it has. */
    bool cleanup_pending;
    /* The submission may have reached the service: only a query by request id
     * can tell. Never recorded again while this is unresolved. */
    bool possibly_sent;
    /* The service confirmed that the stored request id names no task (a
     * refused submission): the record may be archived without a result. */
    bool rejected_without_task;
    /* The stored request id; "" when no task is pending. */
    char request_id[XIAOTAI_RECOGNITION_ID_MAX];
    /* The task snapshot held (phase RESULT and after): its revision, item
     * count and state. revision 0: none held. */
    uint64_t task_revision;
    uint8_t task_items;
    xiaotai_recognition_task_state_t task_state;
    /* The policy as last read, and how the last read went. */
    uint64_t config_revision;
    xiaotai_err_t config_error;
    int config_http_status;
    int32_t config_code;
    bool config_enabled;          /* recognition is switched on and allowed for this device */
    /* The item whose speech is being fetched or played, or was last. */
    char item_id[XIAOTAI_RECOGNITION_ID_MAX];
    xiaotai_recognition_consumption_t consumption;
    uint32_t playback_ms;         /* time since its playback began */
    uint32_t playback_total_ms;   /* its duration */
    /* Playback is suspended: a playback failed, was cut or was cancelled.
     * Autoplay cannot lift the suspension; resume can. An opted-in product
     * also resumes after a successful dismissal. */
    bool playback_paused;
    /* Receipt of the last request: its serial (never 0 once one was handled),
     * what it was and how admission or execution went. A diagnostic, not a
     * durable confirmation. */
    uint32_t decision_serial;
    xiaotai_recognition_action_t decision_action;
    xiaotai_err_t decision_error;
    /* Clean capture rate used for this operation, in Hz. 0 before a recording. */
    uint32_t capture_rate_hz;
} xiaotai_recognition_status_t;

/* [any] Never blocks. XIAOTAI_ERR_TIMEOUT when the owner task is publishing at
 * this moment (*status is untouched): read again. */
xiaotai_err_t xiaotai_recognition_status(xiaotai_recognition_status_t *status);

/* The service's policy for this device: an admission and usage snapshot. */
typedef struct {
    bool valid;                   /* a policy was read for the current binding */
    bool enabled, allowed, effective_enabled;
    uint64_t revision;
    uint32_t monthly_limit;       /* submissions a month; one submission costs scheme_count */
    uint8_t scheme_count;         /* 1..XIAOTAI_RECOGNITION_ITEMS_MAX */
    uint32_t max_file_bytes;      /* 0: the service gave no limit */
    uint32_t max_audio_seconds;
    uint64_t used;
    uint64_t input_tokens, output_tokens, tts_characters, unknown_calls;
} xiaotai_recognition_policy_t;

/* [any] The policy as last read. Never blocks; XIAOTAI_ERR_TIMEOUT as above. */
xiaotai_err_t xiaotai_recognition_policy(xiaotai_recognition_policy_t *policy);

/* [any] Read the policy from the service now, in the background: the outcome
 * is XIAOTAI_EVENT_RECOGNITION_POLICY. Changes no task. Refused later (the
 * event carries XIAOTAI_ERR_INVALID_STATE) while an operation runs. */
xiaotai_err_t xiaotai_recognition_policy_refresh(void);

/* ------------------------------------------------------------------ actions */

/* [any] Record `seconds` (1..XIAOTAI_RECOGNITION_SECONDS_MAX, otherwise
 * XIAOTAI_ERR_INVALID_ARG) of clean microphone audio and submit it. Admitted
 * only when the device is free (no foreground activity), online and bound, the
 * microphone is not muted, no task is pending, nothing borrowed is
 * outstanding, the policy allows it and the memory for the recording is there;
 * otherwise the receipt and the phase tell why. The recording is never
 * repeated or padded: a capture gap fails it. */
xiaotai_err_t xiaotai_recognition_record(uint32_t seconds);

/* [any] Cancel the operation `token` names (xiaotai_recognition_status_t.token
 * as the caller saw it); 0 names whatever runs when the call is made. An
 * operation that has ended or been replaced is not touched
 * (XIAOTAI_ERR_INVALID_STATE in the receipt). Cancelling a download or a
 * playback suspends playback (playback_paused). A submission that may already
 * have reached the service is not undone: the phase becomes RECOVERY. */
xiaotai_err_t xiaotai_recognition_cancel(uint32_t token);

/* [any] Ask the service about the stored request id, in the background. Never
 * records, submits or clears anything. */
xiaotai_err_t xiaotai_recognition_query(void);

/* [any] Play the speech of the task `request_id` (it must be the stored one)
 * and lift a suspension (playback_paused). Items are taken in item order; an
 * item played or dismissed is skipped, and a later item never overtakes an
 * earlier one whose speech may still become playable (dismiss that one to
 * skip it). The request lasts for the task: speech that becomes ready later
 * plays when its item is reached, until no playable item is left or playback
 * is suspended again. Each item starts only while the device is free, online
 * and the speaker is not muted; until then it waits. Speech whose playback had
 * begun is not played again: an item left PLAY_STARTED stops the order, and
 * the receipt says XIAOTAI_ERR_INVALID_STATE. On a board without an audio
 * output nothing ever plays: the receipt says XIAOTAI_ERR_NOT_SUPPORTED. */
xiaotai_err_t xiaotai_recognition_resume(const char *request_id);

/* [any] Mark the speech of one item as dismissed: it will not be played.
 * Dismissing an item that was played or dismissed succeeds. It does not
 * resume playback unless the product chose resume_after_dismiss. */
xiaotai_err_t xiaotai_recognition_dismiss(const char *request_id, const char *item_id);

/* [any] Retire the stored task: the record is removed and a new recording
 * becomes possible. revision is the task revision the user reviewed (it must
 * be the one held, and the task must have reached a final state with no
 * speech left unplayed that could still be played), or 0 to retire a request
 * id the service confirmed it never took (rejected_without_task). Ids are
 * copied. XIAOTAI_ERR_INVALID_ARG for a malformed id or a revision above
 * XIAOTAI_RECOGNITION_REVISION_MAX. */
xiaotai_err_t xiaotai_recognition_archive(const char *request_id, uint64_t revision);

/* ------------------------------------------------------------------- result */

/* A read-only view of the task snapshot held, shared by reference count: the
 * owner task may adopt a newer snapshot while a reader still holds the old
 * one. Presentation only: never the authority for starting, cancelling or
 * consuming anything. */
typedef struct xiaotai_recognition_result xiaotai_recognition_result_t;

typedef struct {
    char request_id[XIAOTAI_RECOGNITION_ID_MAX];
    uint64_t revision;
    uint8_t items;
    xiaotai_recognition_task_state_t state;
} xiaotai_recognition_result_info_t;

typedef struct {
    char item_id[XIAOTAI_RECOGNITION_ID_MAX];
    xiaotai_recognition_item_state_t state;
    xiaotai_recognition_output_t output;
    xiaotai_recognition_speech_t speech;
    int32_t error_code, speech_error_code;   /* the service's codes; 0: none */
    bool has_text;        /* a text result exists (false for audio-only or unsuccessful items) */
    bool alarm;           /* the model's output, not a confirmed real-world event */
    /* What the device did with its speech, as stored when the view was taken. */
    xiaotai_recognition_consumption_t consumption;
} xiaotai_recognition_item_t;

/* [any] Never blocks, never touches the network, media or storage. Every
 * successful acquire is released. XIAOTAI_ERR_NOT_FOUND: no result is held;
 * XIAOTAI_ERR_TIMEOUT: the owner task is publishing, try again;
 * XIAOTAI_ERR_INVALID_STATE: the binding the result belonged to has ended. */
xiaotai_err_t xiaotai_recognition_result_acquire(xiaotai_recognition_result_t **result);
/* [any] NULL is ignored. */
void xiaotai_recognition_result_release(xiaotai_recognition_result_t *result);

/* [any] XIAOTAI_ERR_INVALID_STATE once the result's binding has ended (the
 * view stays valid to release); XIAOTAI_ERR_INVALID_ARG for an index past the
 * items. */
xiaotai_err_t xiaotai_recognition_result_info(const xiaotai_recognition_result_t *result,
                                              xiaotai_recognition_result_info_t *info);
xiaotai_err_t xiaotai_recognition_result_item(const xiaotai_recognition_result_t *result, size_t index,
                                              xiaotai_recognition_item_t *item);

/* [any] The summary of an item, terminated, into the caller's buffer.
 * XIAOTAI_ERR_NOT_FOUND when the item has no text; XIAOTAI_ERR_INVALID_SIZE
 * when capacity is too small (*size, when given, receives the length needed
 * without the terminator in every case). */
xiaotai_err_t xiaotai_recognition_result_summary(const xiaotai_recognition_result_t *result, size_t index,
                                                 char *out, size_t capacity, size_t *size);

/* [any] The whole text result of an item as the JSON object the service sent
 * ({"summary":…,"alarm":…,"labels":[…],"details":{…}}), terminated, into the
 * caller's buffer (allocate it as XIAOTAI_MEM_BULK: it may be tens of
 * kilobytes). Results as xiaotai_recognition_result_summary(). */
xiaotai_err_t xiaotai_recognition_result_detail(const xiaotai_recognition_result_t *result, size_t index,
                                                char *out, size_t capacity, size_t *size);

/* [any] Read one UTF-8 page of the same JSON without allocating or copying
 * the whole result. Start with offset 0 and use next/previous for the same
 * snapshot and capacity (at least five bytes, including NUL). If next ==
 * total, there is no next page. previous may be NULL to skip its lookup. */
xiaotai_err_t xiaotai_recognition_result_detail_page(const xiaotai_recognition_result_t *result,
                                                     size_t index, size_t offset, char *out,
                                                     size_t capacity, size_t *next, size_t *previous,
                                                     size_t *total);

/* -------------------------------------------------- binding and maintenance */

/* [owner] Completion of xiaotai_recognition_scope_advance(). */
typedef void (*xiaotai_recognition_done_fn_t)(xiaotai_err_t result, void *ctx);

/* [owner] A binding attempt is about to be sent to the server (a first binding
 * or a signed re-binding; not an ordinary login). Advances the stored binding
 * scope, so that whatever record exists belongs to the past; the attempt goes
 * on only after done reported XIAOTAI_OK. XIAOTAI_ERR_INVALID_STATE in done:
 * the counter cannot advance any more (it never wraps); any other error is the
 * storage's (a timeout may still have written: the next attempt advances
 * again, which only moves the scope further). Advances run in the order they
 * were asked for on the module's storage worker, each reported by its own
 * done. Returns XIAOTAI_ERR_NO_MEM when the storage queue is full and
 * XIAOTAI_ERR_INVALID_STATE before the module started; done is then not
 * called.
 *
 * With the binding module (xiaotai/modules/binding.h) this module registers
 * the advance as a preparation step (xiaotai_binding_prepare_register, name
 * "recognition"): every attempt is preceded by an advance and is not reported
 * to the server when the advance failed. The function is public for products
 * that bind by other means. */
xiaotai_err_t xiaotai_recognition_scope_advance(xiaotai_recognition_done_fn_t done, void *ctx);

/* [owner] May the service environment be switched (which changes what the
 * stored request id means)? XIAOTAI_OK: the record is known absent in the
 * current scope and nothing is pending. XIAOTAI_ERR_INVALID_STATE: the module has not started,
 * the record has not been read yet (the read started by
 * xiaotai_module_recognition_start() is still running), an operation runs,
 * something borrowed is outstanding, an archive has not finished, or a record
 * exists (also one of another device id, also one whose result was already
 * played: only archiving removes it). The read's error when the last read of
 * the record failed or found it damaged: unreadable is not absent. A failed
 * read is tried again by xiaotai_recognition_query() and by
 * xiaotai_recognition_record() (which is refused meanwhile); the check itself
 * answers from memory and clears, reads, queries and submits nothing.
 *
 * This module registers a quiesce guard (xiaotai_quiesce_guard_register, name
 * "recognition"; xiaotai/framework.h). For the reason
 * XIAOTAI_QUIESCE_ENVIRONMENT (compared with strcmp) it answers with this
 * check; for every other reason (XIAOTAI_QUIESCE_COPROCESSOR,
 * XIAOTAI_QUIESCE_PROVISIONING, XIAOTAI_QUIESCE_BINDING, a restart's own
 * word: none of them changes what the stored request id means, the record
 * survives them, and a binding attempt advances the scope by itself) it
 * agrees. The switch therefore reaches this module through the framework, and
 * the module neither depends on nor includes the environment module, which a
 * product may leave out. */
xiaotai_err_t xiaotai_recognition_maintenance_check(void);

/* ------------------------------------------------------------------- events */

/* Emitted on the framework event bus, in the order things happened; handlers
 * read the status for the present truth. */

/* The phase changed. args.value: the new xiaotai_recognition_phase_t;
 * args.text: the stored request id when it fits (ids of up to 63 characters),
 * else "". The operation's error is in the status. */
#define XIAOTAI_EVENT_RECOGNITION_STATE "recognition.state"
/* Whole captured seconds advanced for the current recording (args.value).
 * Coalesced; status carries the actual sample count, rate and target. */
#define XIAOTAI_EVENT_RECOGNITION_PROGRESS "recognition.progress"
/* A newer task snapshot was adopted (from a query or a notification).
 * args.value: its item count. Readers acquire the result again. */
#define XIAOTAI_EVENT_RECOGNITION_RESULT "recognition.result"
/* The policy was read, or a notification announced a newer one. args.value:
 * the xiaotai_err_t of the read (XIAOTAI_OK: xiaotai_recognition_policy() has
 * it). */
#define XIAOTAI_EVENT_RECOGNITION_POLICY "recognition.policy"
/* A request was handled. args.value: its xiaotai_err_t; data: a
 * xiaotai_recognition_receipt_t. */
#define XIAOTAI_EVENT_RECOGNITION_DECISION "recognition.decision"
/* The speech of an item starts (args.value 1) or stopped (0: played to the
 * end, cut or failed; the status tells which). data: the item id, terminated. */
#define XIAOTAI_EVENT_RECOGNITION_SPEECH "recognition.speech"

typedef struct {
    uint32_t serial;
    xiaotai_recognition_action_t action;
    xiaotai_err_t error;
} xiaotai_recognition_receipt_t;

/* ------------------------------------------------------------------ intents */

/* Handled by this module. Ids do not fit an intent's text, so the intents act
 * on the task that is held when they are handled; archiving, which must name
 * the revision the user reviewed, has no intent: applications call
 * xiaotai_recognition_archive().
 *   recognition.record    args.value: seconds, 1..60
 *   recognition.cancel    no arguments: whatever runs
 *   recognition.query     no arguments
 *   recognition.resume    no arguments: the stored task
 *   recognition.dismiss   args.value: index of the item in the result held */
#define XIAOTAI_INTENT_RECOGNITION_RECORD "recognition.record"
#define XIAOTAI_INTENT_RECOGNITION_CANCEL "recognition.cancel"
#define XIAOTAI_INTENT_RECOGNITION_QUERY "recognition.query"
#define XIAOTAI_INTENT_RECOGNITION_RESUME "recognition.resume"
#define XIAOTAI_INTENT_RECOGNITION_DISMISS "recognition.dismiss"

/* The foreground activity this module begins (the priority table's key). */
#define XIAOTAI_RECOGNITION_ACTIVITY "recognition"

/* -------------------------------------------------------------- diagnostics */

/* Counters since boot. */
typedef struct {
    uint32_t recordings, recordings_completed, recordings_failed;
    uint32_t submissions, submissions_accepted, submissions_uncertain, submissions_conflict;
    uint32_t queries, query_failures, query_retries;
    uint32_t policy_reads, policy_failures;
    uint32_t notifications, notifications_ignored;   /* ignored: another device, another request id, malformed */
    uint32_t results_adopted, results_stale;         /* stale: a revision not newer than the one held */
    uint32_t downloads, download_failures;
    uint32_t playbacks, playbacks_completed, playbacks_cut;
    uint32_t decisions, decisions_refused;
    uint32_t cancels;
    uint32_t revoked;                    /* operations cancelled because the activity was revoked */
    uint32_t activity_busy;              /* foreground work postponed or refused because the device was not free */
    uint32_t journal_writes, journal_failures, journal_timeouts;
    uint32_t scope_advances;
    uint32_t stale;                      /* replies and requests dropped for an old binding generation or token */
    uint32_t release_retries;            /* borrowed things that did not come back at the first attempt */
} xiaotai_recognition_stats_t;

/* [any] */
void xiaotai_recognition_stats(xiaotai_recognition_stats_t *stats);

/* ---------------------------------------------------------------- lifecycle */

/* Module lifecycle (called by the framework through the generated tables).
 * init: applies the configuration, declares what it needs from the platform
 * core, registers its signal subscriptions, intents, its preparation step with
 * the binding module, its quiesce guard, the AT+RECOGNITION command and the
 * diagnostics section "recognition". start: starts the storage worker and
 * reads the stored record and the scope in the background. Neither touches
 * the network or the audio path. */
xiaotai_err_t xiaotai_module_recognition_init(void);
void xiaotai_module_recognition_start(void);
/* Host tests: stop the worker and forget configuration, state, record mirror
 * and counters. */
void xiaotai_recognition_reset(void);

#ifdef __cplusplus
}
#endif
