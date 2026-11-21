// SPDX-License-Identifier: MIT
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "internal.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/port/os.h"

typedef struct {
    const xiaotai_at_io_t *io;
    char *line;
    uint16_t used;
    bool ready, too_long, damaged;
} source_t;

typedef struct {
    const char *name, *help;
    xiaotai_command_fn_t run;
    const xiaotai_at_verb_t *verbs;
    size_t count;
} local_t;

static source_t *s_sources;
static local_t *s_locals;
static atomic_size_t s_source_count, s_local_count;
static atomic_bool s_open, s_frozen, s_locked, s_stopping, s_servicing, s_has_task;
static bool s_owner_wait_timed_out;
static xiaotai_task_t s_task;
static xiaotai_signal_t s_task_stopped;
static xiaotai_at_config_t s_config;
static int64_t s_sampled_us;
static _Atomic uint32_t s_counts[at_counter_count];

enum { AT_CALLER_READER_STACK_BYTES = 4096 };

void at_count(at_counter_t counter)
{
    atomic_fetch_add_explicit(&s_counts[counter], 1, memory_order_relaxed);
}

void xiaotai_at_stats(xiaotai_at_stats_t *stats)
{
    if (!stats) return;
#define AT_READ_COUNT(name) stats->name = atomic_load_explicit(&s_counts[at_count_##name], memory_order_relaxed);
    AT_COUNTERS(AT_READ_COUNT)
#undef AT_READ_COUNT
}

bool at_console_has_source(void)
{
    return atomic_load(&s_open) && atomic_load(&s_source_count) != 0;
}

bool xiaotai_at_active(void)
{
    return at_console_has_source() && !atomic_load(&s_locked);
}

bool at_write_record(const xiaotai_at_io_t *io, const char *record, size_t bytes)
{
    if (io->write(io->self, record, bytes) == XIAOTAI_OK) return true;
    at_count(at_count_write_errors);
    return false;
}

static void finish(const xiaotai_at_io_t *io, at_result_t result)
{
    char line[64], scratch[12];
    static const char output_error[] = "ERROR: FAIL OUTPUT\r\n";
    if (result.error) {
        const char *name = xiaotai_at_error_name(result.error, scratch, sizeof(scratch));
        (void)snprintf(line, sizeof(line), "ERROR: %s%s%s\r\n", name,
                       result.reason ? " " : "", result.reason ? result.reason : "");
    } else {
        memcpy(line, "OK\r\n", sizeof("OK\r\n"));
    }
    if (!at_write_record(io, line, strlen(line))) {
        (void)at_write_record(io, output_error, sizeof(output_error) - 1);
        result.error = XIAOTAI_FAIL;
    }
    at_count(result.error ? at_count_commands_failed : at_count_commands_ok);
#if XIAOTAI_AT_RESTART_COMMAND
    xiaotai_at_guarded_restart_reply(!result.error);
#endif
}

static xiaotai_err_t ping(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argv;
    return argc == 1 ? XIAOTAI_OK : xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_ARG, "ARGUMENT_COUNT");
}

static const char *const gate_words[] = {"ON", "OFF", NULL};

static xiaotai_err_t gate(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    if (argc == 1) {
        xiaotai_at_printf(reply, "+CLI: enabled=%d", !atomic_load(&s_locked));
        return XIAOTAI_OK;
    }
    const int word = xiaotai_at_keyword(argv[1], gate_words);
    if (word < 0) {
        return xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_ARG, "UNKNOWN_VERB");
    }
    if (argc != 2) return xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_ARG, "ARGUMENT_COUNT");
    atomic_store(&s_locked, word != 0);
    return XIAOTAI_OK;
}

static xiaotai_err_t help(int argc, const char *const *argv, const xiaotai_reply_t *reply);

static const local_t s_core[] = {
    {.name = "AT", .run = ping}, {.name = "AT+CLI", .run = gate}, {.name = "AT+HELP", .run = help},
};
#define CORE_COUNT (sizeof(s_core) / sizeof(s_core[0]))

