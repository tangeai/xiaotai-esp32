// SPDX-License-Identifier: MIT
/* The device report: POST <device>/v1/device/report {"mac":"<fingerprint>"},
 * 15 s, signed with the stored identity when one is VALID, unsigned for a
 * first binding. A usable reply is HTTP 200 with business code 200 and three
 * non-empty strings under "data": the verification code (at most 16
 * characters), the temporary token (shorter than 1024) and the temporary
 * client id (at most 64). Anything else ends the attempt; business code 40901
 * (the previous code is still valid) is such a refusal and is kept in the
 * status. A token about to expire is an old code: reported again after
 * 1000 ms, stale_code_retries reports in all. */
#include <string.h>
#include "binding_internal.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/json.h"
#include "xiaotai/net.h"
#include "xiaotai/port/log.h"
#include "xiaotai/port/mem.h"

static const char *TAG = "binding";

/* Copy the non-empty string at index into out (capacity with terminator). */
static bool copy_string(const xiaotai_json_doc_t *doc, int index, char *out, size_t capacity)
{
    size_t length = 0;
    if (index < 0 || doc->tokens[index].type != XIAOTAI_JSON_STRING) return false;
    if (xiaotai_json_string(doc, index, out, capacity, &length) != XIAOTAI_OK || length == 0U) {
        out[0] = '\0';
        return false;
    }
    return true;
}

xiaotai_err_t binding_report_parse(const char *body, size_t size, binding_attempt_t *attempt)
{
    size_t count = 0;
    if (body == NULL || xiaotai_json_parse(body, size, NULL, 0, &count, NULL) != XIAOTAI_OK || count == 0U ||
        count > BINDING_JSON_TOKENS_MAX) {
        return XIAOTAI_FAIL;
    }
    xiaotai_json_token_t *tokens = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, count, sizeof(*tokens));
    if (tokens == NULL) return XIAOTAI_ERR_NO_MEM;
    xiaotai_json_doc_t doc;
    xiaotai_err_t result = XIAOTAI_FAIL;
    if (xiaotai_json_parse(body, size, tokens, count, &count, &doc) == XIAOTAI_OK &&
        tokens[0].type == XIAOTAI_JSON_OBJECT) {
        int data = xiaotai_json_get(&doc, 0, "data");
        if (data >= 0 && tokens[data].type == XIAOTAI_JSON_OBJECT &&
            copy_string(&doc, xiaotai_json_get(&doc, data, "code"), attempt->code, sizeof(attempt->code)) &&
            copy_string(&doc, xiaotai_json_get(&doc, data, "temp_token"), attempt->temp_token,
                        sizeof(attempt->temp_token)) &&
            copy_string(&doc, xiaotai_json_get(&doc, data, "temp_client_id"), attempt->temp_client_id,
                        sizeof(attempt->temp_client_id))) {
            result = XIAOTAI_OK;
        }
    }
    xiaotai_mem_free(tokens);
    if (result != XIAOTAI_OK) {
        xiaotai_identity_wipe(attempt->code, sizeof(attempt->code));
        xiaotai_identity_wipe(attempt->temp_token, sizeof(attempt->temp_token));
        xiaotai_identity_wipe(attempt->temp_client_id, sizeof(attempt->temp_client_id));
    }
    return result;
}

static void report_again(void *ctx)
{
    if (!binding_serial_current(ctx) || binding_phase() != XIAOTAI_BINDING_REPORTING) return;
    binding_report_begin();
}

