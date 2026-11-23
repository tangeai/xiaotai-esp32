// SPDX-License-Identifier: MIT
/* Command (AT) and diagnostics registries. A command line from the console task
 * is split into words and run on the owner task; one command runs at a time. A
 * caller that times out leaves the job to finish; the next caller waits for it.
 * The registries and the job live in one BULK block, allocated with the first
 * registration (the framework's own diagnostic section) and kept. */
#include <string.h>
#include "internal.h"

typedef struct {
    const char *name;
    const char *help;
    xiaotai_command_fn_t handler;
    xiaotai_command_scope_t scope;
} command_t;

typedef struct {
    const char *name;
    xiaotai_diag_fn_t report;
} diagnostic_t;

typedef struct {
    char line[XIAOTAI_COMMAND_LINE_MAX];
    const char *argv[XIAOTAI_COMMAND_ARGS_MAX];
    int argc;
    const command_t *command;
    xiaotai_reply_t reply;
    xiaotai_err_t result;
} job_t;

typedef struct {
    command_t commands[XIAOTAI_FRAMEWORK_MAX_COMMANDS];
    size_t command_count;
    diagnostic_t diagnostics[XIAOTAI_FRAMEWORK_MAX_DIAGNOSTICS];
    size_t diagnostic_count;
    job_t job;
} registry_t;

static registry_t *s_registry;
static xiaotai_signal_t s_job_done;
static xiaotai_mutex_t s_job_lock;   /* one caller at a time */
static bool s_job_running;          /* caller lock: true until one caller consumes completion */
static bool s_job_ready;
static uint32_t s_registration_generation = 1;

void fw_commands_reset(void)
{
    if (s_job_ready) {
        xiaotai_signal_deinit(&s_job_done);
        xiaotai_mutex_deinit(&s_job_lock);
    }
    s_job_ready = false;
    xiaotai_mem_free(s_registry);
    s_registry = NULL;
    s_job_running = false;
    if (++s_registration_generation == 0) ++s_registration_generation;
}

