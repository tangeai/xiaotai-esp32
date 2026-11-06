// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "xmini_command.h"

static void test_contact_commands(void)
{
    static const struct { const char* action; xmini_command_kind_t kind; } actions[] = {
        {"REQUEST", XMINI_CMD_CONTACT_REQUEST}, {"ACCEPT", XMINI_CMD_CONTACT_ACCEPT},
        {"REJECT", XMINI_CMD_CONTACT_REJECT}, {"DELETE", XMINI_CMD_CONTACT_DELETE},
    };
    char text[768];
    for (unsigned i = 0; i < sizeof(actions) / sizeof(actions[0]); ++i) {
        snprintf(text, sizeof(text), "AT+CONTACT %s TIR_test-01", actions[i].action);
        xmini_command_t parsed = xmini_command_parse(text);
        assert(parsed.kind == actions[i].kind && !strcmp(parsed.target, "TIR_test-01"));
        assert(!parsed.remark[0]);
    }
    xmini_command_t parsed = xmini_command_parse("AT+CONTACT REMARK TIR_A e5b08fe9929b");
    assert(parsed.kind == XMINI_CMD_CONTACT_REMARK && !strcmp(parsed.target, "TIR_A"));
    assert(!strcmp(parsed.remark, "\xe5\xb0\x8f\xe9\x92\x9b"));
    parsed = xmini_command_parse("AT+CONTACT REMARK TIR_A E5B08FE9929B");
    assert(parsed.kind == XMINI_CMD_CONTACT_REMARK && !strcmp(parsed.remark, "\xe5\xb0\x8f\xe9\x92\x9b"));
    parsed = xmini_command_parse("AT+CONTACT REMARK TIR_A -");
    assert(parsed.kind == XMINI_CMD_CONTACT_REMARK && !strcmp(parsed.target, "TIR_A") && !parsed.remark[0]);
    parsed = xmini_command_parse("AT+CONTACT REMARK TIR_A 412042");
    assert(parsed.kind == XMINI_CMD_CONTACT_REMARK && !strcmp(parsed.remark, "A B"));

    char identity[66];
    memset(identity, 'A', 64);
    identity[64] = '\0';
    snprintf(text, sizeof(text), "AT+CONTACT REQUEST %s", identity);
    parsed = xmini_command_parse(text);
    assert(parsed.kind == XMINI_CMD_CONTACT_REQUEST && strlen(parsed.target) == 64);
    identity[64] = 'A';
    identity[65] = '\0';
    snprintf(text, sizeof(text), "AT+CONTACT REQUEST %s", identity);
    assert(xmini_command_parse(text).kind == XMINI_CMD_INVALID);

    size_t prefix = (size_t)snprintf(text, sizeof(text), "AT+CONTACT REMARK TIR_A ");
    for (unsigned i = 0; i < 256; ++i) memcpy(text + prefix + 2 * i, "61", 2);
    text[prefix + 512] = '\0';
    parsed = xmini_command_parse(text);
    assert(parsed.kind == XMINI_CMD_CONTACT_REMARK && strlen(parsed.remark) == 256);
    memcpy(text + prefix + 512, "61", 3);
    assert(xmini_command_parse(text).kind == XMINI_CMD_INVALID);

    static const char* invalid[] = {
        "AT+CONTACT", "AT+CONTACT REQUEST", "AT+CONTACT BAD TIR_A", "AT+CONTACT ACCEPT bad/id",
        "AT+CONTACT REJECT a.b", "AT+CONTACT DELETE a:b", "AT+CONTACT REQUEST a+b",
        "AT+CONTACT REQUEST id extra", "AT+CONTACT ACCEPT id extra", "AT+CONTACT REMARK id",
        "AT+CONTACT REMARK id 6", "AT+CONTACT REMARK id 6g", "AT+CONTACT REMARK id 00",
        "AT+CONTACT REMARK id 410042", "AT+CONTACT REMARK id --", "AT+CONTACT REMARK id - extra",
        "AT+CONTACT REMARK id 61 extra",
        "AT+PENDING extra", "AT+WIFI SETUP extra", "AT+WIFI CANCEL extra", "AT+WIFI SETUP,", "AT+WIFI SETUP=1",
    };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        assert(xmini_command_parse(invalid[i]).kind == XMINI_CMD_INVALID);
    /* UTF-8 semantics belong to business_post_contact, not the byte parser. */
    static const struct { const char* hex; const char* decoded; } invalid_utf8[] = {
        {"ff", "\xff"}, {"c0af", "\xc0\xaf"}, {"eda080", "\xed\xa0\x80"},
        {"f4908080", "\xf4\x90\x80\x80"}, {"e5b0", "\xe5\xb0"},
    };
    for (unsigned i = 0; i < sizeof(invalid_utf8) / sizeof(invalid_utf8[0]); ++i) {
        snprintf(text, sizeof(text), "AT+CONTACT REMARK id %s", invalid_utf8[i].hex);
        parsed = xmini_command_parse(text);
        assert(parsed.kind == XMINI_CMD_CONTACT_REMARK && !strcmp(parsed.remark, invalid_utf8[i].decoded));
    }
    prefix = (size_t)snprintf(text, sizeof(text), "AT+CONTACT REMARK TIR_A ");
    for (unsigned i = 0; i < 64; ++i) memcpy(text + prefix + 8 * i, "f09f9880", 8);
    text[prefix + 512] = '\0';
    parsed = xmini_command_parse(text);
    assert(parsed.kind == XMINI_CMD_CONTACT_REMARK && strlen(parsed.remark) == 256);
    for (unsigned i = 0; i < 64; ++i) assert(!memcmp(parsed.remark + 4 * i, "\xf0\x9f\x98\x80", 4));
    assert(xmini_command_parse("AT+PENDING").kind == XMINI_CMD_PENDING);
    assert(xmini_command_parse("AT+WIFI SETUP").kind == XMINI_CMD_WIFI_SETUP);
    assert(xmini_command_parse(" AT+WIFI\tSETUP\t").kind == XMINI_CMD_WIFI_SETUP);
    assert(xmini_command_parse("AT+WIFI SETUP ").kind == XMINI_CMD_WIFI_SETUP);
    assert(xmini_command_parse("AT+WIFI CANCEL").kind == XMINI_CMD_WIFI_CANCEL);
    assert(xmini_command_parse(" AT+WIFI\tCANCEL\t").kind == XMINI_CMD_WIFI_CANCEL);
    static const char* removed_ota[] = {
        "AT+OTA", "AT+OTA CONFIRM", "AT+OTA CANCEL",
        "AT+OTA STAGE https://firmware.example/app.bin 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef 1024",
    };
    for (unsigned i = 0; i < sizeof(removed_ota) / sizeof(removed_ota[0]); ++i)
        assert(xmini_command_parse(removed_ota[i]).kind == XMINI_CMD_INVALID);
}

