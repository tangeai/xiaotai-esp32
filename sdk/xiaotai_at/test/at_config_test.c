// SPDX-License-Identifier: MIT
/* The console engine's build options: the Kconfig a product edits, the
 * configuration header the code reads and the manifest the vocabulary is
 * generated from must agree, and a value set in the build must reach the
 * macros the public headers publish.
 *
 *   - every option in Kconfig has a macro and every macro an option;
 *   - the header's default (the host's value) equals the Kconfig default;
 *   - each int default lies inside its Kconfig range, and the local-command
 *     range leaves room for the engine's own nine commands;
 *   - every bool option controls at least one spelling of engine.json (the
 *     manifest names it as "option", with a handler prefix of the engine's
 *     own), and every option the manifest names is a bool option here;
 *   - AT+RESTART FORCE depends on AT+RESTART in Kconfig and in the header;
 *   - a unit built with every option changed sees the changed values through
 *     xiaotai/at.h (the header callers include), not the defaults. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xiaotai/at.h"

#include "at_config_test.h"

#ifndef XIAOTAI_TEST_KCONFIG
#error "XIAOTAI_TEST_KCONFIG must name the component's Kconfig"
#endif
#ifndef XIAOTAI_TEST_MANIFEST
#error "XIAOTAI_TEST_MANIFEST must name the component's engine.json"
#endif

/* Local commands the engine registers itself (xiaotai/at_config.h). */
#define AT_OWN_LOCAL_COMMANDS 9
/* Handler prefixes of the engine's guarded commands start with this. */
#define AT_GUARDED_PREFIX "xiaotai_at_guarded_"

typedef struct {
    char name[64];
    char depends[64];
    long range_low;
    long range_high;
    long fallback;
    int is_bool;
    int has_range;
    int has_default;
} kconfig_option_t;

static int s_failures;

#define CHECK(condition, ...)                                         \
    do {                                                              \
        if (!(condition)) {                                           \
            ++s_failures;                                             \
            fprintf(stderr, "%s:%d: ", __FILE__, __LINE__);           \
            fprintf(stderr, __VA_ARGS__);                             \
            fputc('\n', stderr);                                      \
        }                                                             \
    } while (0)

/* Reads every "config", "bool", "range", "default" and "depends on" line;
 * returns the option count. */
static size_t read_kconfig(const char *path, kconfig_option_t *options, size_t capacity)
{
    FILE *file = fopen(path, "r");
    CHECK(file != NULL, "cannot open %s", path);
    if (!file) return 0;
    char line[256];
    size_t count = 0;
    kconfig_option_t *current = NULL;
    while (fgets(line, sizeof(line), file)) {
        char keyword[16] = "";
        char word[64];
        long low = 0;
        long high = 0;
        /* Help text is free prose: only a line whose first word is exactly a
         * keyword counts. */
        if (sscanf(line, " %15s", keyword) != 1) continue;
        if (strcmp(keyword, "config") == 0 && sscanf(line, " config %63s", word) == 1) {
            CHECK(count < capacity, "more options than the test expects");
            if (count >= capacity) break;
            current = &options[count++];
            memset(current, 0, sizeof(*current));
            snprintf(current->name, sizeof(current->name), "%s", word);
        } else if (!current) {
            continue;
        } else if (strcmp(keyword, "bool") == 0) {
            current->is_bool = 1;
        } else if (strcmp(keyword, "range") == 0 && sscanf(line, " range %ld %ld", &low, &high) == 2) {
            current->range_low = low;
            current->range_high = high;
            current->has_range = 1;
        } else if (strcmp(keyword, "default") == 0 && current->is_bool &&
                   sscanf(line, " default %63s", word) == 1) {
            CHECK(strcmp(word, "y") == 0 || strcmp(word, "n") == 0, "%s: bool default %s", current->name, word);
            current->fallback = strcmp(word, "y") == 0;
            current->has_default = 1;
        } else if (strcmp(keyword, "default") == 0 && sscanf(line, " default %ld", &low) == 1) {
            current->fallback = low;
            current->has_default = 1;
        } else if (strcmp(keyword, "depends") == 0 && sscanf(line, " depends on %63s", word) == 1) {
            snprintf(current->depends, sizeof(current->depends), "%s", word);
        }
    }
    fclose(file);
    return count;
}