static const local_t *find_local(const char *name)
{
    for (size_t i = 0; i < CORE_COUNT; ++i) {
        if (strcmp(s_core[i].name, name) == 0) return &s_core[i];
    }
    const size_t count = atomic_load(&s_local_count);
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(s_locals[i].name, name) == 0) return &s_locals[i];
    }
    return NULL;
}

static bool allowed(const char *name)
{
    if (!atomic_load(&s_locked) || strcmp(name, "AT") == 0 || strcmp(name, "AT+CLI") == 0) return true;
    for (const char *const *word = s_config.allow; word && *word; ++word) {
        if (strcmp(name, *word) == 0) return true;
    }
    return false;
}

bool at_setting_write_allowed(const char *name) { return at_name_allowed(name, s_config.setting_write_allow); }
bool at_intent_allowed(const char *name) { return at_name_allowed(name, s_config.intent_allow); }

static void help_line(const xiaotai_reply_t *reply, const char *name, const char *word, const char *usage)
{
    xiaotai_at_printf(reply, "+HELP: %s%s%s%s%s", name, word ? " " : "", word ? word : "",
                      usage && *usage ? " " : "", usage ? usage : "");
}

typedef struct {
    const char *command;
    const xiaotai_reply_t *reply;
} help_t;

static void help_verb(const xiaotai_at_verb_t *verb, void *opaque)
{
    const help_t *ctx = opaque;
    help_line(ctx->reply, ctx->command, verb->word, verb->usage);
}

static xiaotai_err_t help(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argv;
    if (argc != 1) return xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_ARG, "ARGUMENT_COUNT");
    for (size_t i = 0; i < CORE_COUNT; ++i) {
        const local_t *command = &s_core[i];
        if (!allowed(command->name)) continue;
        help_line(reply, command->name, NULL, command->help);
        if (command->run == gate) {
            for (const char *const *word = gate_words; *word; ++word) help_line(reply, command->name, *word, NULL);
        }
    }
    const size_t locals = atomic_load(&s_local_count);
    for (size_t i = 0; i < locals; ++i) {
        const local_t *command = &s_locals[i];
        if (!allowed(command->name)) continue;
        if (!command->count) help_line(reply, command->name, NULL, command->help);
        const help_t ctx = {.command = command->name, .reply = reply};
        for (size_t j = 0; j < command->count; ++j)
            help_line(ctx.reply, ctx.command, command->verbs[j].word, command->verbs[j].usage);
    }

    /* Local rescue commands work during startup. The owner's tables are
     * immutable only after init has published them via engine start. */
    if (!atomic_load(&s_frozen)) return XIAOTAI_OK;
    const size_t commands = xiaotai_command_count();
    for (size_t i = 0; i < commands; ++i) {
        const char *name = NULL, *usage = NULL;
        xiaotai_command_at(i, &name, &usage);
        if (!name || !allowed(name) || find_local(name)) continue;
        help_t ctx = {.command = name, .reply = reply};
        if (!at_verbs_visit_distinct(name, help_verb, &ctx)) help_line(reply, name, NULL, usage);
    }
    return XIAOTAI_OK;
}

static bool registered(const char *line, size_t name_len)
{
    const size_t count = xiaotai_command_count();
    for (size_t i = 0; i < count; ++i) {
        const char *name = NULL;
        xiaotai_command_at(i, &name, NULL);
        if (name && strlen(name) == name_len && memcmp(name, line, name_len) == 0) return true;
    }
    return false;
}

/* Temporarily terminate each word for a local handler. For the framework
 * path restore the separators in place; it owns its copy and own splitting. */
static int words(char *line, const char **argv, char **ends)
{
    int count = 0;
    for (char *p = line; *p;) {
        while (*p == ' ') ++p;
        if (!*p) break;
        if (count == XIAOTAI_AT_WORDS_MAX) return -1;
        argv[count] = p;
        while (*p && *p != ' ') ++p;
        ends[count++] = *p ? p : NULL;
        if (*p) *p++ = '\0';
    }
    return count;
}

