// SPDX-License-Identifier: MIT
#pragma once
/* Console engine: reads command lines from one or more byte sources, runs them
 * and frames the replies in the one AT vocabulary (xiaotai/at_command.h).
 *
 * A command is looked up in two tables:
 *   local      registered here (xiaotai_at_local_register) and run on the
 *              console task. They work before the framework has started, when
 *              its start failed and while its owner task is stuck, so a
 *              device that cannot come up still answers. The engine's own AT,
 *              AT+HELP, AT+VERSION, AT+CLI, AT+LOG, AT+MEM, AT+CONSOLE,
 *              AT+RESTART and, in builds whose port lists probes
 *              (xiaotai_port_probes), AT+PORT are local.
 *   framework  registered by engines, modules and applications
 *              (xiaotai_command_register, xiaotai_at_verbs_register) and run
 *              on the owner task. Before the framework has started they answer
 *              `ERROR: INVALID_STATE STARTING`. The engine's own AT+STATUS,
 *              AT+STATE, AT+SET and AT+INTENT are framework commands.
 * The engine's own spellings are declared in its manifest (engine.json,
 * "commands"), one entry per AT+HELP line with the same usage text (bare AT is
 * the syntax itself and has no entry). AT+SET has two: [<name>] reads and
 * <name> <value> writes, registered as two verb tables of one verb without a
 * word each (xiaotai_at_verbs_register), so a console script may allow reading
 * and guard writing. The guarded spellings AT+RESTART, AT+RESTART FORCE,
 * AT+SET <name> <value> and AT+INTENT exist only with their build options
 * (xiaotai/at_config.h), so each product keeps the console it has today.
 *
 * Line input, the same on every source:
 *   - CR or LF ends a line; an empty line is ignored (so CRLF is one line).
 *   - Backspace (8) and delete (127) remove the last character; a tab counts
 *     as a space; leading and trailing spaces are dropped.
 *   - Any other byte outside 0x20..0x7E spoils the line: at its end the reply
 *     is `ERROR: INVALID_ARG BAD_CHARACTER` and nothing runs. A line damaged
 *     on the wire must not execute as a shorter command.
 *   - A line longer than the configured limit is `ERROR: INVALID_SIZE
 *     LINE_TOO_LONG` at its end.
 *   - No echo. One command at a time per console; bytes arriving while a
 *     command runs wait in the source.
 *
 * Output: every line is written as one record ending in CR LF, so log output
 * of other tasks never splits it. A reply goes to the source that sent the
 * command; unsolicited lines go to every source. When a source starts being
 * served it prints `+READY: at=<XIAOTAI_AT_VOCABULARY> source=<name>
 * locked=<0|1>` once.
 *
 * Tasks: the console task (or the task that calls xiaotai_at_service) reads,
 * runs local commands, waits for framework commands, writes, and runs one
 * event-log reader pass per round (xiaotai_eventlog_flush). Every round
 * blocks in a source's read, so there is no round without a source: a
 * console with no source attached creates no task and serves nothing (the
 * event log keeps its own reader). Handlers of
 * framework commands write into a reply buffer on the owner task; the console
 * task sends it after the handler returned. When a reply outgrows the buffer
 * the owner task sends the buffered part itself and is held for that long, so
 * the buffer is sized by configuration for the largest status reply where RAM
 * allows. Function tags as in xiaotai/at_command.h. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/at_command.h"
#include "xiaotai/at_config.h"
#include "xiaotai/framework.h"
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------- sources */

/* A byte stream the console reads commands from and writes replies to: the
 * platform console, a second UART, a debug socket, a test's fake. One more
 * transport is one more xiaotai_at_io_t; nothing else changes. The struct and
 * what self points at have static storage duration. */
typedef struct xiaotai_at_io {
    /* Shown by the ready line and AT+CONSOLE; [a-z0-9_], at most 15 characters. */
    const char *name;
    /* [console] Reads up to capacity bytes, waiting at most timeout_ms for the
     * first one. Returns the count, 0 on timeout or while disconnected. */
    size_t (*read)(void *self, uint8_t *data, size_t capacity, uint32_t timeout_ms);
    /* [console] and, when a reply outgrows its buffer, [owner]; never both at
     * once. Writes all size bytes as one record that other writers of the
     * same transport do not split, and returns when the transport has taken
     * them or its own bound (at most 500 ms per record) has passed.
     * XIAOTAI_OK, XIAOTAI_ERR_TIMEOUT, or XIAOTAI_FAIL when disconnected. */
    xiaotai_err_t (*write)(void *self, const char *data, size_t size);
    /* [console] Optional. Called with true before and false after a long
     * output (a blob or a stream), so the transport can keep the output from
     * starving lower-priority work, for example by lowering the calling
     * task's priority for that time. */
    void (*bulk)(void *self, bool active);
    void *self;
} xiaotai_at_io_t;

