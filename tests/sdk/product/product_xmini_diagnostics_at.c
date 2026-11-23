// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/test_command.c::command
// Replaces legacy: tests/xmini/test_audio_stream.c::audio_stream
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "product_test.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/os.h"
#include "xiaotai_console_host.h"
#include "xiaotai_net_host.h"

static void expect(const char *command, const char *expected, bool success)
{
    char reply[2048] = {0};
    const size_t length = strlen(command);
    assert(xiaotai_console_host_feed(command, length) == length);
    const int64_t deadline = xiaotai_time_us() + INT64_C(2000000);
    size_t used = 0;
    bool ok = false;
    const char *error = NULL;
    bool complete = false;
    do {
        used += xiaotai_console_host_take(reply + used, sizeof(reply) - used - 1U);
        ok = !strncmp(reply, "OK\r\n", 4) || strstr(reply, "\r\nOK\r\n");
        error = !strncmp(reply, "ERROR: ", 7) ? reply : strstr(reply, "\r\nERROR: ");
        complete = ok || (error && strstr(error + (*error == '\r' ? 2U : 0U), "\r\n"));
        if (complete) break;
        xiaotai_delay_ms(1);
    } while (used < sizeof(reply) - 1U && xiaotai_time_us() < deadline);
    if (!complete || ok != success || (error != NULL) == success || (expected && !strstr(reply, expected))) {
        fprintf(stderr, "Xmini diagnostics: %s expected %s; got %s\n", command,
                expected ? expected : "OK", reply);
        assert(false);
    }
}

static void check_settings(void)
{
    expect("AT+CONSOLE\r\n", "+CONSOLE: lines=", true);
    expect(" AT+LINK\tTRACE \r\n", "+LINK-TRACE: count=", true);
    expect("AT+LINK TRACE CLEAR\r\n", "ERROR: INVALID_ARG", false);

    expect("AT+SET audio.volume 0\r\n", NULL, true);
    expect("AT+SET audio.volume\r\n", "value=0 min=0 max=100", true);
    expect("AT+SET audio.volume 100\r\n", NULL, true);
    expect("AT+SET audio.mic_gain 36\r\n", NULL, true);
    expect("AT+SET guidance.enabled 0\r\n", NULL, true);
    expect("AT+SET guidance.enabled\r\n", "value=0 min=0 max=1", true);
    expect(" AT+SET\tguidance.enabled 1 \r\n", NULL, true);
    static const char *const refused[] = {
        "AT+SET audio.volume -1\r\n", "AT+SET audio.volume 101\r\n", "AT+SET audio.volume 99999999999999999999\r\n",
        "AT+SET audio.mic_gain 37\r\n", "AT+SET audio.mic_gain 12x\r\n", "AT+SET audio.mic_gain 1 2\r\n",
        "AT+SET guidance.enabled 2\r\n", "AT+SET guidance.enabled 1 extra\r\n",
    };
    for (size_t i = 0; i < sizeof(refused) / sizeof(*refused); ++i) expect(refused[i], "ERROR: INVALID_ARG", false);
    expect("AT+SET audio.volume\r\n", "value=100 min=0 max=100", true);
    expect("AT+SET audio.mic_gain\r\n", "value=36 min=0 max=36", true);
    expect("AT+SET guidance.enabled\r\n", "value=1 min=0 max=1", true);

    expect("AT+STATUS media\r\n", "+ECHO: enabled=1", true);
    expect("AT+AUDIO ECHO 0\r\n", NULL, true);
    expect("AT+STATUS media\r\n", "+ECHO: enabled=0", true);
    expect("AT+AUDIO ECHO 2\r\n", "ERROR: INVALID_ARG", false);
    expect("AT+STATUS media\r\n", "+ECHO: enabled=0", true);
    expect("AT+AUDIO ECHO 1\r\n", NULL, true);
    expect("AT+STATUS media\r\n", "+ECHO: enabled=1", true);
}

void product_xmini_diagnostics_at(bool cancel)
{
    if (cancel) {
        assert(xiaotai_net_host_wait_calls(XIAOTAI_NET_HOST_AP_START, 1, 3000));
        expect("AT+NET CANCEL NOW\r\n", "ERROR: INVALID_ARG", false);
    } else {
        check_settings();
        expect("AT+NET CANCEL\r\n", "ERROR: INVALID_STATE", false);
        expect("AT+NET PROVISION BOGUS\r\n", "ERROR: INVALID_ARG", false);
    }
    uint8_t marker = 0;
    const char *const space = PRODUCT_HOST_NET_SPACE;
    assert(xiaotai_kv_get_u8(space, "wifi_provision", &marker, 1000) ==
           (cancel ? XIAOTAI_OK : XIAOTAI_ERR_NOT_FOUND));
    if (cancel) assert(marker == 1);

    product_expect_restart();
    expect(cancel ? " AT+NET\tCANCEL \r\n" : " AT+NET\tPROVISION \r\n", NULL, true);
    const int64_t deadline = xiaotai_time_us() + INT64_C(8000000);
    while (!product_restarted() && xiaotai_time_us() < deadline) xiaotai_delay_ms(1);
    assert(product_restarted());
    assert(xiaotai_kv_get_u8(space, "wifi_provision", &marker, 1000) ==
           (cancel ? XIAOTAI_ERR_NOT_FOUND : XIAOTAI_OK));
    if (!cancel) assert(marker == 1);
    xiaotai_net_credentials_t saved;
    size_t size = sizeof(saved);
    assert(xiaotai_kv_get_blob_strict(space, "wifi", &saved, &size, 1000) == XIAOTAI_OK && size == sizeof(saved));
    assert(!strcmp(saved.ssid, PRODUCT_TEST_SSID) && !strcmp(saved.password, PRODUCT_TEST_PASSWORD));
    puts(cancel ? "Xmini diagnostics AT: cancel cleared the boot marker, kept Wi-Fi and requested one restart" :
                  "Xmini diagnostics AT: settings and link trace passed; provision wrote the marker, kept Wi-Fi and requested one restart");
}