static bool elapsed(int64_t since_us, uint32_t ms)
{
    return xiaotai_time_us() - since_us >= (int64_t)ms * 1000;
}

static at_result_t run(const xiaotai_at_io_t *io, char *line, int argc, const char **argv, char **ends)
{
    if (!allowed(argv[0])) {
        at_count(at_count_gate_rejections);
        return (at_result_t){XIAOTAI_ERR_INVALID_STATE, "CLI_DISABLED"};
    }
    const local_t *local = find_local(argv[0]);
    if (!local && !atomic_load(&s_frozen)) return (at_result_t){XIAOTAI_ERR_INVALID_STATE, "STARTING"};
    const xiaotai_command_fn_t fast = local ? NULL : at_verbs_console_match(argc, argv);
    const bool uses_owner = !local && !fast;
    const size_t name_len = strlen(argv[0]);
    if (!local) {
        for (int i = 0; i < argc; ++i) if (ends[i]) *ends[i] = ' ';
        if (strlen(line) >= XIAOTAI_COMMAND_LINE_MAX) {
            at_count(at_count_line_overflows);
            return (at_result_t){XIAOTAI_ERR_INVALID_SIZE, "LINE_TOO_LONG"};
        }
        if (fast) for (int i = 0; i < argc; ++i) if (ends[i]) *ends[i] = '\0';
    }
    const xiaotai_reply_t reply = at_reply_begin(io);
    const uint32_t timeout = s_config.command_timeout_ms;
    const int64_t since_us = xiaotai_time_us();
    at_result_t result = {.error = local ? (local->run ? local->run(argc, argv, &reply) :
                                     xiaotai_at_dispatch(local->verbs, local->count, argc, argv, &reply)) :
                                  fast ? fast(1, argv + 2, &reply) : xiaotai_command_run(line, &reply, timeout)};
    if (fast && result.error == XIAOTAI_AT_PASS) result.error = XIAOTAI_ERR_INVALID_STATE;
    const bool expired = timeout != XIAOTAI_WAIT_FOREVER && elapsed(since_us, timeout);
    const bool timed_out = expired && (!uses_owner || result.error == XIAOTAI_ERR_TIMEOUT);
    /* command_run has one result code for both dispatch and the handler.
     * A BUSY without a preceding AT wait timeout remains a handler error. */
    const bool previous_busy = uses_owner && s_owner_wait_timed_out && expired && result.error == XIAOTAI_ERR_BUSY;
    if (uses_owner) s_owner_wait_timed_out = timed_out || previous_busy;
    if (timed_out) {
        at_count(at_count_handler_timeouts);
        result = (at_result_t){XIAOTAI_ERR_TIMEOUT, "HANDLER"};
    } else if (uses_owner) {
        if (result.error == XIAOTAI_ERR_NOT_FOUND && !registered(line, name_len)) {
            result.reason = "UNKNOWN_COMMAND";
        } else if (previous_busy) {
            result.reason = "PREVIOUS_COMMAND";
        }
    }
    return at_reply_finish(result, timed_out, s_config.stream_timeout_ms);
}

static void announce_source(source_t *source)
{
    if (source->ready) return;
    char line[64];
    const int n = snprintf(line, sizeof(line), "+READY: at=%d source=%s locked=%d\r\n",
                           XIAOTAI_AT_VOCABULARY, source->io->name, atomic_load(&s_locked));
    (void)at_write_record(source->io, line, (size_t)n);
    source->ready = true;
}