/* Most sources one console serves: XIAOTAI_AT_SOURCES_MAX, a build option
 * (xiaotai/at_config.h). */

/* Adds a source. [init], or earlier from the product root (before or after
 * xiaotai_at_open()); not once xiaotai_engine_at_start() has run. The source
 * is read from the next round of the console on; the first source attached
 * to a console opened without any creates the console task then (unless
 * configured XIAOTAI_AT_NO_TASK). XIAOTAI_ERR_INVALID_ARG for a NULL or
 * incomplete io, XIAOTAI_ERR_INVALID_STATE once xiaotai_engine_at_start() has
 * run or when the name is taken, XIAOTAI_ERR_NO_MEM when
 * XIAOTAI_AT_SOURCES_MAX are attached (or the table cannot be allocated); the
 * first error of creating the task, and the source is then not attached.
 * XIAOTAI_ERR_NOT_SUPPORTED if the product omits the AT command tables. */
xiaotai_err_t xiaotai_at_attach(const xiaotai_at_io_t *io);

/* ------------------------------------------------------------- configuration */

/* Pass as task_stack_bytes: create no task; the product calls
 * xiaotai_at_service() from a task it already has (its main task after
 * xiaotai_start() returned), which saves one stack on small chips. If the
 * framework takes over the calling task, an open console with a source uses
 * a 4096-byte reader stack instead, keeping command waits off the owner. */
#define XIAOTAI_AT_NO_TASK UINT32_MAX

/* Every numeric field: 0 selects the default. Buffers come from
 * XIAOTAI_MEM_BULK when the console is opened. */
typedef struct {
    /* Longest input line plus its terminator. Default 768 (a 256-byte text as
     * hexadecimal plus the command), allowed 96..2048. A framework command on
     * a line of XIAOTAI_COMMAND_LINE_MAX (768) characters or more is answered
     * `ERROR: INVALID_SIZE LINE_TOO_LONG`; local commands take the full
     * configured length. */
    uint16_t line_max;
    /* Reply buffer filled on the owner task. Default 8192 with external RAM,
     * 1024 without; at least XIAOTAI_AT_OUT_LINE_MAX + 2. */
    uint16_t reply_buffer_bytes;
    /* Bytes of unsolicited lines that may wait (one ring; a line costs its
     * length plus 2). Default 4096 with external RAM, 512 without; at least
     * 258. A line longer than notify_bytes - 2 is refused with
     * XIAOTAI_ERR_INVALID_SIZE, so a product whose components report longer
     * lines sizes this for them. */
    uint16_t notify_bytes;
    /* Longest wait for a framework command's handler. Default 5000. After it
     * the reply is `ERROR: TIMEOUT HANDLER` and whatever the handler writes
     * later is dropped and counted. */
    uint32_t command_timeout_ms;
    /* Longest a stream or blob may go without completing a line (a stuck
     * producer), not its total time: a screen of 320x480 RGB565 is about
     * 690 KB of text and takes about 60 s at 115200 baud while completing a
     * line every few milliseconds. Default 60000. */
    uint32_t stream_timeout_ms;
    /* Period of xiaotai_mem_sample() on the console task, so the watermarks
     * AT+MEM prints (min_largest=) cover the time between two commands.
     * Default 5000; UINT32_MAX: never. Each sample walks the heaps, which is
     * why it is not done every round. */
    uint32_t mem_sample_ms;
    /* Stack of the console task (XIAOTAI_MEM_BULK). Default 5120 bytes. */
    uint32_t task_stack_bytes;
    /* RTOS priority of the console task. Default 2: below the owner task and
     * every media task, above the event log's reader. */
    uint8_t task_priority;
    /* The command gate is closed at boot: only AT, AT+CLI and the names in
     * `allow` run until `AT+CLI ON`; everything else is `ERROR: INVALID_STATE
     * CLI_DISABLED`. The gate is never stored: every boot starts from this. */
    bool start_locked;
    /* Print each new event-log entry as a `+LOG:` line from boot (as after
     * `AT+LOG FOLLOW 1`). */
    bool follow_log;
    /* Do not attach the platform console (the port's console) when the
     * console is opened; only sources given to xiaotai_at_attach() are served. */
    bool no_platform_console;
    /* A product with no AT reader or programmatic AT consumers may omit
     * both AT tables. Requires no_platform_console; explicit sources need
     * this false. Framework commands and diagnostics are independent. */
    bool omit_command_tables;
    /* Legacy restart policies. With both false, normal restart uses the
     * framework while running. FORCE, if built, remains explicitly direct. */
    bool restart_direct_when_running;
    bool restart_idle_only;
    uint16_t restart_delay_ms; /* Direct restart after OK; 0: 200 ms. */
    /* Command names (first word, for example "AT+VERSION") that stay usable
     * while the gate is closed; NULL-terminated, NULL: none. Static storage. */
    const char *const *allow;
    /* Optional exact allowlists for the guarded write/action forms, not
     * their read forms. Static, NULL-terminated; NULL leaves the module
     * registry rules in charge. An empty list permits no actions. */
    const char *const *setting_write_allow;
    const char *const *intent_allow;
} xiaotai_at_config_t;

