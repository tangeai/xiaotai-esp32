// SPDX-License-Identifier: MIT
#pragma once
/* What a command author uses: argument checks, reply lines, failure reasons,
 * sub-command tables, long output and unsolicited lines. The console itself
 * (sources, configuration, lifecycle) is xiaotai/at.h.
 *
 * Commands are registered with the framework (xiaotai_command_register) or,
 * for a command whose sub-commands come from several components, with
 * xiaotai_at_verbs_register below, which registers with the framework on the
 * components' behalf. Either way the handler runs on the framework's owner
 * task and has the framework's signature. Everything here works whether or not
 * the product runs a console: without one, lines go to whoever ran the command
 * and unsolicited lines are dropped.
 *
 * One vocabulary on every product. The rules below are fixed here; the
 * commands are not listed centrally. Every engine, module and application
 * declares the commands and verbs it registers in its own manifest
 * ("commands"), so a new component adds commands without touching this one.
 * The vocabulary of a build is the console's own commands plus those of the
 * selected manifests; the bench tool's dialect, the console script's
 * permissions and the public command list are generated from that union. The
 * component's specification freezes only how the spellings of earlier
 * firmware map onto it.
 *
 *   request   AT                         words separated by spaces or tabs;
 *             AT+<NAME> [<VERB>] [arg…]  names and verbs are upper case and
 *                                        matched exactly. Arguments are ASCII
 *                                        words: decimal numbers, 0|1 flags,
 *                                        identifiers [0-9A-Za-z_-], dotted
 *                                        names, URLs, and hexadecimal for any
 *                                        other text (UTF-8 bytes, two digits
 *                                        each; a single "-" is the empty
 *                                        text).
 *   reply     zero or more data lines    +<NAME>: key=value key=value …
 *             exactly one final line     OK
 *                                        ERROR: <ERROR-NAME>[ <REASON>]
 *   later     unsolicited lines          +<NAME>: key=value …   (only between
 *                                        two commands, never inside a reply)
 *
 * Data lines: <NAME> is the command's name without "AT+", or that name with a
 * suffix for a second record kind (+SCREEN-DATA:). One space after the colon,
 * pairs separated by one space, no spaces inside a value; text values are
 * hexadecimal with a key ending in _hex. Tools ignore keys they do not know:
 * keys are added over time, never renamed. The one exception is +HELP:, whose
 * lines carry a synopsis for people.
 *
 * OK on an action means accepted, not completed: completion is reported by the
 * owner's state query, by an unsolicited line or by an event-log entry.
 *
 * Function tags: [init] while engines, modules and applications initialize;
 * [owner] the framework owner task; [handler] on the owner task, inside a
 * command handler, with the reply that handler received; [console] the console
 * task; [any] any task. Nothing here may be called from a real-time task or an
 * interrupt handler unless stated. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/at_config.h"
#include "xiaotai/framework.h"
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Version of the vocabulary, printed as at=<n> by AT+VERSION and by the ready
 * line. It increases only when an existing spelling or reply key changes
 * meaning, which the rules above forbid; tools use it to tell this vocabulary
 * from the dialects of earlier firmware. */
#define XIAOTAI_AT_VOCABULARY 1

/* Longest output line, without its line ending. */
#define XIAOTAI_AT_OUT_LINE_MAX 512U
/* Longest unsolicited line, without its line ending. A line is sized for the
 * largest value of every field, not a typical one: a 15-field result with
 * five 64-bit counters reaches about 350 characters. */
#define XIAOTAI_AT_NOTIFY_MAX 512U
/* Longest identifier argument (device, request, contact). */
#define XIAOTAI_AT_ID_MAX 64U
/* Most words in one request, the command name included: the framework's own
 * limit. A longer request is `ERROR: INVALID_ARG TOO_MANY_WORDS` and nothing
 * runs. */
#define XIAOTAI_AT_WORDS_MAX XIAOTAI_COMMAND_ARGS_MAX

/* ------------------------------------------------------------------- results */

/* Results of this component (SDK-specific range 0x7200..0x72FF). */

