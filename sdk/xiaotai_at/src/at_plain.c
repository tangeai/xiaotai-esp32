// SPDX-License-Identifier: MIT
/* Maintenance commands in the spelling the BK7258 firmware's console uses (one
 * lower-case word with dashes and its arguments: `status`, `ai-start`,
 * `wifi-set "Home WiFi" secret`), so one tool and one manual serve both
 * firmware lines. Each is translated into this vocabulary's AT commands and run
 * exactly as if those lines had been typed: the same gate, the same handlers,
 * the same data lines and one final OK or ERROR. A plain command whose AT
 * command is not in the build is answered `ERROR: NOT_SUPPORTED NOT_IN_BUILD`,
 * so a product offers no more than its AT vocabulary does.
 *
 * Arguments are separated by spaces; an argument that holds spaces, or is
 * empty, is written in double quotes ("" is the empty text). Text arguments
 * (Wi-Fi name and password, a secret, a contact name) may be UTF-8 and are
 * passed on hex-encoded; every other argument is an ASCII word. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "internal.h"
#include "xiaotai/port/os.h"

enum {
    PLAIN_ARGS_MAX = 4,
    PLAIN_LINE_MAX = 400,        /* the longest expansion: a 32-byte name and a 64-byte password as hex */
    CONTACT_PAGES_MAX = 16,
    CONTACT_WAIT_MS = 3000,
};

typedef struct {
    int argc;                    /* words after the command name */
    const char *argv[PLAIN_ARGS_MAX];
} plain_args_t;

typedef struct plain plain_t;
typedef at_result_t (*plain_fn_t)(const xiaotai_at_io_t *io, const plain_t *cmd, const plain_args_t *args);

/* `at` is the AT line, with $1..$4 for an argument passed as an ASCII word,
 * $N? for an optional one and $Nx for a text passed hex-encoded. A command
 * with `run` uses `at` for the AT command it depends on. */
struct plain {
    const char *name, *usage, *at;
    uint8_t min_args, max_args;
    plain_fn_t run;
};

static const at_result_t s_ok = {0};
#define RESULT(error, reason) ((at_result_t){(error), (reason)})

/* ------------------------------------------------------------------ output */

static void write_line(const xiaotai_at_io_t *io, const char *text)
{
    char record[128];
    const int n = snprintf(record, sizeof(record), "%s\r\n", text);
    if (n > 0 && (size_t)n < sizeof(record)) (void)at_write_record(io, record, (size_t)n);
}

/* ------------------------------------------------------------------- input */

static bool word_char(char c) { return c > ' ' && c < 127 && c != '"'; }

static bool ascii_word(const char *text)
{
    if (!*text) return false;
    for (; *text; ++text) if (!word_char(*text)) return false;
    return true;
}

/* Splits in place: words end at a space, a quoted argument at its closing
 * quote, which must be followed by a space or the end of the line. */
static const char *split(char *line, const char **name, plain_args_t *args)
{
    int count = -1;
    for (char *p = line; *p;) {
        while (*p == ' ') ++p;
        if (!*p) break;
        if (count == PLAIN_ARGS_MAX) return "ARGUMENT_COUNT";
        char *start = p;
        if (*p == '"') {
            start = ++p;
            while (*p && *p != '"') ++p;
            if (!*p) return "QUOTE";
            *p++ = '\0';
            if (*p && *p != ' ') return "QUOTE";
        } else {
            while (*p && *p != ' ') {
                if (*p == '"') return "QUOTE";
                ++p;
            }
            if (*p) *p++ = '\0';
        }
        if (count < 0) *name = start;
        else args->argv[count] = start;
        ++count;
    }
    args->argc = count < 0 ? 0 : count;
    return NULL;
}

static bool same_word(const char *a, const char *b)
{
    for (; *a && *b; ++a, ++b) {
        const char x = (*a >= 'A' && *a <= 'Z') ? (char)(*a - 'A' + 'a') : *a;
        if (x != *b) return false;
    }
    return !*a && !*b;
}

