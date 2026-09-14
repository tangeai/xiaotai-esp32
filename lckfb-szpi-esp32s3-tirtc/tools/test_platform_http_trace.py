"""Offline tests of scoped HTTP timing and endpoint redaction; no network/board."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = root / 'components/platform_client/src/platform_http_trace.c'
platform = (source.parent / 'platform_client.c').read_text(encoding='utf-8')


def function(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    depth = 0
    for index in range(opening, len(text)):
        depth += (text[index] == '{') - (text[index] == '}')
        if depth == 0:
            return text[start:index + 1]
    raise AssertionError(signature)


with tempfile.TemporaryDirectory(prefix='platform-http-trace-') as temporary:
    temp = Path(temporary)
    headers = {
        'esp_timer.h': '#include <stdint.h>\nint64_t esp_timer_get_time(void);\n',
        'esp_transport.h': 'typedef void *esp_transport_handle_t;\n',
        'freertos/FreeRTOS.h': 'typedef void *TaskHandle_t;\n',
        'freertos/task.h': 'TaskHandle_t xTaskGetCurrentTaskHandle(void);\n',
        'lwip/netdb.h': '#include <netdb.h>\n',
    }
    for name, text in headers.items():
        path = temp / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding='utf-8')
    code = r'''
#define _POSIX_C_SOURCE 200112L
#include <assert.h>
#include <string.h>
#include <stdio.h>
''' + '#include "' + source.as_posix() + '"\n' + r'''
static uint64_t clock_ms = 1000;
static TaskHandle_t task = (void *)1;
static int dns_result, read_result = 12, write_result = -2;
static unsigned native_dns_calls, native_writes;
static struct addrinfo result_storage;
int64_t esp_timer_get_time(void) { return (int64_t)clock_ms * 1000; }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return task; }
int __real_lwip_getaddrinfo(const char *node, const char *service,
        const struct addrinfo *hints, struct addrinfo **result) {
    (void)service; (void)hints; assert(strcmp(node, "host") == 0);
    ++native_dns_calls; clock_ms += 1054; *result = &result_storage;
    errno = EHOSTUNREACH; return dns_result;
}
int __real_esp_transport_connect(esp_transport_handle_t t, const char *host, int port, int timeout) {
    assert(t == (void *)8 && port == 80 && timeout == 15000);
    struct addrinfo *result = NULL;
    int rc = __wrap_lwip_getaddrinfo(host, NULL, NULL, &result);
    assert(result == &result_storage);
    clock_ms += 18; errno = EAGAIN; return rc;
}
int __real_esp_transport_write(esp_transport_handle_t t, const char *data, int n, int timeout) {
    (void)t; (void)timeout; assert(data && n > 0);
    ++native_writes; clock_ms += 3; errno = EPIPE;
    return write_result == -2 ? n : write_result;
}
int __real_esp_transport_read(esp_transport_handle_t t, char *data, int n, int timeout) {
    (void)t; (void)timeout; assert(n >= read_result);
    if (read_result > 0) memset(data, 'x', (size_t)read_result);
    clock_ms += 305; errno = EAGAIN; return read_result;
}
'''
    code += function(platform, 'static const char *http_api_name(') + '\n'
    code += r'''
int main(void) {
    platform_http_trace_t trace, other;
    struct addrinfo *result = NULL;
    assert(__wrap_lwip_getaddrinfo("host", NULL, NULL, &result) == 0);
    assert(errno == EHOSTUNREACH && result == &result_storage);
    platform_http_trace_begin(&trace);
    assert(trace.enabled && trace.first_rx_ms == UINT32_MAX);
    task = (void *)2;
    platform_http_trace_begin(&other);
    assert(!other.enabled);
    assert(__wrap_lwip_getaddrinfo("host", NULL, NULL, &result) == 0);
    platform_http_trace_end(&other);
    assert(trace.dns_calls == 0);
    task = (void *)1;
    assert(__wrap_esp_transport_connect((void *)8, "host", 80, 15000) == 0);
    assert(errno == EAGAIN && trace.connect_ms == 1072 && trace.dns_ms == 1054);
    assert(trace.dns_calls == 1 && trace.connect_calls == 1);
    assert(__wrap_esp_transport_write((void *)8, "HEAD", 4, 15) == 4);
    assert(__wrap_esp_transport_write((void *)8, "BODY", 4, 15) == 4);
    assert(errno == EPIPE && trace.tx_bytes == 8 && trace.write_calls == 2);
    char buffer[64];
    assert(__wrap_esp_transport_read((void *)8, buffer, sizeof(buffer), 15) == 12);
    assert(errno == EAGAIN && trace.first_rx_ms - trace.write_ms == 305);
    uint32_t first = trace.first_rx_ms;
    assert(__wrap_esp_transport_read((void *)8, buffer, sizeof(buffer), 15) == 12);
    assert(trace.first_rx_ms == first && trace.rx_bytes == 24);
    platform_http_trace_end(&trace);
    platform_http_trace_end(&trace);
    assert(__wrap_lwip_getaddrinfo("host", NULL, NULL, &result) == 0);
    assert(trace.dns_calls == 1);
    platform_http_trace_begin(&trace);
    dns_result = -7;
    assert(__wrap_esp_transport_connect((void *)8, "host", 80, 15000) == -7);
    assert(trace.dns_rc == -7 && trace.dns_calls == 1);
    write_result = 2;
    assert(__wrap_esp_transport_write((void *)8, "BODY", 4, 15) == 2);
    write_result = -1;
    assert(__wrap_esp_transport_write((void *)8, "BODY", 4, 15) == -1);
    assert(trace.tx_bytes == 2 && trace.write_calls == 1);
    read_result = 0;
    assert(__wrap_esp_transport_read((void *)8, buffer, sizeof(buffer), 15) == 0);
    read_result = -1;
    assert(__wrap_esp_transport_read((void *)8, buffer, sizeof(buffer), 15) == -1);
    assert(trace.first_rx_ms == UINT32_MAX && trace.rx_bytes == 0);
    platform_http_trace_end(&trace);
    clock_ms = UINT32_MAX - 100U;
    platform_http_trace_begin(&trace);
    assert(__wrap_lwip_getaddrinfo("host", NULL, NULL, &result) == -7);
    assert(trace.dns_ms == 1054); /* Monotonic 32-bit wrap. */
    platform_http_trace_end(&trace);
    assert(native_dns_calls == 6 && native_writes == 4);
    assert(strcmp(http_api_name("http://user:secret@host/v1/ai/token?token=secret"), "/v1/ai/token") == 0);
    assert(strcmp(http_api_name("/v1/call/room?room_id=secret#x"), "/v1/call/room") == 0);
    assert(strcmp(http_api_name("/v1/call/room/secret"), "other") == 0);
    assert(strcmp(http_api_name("http://host?token=secret"), "unknown") == 0);
    assert(strcmp(http_api_name(NULL), "unknown") == 0);
    puts("HTTP trace: scope, pass-through, failure, timing, wrap and redaction passed");
}
'''
    path = temp / 'test.c'
    path.write_text(code, encoding='utf-8')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I', str(temp), str(path),
                    '-o', str(temp / 'test')], check=True)
    subprocess.run([str(temp / 'test')], check=True)

# SDK/IDF are not patched. The observer adds no heap, mutex, task or hot-path log.
trace_text = source.read_text(encoding='utf-8')
for forbidden in ('malloc(', 'xTaskCreate', 'xSemaphore', 'ESP_LOG'):
    assert forbidden not in trace_text
cmake = (source.parent.parent / 'CMakeLists.txt').read_text(encoding='utf-8')
for symbol in ('lwip_getaddrinfo', 'esp_transport_connect', 'esp_transport_write', 'esp_transport_read'):
    assert '--wrap=' + symbol in cmake
event = function(platform, 'static esp_err_t http_event(')
assert event.index('HTTP_EVENT_ON_CONNECTED') < event.index('TCP_NODELAY')
perform = function(platform, 'static esp_err_t http_request_perform(')
assert perform.index('platform_http_trace_begin') < perform.index('esp_http_client_perform')
assert perform.index('esp_http_client_perform') < perform.index('platform_http_trace_end')
runtime = (root / 'components/starter_runtime/src/starter_runtime.c').read_text(encoding='utf-8')
assert 'AI_START_SETTLE_MS' not in runtime
assert '.received_at_ms = (uint32_t)now_ms()' in runtime
assert 'CONN rpc response: api=start_session' in runtime
print('HTTP timing/optimization static contracts passed')