/* Returned by a verb handler: "not mine now, ask the next component that
 * registered this verb" (see xiaotai_at_verbs_register). Never printed. */
#define XIAOTAI_AT_PASS 0x7201

/* Names used on the final line. Returns a constant string for the codes of
 * xiaotai/port/err.h: FAIL, NO_MEM, INVALID_ARG, INVALID_STATE, INVALID_SIZE,
 * NOT_FOUND, NOT_SUPPORTED, TIMEOUT, BUSY, NOT_ALLOWED. Any other code is
 * written into scratch as 0x<HEX> (capacity at least 12) and scratch is
 * returned; with a NULL or short scratch the result is "FAIL". [any] */
const char *xiaotai_at_error_name(xiaotai_err_t error, char *scratch, size_t capacity);

/* [handler], and [console] inside a local command or a stream's next().
 * Attaches a reason to the failure the caller is about to return and returns
 * `error`, so a handler writes `return xiaotai_at_fail(reply,
 * XIAOTAI_ERR_INVALID_ARG, "WIFI_FORMAT");`. reason is a fixed word of
 * [A-Z0-9_], at most 31 characters, with static storage duration (the pointer
 * is kept until the final line is written); it says which rule failed, never
 * user data. Printed as `ERROR: INVALID_ARG WIFI_FORMAT`. Ignored when the
 * command was not run by the console, when error is XIAOTAI_OK, and when the
 * reason is malformed. The last call before the handler returns wins. */
xiaotai_err_t xiaotai_at_fail(const xiaotai_reply_t *reply, xiaotai_err_t error, const char *reason);

/* --------------------------------------------------------------- reply lines */

/* [handler] and diagnostics providers. Formats one line and writes it through
 * reply. The rendered line must not contain a line break or an embedded NUL.
 * It needs no trailing line break.
 * Uses XIAOTAI_AT_OUT_LINE_MAX bytes of the caller's stack. A line that does
 * not fit is not written; on the console the command then ends with
 * `ERROR: INVALID_SIZE OUTPUT_TOO_LONG`. reply or format NULL: no effect.
 *
 * A handler may also call reply->write itself: the text is one or more lines
 * separated by '\n' (a final '\n' is optional), each at most
 * XIAOTAI_AT_OUT_LINE_MAX characters. */
void xiaotai_at_printf(const xiaotai_reply_t *reply, const char *format, ...) __attribute__((format(printf, 2, 3)));

/* ----------------------------------------------------------------- arguments */

/* Pure checks and conversions. [any] None of them accepts a sign, leading
 * white space, a radix prefix or trailing characters; a NULL text fails. */

/* Decimal digits only (at most 10); true when min <= value <= max. */
bool xiaotai_at_parse_u32(const char *text, uint32_t min, uint32_t max, uint32_t *value);
/* As above with an optional leading '-'. */
bool xiaotai_at_parse_i32(const char *text, int32_t min, int32_t max, int32_t *value);
/* Decimal digits only (at most 20); true when value <= max. */
bool xiaotai_at_parse_u64(const char *text, uint64_t max, uint64_t *value);
/* Exactly "0" or "1". */
bool xiaotai_at_parse_flag(const char *text, bool *value);
/* 1..max_length characters of [0-9A-Za-z_-]; max_length at most XIAOTAI_AT_ID_MAX. */
bool xiaotai_at_is_id(const char *text, size_t max_length);
/* Exactly count decimal digits (room codes, pass codes, pairing codes). */
bool xiaotai_at_is_digits(const char *text, size_t count);
/* Index of word in the NULL-terminated list, -1 when absent or word is NULL. */
int xiaotai_at_keyword(const char *word, const char *const *keywords);

/* Hexadecimal, both cases accepted, two digits per byte. *size receives the
 * byte count. XIAOTAI_ERR_INVALID_ARG for an odd length, a non-digit or an
 * empty string; XIAOTAI_ERR_INVALID_SIZE when capacity is too small (nothing
 * usable is left in out). */