/* [glue] Before xiaotai_at_open(); config is copied (the allow list is not).
 * NULL restores every default.
 * XIAOTAI_ERR_INVALID_ARG for a value outside its range,
 * XIAOTAI_ERR_INVALID_STATE once the console is open. */
xiaotai_err_t xiaotai_at_configure(const xiaotai_at_config_t *config);

/* ------------------------------------------------------------ local commands */

/* Most local commands, the engine's own included: XIAOTAI_AT_LOCAL_MAX, a
 * build option (xiaotai/at_config.h). */

/* Registers a command that runs on the console task, not on the owner task:
 * for facts that must be readable when the framework is not running (the
 * engine's own commands, a product root's rescue command). Board and port
 * diagnostics are diagnostics sections, not commands. The handler has the
 * framework's signature and may use everything in xiaotai/at_command.h; it
 * must be safe against every other task by itself (no owner-task state) and
 * may block for up to the command timeout. name and help have static storage;
 * help is the argument synopsis AT+HELP prints after the name. A local
 * command hides a framework command of the same name.
 * [glue, init] (in the glue before or after xiaotai_at_open()); not once
 * xiaotai_engine_at_start() has run.
 * XIAOTAI_ERR_INVALID_ARG for a NULL name or handler or a name that does not
 * start with "AT", XIAOTAI_ERR_INVALID_STATE for a duplicate or once
 * xiaotai_engine_at_start() has run, XIAOTAI_ERR_NO_MEM when the table is full
 * (or cannot be allocated), XIAOTAI_ERR_NOT_SUPPORTED if the product omits
 * the AT command tables.
 * The engine's own local commands that take verbs (AT+CLI, AT+LOG, AT+RESTART,
 * AT+PORT) print one AT+HELP line per verb, as the verbs of a framework command
 * do; a command registered here prints one line with its help. */
xiaotai_err_t xiaotai_at_local_register(const char *name, const char *help, xiaotai_command_fn_t handler);

/* ------------------------------------------------------------------- restart */

/* AT+RESTART asks for an orderly restart. While the framework runs, the
 * console posts XIAOTAI_INTENT_SYSTEM_RESTART (xiaotai/framework.h) with the
 * reason "console" and answers OK; the framework always handles that intent:
 * it quiesces the system, waits for the restart holds and restarts (a
 * refusal, the system being quiesced by another holder, is in the "framework"
 * diagnostic section). There is no "nobody handles it" case and no BUSY
 * answer. AT+RESTART FORCE, and AT+RESTART while the framework is not
 * running, restart 200 ms after OK without asking. Each exists only with its
 * build option (XIAOTAI_AT_RESTART_COMMAND, XIAOTAI_AT_RESTART_FORCE), and
 * AT+INTENT with XIAOTAI_AT_INTENT_COMMAND. AT+INTENT posts with
 * xiaotai_intent_post_external() and answers `ERROR: NOT_ALLOWED` for
 * XIAOTAI_ERR_NOT_ALLOWED (an intent registered with XIAOTAI_INTENT_LOCAL). */