static void test_paging_and_group_password(void)
{
    static const struct { const char *text; int filter; unsigned page; } pages[] = {
        {"AT+CONTACTS ALL", 0, 0}, {"AT+CONTACTS DEV", 1, 0}, {"AT+CONTACTS WX", 2, 0},
        {"AT+CONTACTS ALL 0", 0, 0}, {"AT+CONTACTS DEV 1", 1, 1},
        {"AT+CONTACTS WX 65535", 2, 65535}, {"AT+CONTACTS WX 00002", 2, 2},
    };
    for (unsigned i = 0; i < sizeof(pages) / sizeof(pages[0]); ++i) {
        xmini_command_t command = xmini_command_parse(pages[i].text);
        assert(command.kind == XMINI_CMD_CONTACTS_PAGE && command.value == pages[i].filter && command.page == pages[i].page);
    }
    xmini_command_t command = xmini_command_parse("AT+PENDING 65535");
    assert(command.kind == XMINI_CMD_PENDING_PAGE && command.page == 65535);
    assert(xmini_command_parse("AT+PENDING 0").kind == XMINI_CMD_PENDING_PAGE);
    assert(xmini_command_parse("AT+CONTACTS").kind == XMINI_CMD_CONTACTS);
    assert(xmini_command_parse("AT+PENDING").kind == XMINI_CMD_PENDING);
    command = xmini_command_parse("AT+GROUP JOIN 012345 0000");
    assert(command.kind == XMINI_CMD_GROUP_JOIN && !strcmp(command.code, "012345") && !strcmp(command.password, "0000"));
    command = xmini_command_parse("AT+GROUP CREATE 9876");
    assert(command.kind == XMINI_CMD_GROUP_CREATE && !strcmp(command.password, "9876"));
    command = xmini_command_parse("AT+GROUP CREATE");
    assert(command.kind == XMINI_CMD_GROUP_CREATE && !command.password[0]);
    command = xmini_command_parse("AT+GROUP JOIN 012345");
    assert(command.kind == XMINI_CMD_GROUP_JOIN && !command.password[0]);
    static const char *invalid[] = {
        "AT+CONTACTS 1", "AT+CONTACTS DEVICE 0", "AT+CONTACTS wx", "AT+CONTACTS ALL -1",
        "AT+CONTACTS DEV +1", "AT+CONTACTS WX 65536", "AT+CONTACTS WX 12345678901234567890",
        "AT+CONTACTS ALL 0 extra", "AT+PENDING -1", "AT+PENDING 65536", "AT+PENDING 0 1",
        "AT+GROUP JOIN 12345 1234", "AT+GROUP JOIN 012345 123", "AT+GROUP JOIN 012345 12345",
        "AT+GROUP JOIN 012345 12x4", "AT+GROUP JOIN 012345 -123", "AT+GROUP JOIN 012345 1234 extra",
        "AT+GROUP CREATE 123", "AT+GROUP CREATE 12345", "AT+GROUP CREATE 12a4", "AT+GROUP CREATE 1234 extra",
    };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        assert(xmini_command_parse(invalid[i]).kind == XMINI_CMD_INVALID);
    char boundary[97] = "AT+GROUP JOIN 012345 1234";
    size_t length = strlen(boundary);
    memset(boundary + length, ' ', 95 - length);
    boundary[95] = 0;
    assert(xmini_command_parse(boundary).kind == XMINI_CMD_GROUP_JOIN);
    boundary[95] = ' '; boundary[96] = 0;
    assert(xmini_command_parse(boundary).kind == XMINI_CMD_INVALID);
}

