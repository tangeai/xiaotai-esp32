// SPDX-License-Identifier: MIT
#include <string.h>

#include "internal.h"
#include "xiaotai/at.h"
#include "xiaotai/port/mem.h"

typedef struct {
    const char *command;
    const xiaotai_at_verb_t *verbs;
    size_t count;
    xiaotai_command_scope_t scope;
} verb_set_t;

static verb_set_t *s_sets;
static size_t s_count;

static bool token(const char *word)
{
    if (!word || !*word) return false;
    for (; *word; ++word) {
        if ((*word < 'A' || *word > 'Z') && (*word < '0' || *word > '9') && *word != '_') return false;
    }
    return true;
}

static bool word_is(const char *a, const char *b)
{
    return a == b || (a && b && strcmp(a, b) == 0);
}

typedef struct {
    const xiaotai_reply_t *original;
    bool wrote;
} output_t;

static void forward(void *ctx, const char *line)
{
    output_t *output = ctx;
    if (!line) return;
    if (*line) output->wrote = true;
    output->original->write(output->original->ctx, line);
}

const xiaotai_reply_t *at_reply_original(const xiaotai_reply_t *reply)
{
    while (reply && reply->write == forward) reply = ((const output_t *)reply->ctx)->original;
    return reply;
}

static bool table_is(const verb_set_t *set, const char *command)
{
    return !set->command || word_is(set->command, command);
}

static bool has_named_verb(const verb_set_t *sets, size_t size, const char *command, const char *word)
{
    for (size_t i = 0; i < size; ++i) {
        if (!table_is(&sets[i], command)) continue;
        for (size_t j = 0; j < sets[i].count; ++j) {
            if (word_is(sets[i].verbs[j].word, word)) return true;
        }
    }
    return false;
}

static xiaotai_err_t dispatch(const verb_set_t *sets, size_t size, int argc,
                              const char *const *argv, const xiaotai_reply_t *reply)
{
    if (argc < 1 || !argv || !argv[0] || (argc > 1 && !argv[1])) return XIAOTAI_ERR_INVALID_ARG;
    const bool named = argc > 1 && has_named_verb(sets, size, argv[0], argv[1]);
    const int consumed = named ? 2 : 1;
    const int nargs = argc - consumed;
    output_t output = {.original = reply};
    const xiaotai_reply_t proxy = {.write = forward, .ctx = &output};
    const xiaotai_reply_t *sink = reply && reply->write ? &proxy : reply;
    bool matched = false;
    bool ran = false;
    for (size_t i = 0; i < size; ++i) {
        const verb_set_t *set = &sets[i];
        if (!table_is(set, argv[0])) continue;
        for (size_t j = 0; j < set->count; ++j) {
            const xiaotai_at_verb_t *verb = &set->verbs[j];
            const bool candidate = named ? word_is(verb->word, argv[1])
                : (!verb->word && (argc == 1 ? verb->min_args == 0 : verb->max_args != 0));
            if (!candidate) continue;
            matched = true;
            if (nargs < verb->min_args || nargs > verb->max_args) continue;
            ran = true;
            const xiaotai_err_t result = set->command
                ? xiaotai_command_invoke(set->scope, verb->run, nargs, argv + consumed, sink)
                : verb->run(nargs, argv + consumed, sink);
            if (result != XIAOTAI_AT_PASS) return result;
        }
    }
    if (!matched) return xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_ARG, argc == 1 ? "MISSING_VERB" : "UNKNOWN_VERB");
    if (!ran) return xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_ARG, "ARGUMENT_COUNT");
    return output.wrote ? XIAOTAI_OK : xiaotai_at_fail(reply, XIAOTAI_ERR_INVALID_STATE, "NO_OWNER");
}

xiaotai_err_t xiaotai_at_dispatch(const xiaotai_at_verb_t *verbs, size_t count, int argc,
                                  const char *const *argv, const xiaotai_reply_t *reply)
{
    if (!verbs && count) return XIAOTAI_ERR_INVALID_ARG;
    const verb_set_t set = {.verbs = verbs, .count = count};
    return dispatch(&set, 1, argc, argv, reply);
}

static xiaotai_err_t shared_command(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    return dispatch(s_sets, s_count, argc, argv, reply);
}

static bool seen_variant(const char *command, size_t before, const xiaotai_at_verb_t *verb)
{
    const char *usage = verb->usage ? verb->usage : "";
    for (size_t i = 0; i < before; ++i) {
        const verb_set_t *set = &s_sets[i];
        if (!word_is(set->command, command)) continue;
        for (size_t j = 0; j < set->count; ++j) {
            const xiaotai_at_verb_t *prior = &set->verbs[j];
            if (word_is(prior->word, verb->word) && word_is(prior->usage ? prior->usage : "", usage)) return true;
        }
    }
    return false;
}