xiaotai_err_t xiaotai_at_hex_decode(const char *hex, uint8_t *out, size_t capacity, size_t *size);
/* Text argument: hexadecimal UTF-8 into a terminated string, or "-" for the
 * empty text. A zero byte inside the text is XIAOTAI_ERR_INVALID_ARG; capacity
 * includes the terminator. The bytes are not checked to be valid UTF-8: the
 * owner of the value does that when it matters. A caller that decodes a
 * secret clears out when it is done with it. */
xiaotai_err_t xiaotai_at_text_decode(const char *hex, char *out, size_t capacity);
/* Upper-case hexadecimal of size bytes, terminated. Returns the characters
 * written, 0 when capacity < 2 * size + 1 (out is then an empty string when
 * capacity > 0). */
size_t xiaotai_at_hex_encode(const void *data, size_t size, char *out, size_t capacity);
/* Text value for a key ending in _hex: hexadecimal of the string's bytes, or
 * "-" for an empty or NULL text. Returns the characters written, 0 when it
 * does not fit. */
size_t xiaotai_at_text_encode(const char *text, char *out, size_t capacity);

/* --------------------------------------------------------------------- verbs */

/* One sub-command of a command: AT+CALL DEVICE <device-id> is the verb
 * "DEVICE" of "AT+CALL" with one argument. Tables and their strings have
 * static storage duration. */
typedef struct {
    /* Upper-case word after the command name; NULL: the command without a
     * verb (AT+CALL, or AT+LOG <after> when max_args > 0). */
    const char *word;
    uint8_t min_args;            /* words after the verb */
    uint8_t max_args;
    /* [owner] (or [console] for console_arg) argv[0] is the first word
     * after the verb; argc may be 0.
     * XIAOTAI_AT_PASS: this component does not own the request now. */
    xiaotai_err_t (*run)(int argc, const char *const *argv, const xiaotai_reply_t *reply);
    /* Argument synopsis shown by AT+HELP ("<device-id>", "0|1"); NULL or "": none. */
    const char *usage;
    /* Optional exact one-word argument for a local console release that must
     * work when the owner queue is full. Only for an exclusively owned named
     * verb with min_args == max_args == 1, registered by a module or engine.
     * Runs directly on the console after
     * its normal gate; the handler must be safe there and must not PASS.
     * Other arguments and xiaotai_command_run still go to the owner. */
    const char *console_arg;
} xiaotai_at_verb_t;

/* [owner] Runs the verbs that match argv (the framework handler's own argc
 * and argv, argv[0] being the command name). The candidates are chosen once:
 *   1. no word after the name: the verbs with word NULL and min_args 0;
 *   2. the word equals some verb's word: the verbs with that word, with the
 *      words after it;
 *   3. otherwise the verbs with word NULL that take arguments, with every
 *      word after the name.
 * No candidate is XIAOTAI_ERR_INVALID_ARG (reason UNKNOWN_VERB, or
 * MISSING_VERB for rule 1). Candidates run in table order. One whose argument
 * count does not fit is skipped; when all were skipped the result is
 * XIAOTAI_ERR_INVALID_ARG (reason ARGUMENT_COUNT). A candidate that returns
 * XIAOTAI_AT_PASS lets the next one run; any other result ends the dispatch
 * and is returned. When every candidate that ran passed, the result is
 * XIAOTAI_OK if any of them wrote a line (a status query answered by several
 * components, each adding its lines and passing) and
 * XIAOTAI_ERR_INVALID_STATE (reason NO_OWNER) otherwise (an action nobody
 * owns now: ACCEPT with no call ringing). For a component that keeps a whole
 * command to itself and registers it with xiaotai_command_register. */
xiaotai_err_t xiaotai_at_dispatch(const xiaotai_at_verb_t *verbs, size_t count, int argc,
                                  const char *const *argv, const xiaotai_reply_t *reply);

/* Most verb tables one build may register: XIAOTAI_AT_VERB_SETS_MAX, a build
 * option (xiaotai/at_config.h). */

