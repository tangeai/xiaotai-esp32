"""Exercise real HTTP reuse/cleanup with the selected IDF URL parser, without I/O."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'components/platform_client/src/platform_client.c').read_text(encoding='utf-8')
idf = Path(os.environ['IDF_PATH'])
parser = idf / 'components/http_parser/http_parser.c'
assert parser.is_file(), 'Select ESP-IDF before running this test'


def function(signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 0
    for index in range(opening, len(source)):
        depth += (source[index] == '{') - (source[index] == '}')
        if depth == 0:
            return source[start:index + 1] + '\n'
    raise AssertionError(signature)


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
#include "http_parser.h"
#include "platform_http_trace.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 257
#define ESP_ERR_INVALID_ARG 258
#define ESP_ERR_NOT_FOUND 261
#define ESP_ERR_INVALID_SIZE 260
#define PLATFORM_DEFAULT_HTTP_TIMEOUT_MS 15000U
#define PLATFORM_HTTP_REUSE_IDLE_MS 3000U
enum { HTTP_EVENT_ON_HEADER, HTTP_EVENT_ON_DATA, HTTP_METHOD_GET, HTTP_METHOD_POST };
typedef struct client *esp_http_client_handle_t;
typedef struct {
    int event_id, data_len;
    void *user_data, *data;
    const char *header_key;
} esp_http_client_event_t;
typedef struct {
    const char *url;
    esp_err_t (*event_handler)(esp_http_client_event_t *);
    void *user_data;
    int timeout_ms;
    int (*crt_bundle_attach)(void *);
    bool disable_auto_redirect;
} esp_http_client_config_t;
struct client {
    esp_http_client_config_t config;
    int method;
    const char *body;
    unsigned headers;
};
static uint64_t clock_ms;
static unsigned inits, cleanups, performs, last_headers;
static int last_method, status_code = 200, perform_rc, fail_url;
static bool persistent = true, redirect, had_body;
static int64_t esp_timer_get_time(void) { return (int64_t)clock_ms * 1000; }
static int esp_crt_bundle_attach(void *p) { (void)p; return 0; }
static unsigned header_bit(const char *key) {
    if (!strcmp(key, "Authorization")) return 1;
    if (!strcmp(key, "Content-Type")) return 2;
    if (!strcmp(key, "Content-Length")) return 4;
    if (!strcmp(key, "X-Signature")) return 8;
    if (!strcmp(key, "X-Device-Id")) return 16;
    assert(0); return 0;
}
static esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg) {
    struct client *c = calloc(1, sizeof(*c)); assert(c); c->config = *cfg; ++inits; return c;
}
static int esp_http_client_cleanup(esp_http_client_handle_t c) { ++cleanups; free(c); return 0; }
static bool esp_http_client_is_persistent_connection(esp_http_client_handle_t c) { assert(c); return persistent; }
static int esp_http_client_set_url(esp_http_client_handle_t c, const char *url) {
    if (fail_url) return ESP_ERR_NO_MEM;
    c->config.url = url; return 0;
}
static int esp_http_client_set_timeout_ms(esp_http_client_handle_t c, int ms) { c->config.timeout_ms = ms; return 0; }
static int esp_http_client_set_user_data(esp_http_client_handle_t c, void *p) { c->config.user_data = p; return 0; }
static int esp_http_client_set_method(esp_http_client_handle_t c, int method) { c->method = method; return 0; }
static int esp_http_client_set_post_field(esp_http_client_handle_t c, const char *p, int n) {
    assert((p && (size_t)n == strlen(p)) || (!p && n == 0)); c->body = p;
    if (!p) { bool found=c->headers & 2; c->headers &= ~2U; return found ? 0 : ESP_ERR_NOT_FOUND; }
    return 0;
}
static int esp_http_client_set_header(esp_http_client_handle_t c, const char *key, const char *value) {
    assert(value); c->headers |= header_bit(key); return 0;
}
static int esp_http_client_delete_header(esp_http_client_handle_t c, const char *key) { c->headers &= ~header_bit(key); return 0; }
static int esp_http_client_get_status_code(esp_http_client_handle_t c) { assert(c); return status_code; }
static int esp_http_client_perform(esp_http_client_handle_t c) {
    ++performs; clock_ms += 100; last_headers = c->headers; last_method = c->method; had_body = c->body != NULL;
    esp_http_client_event_t e = {.event_id=HTTP_EVENT_ON_DATA, .data="{}", .data_len=2, .user_data=c->config.user_data};
    c->config.event_handler(&e);
    if (redirect) { e.event_id=HTTP_EVENT_ON_HEADER; e.header_key="Location"; c->config.event_handler(&e); }
    return perform_rc;
}
void platform_http_trace_begin(platform_http_trace_t *t) { *t=(platform_http_trace_t){.started_ms=(uint32_t)clock_ms}; }
void platform_http_trace_end(platform_http_trace_t *t) { (void)t; }
static void mbedtls_platform_zeroize(void *p, size_t n) { memset(p, 0, n); }
static void http_log_result(const char *u, bool p, uint32_t s, uint32_t q, uint32_t pre,
        uint32_t io, int err, int status, const platform_http_trace_t *t, bool r) {
    (void)u; (void)p; (void)s; (void)q; (void)pre; (void)io; (void)err; (void)status; (void)t; (void)r;
}
'''
for name in ('http_output_t', 'http_reuse_t'):
    code += re.search(r'typedef struct \{[^{}]*\} ' + name + ';', source).group(0) + '\n'
code += 'static http_reuse_t s_http_reuse;\n'
for signature in ('static bool http_origin(', 'static void http_reuse_close(',
                  'static void http_reuse_expire(', 'static esp_http_client_handle_t http_reuse_take(',
                  'static void http_reuse_put(', 'static esp_err_t http_event(',
                  'static esp_err_t http_request_timed(', 'static esp_err_t http_request('):
    code += function(signature)
code += r'''
static void request(const char *url, const char *body, const char *bearer,
        const char *const *keys, const char *const *values, size_t n, int expected) {
    char output[32]; int status;
    assert(http_request_timed(url, body, bearer, keys, values, n,
        output, sizeof(output), 15000, &status, 0) == expected);
    if (s_http_reuse.client) {
        assert(!s_http_reuse.client->body && !s_http_reuse.client->config.user_data);
        assert(!s_http_reuse.client->headers && s_http_reuse.client->method == HTTP_METHOD_GET);
    }
}
int main(void) {
    char origin[256];
    assert(http_origin("https://[::1]:443/a?q=1", origin, sizeof(origin)) && !strcmp(origin,"https://[::1]:443"));
    assert(!http_origin("http://user:secret@test/a", origin, sizeof(origin)));
    assert(!http_origin("/relative", origin, sizeof(origin)));
    assert(!http_origin("ftp://test/a", origin, sizeof(origin)));
    assert(!http_origin("http://test/a", origin, 3));
    const char *keys[] = {"X-Signature", "X-Device-Id"}, *values[] = {"signature", "device"};
    request("http://api/token", "", NULL, keys, values, 2, 0);
    assert(inits==1 && cleanups==0 && performs==1 && last_headers==26 && had_body);
    request("http://api/profile", "{}", "token", NULL, NULL, 0, 0);
    assert(inits==1 && cleanups==0 && last_headers==3 && last_method==HTTP_METHOD_POST);
    request("http://api/contacts?q=1", NULL, "new-token", NULL, NULL, 0, 0);
    assert(inits==1 && last_headers==1 && last_method==HTTP_METHOD_GET && !had_body);
    request("https://api/contacts", NULL, NULL, NULL, NULL, 0, 0);
    assert(inits==2 && cleanups==1); /* Scheme boundary. */
    request("https://api:444/contacts", NULL, NULL, NULL, NULL, 0, 0);
    assert(inits==3 && cleanups==2); /* Port boundary. */
    request("https://other/contacts", NULL, NULL, NULL, NULL, 0, 0);
    assert(inits==4 && cleanups==3); /* Host boundary. */
    clock_ms += 3000; http_reuse_expire();
    assert(!s_http_reuse.client && inits==cleanups);
    persistent=false;
    request("http://api/contacts", NULL, NULL, NULL, NULL, 0, 0);
    assert(!s_http_reuse.client && inits==cleanups);
    persistent=true; redirect=true;
    request("http://api/contacts", NULL, NULL, NULL, NULL, 0, 0);
    assert(!s_http_reuse.client && inits==cleanups);
    redirect=false; status_code=401;
    request("http://api/contacts", NULL, "expired", NULL, NULL, 0, 0);
    assert(!s_http_reuse.client && inits==cleanups);
    status_code=200;
    request("http://api/contacts", NULL, NULL, NULL, NULL, 0, 0);
    unsigned before=performs; perform_rc=-7;
    request("http://api/call", "{}", "token", NULL, NULL, 0, -7);
    assert(performs==before+1 && !s_http_reuse.client && inits==cleanups); /* No POST replay. */
    perform_rc=0;
    request("http://api/contacts", NULL, NULL, NULL, NULL, 0, 0);
    before=performs; fail_url=1;
    request("http://api/contacts", NULL, NULL, NULL, NULL, 0, ESP_ERR_NO_MEM);
    assert(performs==before && !s_http_reuse.client && inits==cleanups);
    fail_url=0; clock_ms=UINT32_MAX-1000U;
    request("http://api/contacts", NULL, NULL, NULL, NULL, 0, 0);
    clock_ms+=3000; http_reuse_expire();
    assert(!s_http_reuse.client && inits==cleanups);
    request("http://api/contacts", NULL, "token", NULL, NULL, 0, 0);
    http_reuse_close(); http_reuse_close(); /* Rebind: idempotent, no old credentials/handle. */
    assert(inits==cleanups);
    char output[32]; int status;
    assert(http_request("http://api/report", "{}", NULL, keys, values, 2,
        output, sizeof(output), 15000, &status) == 0);
    assert(!s_http_reuse.client && inits==cleanups); /* No socket during user-input wait. */
    puts("PASS: HTTP reuse origin, identity scrub, POST-to-GET, expiry, rebind, close, errors, no replay");
}
'''
with tempfile.TemporaryDirectory(prefix='http-reuse-') as directory:
    temp = Path(directory)
    path = temp / 'test.c'
    path.write_text(code, encoding='utf-8')
    binary = temp / ('test.exe' if os.name == 'nt' else 'test')
    sanitizers = [] if os.name == 'nt' else ['-fsanitize=address,undefined']
    subprocess.run([os.environ.get('CC', 'gcc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                    *sanitizers, '-I', str(parser.parent), '-I', str(root / 'components/platform_client/src'),
                    str(path), str(parser), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)

assert 'static EXT_RAM_BSS_ATTR http_reuse_t s_http_reuse' in source
assert 'http_reuse_expire();' in function('static void request_loop(')
assert 'http_reuse_close();' in function('esp_err_t platform_client_prepare_rebind(')