static char *read_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    CHECK(file != NULL, "cannot open %s", path);
    if (!file) return NULL;
    static char text[16384];
    const size_t size = fread(text, 1, sizeof(text) - 1, file);
    CHECK(feof(file), "%s is larger than the test reads", path);
    fclose(file);
    text[size] = '\0';
    return text;
}

/* Copies the JSON string value that follows `key` at or after `from` into
 * out; returns the position after it, or NULL when the key does not follow. */
static const char *string_after(const char *from, const char *key, char *out, size_t capacity)
{
    const char *at = strstr(from, key);
    if (!at) return NULL;
    at += strlen(key);
    while (*at == ' ' || *at == ':') ++at;
    if (*at != '"') return NULL;
    ++at;
    size_t length = 0;
    while (at[length] && at[length] != '"') ++length;
    if (length >= capacity) length = capacity - 1;
    memcpy(out, at, length);
    out[length] = '\0';
    return at + length;
}

static const kconfig_option_t *find_option(const kconfig_option_t *options, size_t count, const char *name)
{
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(options[i].name, name) == 0) return &options[i];
    }
    return NULL;
}

/* Every manifest "option" is a bool option here, with a symbol of the
 * engine's own guarded prefix; returns how many spellings name `option`. */
static size_t manifest_spellings(const char *manifest, const kconfig_option_t *options, size_t count,
                                 const char *option)
{
    size_t spellings = 0;
    const char *at = manifest;
    char name[64];
    char symbol[64];
    while ((at = string_after(at, "\"option\"", name, sizeof(name))) != NULL) {
        const kconfig_option_t *found = find_option(options, count, name);
        CHECK(found && found->is_bool, "engine.json names option %s, which is not a bool option in Kconfig", name);
        const char *next = string_after(at, "\"symbol\"", symbol, sizeof(symbol));
        CHECK(next != NULL, "engine.json: option %s without a symbol", name);
        if (next) {
            CHECK(strncmp(symbol, AT_GUARDED_PREFIX, strlen(AT_GUARDED_PREFIX)) == 0,
                  "engine.json: symbol %s of %s is not an engine handler prefix", symbol, name);
            at = next;
        }
        if (option && strcmp(name, option) == 0) ++spellings;
    }
    return spellings;
}