static void drain_notices(void)
{
    /* Finish the pending batch without starving input if another task keeps
     * reporting. Only this task removes lines, so the batch remains FIFO. */
    size_t remaining = at_notify_queued();
    while (remaining && !atomic_load(&s_stopping)) {
        char record[XIAOTAI_AT_NOTIFY_MAX + 2];
        uint32_t ticket;
        const size_t length = at_notify_take(record, &ticket);
        if (!length) break;
        const size_t count = atomic_load(&s_source_count);
        bool sent = false, complete = count != 0;
        for (size_t i = 0; i < count; ++i) {
            announce_source(&s_sources[i]);
            const bool delivered = at_write_record(s_sources[i].io, record, length + 2);
            sent |= delivered;
            complete &= delivered;
        }
        at_notify_delivered(ticket, complete);
        if (sent) at_count(at_count_notify_sent);
        remaining -= length + 2;
    }
}

static void end_line(source_t *source)
{
    at_result_t result = {0};
    if (source->damaged) {
        result = (at_result_t){XIAOTAI_ERR_INVALID_ARG, "BAD_CHARACTER"};
        at_count(at_count_bad_characters);
    } else if (source->too_long) {
        result = (at_result_t){XIAOTAI_ERR_INVALID_SIZE, "LINE_TOO_LONG"};
        at_count(at_count_line_overflows);
    } else {
        char *line = source->line;
        while (line < source->line + source->used && *line == ' ') ++line;
        while (source->used && source->line[source->used - 1] == ' ') --source->used;
        source->line[source->used] = '\0';
        if (line >= source->line + source->used) {
            source->used = 0;
            return;
        }
        const char *argv[XIAOTAI_AT_WORDS_MAX];
        char *ends[XIAOTAI_AT_WORDS_MAX];
        const int argc = words(line, argv, ends);
        result = argc < 0 ? (at_result_t){XIAOTAI_ERR_INVALID_ARG, "TOO_MANY_WORDS"}
                          : run(source->io, line, argc, argv, ends);
    }
    at_count(at_count_lines);
    finish(source->io, result);
    drain_notices();
    source->used = 0;
    source->too_long = source->damaged = false;
}

static void input(source_t *source, uint8_t byte)
{
    if (byte == '\r' || byte == '\n') {
        end_line(source);
    } else if (byte == 8 || byte == 127) {
        if (source->used) --source->used;
    } else if (byte != '\t' && (byte < 32 || byte > 126)) {
        source->damaged = true;
    } else if (source->used == s_config.line_max - 1) {
        source->too_long = true;
    } else {
        source->line[source->used++] = byte == '\t' ? ' ' : (char)byte;
    }
}

static void round_sources(int64_t since_us, uint32_t duration)
{
    const size_t count = atomic_load(&s_source_count);
    const int64_t round_start_us = xiaotai_time_us();
    bool any_read = false;
    drain_notices();
    for (size_t i = 0; i < count && !atomic_load(&s_stopping); ++i) {
        source_t *src = &s_sources[i];
        announce_source(src);
        uint32_t timeout = count == 1 ? 50 : 20;
        if (duration != XIAOTAI_WAIT_FOREVER) {
            const int64_t remaining = (int64_t)duration * 1000 - (xiaotai_time_us() - since_us);
            if (remaining <= 0) break;
            const uint32_t ms = (uint32_t)((remaining + 999) / 1000);
            if (ms < timeout) timeout = ms;
        }
        uint8_t bytes[64];
        size_t got = src->io->read(src->io->self, bytes, sizeof(bytes), timeout);
        if (got > sizeof(bytes)) got = sizeof(bytes);
        any_read |= got != 0;
        for (size_t j = 0; j < got && !atomic_load(&s_stopping); ++j) input(src, bytes[j]);
    }
    xiaotai_eventlog_flush();
    if (s_config.mem_sample_ms != UINT32_MAX && elapsed(s_sampled_us, s_config.mem_sample_ms)) {
        xiaotai_mem_sample();
        s_sampled_us = xiaotai_time_us();
    }
    /* A disconnected source may return zero immediately rather than block. */
    if (!any_read && !elapsed(round_start_us, 1)) xiaotai_delay_ms(1);
}

