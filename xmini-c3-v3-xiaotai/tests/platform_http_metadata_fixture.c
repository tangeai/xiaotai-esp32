// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform_internal.h"
#include "platform_request_types.h"
#include "platform_routes.h"

#define TAG "platform"
#define JSON_CAPACITY 8192
#define AUTH_FAILED 16
#define UNBOUND 8

static const char *s_device_url = "https://URL_SECRET", *s_voip_url = "https://URL_SECRET";
static const char *s_ai_url = "https://URL_SECRET", *s_call_url = "https://URL_SECRET";
static const char *s_token = "TOKEN_SECRET";
static message_buffer_t s_response;
static atomic_uint s_epoch;
static atomic_int s_last_ack;
static int s_events, online, calls, deliveries, releases, bits, change_epoch;
static esp_err_t http_error, delivered_error;
static http_result_t http_result;
static char logs[4096], response[] = "RESPONSE_SECRET";

static void capture_log(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    size_t used = strlen(logs);
    int count = vsnprintf(logs + used, sizeof(logs) - used, format, args);
    va_end(args);
    assert(count >= 0 && (size_t)count < sizeof(logs) - used);
    strcat(logs, "\n");
}
#define ESP_LOGI(tag, ...) capture_log(__VA_ARGS__)

const char *esp_err_to_name(esp_err_t error)
{
    if (error == ESP_OK) return "ESP_OK";
    if (error == ESP_FAIL) return "ESP_FAIL";
    return "ESP_ERR_INVALID_STATE";
}
bool platform_is_online(void) { return online; }
static void offline(void) { online = 0; }
static void xEventGroupSetBits(int events, int value) { (void)events; bits |= value; }
esp_err_t platform_http(const char *url, esp_http_client_method_t method, const char *body,
                       const char *token, const http_header_t *headers, size_t header_count,
                       message_buffer_t *buffer, size_t limit, http_result_t *result)
{
    assert(strstr(logs, "request begin:") && !strstr(logs, "HTTP done:"));
    assert(strstr(url, "URL_SECRET") && token == s_token && headers == NULL && header_count == 0);
    assert(limit == JSON_CAPACITY && (method == HTTP_METHOD_GET ? body == NULL : strcmp(body, "BODY_SECRET") == 0));
    *result = http_result;
    buffer->data = response;
    ++calls;
    if (change_epoch) atomic_fetch_add(&s_epoch, 1);
    return http_error;
}
static void deliver_response(const request_t *request, esp_err_t error, const char *json)
{
    assert(request->generation == 42 && request->epoch == 7);
    if (calls) assert(strstr(logs, "request HTTP done:") && json == response);
    else assert(json == NULL && !strstr(logs, "request HTTP done:"));
    delivered_error = error;
    ++deliveries;
}
void message_buffer_release(message_buffer_t *buffer)
{
    assert(deliveries == 1 && buffer->data == response);
    buffer->data = NULL;
    ++releases;
}

/* ACTUAL_PROCESS_REQUEST */

static void reset(void)
{
    online = 1;
    calls = deliveries = releases = bits = change_epoch = 0;
    logs[0] = 0;
    s_response = (message_buffer_t){0};
    http_error = ESP_OK;
    http_result = (http_result_t){.status = 200, .code = 200};
    atomic_store(&s_epoch, 7);
    atomic_store(&s_last_ack, 0);
}
static void private_metadata_only(void)
{
    char expected[80];
    snprintf(expected, sizeof(expected), "op=%d generation=42 epoch=7", PLATFORM_GROUP_PRESENCE);
    assert(strstr(logs, expected));
    for (size_t index = 0; index < 4; ++index)
        assert(!strstr(logs, (const char *[]){"URL_SECRET", "TOKEN_SECRET", "BODY_SECRET", "RESPONSE_SECRET"}[index]));
}
int main(void)
{
    request_t request = {.operation = PLATFORM_GROUP_PRESENCE, .generation = 42, .epoch = 7, .body = "BODY_SECRET"};
    reset();
    process_request(&request);
    assert(calls == 1 && deliveries == 1 && releases == 1 && delivered_error == ESP_OK);
    assert(strstr(logs, "http=200 code=200 result=ESP_OK"));
    private_metadata_only();
    reset(); http_error = ESP_FAIL; http_result = (http_result_t){.status = 0, .code = -1};
    process_request(&request);
    assert(delivered_error == ESP_FAIL && releases == 1 && strstr(logs, "http=0 code=-1 result=ESP_FAIL"));
    private_metadata_only();
    reset(); http_result.code = 40400;
    process_request(&request);
    assert(delivered_error == ESP_FAIL && strstr(logs, "http=200 code=40400 result=ESP_FAIL"));
    reset(); change_epoch = 1;
    process_request(&request);
    assert(delivered_error == ESP_ERR_INVALID_STATE && atomic_load(&s_epoch) == 8);
    private_metadata_only();
    reset(); online = 0;
    process_request(&request);
    assert(calls == 0 && deliveries == 1 && releases == 0 && delivered_error == ESP_ERR_INVALID_STATE);
    private_metadata_only();
    reset(); atomic_store(&s_epoch, 8);
    process_request(&request);
    assert(calls == 0 && delivered_error == ESP_ERR_INVALID_STATE);
    reset(); request.operation = PLATFORM_OPERATION_COUNT;
    process_request(&request);
    assert(calls == 0 && delivered_error == ESP_ERR_INVALID_ARG);
    reset(); request.operation = PLATFORM_GROUP_ASSIGNMENT;
    process_request(&request);
    assert(calls == 1 && delivered_error == ESP_OK);
    reset(); request.operation = PLATFORM_GROUP_PRESENCE;
    http_result = (http_result_t){.status = 410, .code = 6006};
    process_request(&request);
    assert((bits & UNBOUND) && !online && delivered_error == ESP_ERR_INVALID_STATE && atomic_load(&s_last_ack) == -1);
    reset(); http_result.status = 401;
    process_request(&request);
    assert(bits & AUTH_FAILED);
    puts("Actual process_request metadata/ownership/privacy cases PASS");
    return 0;
}
