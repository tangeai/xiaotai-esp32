#!/usr/bin/env python3
"""Verify actual shared WeChat parser against P4 and S3 capability contracts."""
from pathlib import Path
import subprocess
import tempfile

project = Path(__file__).resolve().parents[1]
source = (project.parent / 'lckfb-szpi-esp32s3-tirtc/components/starter_runtime/src/starter_runtime.c').read_text(encoding='utf-8')
start = source.index('static const char *wechat_incoming_room_type(')
function = source[start:source.index('\n}', start) + 2]
cjson = project / 'managed_components/espressif__cjson/cJSON'
test = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"
FUNCTION
static void check(const char *json, const char *expected, bool fallback) {
    cJSON *payload = cJSON_Parse(json);
    assert(payload);
    bool inferred = !fallback;
    assert(strcmp(wechat_incoming_room_type(payload, &inferred), expected) == 0);
    assert(inferred == fallback);
    cJSON_Delete(payload);
}
int main(void) {
    const char *default_type = CONFIG_IDF_TARGET_ESP32P4 ? "video" : "voice";
    for (int i = 0; i < 500; ++i) {
        check("{}", default_type, true);
        check("{\"wx_room_type\":null}", default_type, true);
        check("{\"wx_room_type\":\"\"}", default_type, true);
        check("{\"wx_room_type\":\"voice\"}", "voice", false);
        check("{\"wx_room_type\":\"audio\"}", "audio", false);
        check("{\"wx_room_type\":\"video\"}", "video", false);
        check("{\"wxa_room_type\":\"video\"}", "video", false);
        check("{\"room_type\":\"voice\"}", "voice", false);
        check("{\"media_type\":\"video\"}", "video", false);
        check("{\"wx_room_type\":\"voice\",\"media_type\":\"video\"}", "voice", false);
        check("{\"wx_room_type\":\"unknown\"}", "unknown", false);
        check("{\"wx_room_type\":1}", "invalid", false);
        check("{\"wx_room_type\":false}", "invalid", false);
    }
    puts("PASS: explicit voice/video, aliases, optional fallback, malformed types; 500 repeats");
}
'''.replace('FUNCTION', function)
with tempfile.TemporaryDirectory(prefix='voip-media-') as directory:
    root = Path(directory)
    (root / 'test.c').write_text(test, encoding='utf-8')
    for p4 in (0, 1):
        binary = root / f'test-{p4}'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', f'-DCONFIG_IDF_TARGET_ESP32P4={p4}',
                        '-I', str(cjson), str(root / 'test.c'), str(cjson / 'cJSON.c'),
                        '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