static xiaotai_err_t prepare(void)
{
    if (s_job_ready) return XIAOTAI_OK;
    if (!s_registry) s_registry = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*s_registry));
    if (!s_registry) return XIAOTAI_ERR_NO_MEM;
    if (xiaotai_signal_init(&s_job_done) != XIAOTAI_OK) return XIAOTAI_FAIL;
    if (xiaotai_mutex_init(&s_job_lock) != XIAOTAI_OK) {
        xiaotai_signal_deinit(&s_job_done);
        return XIAOTAI_FAIL;
    }
    s_job_ready = true;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_command_register(const char *name, const char *help, xiaotai_command_fn_t handler)
{
    if (!name || !handler) return XIAOTAI_ERR_INVALID_ARG;
    xiaotai_command_scope_t scope;
    xiaotai_err_t err = xiaotai_command_scope_capture(&scope);
    if (err == XIAOTAI_OK) err = prepare();
    if (err != XIAOTAI_OK) return err;
    for (size_t i = 0; i < s_registry->command_count; ++i) {
        if (strcmp(s_registry->commands[i].name, name) == 0) return XIAOTAI_ERR_INVALID_STATE;
    }
    if (s_registry->command_count >= XIAOTAI_FRAMEWORK_MAX_COMMANDS) return XIAOTAI_ERR_NO_MEM;
    s_registry->commands[s_registry->command_count++] =
        (command_t){.name = name, .help = help ? help : "", .handler = handler, .scope = scope};
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_command_scope_capture(xiaotai_command_scope_t *scope)
{
    if (!scope) return XIAOTAI_ERR_INVALID_ARG;
    if (!fw_initializing() || !xiaotai_on_owner()) return XIAOTAI_ERR_INVALID_STATE;
    *scope = (xiaotai_command_scope_t){.application = fw_app_current(), .generation = s_registration_generation};
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_command_invoke(xiaotai_command_scope_t scope, xiaotai_command_fn_t handler,
                                    int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    if (!handler) return XIAOTAI_ERR_INVALID_ARG;
    if (!xiaotai_on_owner() || scope.generation != s_registration_generation) return XIAOTAI_ERR_INVALID_STATE;
    const int16_t previous = fw_app_enter(scope.application);
    const xiaotai_err_t result = handler(argc, argv, reply);
    fw_app_leave(previous);
    return result;
}

size_t xiaotai_command_count(void) { return s_registry ? s_registry->command_count : 0; }

void xiaotai_command_at(size_t index, const char **name, const char **help)
{
    if (!s_registry || index >= s_registry->command_count) return;
    if (name) *name = s_registry->commands[index].name;
    if (help) *help = s_registry->commands[index].help;
}

static void run_job(void *ctx)
{
    job_t *job = ctx;
    job->result = xiaotai_command_invoke(job->command->scope, job->command->handler,
                                         job->argc, job->argv, &job->reply);
    xiaotai_signal_give(&s_job_done);
}

static bool is_blank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/* Splits at spaces and tabs (a line end counts as one); nothing else separates
 * words and quotes are not interpreted, so a comma or a URL stays inside its
 * word. Returns the word count, or -1 for more than XIAOTAI_COMMAND_ARGS_MAX
 * words: a line that does not fit must not run as a shorter command. */
static int split(char *line, const char **argv)
{
    int argc = 0;
    for (char *cursor = line; *cursor;) {
        while (is_blank(*cursor)) *cursor++ = '\0';
        if (!*cursor) break;
        if (argc >= (int)XIAOTAI_COMMAND_ARGS_MAX) return -1;
        argv[argc++] = cursor;
        while (*cursor && !is_blank(*cursor)) ++cursor;
    }
    return argc;
}

xiaotai_err_t xiaotai_command_run(const char *line, const xiaotai_reply_t *reply, uint32_t timeout_ms)
{
    if (!line || !reply || !reply->write) return XIAOTAI_ERR_INVALID_ARG;
    if (!fw_started() || !s_job_ready || xiaotai_on_owner()) return XIAOTAI_ERR_INVALID_STATE;
    if (strlen(line) >= XIAOTAI_COMMAND_LINE_MAX) return XIAOTAI_ERR_INVALID_SIZE;
    if (!xiaotai_mutex_lock(&s_job_lock, timeout_ms)) return XIAOTAI_ERR_TIMEOUT;
    job_t *job = &s_registry->job;
    xiaotai_err_t result = XIAOTAI_OK;
    /* A timed-out caller leaves the slot occupied until completion is
     * consumed. Otherwise a delayed old signal may certify a new request. */
    if (s_job_running) {
        if (!xiaotai_signal_take(&s_job_done, timeout_ms)) result = XIAOTAI_ERR_BUSY;
        else s_job_running = false;
    }
    if (result == XIAOTAI_OK) {
        memset(job, 0, sizeof(*job));
        strcpy(job->line, line);
        job->argc = split(job->line, job->argv);
        if (job->argc <= 0) result = XIAOTAI_ERR_INVALID_ARG;
        for (size_t i = 0; result == XIAOTAI_OK && i < s_registry->command_count && !job->command; ++i) {
            if (strcmp(s_registry->commands[i].name, job->argv[0]) == 0) job->command = &s_registry->commands[i];
        }
        if (result == XIAOTAI_OK && !job->command) result = XIAOTAI_ERR_NOT_FOUND;
    }
    if (result == XIAOTAI_OK) {
        job->reply = *reply;
        result = xiaotai_run(run_job, job);
        if (result == XIAOTAI_OK) {
            s_job_running = true;
            if (xiaotai_signal_take(&s_job_done, timeout_ms)) {
                s_job_running = false;
                result = job->result;
            } else {
                result = XIAOTAI_ERR_TIMEOUT;
            }
        }
    }
    xiaotai_mutex_unlock(&s_job_lock);
    return result;
}

xiaotai_err_t fw_diag_add(const char *name, xiaotai_diag_fn_t report)
{
    if (!name || !report) return XIAOTAI_ERR_INVALID_ARG;
    const xiaotai_err_t err = prepare();
    if (err != XIAOTAI_OK) return err;
    if (s_registry->diagnostic_count >= XIAOTAI_FRAMEWORK_MAX_DIAGNOSTICS) return XIAOTAI_ERR_NO_MEM;
    s_registry->diagnostics[s_registry->diagnostic_count++] = (diagnostic_t){.name = name, .report = report};
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_diag_register(const char *name, xiaotai_diag_fn_t report)
{
    if (!name || !report) return XIAOTAI_ERR_INVALID_ARG;
    if (!fw_initializing()) return XIAOTAI_ERR_INVALID_STATE;
    return fw_diag_add(name, report);
}

void xiaotai_diag_report(const char *name, const xiaotai_reply_t *out)
{
    if (!out || !out->write || !s_registry) return;
    for (size_t i = 0; i < s_registry->diagnostic_count; ++i) {
        if (!name || strcmp(s_registry->diagnostics[i].name, name) == 0) s_registry->diagnostics[i].report(out);
    }
}

void xiaotai_diag_visit_sections(const char *name, xiaotai_diag_visit_fn_t visit, void *ctx)
{
    if (!visit || !s_registry) return;
    for (size_t i = 0; i < s_registry->diagnostic_count; ++i) {
        const diagnostic_t *section = &s_registry->diagnostics[i];
        if (!name || strcmp(name, section->name) == 0) visit(section->name, section->report, ctx);
    }
}