/* ---------------------------------------------------------------- expansion */

static bool append(char *out, size_t *used, const char *text, size_t length)
{
    if (*used + length + 1 >= PLAIN_LINE_MAX) return false;
    memcpy(out + *used, text, length);
    *used += length;
    out[*used] = '\0';
    return true;
}

static at_result_t expand(const char *at, const plain_args_t *args, char out[PLAIN_LINE_MAX])
{
    size_t used = 0;
    out[0] = '\0';
    for (const char *p = at; *p;) {
        const char *end = strchr(p, ' ');
        const size_t length = end ? (size_t)(end - p) : strlen(p);
        if (*p == '$') {
            const int index = p[1] - '1';
            const bool optional = length == 3 && p[2] == '?', hex = length == 3 && p[2] == 'x';
            const char *value = index < args->argc ? args->argv[index] : NULL;
            if (!value && !optional) return RESULT(XIAOTAI_ERR_INVALID_ARG, "ARGUMENT_COUNT");
            if (value) {
                char text[2 * 96 + 1];
                if (hex) {
                    if (!xiaotai_at_text_encode(value, text, sizeof(text)))
                        return RESULT(XIAOTAI_ERR_INVALID_SIZE, "ARGUMENT_TOO_LONG");
                    value = text;
                } else if (!ascii_word(value)) {
                    return RESULT(XIAOTAI_ERR_INVALID_ARG, "ARGUMENT");
                }
                if ((used && !append(out, &used, " ", 1)) || !append(out, &used, value, strlen(value)))
                    return RESULT(XIAOTAI_ERR_INVALID_SIZE, "LINE_TOO_LONG");
            }
        } else if ((used && !append(out, &used, " ", 1)) || !append(out, &used, p, length)) {
            return RESULT(XIAOTAI_ERR_INVALID_SIZE, "LINE_TOO_LONG");
        }
        p = end ? end + 1 : p + length;
    }
    return s_ok;
}

/* The AT command (and verb) a template starts with is in this build. */
static bool available(const char *at)
{
    char name[XIAOTAI_AT_ID_MAX + 4], verb[24] = "";
    const char *space = strchr(at, ' ');
    size_t length = space ? (size_t)(space - at) : strlen(at);
    if (length >= sizeof(name)) return false;
    memcpy(name, at, length);
    name[length] = '\0';
    if (space && space[1] != '$') {
        const char *next = strchr(space + 1, ' ');
        const size_t verb_length = next ? (size_t)(next - space - 1) : strlen(space + 1);
        if (verb_length < sizeof(verb)) {
            memcpy(verb, space + 1, verb_length);
            verb[verb_length] = '\0';
        }
    }
    return at_console_knows(name, verb[0] ? verb : NULL);
}

/* Before the framework has started only the console's own commands exist. */
static at_result_t missing(void)
{
    return at_console_started() ? RESULT(XIAOTAI_ERR_NOT_SUPPORTED, "NOT_IN_BUILD")
                                : RESULT(XIAOTAI_ERR_INVALID_STATE, "STARTING");
}

static at_result_t run_at(const xiaotai_at_io_t *io, const char *at, const plain_args_t *args)
{
    char line[PLAIN_LINE_MAX];
    if (!available(at)) return missing();
    const at_result_t expanded = expand(at, args, line);
    return expanded.error ? expanded : at_console_run(io, line);
}

static const plain_args_t s_none;

/* ------------------------------------------------------------ captured runs */

/* A command run for the facts in its data lines: each line is handed to
 * `line` instead of being printed. */
typedef struct {
    void (*line)(void *ctx, const char *text);
    void *ctx;
} capture_t;