int main(void)
{
    assert(xmini_command_parse("AT+GUIDANCE").kind == XMINI_CMD_GUIDANCE);
    assert(xmini_command_parse(" AT+GUIDANCE\t").kind == XMINI_CMD_GUIDANCE);
    assert(xmini_command_parse("AT+GUIDANCE 0").kind == XMINI_CMD_GUIDANCE_SET);
    assert(xmini_command_parse("AT+GUIDANCE 0").value == 0);
    assert(xmini_command_parse(" AT+GUIDANCE\t1 ").kind == XMINI_CMD_GUIDANCE_SET);
    assert(xmini_command_parse("AT+GUIDANCE 1").value == 1);
    static const char *bad_guidance[] = {"AT+GUIDANCE -1", "AT+GUIDANCE 2", "AT+GUIDANCE 01",
        "AT+GUIDANCE ON", "AT+GUIDANCE 1 extra", "AT+GUIDANCE=0", "AT+GUIDANCE +1", "AT+GUIDANCE 0.0"};
    for (unsigned i = 0; i < sizeof(bad_guidance) / sizeof(bad_guidance[0]); ++i)
        assert(xmini_command_parse(bad_guidance[i]).kind == XMINI_CMD_INVALID);
    assert(xmini_command_parse("AT+ECHO").kind == XMINI_CMD_ECHO);
    assert(xmini_command_parse("AT+ECHO 0").kind == XMINI_CMD_ECHO_SET);
    assert(xmini_command_parse("AT+ECHO 0").value == 0);
    assert(xmini_command_parse(" AT+ECHO\t1 ").kind == XMINI_CMD_ECHO_SET);
    assert(xmini_command_parse("AT+ECHO 1").value == 1);
    static const char *bad_echo[] = {"AT+ECHO 2", "AT+ECHO ON", "AT+ECHO 01", "AT+ECHO=1", "AT+ECHO 1 x"};
    for (unsigned i = 0; i < sizeof(bad_echo) / sizeof(bad_echo[0]); ++i)
        assert(xmini_command_parse(bad_echo[i]).kind == XMINI_CMD_INVALID);
    static const struct { const char *text; xmini_command_kind_t kind; int value; } valid[] = {
        {"AT", XMINI_CMD_AT, 0}, {" AT+HELP\t", XMINI_CMD_HELP, 0},
        {"AT+VERSION", XMINI_CMD_VERSION, 0}, {"AT+STATUS", XMINI_CMD_STATUS, 0},
        {"AT+UI", XMINI_CMD_UI, 0}, {"AT+SCREEN", XMINI_CMD_SCREEN, 0},
        {"AT+DISPLAY", XMINI_CMD_DISPLAY, 0}, {"AT+AUDIO TONE", XMINI_CMD_TONE, 0},
        {"AT+AUDIO RECORD", XMINI_CMD_RECORD, 0}, {"AT+AUDIO PLAY", XMINI_CMD_PLAY, 0},
        {"AT+AUDIO STOP", XMINI_CMD_STOP, 0}, {"AT+VOL 0", XMINI_CMD_VOLUME, 0},
        {"AT+VOL 100", XMINI_CMD_VOLUME, 100}, {"AT+MIC 36", XMINI_CMD_GAIN, 36},
        {"AT+KEY CLICK", XMINI_CMD_CLICK, 0}, {"AT+KEY DOUBLE", XMINI_CMD_DOUBLE, 0},
        {"AT+KEY HOLD", XMINI_CMD_HOLD, 0}, {"tone", XMINI_CMD_TONE, 0},
        {"record", XMINI_CMD_RECORD, 0}, {"play", XMINI_CMD_PLAY, 0},
        {"stop", XMINI_CMD_STOP, 0}, {"status", XMINI_CMD_STATUS, 0},
        {"display", XMINI_CMD_DISPLAY, 0}, {"help", XMINI_CMD_HELP, 0},
    };
    for (unsigned i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
        xmini_command_t command = xmini_command_parse(valid[i].text);
        assert(command.kind == valid[i].kind && command.value == valid[i].value);
    }
    static const char *invalid[] = {
        "", " ", "AT x", "AT+VOL", "AT+VOL -1", "AT+VOL +1", "AT+VOL 101",
        "AT+VOL 99999999999999999999", "AT+MIC 37", "AT+MIC 12x", "AT+MIC 1 2",
        "AT+AUDIO", "AT+AUDIO BAD", "AT+AUDIO TONE junk", "AT+KEY BAD",
        "AT+KEY CLICK junk", "AT+RESET", "AT+STATUS x y z", "AT+VOL=40",
    };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        assert(xmini_command_parse(invalid[i]).kind == XMINI_CMD_INVALID);
    assert(xmini_command_parse(NULL).kind == XMINI_CMD_INVALID);
    assert(xmini_command_parse("AT+PRODUCT").kind == XMINI_CMD_PRODUCT);
    assert(xmini_command_parse("AT+CONSOLE").kind == XMINI_CMD_CONSOLE);
    assert(xmini_command_parse("AT+RTC").kind == XMINI_CMD_RTC);
    assert(xmini_command_parse("AT+CMDTRACE").kind == XMINI_CMD_CMDTRACE);
    assert(xmini_command_parse(" AT+CMDTRACE\t").kind == XMINI_CMD_CMDTRACE);
    static const char *bad_trace[] = {"AT+CMDTRACE CLEAR", "AT+CMDTRACE RESET", "AT+CMDTRACE ARM",
        "AT+CMDTRACE 0", "AT+CMDTRACE 1", "AT+CMDTRACE=1", "AT+CMDTRACE?", "AT+CMDTRACE x y z"};
    for (unsigned i = 0; i < sizeof(bad_trace) / sizeof(bad_trace[0]); ++i)
        assert(xmini_command_parse(bad_trace[i]).kind == XMINI_CMD_INVALID);
    assert(xmini_command_parse("AT+AI START").kind == XMINI_CMD_AI_START);
    assert(xmini_command_parse("AT+AI INTERRUPT").kind == XMINI_CMD_AI_INTERRUPT);
    assert(xmini_command_parse("AT+CALL ACCEPT").kind == XMINI_CMD_ACCEPT);
    assert(xmini_command_parse("AT+CALL DEVICE TIR68BK6APN4").kind == XMINI_CMD_DEVICE_CALL);
    assert(xmini_command_parse("AT+CALL DEVICE bad/id").kind == XMINI_CMD_INVALID);
    assert(xmini_command_parse("AT+GROUP JOIN 012345").kind == XMINI_CMD_GROUP_JOIN);
    assert(xmini_command_parse("AT+GROUP JOIN 01234a").kind == XMINI_CMD_INVALID);
    assert(xmini_command_parse("AT+GROUP LEAVE").kind == XMINI_CMD_GROUP_LEAVE);
    assert(xmini_command_parse("AT+GROUP PTT_UP").kind == XMINI_CMD_GROUP_UP);
    assert(xmini_command_parse("AT+BIND REPLAY").kind == XMINI_CMD_BIND_REPLAY);
    assert(xmini_command_parse("AT+GUIDE 0").kind == XMINI_CMD_GUIDE);
    xmini_command_t binding = xmini_command_parse("AT+GUIDE 5 012345");
    assert(binding.kind == XMINI_CMD_GUIDE && binding.value == 5 && !strcmp(binding.code, "012345"));
    assert(xmini_command_parse("AT+GUIDE 5").kind == XMINI_CMD_INVALID);
    assert(xmini_command_parse("AT+GUIDE 5 12345").kind == XMINI_CMD_INVALID);
    assert(xmini_command_parse("AT+GUIDE 5 12345a").kind == XMINI_CMD_INVALID);
    assert(xmini_command_parse("AT+GUIDE 5 1234567").kind == XMINI_CMD_INVALID);
    assert(xmini_command_parse("AT+GUIDE 13").kind == XMINI_CMD_INVALID);
    assert(xmini_command_parse("AT+GUIDE 9999999999999999").kind == XMINI_CMD_INVALID);
    xmini_command_t wifi = xmini_command_parse("AT+WIFI 5469,3132333435363738");
    assert(wifi.kind == XMINI_CMD_WIFI && !strcmp(wifi.ssid, "Ti") && !strcmp(wifi.password, "12345678"));
    wifi = xmini_command_parse("AT+WIFI e5b08fe9929b,");
    assert(wifi.kind == XMINI_CMD_WIFI && strlen(wifi.ssid) == 6 && !wifi.password[0]);
    static const char *bad_wifi[] = {"AT+WIFI ,", "AT+WIFI 00,", "AT+WIFI 5,", "AT+WIFI 5g,", "AT+WIFI 5469,00",
        "AT+WIFI 5469,31323334", "AT+WIFI 5469,,", "AT+WIFI 5469,3132333435363738 trailing"};
    for (unsigned i = 0; i < sizeof(bad_wifi) / sizeof(bad_wifi[0]); ++i)
        assert(xmini_command_parse(bad_wifi[i]).kind == XMINI_CMD_INVALID);
    char max_wifi[203] = "AT+WIFI ";
    memset(max_wifi + 8, 'a', 64);
    max_wifi[72] = ',';
    memset(max_wifi + 73, 'b', 128);
    max_wifi[201] = '\0';
    wifi = xmini_command_parse(max_wifi);
    assert(wifi.kind == XMINI_CMD_WIFI && strlen(wifi.ssid) == 32 && strlen(wifi.password) == 64);
    max_wifi[201] = 'b'; max_wifi[202] = '\0';
    assert(xmini_command_parse(max_wifi).kind == XMINI_CMD_INVALID);
    char oversized[100];
    memset(oversized, 'A', sizeof(oversized));
    oversized[99] = '\0';
    assert(xmini_command_parse(oversized).kind == XMINI_CMD_INVALID);
    test_contact_commands();
    test_paging_and_group_password();
    return 0;
}
