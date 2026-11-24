// SPDX-License-Identifier: MIT
#include "recognition_protocol.h"
#include "xiaotai/platform.h"
#include "xiaotai_mem_host.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static bool cancelled(void *ctx) { return *(const bool *)ctx; }

static void receipt_test(void)
{
    const char *states[] = {"queued", "processing", "succeeded", "partial_failed", "failed", "unknown", "cancelled"};
    const char valid[] = "{\"code\":200,\"data\":{\"status\":\"queued\",\"device_id\":\"device-1\","
        "\"request_id\":\"task_1\",\"status_revision\":1,\"results\":[]}}";
    char body[512];
    rc_receipt_t receipt = {0};
    int code;
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); ++i) {
        int n = snprintf(body, sizeof(body), "{\"code\":200,\"data\":{\"status\":\"%s\",\"device_id\":\"device-1\","
            "\"request_id\":\"task_1\",\"status_revision\":9007199254740991,\"results\":[]}}", states[i]);
        assert(n > 0 && (size_t)n < sizeof(body));
        assert(rc_receipt_decode(body, (size_t)n, 200, "device-1", "task_1", &code, &receipt) == XIAOTAI_OK);
        assert(code == 200 && (size_t)receipt.state == i && receipt.revision == XIAOTAI_RECOGNITION_REVISION_MAX);
        assert(rc_receipt_decode(body, (size_t)n, 202, "device-1", "task_1", &code, &receipt) ==
               (i == 0 ? XIAOTAI_OK : RC_INVALID_RESPONSE));
    }
    assert(rc_receipt_decode(valid, strlen(valid), 200, "other", "task_1", &code, &receipt) == RC_INVALID_RESPONSE);
    assert(rc_receipt_decode(valid, strlen(valid), 200, "device-1", "other", &code, &receipt) == RC_INVALID_RESPONSE);
    assert(rc_receipt_decode(valid, strlen(valid), 201, "device-1", "task_1", &code, &receipt) == RC_INVALID_RESPONSE);
    const char *const invalid[] = {"{\"code\":40901,\"co\\u0064e\":40901}", "{\"code\":\"40901\"}",
        "{\"code\":1.4}", "{\"code\":2147483648}", "{\"code\":40901} trailing", NULL};
    for (size_t i = 0; invalid[i]; ++i) {
        assert(rc_receipt_decode(invalid[i], strlen(invalid[i]), 409, "device-1", "task_1", &code, &receipt) == RC_INVALID_RESPONSE);
        assert(code == -1);
    }
    const char conflict[] = "{\"code\":40901}";
    assert(rc_receipt_decode(conflict, strlen(conflict), 409, "device-1", "task_1", &code, &receipt) == RC_INVALID_RESPONSE);
    assert(code == 40901);
    const char nul_id[] = "{\"code\":200,\"data\":{\"status\":\"queued\",\"device_id\":\"device-1\\u0000other\","
        "\"request_id\":\"task_1\",\"status_revision\":1,\"results\":[]}}";
    assert(rc_receipt_decode(nul_id, strlen(nul_id), 202, "device-1", "task_1", &code, &receipt) == RC_INVALID_RESPONSE);
    xiaotai_mem_host_fail_nth(1);
    assert(rc_receipt_decode(valid, strlen(valid), 202, "device-1", "task_1", &code, &receipt) == XIAOTAI_ERR_NO_MEM);
    assert(!xiaotai_mem_host_outstanding());
}

int main(void)
{
    receipt_test();
    xiaotai_recognition_policy_t policy = {.enabled = true, .allowed = true, .effective_enabled = true,
        .revision = 4, .monthly_limit = 10, .scheme_count = 1, .max_file_bytes = 16044, .max_audio_seconds = 1};
    const uint8_t random[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0xab, 0xcd, 0xef};
    const char match[] = "--xiaotai-00010203040506070809abcdef000000";
    static rc_multipart_t form;
    static uint8_t pcm[16000], chunk[2048];
    memcpy(pcm, match, strlen(match));
    assert(rc_multipart_prepare(&form, "task_1", &policy, pcm, sizeof(pcm), 8000, 1, random) == XIAOTAI_ERR_INVALID_ARG);
    pcm[0] = 0;
    pcm[10] = '\r'; pcm[11] = '\n';
    memcpy(pcm + 12, match, strlen(match));
    assert(rc_multipart_prepare(&form, "task_1", &policy, pcm, sizeof(pcm), 8000, 1, random) == XIAOTAI_ERR_INVALID_ARG);
    pcm[11] = 0;
    assert(rc_multipart_prepare(&form, "task_1", &policy, pcm, sizeof(pcm), 8000, 1, random) == XIAOTAI_OK);
    assert(rc_multipart_prepare(&form, "task/1", &policy, pcm, sizeof(pcm), 8000, 1, random) == XIAOTAI_ERR_INVALID_ARG);
    assert(rc_multipart_prepare(&form, "task_1", &policy, NULL, sizeof(pcm), 8000, 1, random) == XIAOTAI_ERR_INVALID_ARG);
    for (size_t i = 0; i < sizeof(pcm); ++i) pcm[i] = (uint8_t)(i % 251);
    assert(rc_multipart_prepare(&form, "task_1", &policy, pcm, sizeof(pcm), 8000, 1, random) == XIAOTAI_OK);
    assert(form.pcm == pcm && form.total_bytes == form.prefix_bytes + sizeof(pcm) + form.suffix_bytes);
    bool cancel = true;
    form.cancelled = cancelled;
    form.cancel_ctx = &cancel;
    size_t count = 99;
    assert(rc_multipart_read(&form, chunk, sizeof(chunk), &count) == XIAOTAI_ERR_PLATFORM_CANCELLED);
    assert(count == 0 && form.offset == 0);
    cancel = false;
    printf("Content-Type: %s\r\nMIME-Version: 1.0\r\n\r\n", form.content_type);
    size_t total = 0;
    while (total < form.total_bytes) {
        size_t cap = total ? sizeof(chunk) : 1;
        assert(rc_multipart_read(&form, chunk, cap, &count) == XIAOTAI_OK);
        assert(count > 0 && count <= cap && count <= RC_STREAM_CHUNK);
        assert(fwrite(chunk, 1, count, stdout) == count);
        total += count;
    }
    assert(form.offset == form.total_bytes);
    assert(rc_multipart_read(&form, chunk, sizeof(chunk), &count) == XIAOTAI_OK && count == 0);
}