/* Only the leading keys of a line are read; a longer line is cut. */
static xiaotai_err_t capture_write(void *self, const char *data, size_t size)
{
    const capture_t *capture = self;
    char text[256];
    while (size && (data[size - 1] == '\n' || data[size - 1] == '\r')) --size;
    if (size >= sizeof(text)) size = sizeof(text) - 1;
    memcpy(text, data, size);
    text[size] = '\0';
    capture->line(capture->ctx, text);
    return XIAOTAI_OK;
}

static size_t capture_read(void *self, uint8_t *data, size_t capacity, uint32_t timeout_ms)
{
    (void)self; (void)data; (void)capacity; (void)timeout_ms;
    return 0;
}

static at_result_t run_captured(const char *at, capture_t *capture)
{
    const xiaotai_at_io_t io = {.name = "plain", .read = capture_read, .write = capture_write, .self = capture};
    return run_at(&io, at, &s_none);
}

/* The value of key= in a data line, copied into out; false when absent. */
static bool field(const char *line, const char *key, char *out, size_t capacity)
{
    const size_t key_length = strlen(key);
    for (const char *p = strchr(line, ' '); p; p = strchr(p + 1, ' ')) {
        if (strncmp(p + 1, key, key_length) || p[1 + key_length] != '=') continue;
        const char *value = p + 2 + key_length;
        size_t length = strcspn(value, " ");
        if (length >= capacity) length = capacity - 1;
        memcpy(out, value, length);
        out[length] = '\0';
        return true;
    }
    return false;
}

static bool has_prefix(const char *line, const char *prefix) { return !strncmp(line, prefix, strlen(prefix)); }

/* ---------------------------------------------------------------- calls */

static const char *call_kind(const plain_t *cmd) { return has_prefix(cmd->name, "wechat") ? "wechat" : "device"; }

typedef struct {
    const char *kind;
    bool incoming[2];            /* [0] the asked kind, [1] any kind */
} call_state_t;

static void on_call(void *ctx, const char *line)
{
    call_state_t *state = ctx;
    char kind[16], value[16];
    if (!has_prefix(line, "+CALL: ") || !field(line, "kind", kind, sizeof(kind)) ||
        !field(line, "state", value, sizeof(value)) || strcmp(value, "incoming"))
        return;
    state->incoming[1] = true;
    if (state->kind && !strcmp(kind, state->kind)) state->incoming[0] = true;
}

static at_result_t answer(const xiaotai_at_io_t *io, const plain_t *cmd, const plain_args_t *args)
{
    (void)args;
    call_state_t state = {.kind = call_kind(cmd)};
    capture_t capture = {on_call, &state};
    const at_result_t read = run_captured("AT+CALL", &capture);
    if (read.error) return read;
    return state.incoming[0] ? run_at(io, cmd->at, &s_none) : RESULT(XIAOTAI_ERR_INVALID_STATE, "NO_INCOMING");
}

/* Hanging up a call that is still ringing is rejecting it. */
static at_result_t hangup(const xiaotai_at_io_t *io, const plain_t *cmd, const plain_args_t *args)
{
    (void)args;
    call_state_t state = {0};
    capture_t capture = {on_call, &state};
    const at_result_t read = run_captured("AT+CALL", &capture);
    if (read.error) return read;
    return run_at(io, state.incoming[1] ? "AT+CALL REJECT" : cmd->at, &s_none);
}

typedef struct {
    const char *kind;            /* contacts source: "wechat", "device" */
    const char *name_hex;        /* wanted name; NULL: the first of its kind */
    const char *id;              /* also accepted as the contact's id */
    unsigned total, listed, loading;
    int found;
} contact_search_t;