#define XIAOTAI_AT_INTENT_RESTART XIAOTAI_INTENT_SYSTEM_RESTART

/* ------------------------------------------------------------------- service */

/* For XIAOTAI_AT_NO_TASK: serves the console on the calling task for up to
 * timeout_ms (XIAOTAI_WAIT_FOREVER never returns): reads the sources, runs
 * commands, prints unsolicited lines and runs one event-log reader pass per
 * round. Always the same task, never the owner task: a framework command's
 * handler runs on the owner task while the console waits for it, so the
 * owner task (and anything it drives, an engine's start() included) can
 * never serve the console.
 * The console engine is part of every product, so a main task shared by
 * every product calls `xiaotai_start(...); xiaotai_at_service(
 * XIAOTAI_WAIT_FOREVER);` without knowing the configuration: where the
 * console has its own task, or none was opened, the call returns at once.
 * XIAOTAI_OK when the time is up; XIAOTAI_ERR_INVALID_STATE at once before
 * the console was opened, when it has its own task, when no source is
 * attached (nothing to wait on), or on the owner task. */
xiaotai_err_t xiaotai_at_service(uint32_t timeout_ms);

/* --------------------------------------------------------------- diagnostics */

typedef struct {
    uint32_t lines;              /* complete input lines, the rejected ones included */
    uint32_t commands_ok;        /* final line OK */
    uint32_t commands_failed;    /* final line ERROR */
    uint32_t line_overflows;     /* LINE_TOO_LONG */
    uint32_t bad_characters;     /* BAD_CHARACTER */
    uint32_t gate_rejections;    /* CLI_DISABLED */
    uint32_t handler_timeouts;   /* TIMEOUT HANDLER */
    uint32_t late_dropped;       /* lines written by a handler after its timeout */
    uint32_t write_errors;       /* records a source did not take */
    uint32_t owner_flushes;      /* times the owner task had to send a full reply buffer */
    uint32_t streams;            /* blobs and streams completed */
    uint32_t notify_sent;
    uint32_t notify_dropped;     /* queue full */
} xiaotai_at_stats_t;

/* [any] */
void xiaotai_at_stats(xiaotai_at_stats_t *stats);

/* ----------------------------------------------------------------- lifecycle */

/* [glue, init] The one start [glue] allows (xiaotai/framework.h), after the
 * event log. Opens the console: allocates the buffers,
 * attaches the platform console unless configured otherwise, registers the
 * engine's local commands and starts reading (its own task, or
 * xiaotai_at_service). Local commands answer from here on. The product glue
 * (xiaotai_selected_configure, xiaotai/selection.h) calls xiaotai_at_configure
 * and then this before the board and the framework start, so the console is
 * up before anything that can fail; later calls return XIAOTAI_OK and change
 * nothing. With no source attached it succeeds, creates no task and the
 * console stays silent until xiaotai_at_attach() adds one.
 * XIAOTAI_ERR_NO_MEM; the first error of creating the task. */
xiaotai_err_t xiaotai_at_open(void);

/* Engine lifecycle (called by the framework through the generated tables).
 * init: xiaotai_at_open() unless already done; registers the engine's
 * framework commands (AT+STATUS, AT+STATE, AT+SET, AT+INTENT), the
 * diagnostics providers "state", "mem", "board" and "port", and its event-log
 * sink. "board" prints what xiaotai_hal_report() (xiaotai/hal.h) writes and
 * "port" what xiaotai_port_report() (xiaotai/port/sys.h) writes: the board,
 * its parts and the port sit below every engine and register nothing
 * themselves. AT+STATUS gives every provider line that does not start with
 * '+' the prefix "+<SECTION>: " (the section name upper case, '.' as '-',
 * '_' kept: "app.door" gives +APP-DOOR:, "wake_engine" +WAKE_ENGINE:).
 * Port probes (xiaotai_port_probe) are the local command AT+PORT PROBE
 * [<name>], registered at open only when the port lists any.
 * start: framework commands run from here on. */
xiaotai_err_t xiaotai_engine_at_init(void);
void xiaotai_engine_at_start(void);

/* Host tests: stop the console task, free the buffers, detach the sources and
 * forget the configuration, the local commands and the verb tables. */
void xiaotai_at_reset(void);

#ifdef __cplusplus
}
#endif