/* [init] Adds verbs to a command that several components share: the device
 * call module contributes AT+CALL DEVICE, the video call module AT+CALL
 * CAMERA, neither knows the other. The first registration for a command name
 * registers it with the framework; the command then dispatches over all its
 * tables, in registration order, by the rules of xiaotai_at_dispatch, and
 * AT+HELP lists every verb with its usage. A component that is not part of the
 * build contributes nothing, so its verbs do not exist.
 *
 * Two components may register the same word (both call modules own ACCEPT):
 * each handler returns XIAOTAI_AT_PASS unless the request is its own. Inside
 * one table a word appears once. A verb with console_arg cannot be shared.
 * Works before the console engine is initialized. If the product explicitly
 * omits the AT command tables, individual descriptors and owner scope are
 * still validated, but optional AT verbs are not installed. Calls to
 * xiaotai_command_register remain independent of this policy.
 *
 * XIAOTAI_ERR_INVALID_ARG for a bad name or table (a NULL run, min_args above
 * max_args, a repeated word); XIAOTAI_ERR_INVALID_STATE when the name was
 * registered directly with xiaotai_command_register, or outside
 * initialization; XIAOTAI_ERR_NO_MEM when XIAOTAI_AT_VERB_SETS_MAX tables or
 * the framework's command table are full, or the table of tables cannot be
 * allocated. */
xiaotai_err_t xiaotai_at_verbs_register(const char *command, const xiaotai_at_verb_t *verbs, size_t count);

/* ------------------------------------------------------------- long output */

/* Output that is long (a screen, a recording, a trace) or slow to produce must
 * not hold the owner task. The handler prepares it, hands it over with one of
 * the two calls below and returns XIAOTAI_OK; the console task then writes it
 * and prints the final line when it is done. At most one hand-over per
 * command. While it runs the console accepts no other command.
 *
 * A handler never waits for another task (the UI task rendering a capture,
 * a companion chip answering): it hands over a stream whose next() does the
 * waiting on the console task, and a stream whose data only exists after
 * that wait writes it with xiaotai_at_blob_write().
 *
 * Every stream ends by itself (a fixed count, a bounded wait). The console
 * cuts off a stream or blob that goes the configured stream timeout without
 * completing a line, and the command then ends with `ERROR: TIMEOUT STREAM`;
 * a long blob on a slow source keeps completing lines and is not cut off.
 *
 * Both calls return XIAOTAI_ERR_NOT_SUPPORTED when the command was not run by
 * the console (nothing is taken over: the caller still owns its data),
 * XIAOTAI_ERR_INVALID_STATE for a second hand-over in one command and
 * XIAOTAI_ERR_INVALID_ARG for a malformed argument. After XIAOTAI_OK the
 * console owns the hand-over and calls release exactly once on the console
 * task: after the last line, when output fails or times out, and also when
 * the handler returns an error after handing over (then nothing is written). */

typedef struct {
    /* [console] Write the next lines through reply; return true while more
     * remain. May block for a bounded time (waiting for a companion chip to
     * answer, for the UI task to render) and may write a whole blob with
     * xiaotai_at_blob_write. May call xiaotai_at_fail to end the command
     * with that error instead of OK (then return false). */
    bool (*next)(void *ctx, const xiaotai_reply_t *reply);
    /* [console] Free what ctx owns. May be NULL. */
    void (*release)(void *ctx);
    void *ctx;
} xiaotai_at_stream_t;

/* [handler] The struct is copied. */
xiaotai_err_t xiaotai_at_stream(const xiaotai_reply_t *reply, const xiaotai_at_stream_t *stream);

/* Raw bytes per data line of a blob. */
#define XIAOTAI_AT_BLOB_CHUNK 128U
/* Longest blob header text. */
#define XIAOTAI_AT_BLOB_HEADER_MAX 160U

/* A block of bytes in the one wire format every dump uses:
 *   +<NAME>: <header> bytes=<size> fnv=<8 hex digits>
 *   +<NAME>-DATA: seq=<0,1,2,…> hex=<up to 256 hex digits>
 *   +<NAME>-END: chunks=<count> fnv=<8 hex digits>
 * fnv is FNV-1a (32 bit) over the raw bytes. Every data line but the last
 * carries XIAOTAI_AT_BLOB_CHUNK bytes. */
