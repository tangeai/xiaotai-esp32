"""Test bounded, credential-free startup diagnostics from the production callback."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "components/starter_tirtc/src/starter_tirtc.c").read_text(encoding="utf-8")
start = source.index("static int sdk_start_failure_code(")
end = source.index("static void on_event(", start)
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
static char output[256];
static const char *TAG = "test";
static void capture(const char *tag, const char *format, ...) {
    (void)tag;
    va_list args; va_start(args, format);
    vsnprintf(output, sizeof(output), format, args);
    va_end(args);
}
#define ESP_LOGE capture
#define ESP_LOGD(...) ((void)0)
''' + source[start:end] + r'''
int main(void) {
    const char *samples[] = {
        "request to https://host/v1/start: 40200(private response)",
        "prefix request to https://host/v1/start, status 503: token=secret",
        "request to https://host/v1/start, status 302, location: secret"
    };
    const int expected[] = {40200, 503, 302};
    for (unsigned i=0; i<3; ++i) {
        bool http=false;
        assert(sdk_start_failure_code(samples[i], strlen(samples[i]), &http)==expected[i]);
        assert(http==(i>0));
        sdk_log(samples[i], strlen(samples[i]));
        assert(strstr(output, "stage=start"));
        assert(!strstr(output, "secret") && !strstr(output, "private") && !strstr(output, "host"));
    }
    const char *invalid[] = {
        "request to https://host/v1/connect: 40000(secret)",
        "/v1/start: 9999999999999999999(secret)", "/v1/start: -1(secret)",
        "/v1/start: 12token", "/v1/start: ", "/v1/start: 0(ok)",
        "/v1/start?token=secret: 403(secret)", "body: secret"
    };
    for (unsigned i=0; i<sizeof(invalid)/sizeof(invalid[0]); ++i) {
        bool http=false;
        assert(sdk_start_failure_code(invalid[i], strlen(invalid[i]), &http)==0);
        output[0]=0; sdk_log(invalid[i], strlen(invalid[i])); assert(output[0]==0);
    }
    bool http=false;
    assert(sdk_start_failure_code(NULL, 20, &http)==0);
    assert(sdk_start_failure_code("short", 2049, &http)==0);
    char not_terminated[]={'/','v','1','/','s','t','a','r','t',':',' ','5','0','3'};
    assert(sdk_start_failure_code(not_terminated, sizeof(not_terminated), &http)==503);
    for (size_t n=0;n<11;++n)
        assert(sdk_start_failure_code(not_terminated, n, &http)==0);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="sdk-start-diag-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(code, encoding="utf-8")
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
print("PASS: SDK startup numeric error only; bounded non-NUL input and secret exclusion")