static bool visit(const char *command, at_verb_visitor_t visitor, void *ctx, bool distinct)
{
    bool found = false;
    for (size_t i = 0; i < s_count; ++i) {
        const verb_set_t *set = &s_sets[i];
        if (!word_is(set->command, command)) continue;
        found = true;
        if (visitor) {
            for (size_t j = 0; j < set->count; ++j) {
                const xiaotai_at_verb_t *verb = &set->verbs[j];
                if (!distinct || !seen_variant(command, i, verb)) visitor(verb, ctx);
            }
        }
    }
    return found;
}

bool at_verbs_visit(const char *command, at_verb_visitor_t visitor, void *ctx)
{
    return visit(command, visitor, ctx, false);
}

bool at_verbs_visit_distinct(const char *command, at_verb_visitor_t visitor, void *ctx)
{
    return visit(command, visitor, ctx, true);
}

xiaotai_command_fn_t at_verbs_console_match(int argc, const char *const *argv)
{
    if (argc != 3) return NULL;
    for (size_t i = 0; i < s_count; ++i) {
        const verb_set_t *set = &s_sets[i];
        if (!word_is(set->command, argv[0])) continue;
        for (size_t j = 0; j < set->count; ++j) {
            const xiaotai_at_verb_t *verb = &set->verbs[j];
            if (verb->console_arg && word_is(verb->word, argv[1]) && word_is(verb->console_arg, argv[2]))
                return verb->run;
        }
    }
    return NULL;
}

xiaotai_err_t xiaotai_at_verbs_register(const char *command, const xiaotai_at_verb_t *verbs, size_t count)
{
    if (!command || strncmp(command, "AT+", 3) != 0 || !token(command + 3) || !verbs || !count) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < count; ++i) {
        const xiaotai_at_verb_t *verb = &verbs[i];
        const unsigned limit = XIAOTAI_AT_WORDS_MAX - (verb->word ? 2U : 1U);
        if (!verb->run || verb->min_args > verb->max_args || verb->max_args > limit ||
            (verb->word && !token(verb->word)) ||
            (verb->console_arg && (!verb->word || verb->min_args != 1 || verb->max_args != 1 ||
                                   !token(verb->console_arg)))) return XIAOTAI_ERR_INVALID_ARG;
        for (size_t j = 0; j < i; ++j) {
            if (word_is(verb->word, verbs[j].word)) return XIAOTAI_ERR_INVALID_ARG;
        }
    }
    xiaotai_command_scope_t scope;
    xiaotai_err_t err = xiaotai_command_scope_capture(&scope);
    if (err != XIAOTAI_OK) return err;
    if (scope.application >= 0) {
        for (size_t i = 0; i < count; ++i) if (verbs[i].console_arg) return XIAOTAI_ERR_INVALID_STATE;
    }
    /* The product still initializes the modules that offer optional AT verbs. */
    if (at_command_tables_omitted()) return XIAOTAI_OK;
    /* A failed framework init discards its commands, but the early console
     * can still be running. A retry must not use the abandoned verb tables. */
    if (s_count && s_sets[0].scope.generation != scope.generation) at_verbs_reset();
    for (size_t i = 0; i < s_count; ++i) {
        const verb_set_t *set = &s_sets[i];
        if (!word_is(set->command, command)) continue;
        for (size_t j = 0; j < set->count; ++j) {
            const xiaotai_at_verb_t *prior = &set->verbs[j];
            for (size_t k = 0; k < count; ++k) {
                if ((prior->console_arg || verbs[k].console_arg) && word_is(prior->word, verbs[k].word))
                    return XIAOTAI_ERR_INVALID_STATE;
            }
        }
    }
    if (s_count >= XIAOTAI_AT_VERB_SETS_MAX) return XIAOTAI_ERR_NO_MEM;
    if (!s_sets) {
        s_sets = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, XIAOTAI_AT_VERB_SETS_MAX, sizeof(*s_sets));
        if (!s_sets) return XIAOTAI_ERR_NO_MEM;
    }
    if (!at_verbs_visit(command, NULL, NULL)) {
        err = xiaotai_command_register(command, NULL, shared_command);
        if (err != XIAOTAI_OK) return err;
    }
    s_sets[s_count++] = (verb_set_t){.command = command, .verbs = verbs, .count = count, .scope = scope};
    return XIAOTAI_OK;
}

void at_verbs_reset(void)
{
    xiaotai_mem_free(s_sets);
    s_sets = NULL;
    s_count = 0;
}
