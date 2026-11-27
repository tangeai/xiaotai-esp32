// SPDX-License-Identifier: MIT
/* src/coprocessor_link.h on the host: a scripted coprocessor that records
 * every call (xiaotai_coprocessor_host.h). */
#include "coprocessor_link.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "xiaotai_coprocessor_host.h"

static xiaotai_coprocessor_host_t s_host = {.contiguous = true};
static char s_journal[XIAOTAI_COPROCESSOR_HOST_JOURNAL_BYTES];
static size_t s_journal_length;

xiaotai_coprocessor_host_t *xiaotai_coprocessor_host(void) { return &s_host; }

void xiaotai_coprocessor_host_reset(void)
{
    memset(&s_host, 0, sizeof(s_host));
    s_host.contiguous = true;
    xiaotai_coprocessor_host_journal_clear();
}

void xiaotai_coprocessor_host_note(const char *format, ...)
{
    char line[128];
    va_list args;
    va_start(args, format);
    const int length = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (length < 0 || (size_t)length + 1U >= sizeof(line)) return;
    if (s_journal_length + (size_t)length + 2U > sizeof(s_journal)) return;
    memcpy(s_journal + s_journal_length, line, (size_t)length);
    s_journal_length += (size_t)length;
    s_journal[s_journal_length++] = '\n';
    s_journal[s_journal_length] = '\0';
}

const char *xiaotai_coprocessor_host_journal(void) { return s_journal; }

void xiaotai_coprocessor_host_journal_clear(void)
{
    s_journal_length = 0;
    s_journal[0] = '\0';
}

xiaotai_err_t xiaotai_coprocessor_link_wait(uint32_t timeout_ms)
{
    ++s_host.waits;
    s_host.last_timeout_ms = timeout_ms;
    xiaotai_coprocessor_host_note("wait %lu", (unsigned long)timeout_ms);
    return s_host.wait_result;
}

xiaotai_err_t xiaotai_coprocessor_link_start(uint32_t timeout_ms)
{
    ++s_host.starts;
    s_host.last_timeout_ms = timeout_ms;
    xiaotai_coprocessor_host_note("start %lu", (unsigned long)timeout_ms);
    return s_host.start_result;
}

xiaotai_err_t xiaotai_coprocessor_link_stop(void)
{
    ++s_host.stops;
    xiaotai_coprocessor_host_note("stop");
    return s_host.stop_result;
}

uint16_t xiaotai_coprocessor_link_chip(void)
{
    xiaotai_coprocessor_host_note("chip");
    return s_host.chip;
}

void xiaotai_coprocessor_link_facts(xiaotai_coprocessor_link_facts_t *facts)
{
    xiaotai_coprocessor_host_note("facts");
    facts->firmware = s_host.firmware;
    facts->capabilities = s_host.capabilities;
    facts->ext_capabilities = s_host.ext_capabilities;
    facts->rpc_version_advertised = s_host.rpc_version_advertised;
    facts->bus_buffer_config = s_host.bus_buffer_config;
}

xiaotai_err_t xiaotai_coprocessor_link_version(xiaotai_coprocessor_version_t *version)
{
    ++s_host.versions;
    xiaotai_coprocessor_host_note("version");
    version->major = s_host.major;
    version->minor = s_host.minor;
    version->patch = s_host.patch;
    return s_host.version_result;
}

xiaotai_err_t xiaotai_coprocessor_link_app(xiaotai_coprocessor_app_t *app)
{
    ++s_host.apps;
    xiaotai_coprocessor_host_note("app");
    app->magic = s_host.app_magic;
    memcpy(app->project, s_host.project, sizeof(app->project));
    memcpy(app->version, s_host.version, sizeof(app->version));
    memcpy(app->platform, s_host.platform, sizeof(app->platform));
    memcpy(app->elf_sha256, s_host.elf_sha256, sizeof(app->elf_sha256));
    return s_host.app_result;
}

xiaotai_err_t xiaotai_coprocessor_link_probe_unversioned(bool *matched)
{
    ++s_host.probes;
    xiaotai_coprocessor_host_note("probe");
    *matched = s_host.probe_result == XIAOTAI_OK && s_host.probe_matched;
    return s_host.probe_result;
}

xiaotai_err_t xiaotai_coprocessor_link_ota_begin(void)
{
    ++s_host.begins;
    xiaotai_coprocessor_host_note("begin");
    return s_host.begin_result;
}

xiaotai_err_t xiaotai_coprocessor_link_ota_write(const uint8_t *data, size_t size)
{
    ++s_host.writes;
    long offset = -1;
    if (s_host.image && data >= s_host.image) offset = (long)(data - s_host.image);
    xiaotai_coprocessor_host_note("write +%ld %lu", offset, (unsigned long)size);
    if (size > s_host.largest_write) s_host.largest_write = size;
    if (!s_host.image || data != s_host.image + s_host.written) s_host.contiguous = false;
    if (s_host.fail_write && s_host.writes == s_host.fail_write) return s_host.write_error;
    s_host.written += size;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_coprocessor_link_ota_end(void)
{
    ++s_host.ends;
    xiaotai_coprocessor_host_note("end");
    return s_host.end_result;
}

xiaotai_err_t xiaotai_coprocessor_link_ota_activate(void)
{
    ++s_host.activations;
    xiaotai_coprocessor_host_note("activate");
    return s_host.activate_result;
}

xiaotai_err_t xiaotai_coprocessor_link_command(uint32_t channel, const char *command, bool wait_for_reply,
                                              char *reply, size_t capacity)
{
    ++s_host.commands;
    xiaotai_coprocessor_host_note("command %lx %s %u", (unsigned long)channel, command, (unsigned)wait_for_reply);
    if (s_host.command_result == XIAOTAI_OK && wait_for_reply && capacity && s_host.command_reply) {
        const size_t n = strnlen(s_host.command_reply, capacity - 1U);
        memcpy(reply, s_host.command_reply, n);
        reply[n] = '\0';
    }
    return s_host.command_result;
}
