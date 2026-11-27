// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_recognition_config.py
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "product_platform.h"
#include "xiaotai/at_command.h"
#include "xiaotai/framework.h"
#include "xiaotai/identity.h"
#include "xiaotai/modules/recognition_config.h"
#include "xiaotai/platform.h"
#include "xiaotai/port/os.h"
#include "xiaotai_product.h"

void product_on_owner(void (*fn)(void *), void *ctx);
void product_recognition_config_prepare(void);
void product_recognition_config_run(void);
xiaotai_err_t __real_xiaotai_at_notify(const char *line);
xiaotai_err_t __wrap_xiaotai_at_notify(const char *line);

enum { WAIT_MS = 8000 };
enum { SUCCESS, BUSINESS_ERROR, RETIRED };
static atomic_uint scenario, gets, profiles, notices;
static xiaotai_signal_t blocked, release, answered;
static bool xmini;
static const char url[] = "http://recognition.test:8081/tenant" XIAOTAI_RECOGNITION_CONFIG_PATH;
static const char discovery[] =
    "{\"device-srv\":\"http://fixture.invalid/device\",\"mqtt-srv\":\"mqtts://fixture.invalid\","
    "\"ai-srv\":\"http://fixture.invalid/ai\",\"call-srv\":\"http://fixture.invalid/call\","
    "\"voip-srv\":\"http://fixture.invalid/voip\",\"tirtc-srv\":\"http://fixture.invalid/rtc\","
    "\"recognition-srv\":\"http://recognition.test:8081/tenant\"}";
static const char old_s3[] =
    "{\"code\":200,\"msg\":\"private text not exposed\",\"data\":{"
    "\"policy\":{\"enabled\":true,\"allowed\":true,\"monthly_limit\":100,\"revision\":4,"
    "\"schemes\":[{\"scheme_id\":\"fixture\",\"scheme_version\":2}]},"
    "\"effective_enabled\":true,\"upload_limits\":{\"max_file_bytes\":320044,\"max_audio_seconds\":10},"
    "\"usage\":{\"used\":3,\"input_tokens\":8,\"output_tokens\":9,\"tts_characters\":7,\"unknown_calls\":1}}}";
static const char old_xmini[] =
    "{\"code\":200,\"data\":{\"policy\":{\"enabled\":false,\"allowed\":true,\"monthly_limit\":100,"
    "\"revision\":0,\"schemes\":[]},\"effective_enabled\":false,"
    "\"upload_limits\":{\"max_file_bytes\":5242880,\"max_audio_seconds\":60},"
    "\"usage\":{\"used\":0,\"input_tokens\":0,\"output_tokens\":0,\"tts_characters\":0,\"unknown_calls\":2}}}";

static void bearer(const xiaotai_http_host_request_t *req)
{
    unsigned authorization = 0;
    for (size_t i = 0; i < req->header_count; ++i) {
        if (strcmp(req->headers[i].name, "Authorization")) continue;
        assert(!strcmp(req->headers[i].value, "Bearer a.eyJleHAiOjE3MDAwMDA1MDB9.b"));
        ++authorization;
    }
    assert(authorization == 1);
}

static void profile(const xiaotai_http_host_request_t *req)
{
    assert(req->method == XIAOTAI_HTTP_POST && req->body && req->body_size);
    assert(!strcmp(req->url, "http://fixture.invalid/device/v1/device/profile"));
    bearer(req);
    atomic_fetch_add(&profiles, 1U);
}

static void server(void *ctx, const xiaotai_http_host_request_t *req, xiaotai_http_host_reply_t *reply)
{
    (void)ctx;
    if (!strcmp(req->url, "http://fixture.invalid/device/hold-config-queue")) {
        assert(req->method == XIAOTAI_HTTP_GET);
        xiaotai_signal_give(&blocked);
        assert(xiaotai_signal_take(&release, WAIT_MS));
        reply->status = 200;
        return;
    }
    assert(!strcmp(req->url, url));
    assert(req->method == XIAOTAI_HTTP_GET && !req->streamed && !req->body && !req->body_size &&
           !req->has_content_length);
    bearer(req);
    atomic_fetch_add(&gets, 1U);
    const char *body = atomic_load(&scenario) == BUSINESS_ERROR ? "{\"code\":500}" : (xmini ? old_xmini : old_s3);
    reply->status = 200;
    reply->body = (const uint8_t *)body;
    reply->body_size = strlen(body);
}

