#!/usr/bin/env python3
"""Check the real transport-log filter with host C stubs, without a device.

Requires a host C11 compiler (CC or cc). Does not build ESP-IDF firmware.
SDK log formats match TiRTC 2.3.0 / e3911473 and 2.5.0 / f72f5d3c libraries;
this verifies extraction and lifecycle wiring, not transport negotiation.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "components/starter_tirtc/src/starter_tirtc.c").read_text(encoding="utf-8")


def function(name):
    match = re.search(r"^(?:static )?(?:int|void) " + name + r"\([^;{}]*\)\s*\{", SOURCE, re.M)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        end += 1
    return SOURCE[match.start():end] + "\n"


start = function("starter_tirtc_start")
assert start.index("TiRtcLogSetCallback(sdk_log)") < start.index("tgtrp_set_log_callback(sdk_transport_log)")
assert start.index("TiRtcLogSetLevel(") < start.index("sdk_transport_probe(true)") < start.index("rc = TiRtcInit()")
assert "sdk_transport_probe(false)" in function("rollback_start")
for name in ("on_conn_accepted", "on_external_connect"):
    body = function(name)
    assert body.index("sdk_transport_probe(false)") < body.index("true, 0);")
for name in ("on_disconnected", "starter_tirtc_disconnect", "external_connect"):
    assert "sdk_transport_probe(true)" in function(name)

filter_source = function("sdk_transport_log")
assert "TiRtc" not in filter_source and "tgtrp_set_" not in filter_source
assert all(token not in filter_source for token in ("malloc(", "calloc(", "vsnprintf(", "ESP_LOGD("))

code = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define TAG "test"
static unsigned logs, errors;
static int lower_level;
static char output[384];
static void record(bool error, const char *tag, const char *fmt, ...) {
    (void)tag;
    va_list args;
    va_start(args, fmt);
    vsnprintf(output, sizeof(output), fmt, args);
    va_end(args);
    logs++;
    errors += error;
}
#define ESP_LOGI(tag, ...) record(false, tag, __VA_ARGS__)
#define ESP_LOGE(tag, ...) record(true, tag, __VA_ARGS__)
static void tgtrp_set_log_level(int level) { lower_level = level; }
'''
code += filter_source + function("sdk_transport_probe")
code += r'''
static void emit(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    sdk_transport_log(fmt, args);
    va_end(args);
}
int main(void) {
    emit(NULL);
    emit("token=do-not-print device_secret=do-not-print");
    emit("(%s:%d) peer_connection_set_transport_ext_data tgtrp=%u",
         "private-source", 3013, 1U);
    emit("(%s:%d) peer_connection_tgtrp_init ok changed-format");
    assert(logs == 0);

    emit("(%s:%d) peer_connection_tgtrp_init ok ice=%p mtu=%u "
         "poll_segments=%u pacing=%u send_buffer_max=%zu",
         "private-source", 1927, (void *)output, 1200U, 16U, 1000000000U,
         (size_t)2097152U);
    assert(logs == 1 && errors == 0);
    assert(strstr(output, "TP init proto=TGMP transport=tgtrp ice="));
    assert(strstr(output, "mtu=1200 poll=16 sndcap=2097152"));
    assert(!strstr(output, "private-source") && !strstr(output, "1000000000"));

    emit("(%s:%d) peer_connection_kcp_init ikcp_nodelay(%d %d %d %d) "
         "sndwnd=%d mtu=%d", "private-source", 645, 2, 20, 2, 1, 128, 1200);
    assert(logs == 2 && errors == 0);
    assert(strcmp(output, "TP init proto=KCP transport=kcp nodelay=2 interval=20 "
                  "resend=2 nc=1 sndwnd=128 mtu=1200") == 0);

    emit("(%s:%d) peer_connection_tgtrp_init failed ice=%p ret=%d mtu=%u "
         "poll_segments=%u send_buffer_max=%zu",
         "private-source", 1918, (void *)output, -12, 1200U, 16U, (size_t)2097152U);
    assert(logs == 3 && errors == 1);
    assert(strstr(output, "TP init-failed proto=TGMP"));
    assert(strstr(output, "rc=-12 mtu=1200 poll=16 sndcap=2097152"));
    assert(!strstr(output, "TP init proto=") && !strstr(output, "private-source"));

    /* Re-arm after a close must expose the next connection's real protocol. */
    sdk_transport_probe(true);
    assert(lower_level == 5);
    sdk_transport_probe(false);
    assert(lower_level == 0);
    sdk_transport_probe(true);
    assert(lower_level == 5);
    emit("(%s:%d) peer_connection_kcp_init ikcp_nodelay(%d %d %d %d) "
         "sndwnd=%d mtu=%d", "private-source", 645, 0, 30, 0, 0, 64, 1200);
    assert(logs == 4 && strstr(output, "proto=KCP"));
    sdk_transport_probe(false);
    assert(lower_level == 0);
    emit("(%s:%d) peer_connection_tgtrp_init ok ice=%p mtu=%u "
         "poll_segments=%u pacing=%u send_buffer_max=%zu",
         "private-source", 1927, (void *)output, 1200U, 16U, 1000000000U,
         (size_t)16384U);
    assert(logs == 5 && strstr(output, "sndcap=16384"));
    assert(strstr(output, "proto=TGMP") && !strstr(output, "private-source"));
    puts("transport diagnostics: PASS (filter, redaction, failures, re-arm)");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="p4-transport-log-") as directory:
    source = Path(directory) / "test.c"
    executable = Path(directory) / ("test.exe" if os.name == "nt" else "test")
    source.write_text(code, encoding="utf-8")
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(source), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
