// SPDX-License-Identifier: MIT
#include <inttypes.h>
#include <string.h>
#include "internal.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/sys.h"

static void token(char out[64], const char *value)
{
    size_t n = 0;
    for (; value && value[n] && n < 63; ++n) {
        const unsigned char c = (unsigned char)value[n];
        out[n] = c > ' ' && c < 127 ? (char)c : '_';
    }
    if (!n) out[n++] = '-';
    out[n] = '\0';
}

static xiaotai_err_t version(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    xiaotai_firmware_t firmware = {0};
    xiaotai_board_identity_t identity = {0};
    char board[64], revision[64], app[64], platform[64], chip[64];
    xiaotai_firmware(&firmware);
    at_board_identity(&identity);
    token(board, identity.model);
    token(revision, identity.revision);
    token(app, firmware.version);
    token(platform, firmware.platform);
    token(chip, firmware.chip);
    xiaotai_at_printf(reply, "+VERSION: board=%s revision=%s version=%s platform=%s chip=%s cores=%u at=%d",
                      board, revision, app, platform, chip, firmware.cores, XIAOTAI_AT_VOCABULARY);
    xiaotai_at_printf(reply, "+BUILD: elf_sha256=%s", firmware.elf_sha256[0] ? firmware.elf_sha256 : "-");
    return XIAOTAI_OK;
}

static const char *hex_name(const char *value, char *out, size_t capacity)
{
    return *value && xiaotai_at_hex_encode(value, strlen(value), out, capacity) ? out : "-";
}

void at_mem_report(const xiaotai_reply_t *reply)
{
    static const struct { xiaotai_mem_kind_t kind; const char *name; } kinds[] = {
        {XIAOTAI_MEM_INTERNAL, "internal"}, {XIAOTAI_MEM_BULK, "bulk"}, {XIAOTAI_MEM_DMA, "dma"},
    };
    for (size_t i = 0; i < sizeof(kinds) / sizeof(*kinds); ++i) {
        xiaotai_mem_stats_t stats;
        xiaotai_mem_stats(kinds[i].kind, &stats);
        xiaotai_at_printf(reply, "+MEM: kind=%s total=%zu free=%zu min_free=%zu largest=%zu min_largest=%zu samples=%" PRIu32,
                          kinds[i].name, stats.total, stats.free, stats.minimum_free, stats.largest_block,
                          stats.minimum_largest_block, stats.samples);
    }
    xiaotai_mem_failure_t failure;
    xiaotai_mem_failure(&failure);
    if (failure.has_first) {
        char function[sizeof(failure.function) * 2 + 1], task[sizeof(failure.task) * 2 + 1];
        xiaotai_at_printf(reply, "+MEM-FAIL: installed=%d error=%ld failures=%" PRIu32 " first_us=%" PRId64
                          " size=%zu caps=%" PRIu32 " isr=%d cache=%d function_hex=%s task_hex=%s",
                          failure.installed, (long)failure.error, failure.failures, failure.first_at_us,
                          failure.requested_size, failure.platform_caps, failure.in_isr, failure.cache_enabled,
                          hex_name(failure.function, function, sizeof(function)), hex_name(failure.task, task, sizeof(task)));
    } else {
        xiaotai_at_printf(reply, "+MEM-FAIL: installed=%d error=%ld failures=%" PRIu32
                          " first_us=- size=- caps=- isr=- cache=- function_hex=- task_hex=-",
                          failure.installed, (long)failure.error, failure.failures);
    }
    xiaotai_at_printf(reply, "+MEM-INFO: external=%d", xiaotai_mem_has_external());
}

static xiaotai_err_t mem(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    at_mem_report(reply);
    return XIAOTAI_OK;
}

static xiaotai_err_t mem_rtc(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    xiaotai_mem_rtc_stats_t stats;
    const xiaotai_err_t err = xiaotai_mem_rtc_stats(&stats);
    if (err != XIAOTAI_OK) return err;
    xiaotai_at_printf(reply, "+MEM-RTC: free=%zu largest=%zu min_free=%zu", stats.free, stats.largest_block,
                      stats.minimum_free);
    return XIAOTAI_OK;
}

static xiaotai_err_t console(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    xiaotai_at_stats_t st;
    xiaotai_at_stats(&st);
    xiaotai_at_printf(reply, "+CONSOLE: lines=%" PRIu32 " ok=%" PRIu32 " failed=%" PRIu32 " overflows=%" PRIu32
                      " bad_chars=%" PRIu32 " gate=%" PRIu32 " timeouts=%" PRIu32 " late=%" PRIu32
                      " write_errors=%" PRIu32 " owner_flushes=%" PRIu32 " streams=%" PRIu32
                      " notify_sent=%" PRIu32 " notify_dropped=%" PRIu32,
                      st.lines, st.commands_ok, st.commands_failed, st.line_overflows, st.bad_characters,
                      st.gate_rejections, st.handler_timeouts, st.late_dropped, st.write_errors,
                      st.owner_flushes, st.streams, st.notify_sent, st.notify_dropped);
    at_console_describe_sources(reply);
    return XIAOTAI_OK;
}

static void probe_line(void *ctx, const char *line) { xiaotai_at_printf(ctx, "+PORT-PROBE: %s", line); }

static xiaotai_err_t probe(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    if (argc) {
        /* The port is the sole authority for which platform action exists. */
        for (const char *const *name = xiaotai_port_probes(); *name; ++name) {
            if (!strcmp(argv[0], *name)) return xiaotai_port_probe(*name, probe_line, (void *)reply);
        }
        return XIAOTAI_ERR_NOT_FOUND;
    }
    for (const char *const *name = xiaotai_port_probes(); *name; ++name)
        xiaotai_at_printf(reply, "+PORT-PROBE: name=%s", *name);
    return XIAOTAI_OK;
}

xiaotai_err_t at_local_init(const xiaotai_at_config_t *config)
{
    static const xiaotai_at_verb_t version_verb[] = {{.run = version}};
    static const xiaotai_at_verb_t mem_verb[] = {{.run = mem}, {.word = "RTC", .run = mem_rtc}};
    static const xiaotai_at_verb_t console_verb[] = {{.run = console}};
    static const xiaotai_at_verb_t port_verb[] = {{.word = "PROBE", .max_args = 1, .run = probe, .usage = "[<name>]"}};
    xiaotai_err_t err = at_local_verbs_register("AT+VERSION", version_verb, 1);
    if (err == XIAOTAI_OK) err = at_log_init(config->follow_log);
    if (err == XIAOTAI_OK) err = at_local_verbs_register("AT+MEM", mem_verb, sizeof(mem_verb) / sizeof(*mem_verb));
    if (err == XIAOTAI_OK) err = at_local_verbs_register("AT+CONSOLE", console_verb, 1);
#if XIAOTAI_AT_RESTART_COMMAND
    if (err == XIAOTAI_OK) err = xiaotai_at_guarded_restart_register(config);
#endif
    const char *const *probes = xiaotai_port_probes();
    if (err == XIAOTAI_OK && probes && *probes) err = at_local_verbs_register("AT+PORT", port_verb, 1);
    return err;
}