xiaotai_err_t __wrap_xiaotai_at_notify(const char *line)
{
    if (!strncmp(line, "+RECOGNITION: ", sizeof("+RECOGNITION: ") - 1U)) {
        assert(xiaotai_on_owner());
        const unsigned current = atomic_load(&scenario);
        const char *expected = current == RETIRED ? "+RECOGNITION: error=ESP_ERR_INVALID_STATE http=0 code=-1" :
            current == BUSINESS_ERROR ? (xmini ? "+RECOGNITION: error=ESP_FAIL http=200 code=500" :
                                                "+RECOGNITION: error=ESP_ERR_INVALID_RESPONSE http=200 code=500") :
            xmini ? "+RECOGNITION: http=200 code=200 enabled=0 allowed=1 effective=0 revision=0 schemes=0 "
                    "max-bytes=5242880 max-seconds=60 monthly-limit=100 used=0 input-tokens=0 output-tokens=0 "
                    "tts-characters=0 unknown-calls=2" :
                    "+RECOGNITION: http=200 code=200 enabled=1 allowed=1 effective=1 revision=4 schemes=1 "
                    "max-bytes=320044 max-seconds=10 monthly-limit=100 used=3 input-tokens=8 output-tokens=9 "
                    "tts-characters=7 unknown-calls=1";
        if (strcmp(line, expected)) fprintf(stderr, "%s: wanted '%s', got '%s'\n", XIAOTAI_PRODUCT_NAME, expected, line);
        assert(!strcmp(line, expected));
        atomic_fetch_add(&notices, 1U);
        xiaotai_signal_give(&answered);
    }
    return __real_xiaotai_at_notify(line);
}

void product_recognition_config_prepare(void)
{
    xmini = !strcmp(XIAOTAI_PRODUCT_NAME, "xiaotai-xmini-c3-v3");
    assert(xiaotai_signal_init(&blocked) == XIAOTAI_OK && xiaotai_signal_init(&release) == XIAOTAI_OK &&
           xiaotai_signal_init(&answered) == XIAOTAI_OK);
    product_platform_prepare(true);
    product_platform_discovery_response(200, discovery, "mqtts://fixture.invalid");
    product_platform_http_responder(server, NULL);
    if (xmini) product_platform_profile_observer(profile);
}

static void ignore_reply(void *ctx, const char *line) { (void)ctx; (void)line; }

static void read_config(xiaotai_err_t expected)
{
    const xiaotai_reply_t reply = {.write = ignore_reply};
    const xiaotai_err_t result = xiaotai_command_run("AT+RECOGNITION CONFIG", &reply, WAIT_MS);
    if (result != expected)
        fprintf(stderr, "%s: config admission got %d, wanted %d\n", XIAOTAI_PRODUCT_NAME, result, expected);
    assert(result == expected);
}

static void discard(const xiaotai_platform_reply_t *reply, void *ctx) { (void)reply; (void)ctx; }
static void retire_generation(void *ctx)
{
    (void)ctx;
    const uint32_t previous = xiaotai_identity_generation();
    assert(xiaotai_identity_generation_advance() != previous);
}

static void idle(unsigned expected_profiles, unsigned expected_gets, unsigned expected_notices)
{
    xiaotai_platform_status_t status;
    for (unsigned i = 0; i < WAIT_MS; i += 5U) {
        xiaotai_platform_status(&status);
        if (atomic_load(&profiles) >= expected_profiles && (!xmini || status.profile_ready) &&
            !xiaotai_platform_exchanges_busy()) break;
        xiaotai_delay_ms(5);
    }
    assert(atomic_load(&profiles) == expected_profiles && !xiaotai_platform_exchanges_busy());
    assert(!xmini || status.profile_ready);
    assert(atomic_load(&gets) == expected_gets && atomic_load(&notices) == expected_notices);
}

void product_recognition_config_run(void)
{
    read_config(XIAOTAI_ERR_INVALID_STATE);
    product_platform_online();
    const unsigned expected_profiles = xmini ? 1U : 0U;
    idle(expected_profiles, 0, 0);

    read_config(XIAOTAI_OK);
    assert(xiaotai_signal_take(&answered, WAIT_MS));
    idle(expected_profiles, 1, 1);
    atomic_store(&scenario, BUSINESS_ERROR);
    read_config(XIAOTAI_OK);
    assert(xiaotai_signal_take(&answered, WAIT_MS));
    idle(expected_profiles, 2, 2);

    const xiaotai_platform_request_t hold = {.service = "device", .path = "/hold-config-queue",
        .method = XIAOTAI_PLATFORM_GET, .auth = XIAOTAI_PLATFORM_AUTH_SESSION, .timeout_ms = 15000U};
    assert(xiaotai_platform_request(&hold, discard, NULL) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&blocked, WAIT_MS));
    atomic_store(&scenario, RETIRED);
    read_config(XIAOTAI_OK);
    assert(atomic_load(&gets) == 2 && atomic_load(&notices) == 2);
    product_on_owner(retire_generation, NULL);
    xiaotai_signal_give(&release);
    assert(xiaotai_signal_take(&answered, WAIT_MS));
    idle(expected_profiles, 2, 3);
    printf("%s: tenant GET + session bearer x2, profile=%u, retired config wire=0\n",
           XIAOTAI_PRODUCT_NAME, expected_profiles);
    xiaotai_signal_deinit(&answered);
    xiaotai_signal_deinit(&release);
    xiaotai_signal_deinit(&blocked);
}