static void reader(void *ctx)
{
    (void)ctx;
    /* Source entry and open state are published only after creation succeeds. */
    (void)xiaotai_task_wait_notify(XIAOTAI_WAIT_FOREVER);
    while (!atomic_load(&s_stopping)) round_sources(0, XIAOTAI_WAIT_FOREVER);
    xiaotai_signal_give(&s_task_stopped);
    xiaotai_task_exit();
}

static xiaotai_err_t start_task(void)
{
    uint32_t stack_bytes = s_config.task_stack_bytes;
    if (stack_bytes == XIAOTAI_AT_NO_TASK) {
        if (!xiaotai_framework_owner_on_caller()) return XIAOTAI_OK;
        stack_bytes = AT_CALLER_READER_STACK_BYTES;
    }
    xiaotai_err_t err = xiaotai_signal_init(&s_task_stopped);
    if (err != XIAOTAI_OK) return err;
    const xiaotai_task_config_t task = {
        .name = "at", .entry = reader, .stack_bytes = stack_bytes,
        .stack = XIAOTAI_MEM_BULK, .priority = s_config.task_priority, .core = XIAOTAI_CORE_ANY,
    };
    err = xiaotai_task_create(&task, &s_task);
    if (err != XIAOTAI_OK) xiaotai_signal_deinit(&s_task_stopped);
    else atomic_store(&s_has_task, true);
    return err;
}

xiaotai_err_t at_console_ensure_off_owner_reader(void)
{
    if (!xiaotai_framework_owner_on_caller() || !at_console_has_source() || atomic_load(&s_has_task) ||
        s_config.task_stack_bytes != XIAOTAI_AT_NO_TASK)
        return XIAOTAI_OK;
    /* Replies and long streams must still have an executor off the owner. */
    if (atomic_exchange(&s_servicing, true)) return XIAOTAI_ERR_INVALID_STATE;
    const xiaotai_err_t err = start_task();
    if (err == XIAOTAI_OK) xiaotai_task_notify(&s_task);
    atomic_store(&s_servicing, false);
    return err;
}

static xiaotai_err_t start_service(void)
{
    xiaotai_err_t err = at_reply_open(s_config.reply_buffer_bytes);
    if (err == XIAOTAI_OK) err = at_notify_open(s_config.notify_bytes);
    if (err == XIAOTAI_OK) err = start_task();
    if (err != XIAOTAI_OK) {
        at_notify_close();
        at_reply_close();
    }
    return err;
}

static bool source_name_valid(const char *name)
{
    if (!name || !*name) return false;
    for (size_t i = 0; name[i]; ++i) {
        const char c = name[i];
        if (i == 15 || !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

xiaotai_err_t xiaotai_at_attach(const xiaotai_at_io_t *io)
{
    if (!io || !io->read || !io->write || !source_name_valid(io->name)) return XIAOTAI_ERR_INVALID_ARG;
    if (atomic_load(&s_frozen)) return XIAOTAI_ERR_INVALID_STATE;
    if (s_config.omit_command_tables) return XIAOTAI_ERR_NOT_SUPPORTED;
    const size_t count = atomic_load(&s_source_count);
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(s_sources[i].io->name, io->name) == 0) return XIAOTAI_ERR_INVALID_STATE;
    }
    if (count == XIAOTAI_AT_SOURCES_MAX) return XIAOTAI_ERR_NO_MEM;
    if (!s_sources) s_sources = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, XIAOTAI_AT_SOURCES_MAX, sizeof(*s_sources));
    if (!s_sources) return XIAOTAI_ERR_NO_MEM;
    source_t *source = &s_sources[count];
    *source = (source_t){.io = io};
    if (atomic_load(&s_open)) {
        source->line = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, s_config.line_max);
        if (!source->line) return XIAOTAI_ERR_NO_MEM;
        if (!count) {
            const xiaotai_err_t err = start_service();
            if (err != XIAOTAI_OK) {
                xiaotai_mem_free(source->line);
                *source = (source_t){0};
                return err;
            }
        }
    }
    atomic_store(&s_source_count, count + 1);
    if (!count && atomic_load(&s_has_task)) xiaotai_task_notify(&s_task);
    return XIAOTAI_OK;
}