static void report_reply(const xiaotai_platform_reply_t *reply, void *ctx)
{
    binding_attempt_t *attempt = binding_attempt();
    if (!binding_serial_current(ctx) || binding_phase() != XIAOTAI_BINDING_REPORTING) {
        /* The attempt this reply belonged to has ended. */
        xiaotai_eventlog_warn(BINDING_LOG_SOURCE, "reply.stale", reply->generation, reply->result);
        return;
    }
    if (reply->result != XIAOTAI_OK) {
        binding_fail(reply->result, "report", XIAOTAI_PLATFORM_NO_CODE);
        return;
    }
    if (reply->http_status != 200 || reply->code != BINDING_REPORT_CODE_OK) {
        binding_count(BINDING_STAT_REPORTS_REFUSED);
        XIAOTAI_LOGW(TAG, "report refused: HTTP=%d code=%ld", reply->http_status, (long)reply->code);
        binding_fail(XIAOTAI_FAIL, "report", reply->code);
        return;
    }
    xiaotai_err_t parsed = binding_report_parse(reply->body, reply->size, attempt);
    if (parsed != XIAOTAI_OK) {
        if (parsed == XIAOTAI_FAIL) binding_count(BINDING_STAT_REPORTS_REFUSED);
        binding_fail(parsed, "report", reply->code);
        return;
    }
    bool stale = false;
    const xiaotai_err_t window = binding_window_open(attempt, &stale);
    if (window != XIAOTAI_OK) {
        binding_fail(window, "report", XIAOTAI_PLATFORM_NO_CODE);
        return;
    }
    if (stale) {
        xiaotai_identity_wipe(attempt->code, sizeof(attempt->code));
        xiaotai_identity_wipe(attempt->temp_token, sizeof(attempt->temp_token));
        xiaotai_identity_wipe(attempt->temp_client_id, sizeof(attempt->temp_client_id));
        binding_count(BINDING_STAT_STALE_CODE_RETRIES);
        xiaotai_eventlog_warn(BINDING_LOG_SOURCE, "stale.code", 0, attempt->stale_reports + 1);
        if (++attempt->stale_reports >= binding_config()->stale_code_retries) {
            binding_fail(XIAOTAI_ERR_TIMEOUT, "report", XIAOTAI_PLATFORM_NO_CODE);
            return;
        }
        binding_timer_arm(BINDING_STALE_DELAY_MS, report_again);
        return;
    }
    binding_enter(XIAOTAI_BINDING_CONNECTING);
    binding_connect_begin();
}

void binding_report_begin(void)
{
    binding_attempt_t *attempt = binding_attempt();
    if (attempt == NULL) return;
    xiaotai_net_status_t net;
    xiaotai_net_status(&net);
    if (net.state != XIAOTAI_NET_UP || !net.mac_valid) {
        binding_fail(XIAOTAI_ERR_PLATFORM_OFFLINE, "report", XIAOTAI_PLATFORM_NO_CODE);
        return;
    }
    xiaotai_err_t result = xiaotai_identity_fingerprint(net.mac, attempt->fingerprint, sizeof(attempt->fingerprint));
    if (result != XIAOTAI_OK) {
        binding_fail(result, "report", XIAOTAI_PLATFORM_NO_CODE);
        return;
    }
    char body[BINDING_REQUEST_BODY_BYTES];
    xiaotai_json_writer_t writer;
    xiaotai_json_writer_init(&writer, body, sizeof(body));
    xiaotai_json_object_begin(&writer, NULL);
    xiaotai_json_put_string(&writer, "mac", attempt->fingerprint);
    xiaotai_json_object_end(&writer);
    result = xiaotai_json_writer_end(&writer, NULL);
    if (result != XIAOTAI_OK) {
        binding_fail(result, "report", XIAOTAI_PLATFORM_NO_CODE);
        return;
    }
    const xiaotai_platform_request_t request = {
        .service = XIAOTAI_PLATFORM_SERVICE_DEVICE,
        .method = XIAOTAI_PLATFORM_POST,
        .path = BINDING_REPORT_PATH,
        .body = body,
        .timeout_ms = BINDING_REPORT_TIMEOUT_MS,
        .auth = attempt->signed_report ? XIAOTAI_PLATFORM_AUTH_SIGNED : XIAOTAI_PLATFORM_AUTH_NONE,
    };
    result = xiaotai_platform_request(&request, report_reply, binding_serial_ctx(attempt->serial));
    if (result != XIAOTAI_OK) binding_fail(result, "report", XIAOTAI_PLATFORM_NO_CODE);
}