typedef struct {
    const char *name;            /* "SCREEN", "AUDIO-DUMP": [A-Z0-9-], static storage */
    /* key=value pairs describing the bytes ("format=RGB565LE width=320
     * height=240"); copied, at most XIAOTAI_AT_BLOB_HEADER_MAX characters. */
    const char *header;
    const uint8_t *data;         /* stays valid and unchanged until release */
    size_t size;                 /* at least 1 */
    /* [console] Free data. May be NULL for data with static storage. */
    void (*release)(void *ctx);
    void *ctx;
} xiaotai_at_blob_t;

/* [handler] The struct is copied; data is not. */
xiaotai_err_t xiaotai_at_blob(const xiaotai_reply_t *reply, const xiaotai_at_blob_t *blob);

/* [console] inside a stream's next(), with the reply next() received. Writes
 * the whole blob (name, header, data and size as in xiaotai_at_blob_t) in the
 * wire format above and returns when the last line is written; data stays the
 * caller's and only has to live until then. For dumps whose bytes are
 * produced on the console task (a screen captured while the owner task stays
 * free). XIAOTAI_OK; XIAOTAI_ERR_INVALID_ARG for a malformed argument;
 * XIAOTAI_ERR_NOT_SUPPORTED outside a console stream; XIAOTAI_FAIL when the
 * source stopped taking lines (the command then ends `ERROR: FAIL OUTPUT`;
 * next() returns false). */
xiaotai_err_t xiaotai_at_blob_write(const xiaotai_reply_t *reply, const char *name, const char *header,
                                    const uint8_t *data, size_t size);

/* [console, inside next()] Confirms that the records already written through
 * this reply were taken by the byte source. A caller that must perform a
 * side effect after a record uses this result before releasing the stream.
 * XIAOTAI_ERR_NOT_SUPPORTED for any other reply or a completed next(). */
xiaotai_err_t xiaotai_at_stream_result(const xiaotai_reply_t *reply);

/* [owner, inside a verb] True only for a live local-console command reply.
 * Use before taking resources for a console-only operation. */
bool xiaotai_at_reply_is_console(const xiaotai_reply_t *reply);

/* --------------------------------------------------------- unsolicited lines */

/* [any] Queues one line (a completed background action, a state change a test
 * bench waits for) to be printed on every console source between two
 * commands, never inside a reply. The text is copied. It must start with '+',
 * follow the data-line format and hold no line break.
 * XIAOTAI_OK: queued. XIAOTAI_ERR_INVALID_STATE: no console is running (the
 * caller needs no fallback). XIAOTAI_ERR_INVALID_SIZE: longer than
 * XIAOTAI_AT_NOTIFY_MAX, or than the configured queue can ever hold (counted
 * as dropped). XIAOTAI_ERR_INVALID_ARG: malformed.
 * XIAOTAI_ERR_NO_MEM: the queue is full; the line is dropped and counted.
 * Never blocks; not for real-time tasks (they post to an event-log topic). */
xiaotai_err_t xiaotai_at_notify(const char *line);
/* [any] As above, formatted on the caller's stack (XIAOTAI_AT_NOTIFY_MAX + 1
 * bytes). A task with a tight stack formats into its own smaller buffer and
 * calls xiaotai_at_notify. */
xiaotai_err_t xiaotai_at_notifyf(const char *format, ...) __attribute__((format(printf, 1, 2)));

/* [background, blocking] Queues the same notification with a delivery
 * receipt. Returns once all attached sources have taken the record, or on
 * error/timeout. Not for the owner, console, or a real-time task; accepts at
 * most one waiter (BUSY), 1..5000 ms. Timeout is not cancellation or a failed
 * operation: the copied line may still be delivered later. */
xiaotai_err_t xiaotai_at_notify_wait(const char *line, uint32_t timeout_ms);

/* [any] True while a console is running and its command gate is open: lets a
 * component skip preparing output nobody will read. */
bool xiaotai_at_active(void);

#ifdef __cplusplus
}
#endif