static void on_contact(void *ctx, const char *line)
{
    contact_search_t *search = ctx;
    char value[2 * 64 + 1], index[8];
    if (has_prefix(line, "+CONTACTS: ")) {
        if (field(line, "total", value, sizeof(value))) search->total = (unsigned)strtoul(value, NULL, 10);
        if (field(line, "loading", value, sizeof(value))) search->loading = (unsigned)strtoul(value, NULL, 10);
        return;
    }
    if (!has_prefix(line, "+CONTACT: ")) return;
    ++search->listed;
    if (search->found >= 0 || !field(line, "source", value, sizeof(value)) || strcmp(value, search->kind) ||
        !field(line, "index", index, sizeof(index)))
        return;
    /* Both sides are xiaotai_at_text_encode output: upper-case hex. */
    bool match = !search->name_hex;
    if (!match && field(line, "name_hex", value, sizeof(value))) match = !strcmp(value, search->name_hex);
    if (!match && search->id && field(line, "id", value, sizeof(value))) match = !strcmp(value, search->id);
    if (match) search->found = (int)strtol(index, NULL, 10);
}

/* Shows the contact pages one by one (the list a screen or AT+CALL INDEX
 * uses) until the contact is on the shown page, then dials it there. */
static at_result_t call_contact(const xiaotai_at_io_t *io, const plain_t *cmd, const plain_args_t *args)
{
    if (!available("AT+CONTACTS PAGE") || !available(cmd->at)) return missing();
    const char *wanted = args->argc ? args->argv[0] : NULL;
    char name_hex[2 * 96 + 1];
    if (wanted && *wanted && !xiaotai_at_text_encode(wanted, name_hex, sizeof(name_hex)))
        return RESULT(XIAOTAI_ERR_INVALID_SIZE, "ARGUMENT_TOO_LONG");
    contact_search_t search = {.kind = call_kind(cmd), .name_hex = wanted && *wanted ? name_hex : NULL,
                               .id = wanted && xiaotai_at_is_id(wanted, XIAOTAI_AT_ID_MAX) ? wanted : NULL};
    for (unsigned page = 0; page < CONTACT_PAGES_MAX; ++page) {
        char at[40];
        (void)snprintf(at, sizeof(at), "AT+CONTACTS PAGE ALL %u", page);
        search.found = -1;
        search.listed = search.total = 0;
        capture_t capture = {on_contact, &search};
        at_result_t read = run_captured(at, &capture);
        /* A page not cached yet is fetched; its list arrives shortly after. */
        for (unsigned waited = 0; !read.error && search.loading && search.found < 0 && waited < CONTACT_WAIT_MS;
             waited += 250) {
            xiaotai_delay_ms(250);
            search.listed = search.total = search.loading = 0;
            read = run_captured("AT+CONTACTS", &capture);
        }
        if (read.error) return read;
        if (search.found >= 0) {
            char dial[40];
            (void)snprintf(dial, sizeof(dial), "%s %d", cmd->at, search.found);
            return run_at(io, dial, &s_none);
        }
        if (search.loading) return RESULT(XIAOTAI_ERR_BUSY, "CONTACTS_LOADING");
        if (!search.listed || (page + 1) * search.listed >= search.total) break;
    }
    /* A device may also be dialled by its id while it is not on a page. */
    if (search.id && !strcmp(search.kind, "device") && available("AT+CALL DEVICE")) {
        const plain_args_t id = {.argc = 1, .argv = {search.id}};
        return run_at(io, "AT+CALL DEVICE $1", &id);
    }
    return RESULT(XIAOTAI_ERR_NOT_FOUND, "CONTACT");
}

/* ------------------------------------------------------------ other forms */

static at_result_t sequence(const xiaotai_at_io_t *io, const plain_t *cmd, const plain_args_t *args)
{
    (void)args;
    at_result_t result = s_ok;
    bool ran = false;
    for (const char *p = cmd->at; *p;) {
        const char *end = strchr(p, '|');
        char at[32];
        const size_t length = end ? (size_t)(end - p) : strlen(p);
        if (length < sizeof(at)) {
            memcpy(at, p, length);
            at[length] = '\0';
            if (available(at)) {
                const at_result_t one = run_at(io, at, &s_none);
                if (!result.error) result = one;
                ran = true;
            }
        }
        p = end ? end + 1 : p + length;
    }
    return ran ? result : missing();
}