static xiaotai_err_t register_local(local_t command)
{
    const char *name = command.name;
    const char *help = command.help;
    if (!name || strncmp(name, "AT", 2)) return XIAOTAI_ERR_INVALID_ARG;
    const char *query = strchr(name, '?');
    if (query) {
        if (strncmp(name, "AT+", 3) || query == name + 3 || query[1] || (help && *help))
            return XIAOTAI_ERR_INVALID_ARG;
        for (const char *p = name + 3; p != query; ++p) {
            if (!((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_'))
                return XIAOTAI_ERR_INVALID_ARG;
        }
    }
    if (s_config.omit_command_tables) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (atomic_load(&s_frozen) || find_local(name)) return XIAOTAI_ERR_INVALID_STATE;
    const size_t count = atomic_load(&s_local_count);
    if (count + CORE_COUNT == XIAOTAI_AT_LOCAL_MAX) return XIAOTAI_ERR_NO_MEM;
    /* The core commands already have storage in s_core. */
    if (!s_locals) s_locals = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, (XIAOTAI_AT_LOCAL_MAX - CORE_COUNT) * sizeof(*s_locals));
    if (!s_locals) return XIAOTAI_ERR_NO_MEM;
    s_locals[count] = command;
    atomic_store(&s_local_count, count + 1);
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_at_local_register(const char *name, const char *help, xiaotai_command_fn_t handler)
{
    if (!handler) return XIAOTAI_ERR_INVALID_ARG;
    return register_local((local_t){.name = name, .help = help, .run = handler});
}

xiaotai_err_t at_local_verbs_register(const char *name, const xiaotai_at_verb_t *verbs, size_t count)
{
    return register_local((local_t){.name = name, .verbs = verbs, .count = count});
}

void at_console_describe_sources(const xiaotai_reply_t *reply)
{
    const size_t count = atomic_load(&s_source_count);
    for (size_t i = 0; i < count; ++i) at_describe_source(s_sources[i].io, reply);
}

xiaotai_err_t xiaotai_at_configure(const xiaotai_at_config_t *config)
{
    if (atomic_load(&s_open)) return XIAOTAI_ERR_INVALID_STATE;
    if (config && config->omit_command_tables) {
        if (!config->no_platform_console || config->follow_log) return XIAOTAI_ERR_INVALID_ARG;
        if (atomic_load(&s_source_count) || atomic_load(&s_local_count)) return XIAOTAI_ERR_INVALID_STATE;
    }
    if (config &&
        ((config->line_max && (config->line_max < 96 || config->line_max > 2048)) ||
         (config->reply_buffer_bytes && config->reply_buffer_bytes < XIAOTAI_AT_OUT_LINE_MAX + 2) ||
         (config->notify_bytes && config->notify_bytes < 258))) return XIAOTAI_ERR_INVALID_ARG;
    s_config = config ? *config : (xiaotai_at_config_t){0};
    return XIAOTAI_OK;
}

bool at_command_tables_omitted(void) { return s_config.omit_command_tables; }

xiaotai_err_t xiaotai_at_open(void)
{
    if (atomic_load(&s_open)) return XIAOTAI_OK;
    if (s_config.omit_command_tables) {
        atomic_store(&s_open, true);
        return XIAOTAI_OK;
    }
    if (!s_config.line_max) s_config.line_max = 768;
    if (!s_config.reply_buffer_bytes) s_config.reply_buffer_bytes = xiaotai_mem_has_external() ? 8192 : 1024;
    if (!s_config.notify_bytes) s_config.notify_bytes = xiaotai_mem_has_external() ? 4096 : 512;
    if (!s_config.command_timeout_ms) s_config.command_timeout_ms = 5000;
    if (!s_config.stream_timeout_ms) s_config.stream_timeout_ms = 60000;
    if (!s_config.mem_sample_ms) s_config.mem_sample_ms = 5000;
    if (!s_config.task_stack_bytes) s_config.task_stack_bytes = 5120;
    if (!s_config.task_priority) s_config.task_priority = 2;
    atomic_store(&s_locked, s_config.start_locked);
    s_sampled_us = xiaotai_time_us();

    const size_t custom_locals = atomic_load(&s_local_count);
    const size_t custom_sources = atomic_load(&s_source_count);
    xiaotai_err_t err = at_local_init(&s_config);
    if (err == XIAOTAI_OK && s_config.follow_log) err = at_log_attach();
    if (err == XIAOTAI_OK && !s_config.no_platform_console) {
        err = at_platform_open();
        if (err == XIAOTAI_OK) err = xiaotai_at_attach(at_platform_source());
        else if (err == XIAOTAI_ERR_NOT_SUPPORTED) err = XIAOTAI_OK;
    }

    const size_t count = atomic_load(&s_source_count);
    if (count && err == XIAOTAI_OK) {
        for (size_t i = 0; i < count; ++i) {
            s_sources[i].line = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, s_config.line_max);
            if (!s_sources[i].line) {
                err = XIAOTAI_ERR_NO_MEM;
                break;
            }
        }
        if (err == XIAOTAI_OK) err = start_service();
        if (err != XIAOTAI_OK) {
            for (size_t i = 0; i < count; ++i) {
                xiaotai_mem_free(s_sources[i].line);
                s_sources[i].line = NULL;
            }
        }
    }
    if (err != XIAOTAI_OK) {
        atomic_store(&s_source_count, custom_sources);
        atomic_store(&s_local_count, custom_locals);
        return err;
    }
    atomic_store(&s_open, true);
    if (atomic_load(&s_has_task)) xiaotai_task_notify(&s_task);
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_at_service(uint32_t timeout_ms)
{
    if (xiaotai_on_owner() || atomic_exchange(&s_servicing, true)) return XIAOTAI_ERR_INVALID_STATE;
    if (!at_console_has_source() || atomic_load(&s_has_task) || s_config.task_stack_bytes != XIAOTAI_AT_NO_TASK) {
        atomic_store(&s_servicing, false);
        return XIAOTAI_ERR_INVALID_STATE;
    }
    const int64_t since_us = xiaotai_time_us();
    while (!atomic_load(&s_stopping) &&
           (timeout_ms == XIAOTAI_WAIT_FOREVER || !elapsed(since_us, timeout_ms))) {
        round_sources(since_us, timeout_ms);
    }
    atomic_store(&s_servicing, false);
    return XIAOTAI_OK;
}

void xiaotai_engine_at_start(void) { atomic_store(&s_frozen, true); }

void xiaotai_at_reset(void)
{
    atomic_store(&s_stopping, true);
    atomic_store(&s_open, false);
    if (atomic_load(&s_has_task)) {
        (void)xiaotai_signal_take(&s_task_stopped, XIAOTAI_WAIT_FOREVER);
        xiaotai_signal_deinit(&s_task_stopped);
        s_task = (xiaotai_task_t){0};
        atomic_store(&s_has_task, false);
    }
    while (atomic_load(&s_servicing)) xiaotai_delay_ms(1);
    at_log_stop();
    at_notify_close();
    at_reply_close();
    for (size_t i = 0; i < atomic_load(&s_source_count); ++i) xiaotai_mem_free(s_sources[i].line);
    xiaotai_mem_free(s_sources);
    xiaotai_mem_free(s_locals);
    s_sources = NULL;
    s_locals = NULL;
    s_config = (xiaotai_at_config_t){0};
    at_verbs_reset();
    atomic_store(&s_source_count, 0);
    atomic_store(&s_local_count, 0);
    atomic_store(&s_frozen, false);
    atomic_store(&s_locked, false);
    s_owner_wait_timed_out = false;
    for (size_t i = 0; i < at_counter_count; ++i) atomic_store(&s_counts[i], 0);
    atomic_store(&s_stopping, false);
}
