// SPDX-License-Identifier: MIT
#pragma once
#include "at_test.h"
#include "xiaotai/at.h"
#include "xiaotai/port/os.h"
#include "xiaotai_os_host.h"

typedef struct {
    const char *input;
    uint32_t write_us;
    const char *fail_prefix;
    bool bulk;
    unsigned begins, ends;
    size_t used;
    char text[12288];
} long_sink_t;

static inline size_t long_read(void *ctx, uint8_t *data, size_t capacity, uint32_t timeout_ms)
{
    long_sink_t *sink = ctx;
    size_t size = sink->input ? strlen(sink->input) : 0;
    if (!size) {
        xiaotai_os_host_advance_us((int64_t)timeout_ms * 1000);
        return 0;
    }
    if (size > capacity) size = capacity;
    memcpy(data, sink->input, size);
    sink->input += size;
    return size;
}

static inline xiaotai_err_t long_write(void *ctx, const char *text, size_t length)
{
    long_sink_t *sink = ctx;
    CHECK(length >= 2 && !memcmp(text + length - 2, "\r\n", 2));
    CHECK(!memchr(text, '\n', length - 2) && !memchr(text, '\r', length - 2));
    CHECK(!xiaotai_on_owner());
    if (sink->fail_prefix && length >= strlen(sink->fail_prefix) &&
        !memcmp(text, sink->fail_prefix, strlen(sink->fail_prefix))) {
        sink->fail_prefix = NULL;
        return XIAOTAI_FAIL;
    }
    if (sink->used + length >= sizeof(sink->text)) return XIAOTAI_ERR_NO_MEM;
    memcpy(sink->text + sink->used, text, length);
    sink->used += length;
    sink->text[sink->used] = '\0';
    if (sink->write_us) xiaotai_os_host_advance_us(sink->write_us);
    return XIAOTAI_OK;
}

static inline void long_bulk(void *ctx, bool active)
{
    long_sink_t *sink = ctx;
    CHECK(sink->bulk != active);
    sink->bulk = active;
    if (active) ++sink->begins; else ++sink->ends;
}

static inline void long_input(long_sink_t *sink, const char *command)
{
    sink->input = command;
    sink->used = 0;
    sink->text[0] = '\0';
    for (size_t i = 0; *sink->input && i <= strlen(command); ++i) {
        CHECK(xiaotai_at_service(2) == XIAOTAI_OK);
    }
    CHECK(!*sink->input);
}