int main(void)
{
    const struct {
        const char *name;
        long value;
    } macros[] = {
        {"XIAOTAI_AT_SOURCES_MAX", XIAOTAI_AT_SOURCES_MAX},
        {"XIAOTAI_AT_LOCAL_MAX", XIAOTAI_AT_LOCAL_MAX},
        {"XIAOTAI_AT_VERB_SETS_MAX", XIAOTAI_AT_VERB_SETS_MAX},
        {"XIAOTAI_AT_RESTART_COMMAND", XIAOTAI_AT_RESTART_COMMAND},
        {"XIAOTAI_AT_RESTART_FORCE", XIAOTAI_AT_RESTART_FORCE},
        {"XIAOTAI_AT_SETTING_WRITE", XIAOTAI_AT_SETTING_WRITE},
        {"XIAOTAI_AT_INTENT_COMMAND", XIAOTAI_AT_INTENT_COMMAND},
    };
    const size_t macro_count = sizeof(macros) / sizeof(macros[0]);

    kconfig_option_t options[16];
    const size_t count = read_kconfig(XIAOTAI_TEST_KCONFIG, options, sizeof(options) / sizeof(options[0]));
    CHECK(count == macro_count, "Kconfig has %zu options, the configuration header %zu", count, macro_count);

    const char *manifest = read_file(XIAOTAI_TEST_MANIFEST);
    if (manifest) (void)manifest_spellings(manifest, options, count, NULL);

    for (size_t i = 0; i < count; ++i) {
        const kconfig_option_t *option = &options[i];
        size_t found = macro_count;
        for (size_t m = 0; m < macro_count; ++m) {
            if (strcmp(option->name, macros[m].name) == 0) found = m;
        }
        CHECK(found < macro_count, "Kconfig option %s has no macro in xiaotai/at_config.h", option->name);
        CHECK(option->has_default, "%s needs a default", option->name);
        CHECK(option->is_bool || option->has_range, "%s needs a range", option->name);
        if (found == macro_count || !option->has_default) continue;
        CHECK(option->fallback == macros[found].value, "%s: Kconfig default %ld, header default %ld",
              option->name, option->fallback, macros[found].value);
        if (option->is_bool) {
            CHECK(manifest && manifest_spellings(manifest, options, count, option->name) > 0,
                  "bool option %s controls no spelling of engine.json", option->name);
            continue;
        }
        if (!option->has_range) continue;
        CHECK(option->range_low <= option->fallback && option->fallback <= option->range_high,
              "%s: default %ld outside %ld..%ld", option->name, option->fallback, option->range_low,
              option->range_high);
        if (strcmp(option->name, "XIAOTAI_AT_LOCAL_MAX") == 0) {
            CHECK(option->range_low > AT_OWN_LOCAL_COMMANDS,
                  "XIAOTAI_AT_LOCAL_MAX may go down to %ld: no room beside the engine's %d own commands",
                  option->range_low, AT_OWN_LOCAL_COMMANDS);
        }
    }
    for (size_t m = 0; m < macro_count; ++m) {
        size_t seen = 0;
        for (size_t i = 0; i < count; ++i) seen += strcmp(options[i].name, macros[m].name) == 0;
        CHECK(seen == 1, "%s appears %zu times in Kconfig", macros[m].name, seen);
    }
    const kconfig_option_t *force = find_option(options, count, "XIAOTAI_AT_RESTART_FORCE");
    CHECK(force && strcmp(force->depends, "XIAOTAI_AT_RESTART_COMMAND") == 0,
          "XIAOTAI_AT_RESTART_FORCE must depend on XIAOTAI_AT_RESTART_COMMAND in Kconfig");
    /* Two spellings carry AT+RESTART's own option or FORCE's: one each. */
    if (manifest) {
        CHECK(manifest_spellings(manifest, options, count, "XIAOTAI_AT_RESTART_COMMAND") == 1,
              "AT+RESTART: one spelling expected");
        CHECK(manifest_spellings(manifest, options, count, "XIAOTAI_AT_RESTART_FORCE") == 1,
              "AT+RESTART FORCE: one spelling expected");
        CHECK(manifest_spellings(manifest, options, count, "XIAOTAI_AT_SETTING_WRITE") == 1,
              "AT+SET <name> <value>: one spelling expected (the read form has no option)");
    }

    /* The defaults the host uses (Kconfig defaults, so equal on ESP-IDF). */
    CHECK(XIAOTAI_AT_SOURCES_MAX == 3, "sources %d", XIAOTAI_AT_SOURCES_MAX);
    CHECK(XIAOTAI_AT_LOCAL_MAX == 24, "local %d", XIAOTAI_AT_LOCAL_MAX);
    CHECK(XIAOTAI_AT_VERB_SETS_MAX == 48, "verb sets %d", XIAOTAI_AT_VERB_SETS_MAX);
    CHECK(XIAOTAI_AT_RESTART_COMMAND == 1 && XIAOTAI_AT_RESTART_FORCE == 1, "restart %d force %d",
          XIAOTAI_AT_RESTART_COMMAND, XIAOTAI_AT_RESTART_FORCE);
    CHECK(XIAOTAI_AT_SETTING_WRITE == 1 && XIAOTAI_AT_INTENT_COMMAND == 1, "set write %d intent %d",
          XIAOTAI_AT_SETTING_WRITE, XIAOTAI_AT_INTENT_COMMAND);

    at_config_values_t changed;
    at_config_overridden(&changed);
    CHECK(changed.sources == 5, "overridden sources %ld", changed.sources);
    CHECK(changed.local == 40, "overridden local %ld", changed.local);
    CHECK(changed.verb_sets == 96, "overridden verb sets %ld", changed.verb_sets);
    CHECK(changed.restart == 0 && changed.restart_force == 0, "overridden restart %ld force %ld", changed.restart,
          changed.restart_force);
    CHECK(changed.setting_write == 0 && changed.intent == 0, "overridden set write %ld intent %ld",
          changed.setting_write, changed.intent);

    /* FORCE without AT+RESTART is off; the other options keep their defaults. */
    at_config_values_t force_only;
    at_config_force_only(&force_only);
    CHECK(force_only.restart == 0 && force_only.restart_force == 0, "force only: restart %ld force %ld",
          force_only.restart, force_only.restart_force);
    CHECK(force_only.setting_write == 1 && force_only.intent == 1 && force_only.local == 24,
          "force only: set write %ld intent %ld local %ld", force_only.setting_write, force_only.intent,
          force_only.local);

    if (s_failures) {
        fprintf(stderr, "at_config: %d failure(s)\n", s_failures);
        return EXIT_FAILURE;
    }
    printf("at_config: ok\n");
    return EXIT_SUCCESS;
}