static at_result_t log_level(const xiaotai_at_io_t *io, const plain_t *cmd, const plain_args_t *args)
{
    if (args->argc == 0) return run_at(io, cmd->at, args);
    if (args->argc == 1) {
        if (same_word(args->argv[0], "save")) return run_at(io, "AT+LOGLEVEL SAVE", &s_none);
        if (same_word(args->argv[0], "forget")) return run_at(io, "AT+LOGLEVEL FORGET", &s_none);
        return RESULT(XIAOTAI_ERR_INVALID_ARG, "ARGUMENT_COUNT");
    }
    if (same_word(args->argv[0], "module")) {
        if (args->argc != 3) return RESULT(XIAOTAI_ERR_INVALID_ARG, "ARGUMENT_COUNT");
        const plain_args_t module = {.argc = 2, .argv = {args->argv[1], args->argv[2]}};
        return run_at(io, "AT+LOGLEVEL MODULE $1 $2", &module);
    }
    if (args->argc != 2) return RESULT(XIAOTAI_ERR_INVALID_ARG, "ARGUMENT_COUNT");
    const plain_args_t level = {.argc = 1, .argv = {args->argv[1]}};
    if (same_word(args->argv[0], "app")) return run_at(io, "AT+LOGLEVEL APP $1", &level);
    if (same_word(args->argv[0], "sys")) return run_at(io, "AT+LOGLEVEL SYS $1", &level);
    if (same_word(args->argv[0], "tirtc")) return run_at(io, "AT+LOGLEVEL TIRTC $1", &level);
    return RESULT(XIAOTAI_ERR_INVALID_ARG, "LOG_TARGET");
}

static at_result_t voice_test(const xiaotai_at_io_t *io, const plain_t *cmd, const plain_args_t *args)
{
    return same_word(args->argv[0], "self-test") ? run_at(io, cmd->at, &s_none)
                                                 : RESULT(XIAOTAI_ERR_INVALID_ARG, "UNKNOWN_TEST");
}

static at_result_t help(const xiaotai_at_io_t *io, const plain_t *cmd, const plain_args_t *args);

/* In the order of the shared manual (SERIAL_COMMANDS.md of the BK7258 line). */
static const plain_t s_plain[] = {
    {"help", "", "", 0, 0, help},
    {"version", "", "AT+VERSION", 0, 0, NULL},
    {"status", "", "AT+NET|AT+PLATFORM|AT+LINK|AT+STATE|AT+MEM", 0, 0, sequence},
    {"call-status", "", "AT+CALL", 0, 0, NULL},
    {"log-level", "[app <0..4>|sys <0..3>|module <name> <0..4|->|tirtc <0..17>|save|forget]", "AT+LOGLEVEL", 0, 3,
     log_level},
    {"wifi-change", "", "AT+NET PROVISION", 0, 0, NULL},
    {"wifi-set", "<ssid> <password>", "AT+NET WIFI $1x $2x", 2, 2, NULL},
    {"wifi-clear", "", "AT+NET PROVISION FORGET", 0, 0, NULL},
    {"wificlear", "", "AT+NET PROVISION FORGET", 0, 0, NULL},
    {"tirtc-set", "<device_id> <device_secret> [client_id]", "AT+BIND SET $1 $2x $3?", 2, 3, NULL},
    {"tirtc-clear", "", "AT+BIND CLEAR", 0, 0, NULL},
    {"restart", "", "AT+RESTART", 0, 0, NULL},
    {"ai-start", "", "AT+AI START", 0, 0, NULL},
    {"ai-stop", "", "AT+AI STOP", 0, 0, NULL},
    {"wechat-call", "[\"name\"]", "AT+CALL INDEX", 0, 1, call_contact},
    {"call-answer", "", "AT+CALL ACCEPT", 0, 0, NULL},
    {"wechat-answer", "", "AT+CALL ACCEPT", 0, 0, answer},
    {"call-hangup", "", "AT+CALL HANGUP", 0, 0, hangup},
    {"device-call", "\"name\"|<device-id>", "AT+CALL INDEX", 1, 1, call_contact},
    {"device-answer", "", "AT+CALL ACCEPT", 0, 0, answer},
    {"room-status", "", "AT+ROOM", 0, 0, NULL},
    {"room-sync", "", "AT+ROOM REFRESH", 0, 0, NULL},
    {"room-join", "<code> [password]", "AT+ROOM JOIN $1 $2?", 1, 2, NULL},
    {"room-talk-start", "", "AT+ROOM PTT 1", 0, 0, NULL},
    {"room-talk-stop", "", "AT+ROOM PTT 0", 0, 0, NULL},
    {"room-leave", "", "AT+ROOM LEAVE", 0, 0, NULL},
    {"ep-show", "", "AT+EP", 0, 0, NULL},
    {"ep-set", "<url>", "AT+EP SET $1", 1, 1, NULL},
    {"ep-reset", "", "AT+EP RESET", 0, 0, NULL},
    {"ntp-show", "", "AT+NTP", 0, 0, NULL},
    {"ntp-set", "<server>", "AT+NTP SET $1", 1, 1, NULL},
    {"ntp-reset", "", "AT+NTP RESET", 0, 0, NULL},
    {"task-diag", "", "AT+STATUS tasks", 0, 0, NULL},
    {"voice-config", "<50..990>", "AT+WAKE THRESHOLD $1", 1, 1, NULL},
    {"voice-diag", "[5..30]", "AT+WAKE WATCH $1?", 0, 1, NULL},
    {"voice-test", "self-test", "AT+WAKE SELFTEST", 1, 1, voice_test},
};
#define PLAIN_COUNT (sizeof(s_plain) / sizeof(s_plain[0]))

