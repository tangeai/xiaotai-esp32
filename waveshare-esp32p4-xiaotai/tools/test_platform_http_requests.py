#!/usr/bin/env python3
"""Inject HTTP setup/I/O failures into production functions; no board or network."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "components/platform_client/src/platform_client.c").read_text(encoding="utf-8")


def function(name):
    start = SOURCE.index("static esp_err_t " + name + "(")
    opening = SOURCE.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        end += 1
    return SOURCE[start:end] + "\n"


output_type = re.search(r"typedef struct \{[^{}]*\} http_output_t;", SOURCE)
assert output_type
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#else
#include <strings.h>
#endif
#include "platform_http_trace.h"
typedef int esp_err_t;
typedef void *esp_http_client_handle_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 257
#define ESP_ERR_INVALID_ARG 258
#define ESP_ERR_INVALID_SIZE 260
#define ESP_ERR_INVALID_RESPONSE 264
#define PLATFORM_DEFAULT_HTTP_TIMEOUT_MS 15000
#define PLATFORM_TTS_HTTP_TIMEOUT_MS 20000
enum { HTTP_EVENT_ON_HEADER, HTTP_EVENT_ON_DATA, HTTP_METHOD_POST };
typedef struct {
    int event_id, data_len;
    void *user_data, *data;
    char *header_key, *header_value;
} esp_http_client_event_t;
typedef struct {
    const char *url;
    esp_err_t (*event_handler)(esp_http_client_event_t *);
    void *user_data;
    int timeout_ms;
    int (*crt_bundle_attach)(void *);
    bool disable_auto_redirect;
} esp_http_client_config_t;
static esp_http_client_config_t config;
static int setters, fail_at, performs, cleanups, begins, ends, logs, zeroes;
static int perform_rc, response_status = 200, status_calls;
static bool init_fails, post_set;
static uint64_t clock_ms = 1000;
static uint32_t last_queue;
static esp_err_t last_err;
static int last_status;
static int64_t esp_timer_get_time(void) { return (int64_t)clock_ms * 1000; }
static int esp_crt_bundle_attach(void *p) { (void)p; return 0; }
static esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *p) {
    config = *p; clock_ms += 2;
    assert(!p->disable_auto_redirect && p->crt_bundle_attach == esp_crt_bundle_attach);
    return init_fails ? NULL : (void *)1;
}
static esp_err_t setting(void) {
    setters++; clock_ms += 5;
    return setters == fail_at ? ESP_ERR_NO_MEM : ESP_OK;
}
static esp_err_t esp_http_client_set_method(esp_http_client_handle_t c, int method) {
    assert(c && method == HTTP_METHOD_POST); post_set = true; return setting();
}
static esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *key, const char *value) {
    assert(c && key && value);
    if (!strcmp(key, "Authorization")) assert(!strcmp(value, "Bearer test-token"));
    return setting();
}
static esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t c, const char *body, int len) {
    assert(c && body && len == (int)strlen(body) && post_set); return setting();
}
void platform_http_trace_begin(platform_http_trace_t *t) {
    *t = (platform_http_trace_t){.enabled=true, .started_ms=(uint32_t)clock_ms}; begins++;
}
void platform_http_trace_end(platform_http_trace_t *t) { assert(t->enabled); ends++; }
static esp_err_t esp_http_client_perform(esp_http_client_handle_t c) {
    assert(c); performs++; clock_ms += 700;
    esp_http_client_event_t e = {.event_id=HTTP_EVENT_ON_HEADER, .user_data=config.user_data,
        .header_key="Content-Type", .header_value="audio/pcm"};
    config.event_handler(&e);
    e.event_id=HTTP_EVENT_ON_DATA; e.data="ab"; e.data_len=2;
    config.event_handler(&e);
    e.data="c"; e.data_len=1; config.event_handler(&e);
    return perform_rc;
}
static int esp_http_client_get_status_code(esp_http_client_handle_t c) {
    assert(c); status_calls++; return performs ? response_status : 0;
}
static esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) {
    assert(c); cleanups++; clock_ms += 1; return 0;
}
static void mbedtls_platform_zeroize(void *p, size_t n) { memset(p, 0, n); zeroes++; }
static void http_log_result(const char *url, bool post, uint32_t start, uint32_t queue,
        uint32_t prep, uint32_t io, esp_err_t err, int status,
        const platform_http_trace_t *trace, bool redirected) {
    (void)url; (void)post; (void)start; (void)prep; (void)redirected;
    assert(io == (performs ? 700U : 0U));
    assert(trace->enabled == (performs != 0));
    logs++; last_queue=queue; last_err=err; last_status=status;
}
static void reset(void) {
    setters=fail_at=performs=cleanups=begins=ends=logs=zeroes=status_calls=0;
    perform_rc=0; response_status=200; init_fails=post_set=false;
    clock_ms=1000;
}
'''
code += output_type.group(0) + "\n"
for name in ("http_event", "http_request_timed", "http_request", "http_binary_event", "http_binary_get"):
    code += function(name)
code += r'''
int main(void) {
    char response[32]; int status=999;
    const char *names[]={"X-Device-Id", "X-Signature"};
    const char *values[]={"test-device", "test-signature"};
    reset();
    assert(http_request_timed("http://test/v1/ai/token", "{}", "test-token", names, values, 2,
        response, sizeof(response), 1234, &status, 750)==ESP_OK);
    assert(setters==6 && performs==1 && cleanups==1 && begins==1 && ends==1 && logs==1);
    assert(config.timeout_ms==1234 && last_queue==250 && !last_err && last_status==200);
    assert(status==200 && !strcmp(response,"abc") && zeroes==1);
    for(int failure=1; failure<=6; ++failure) {
        reset(); fail_at=failure; status=999; strcpy(response,"stale");
        assert(http_request("http://test", "{}", "test-token", names, values, 2,
            response, sizeof(response), 0, &status)==ESP_ERR_NO_MEM);
        assert(setters==failure && !performs && cleanups==1 && !begins && !ends);
        assert(!status_calls && !status && !response[0] && last_err==ESP_ERR_NO_MEM && zeroes==1);
    }
    reset(); init_fails=true; status=999;
    assert(http_request("http://test",NULL,NULL,NULL,NULL,0,response,sizeof(response),0,&status)==ESP_ERR_NO_MEM);
    assert(!performs && !cleanups && !status && logs==1);
    reset();
    assert(http_request("http://test",NULL,NULL,NULL,NULL,0,response,sizeof(response),0,&status)==ESP_OK);
    assert(!setters && !post_set && performs==1 && config.timeout_ms==15000);
    reset();
    assert(http_request("http://test","",NULL,NULL,NULL,0,response,sizeof(response),0,&status)==ESP_OK);
    assert(post_set && setters==3 && performs==1);
    reset(); perform_rc=-9;
    assert(http_request("http://test","{}",NULL,NULL,NULL,0,response,sizeof(response),0,&status)==-9);
    assert(performs==1 && cleanups==1 && begins==ends && last_err==-9);
    reset(); response_status=409;
    assert(http_request("http://test","{}",NULL,NULL,NULL,0,response,sizeof(response),0,&status)==ESP_OK);
    assert(status==409 && last_status==409 && performs==1); /* Preserve HTTP failure body. */
    reset();
    assert(http_request("http://test",NULL,NULL,NULL,NULL,0,response,3,0,&status)==ESP_ERR_INVALID_SIZE);
    assert(!strcmp(response,"ab") && performs==1 && cleanups==1);
    reset();
    assert(http_request("http://test",NULL,NULL,NULL,NULL,0,NULL,0,0,&status)==ESP_ERR_INVALID_ARG);
    assert(!performs && !cleanups);
    char bearer[1100]; memset(bearer,'x',sizeof(bearer)-1); bearer[sizeof(bearer)-1]=0;
    reset();
    assert(http_request("http://test",NULL,bearer,NULL,NULL,0,response,sizeof(response),0,&status)==ESP_ERR_INVALID_SIZE);
    assert(!performs && cleanups==1);
    uint8_t pcm[32]; size_t length=99;
    reset(); fail_at=1; status=999;
    assert(http_binary_get("http://test","test-token",pcm,sizeof(pcm),&length,&status)==ESP_ERR_NO_MEM);
    assert(!performs && !length && !status && cleanups==1 && zeroes==1);
    reset(); init_fails=true; length=99; status=999;
    assert(http_binary_get("http://test","test-token",pcm,sizeof(pcm),&length,&status)==ESP_ERR_NO_MEM);
    assert(!performs && !length && !status);
    reset();
    assert(http_binary_get("http://test","test-token",pcm,sizeof(pcm),&length,&status)==ESP_OK);
    assert(length==3 && status==200 && !memcmp(pcm,"abc",3));
    puts("PASS: HTTP setup failure, cleanup, no retry, status/body, timeout, overflow and PCM authorization");
}
'''
with tempfile.TemporaryDirectory(prefix="platform-http-request-") as directory:
    temp = Path(directory)
    path = temp / "test.c"
    path.write_text(code, encoding="utf-8")
    binary = temp / ("test.exe" if os.name == "nt" else "test")
    sanitizers = [] if os.name == "nt" else ["-fsanitize=address,undefined"]
    subprocess.run([os.environ.get("CC", "gcc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    *sanitizers, "-I", str(ROOT / "components/platform_client/src"),
                    str(path), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
