// SPDX-License-Identifier: MIT
#pragma once
/* Seams between the console engine's own sources; not part of the contract.
 * The public functions of xiaotai/at_command.h are spread over several
 * sources (text helpers, reply framing, verbs, the console itself); what one
 * of them needs from another is declared here so each stays testable on its
 * own with a fake of the other side. */
#include "xiaotai/at_command.h"
#include "xiaotai/at.h"
#include "xiaotai/hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Why xiaotai_at_printf left a line out. */
typedef enum {
    /* The formatted line is longer than XIAOTAI_AT_OUT_LINE_MAX: a reply that
     * silently lost a line would look complete to a test bench, so the
     * command must fail (`ERROR: INVALID_SIZE OUTPUT_TOO_LONG`). */
    XIAOTAI_AT_OUTPUT_TOO_LONG = 1,
    /* The formatted text holds a CR or LF (a broken format or an argument that
     * was not hex-encoded) or the C library reported a format error: written
     * as is it would end the record early and inject a line the tools would
     * parse as a reply. The console ends the command `ERROR: FAIL OUTPUT`. */
    XIAOTAI_AT_OUTPUT_MALFORMED = 2,
} xiaotai_at_output_fault_t;

/* [handler] Reports a line xiaotai_at_printf did not write. For a reply the
 * console issued (recognized by its write function) the fault decides the
 * command's final line; any other reply (a debug page running a command
 * directly) is left alone, the line is simply missing there. Implemented in
 * at_reply.c. */
void xiaotai_at_reply_output_fault(const xiaotai_reply_t *reply, xiaotai_at_output_fault_t fault);

/* Dispatch uses a synchronous forwarding reply to count output after PASS.
 * Console helpers unwrap before checking who owns the reply. */
const xiaotai_reply_t *at_reply_original(const xiaotai_reply_t *reply);
typedef void (*at_verb_visitor_t)(const xiaotai_at_verb_t *verb, void *ctx);
bool at_verbs_visit(const char *command, at_verb_visitor_t visitor, void *ctx);
bool at_verbs_visit_distinct(const char *command, at_verb_visitor_t visitor, void *ctx);
xiaotai_command_fn_t at_verbs_console_match(int argc, const char *const *argv);
void at_verbs_reset(void);

#define AT_COUNTERS(X) \
    X(lines) X(commands_ok) X(commands_failed) X(line_overflows) X(bad_characters) \
    X(gate_rejections) X(handler_timeouts) X(late_dropped) X(write_errors) \
    X(owner_flushes) X(streams) X(notify_sent) X(notify_dropped)
typedef enum {
#define AT_COUNTER(name) at_count_##name,
    AT_COUNTERS(AT_COUNTER)
#undef AT_COUNTER
    at_counter_count,
} at_counter_t;
void at_count(at_counter_t counter);
bool at_write_record(const xiaotai_at_io_t *io, const char *record, size_t bytes);
bool at_console_has_source(void);
xiaotai_err_t at_console_ensure_off_owner_reader(void);
bool at_valid_reason(const char *reason);
xiaotai_err_t at_local_verbs_register(const char *name, const xiaotai_at_verb_t *verbs, size_t count);
bool at_command_tables_omitted(void);
void at_console_describe_sources(const xiaotai_reply_t *reply);
void at_describe_source(const xiaotai_at_io_t *io, const xiaotai_reply_t *reply);
const xiaotai_at_io_t *at_platform_source(void);
xiaotai_err_t at_platform_open(void);
void at_board_identity(xiaotai_board_identity_t *identity);
void at_board_report(const xiaotai_reply_t *reply);
xiaotai_err_t at_local_init(const xiaotai_at_config_t *config);
xiaotai_err_t at_log_attach(void);
xiaotai_err_t at_log_init(bool follow);
void at_log_stop(void);
void at_mem_report(const xiaotai_reply_t *reply);
bool at_id_valid(const char *name);
bool at_name_allowed(const char *name, const char *const *allow);
bool at_setting_write_allowed(const char *name);
bool at_intent_allowed(const char *name);
const xiaotai_setting_t *at_setting_find(const char *name);
#if XIAOTAI_AT_RESTART_COMMAND
xiaotai_err_t xiaotai_at_guarded_restart_register(const xiaotai_at_config_t *config);
xiaotai_err_t xiaotai_at_guarded_restart_schedule(const xiaotai_reply_t *reply);
void xiaotai_at_guarded_restart_reply(bool acknowledged);
#endif
#if XIAOTAI_AT_RESTART_FORCE
xiaotai_err_t xiaotai_at_guarded_force_run(int argc, const char *const *argv, const xiaotai_reply_t *reply);
#endif
#if XIAOTAI_AT_SETTING_WRITE
xiaotai_err_t xiaotai_at_guarded_set_register(void);
#endif
#if XIAOTAI_AT_INTENT_COMMAND
xiaotai_err_t xiaotai_at_guarded_intent_register(void);
#endif

typedef struct {
    xiaotai_err_t error;
    const char *reason;
} at_result_t;

typedef enum { AT_LONG_NONE, AT_LONG_STREAM, AT_LONG_BLOB } at_long_kind_t;
typedef struct {
    at_long_kind_t kind;
    union {
        xiaotai_at_stream_t stream;
        xiaotai_at_blob_t blob;
    } as;
    char header[XIAOTAI_AT_BLOB_HEADER_MAX + 1];
} at_long_output_t;

/* at_console.c: runs one AT line (split in place) through the gate and the
 * handlers and returns its result without writing the final line; true when
 * a command name, and verb if given, is in this build and open at the gate. */
at_result_t at_console_run(const xiaotai_at_io_t *io, char *line);
bool at_console_knows(const char *name, const char *verb);
bool at_console_started(void);
/* at_plain.c: a line that is not an AT command is a plain maintenance
 * command; runs it on the console task and returns its result. */
bool at_plain_line(const char *line);
at_result_t at_plain_run(const xiaotai_at_io_t *io, char *line);

xiaotai_err_t at_reply_open(size_t capacity);
void at_reply_close(void);
xiaotai_reply_t at_reply_begin(const xiaotai_at_io_t *io);
at_result_t at_reply_finish(at_result_t result, bool timed_out, uint32_t stream_timeout_ms);
xiaotai_err_t at_reply_handover(const xiaotai_reply_t *reply, const at_long_output_t *output);

/* Only the console executes or releases an accepted hand-over. */
at_result_t at_stream_finish(const xiaotai_at_io_t *io, const at_long_output_t *output,
                             at_result_t result, uint32_t timeout_ms);
void at_stream_fail(const xiaotai_reply_t *reply, xiaotai_err_t error, const char *reason);
void at_stream_fault(const xiaotai_reply_t *reply, xiaotai_at_output_fault_t fault);

xiaotai_err_t at_notify_open(uint16_t bytes);
void at_notify_close(void);
size_t at_notify_queued(void);
/* Returns text length (CRLF appended, not included in return), 0 if empty. */
size_t at_notify_take(char record[XIAOTAI_AT_NOTIFY_MAX + 2], uint32_t *ticket);
void at_notify_delivered(uint32_t ticket, bool delivered);

#ifdef __cplusplus
}
#endif