static bool listed(const plain_t *cmd)
{
    if (cmd->run == help) return true;
    if (cmd->run != sequence) return available(cmd->at);
    for (const char *p = cmd->at; *p;) {
        const char *end = strchr(p, '|');
        char at[32];
        const size_t length = end ? (size_t)(end - p) : strlen(p);
        if (length < sizeof(at)) {
            memcpy(at, p, length);
            at[length] = '\0';
            if (available(at)) return true;
        }
        p = end ? end + 1 : p + length;
    }
    return false;
}

static at_result_t help(const xiaotai_at_io_t *io, const plain_t *cmd, const plain_args_t *args)
{
    (void)cmd;
    (void)args;
    for (size_t i = 0; i < PLAIN_COUNT; ++i) {
        if (!listed(&s_plain[i])) continue;
        char line[96];
        (void)snprintf(line, sizeof(line), "+HELP: %s%s%s", s_plain[i].name, *s_plain[i].usage ? " " : "",
                       s_plain[i].usage);
        write_line(io, line);
    }
    write_line(io, "+HELP: AT+HELP lists the AT commands");
    return s_ok;
}

bool at_plain_line(const char *line) { return strncmp(line, "AT", 2) != 0; }

at_result_t at_plain_run(const xiaotai_at_io_t *io, char *line)
{
    const char *name = NULL;
    plain_args_t args = {0};
    const char *fault = split(line, &name, &args);
    if (fault) return RESULT(XIAOTAI_ERR_INVALID_ARG, fault);
    if (!name) return s_ok;
    for (size_t i = 0; i < PLAIN_COUNT; ++i) {
        const plain_t *cmd = &s_plain[i];
        if (!same_word(name, cmd->name)) continue;
        if (args.argc < cmd->min_args || args.argc > cmd->max_args)
            return RESULT(XIAOTAI_ERR_INVALID_ARG, "ARGUMENT_COUNT");
        return cmd->run ? cmd->run(io, cmd, &args) : run_at(io, cmd->at, &args);
    }
    return RESULT(XIAOTAI_ERR_NOT_FOUND, "UNKNOWN_COMMAND");
}
