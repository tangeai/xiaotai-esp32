// SPDX-License-Identifier: MIT
#include "portal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static xiaotai_net_submission_t parse(const char *text)
{
    xiaotai_net_submission_t result;
    assert(net_portal_json_credentials((const uint8_t *)text, strlen(text), &result));
    assert(result.method == XIAOTAI_PROVISION_PORTAL);
    return result;
}

static void reject(const char *text)
{
    xiaotai_net_submission_t result;
    memset(&result, 0xfe, sizeof(result));
    assert(!net_portal_json_credentials((const uint8_t *)text, strlen(text), &result));
    for (size_t i = 0; i < sizeof(result); ++i) assert(!((const uint8_t *)&result)[i]);
}

int main(void)
{
    xiaotai_net_submission_t result = parse(" \n { \"ssid\" : \"x\", \"password\":\"\", \"ignored\":false }\r ");
    assert(!strcmp(result.credentials.ssid, "x") && !result.credentials.password[0] && !result.use_saved);
    result = parse("{\"ssid\":\"text\",\"ssid_hex\":\"fF229F\",\"use_saved\":true}");
    const uint8_t raw[] = {0xFF, '"', 0x9F, 0};
    assert(!memcmp(result.credentials.ssid, raw, sizeof(raw)) && result.use_saved);
    result = parse("{\"ssid\":\"quoted\\\"\\u4e2d\\uD83D\\uDE00\\\\\\t\",\"password\":\"12345678\",\"use_saved\":false}");
    assert(!strcmp(result.credentials.ssid, "quoted\"\xe4\xb8\xad\xf0\x9f\x98\x80\\\t"));
    assert(!strcmp(result.credentials.password, "12345678"));
    result = parse("{\"ssid\":\"three: \xe4\xb8\xad\",\"password\":\"\"}");
    assert(!strcmp(result.credentials.ssid, "three: \xe4\xb8\xad"));

    const char *cases[] = {
        "", "{}", "[]", "{\"ssid\":\"x\"}", "{\"ssid\":\"\",\"password\":\"\"}",
        "{\"ssid_hex\":\"\",\"ssid\":\"x\",\"password\":\"\"}", "{\"ssid_hex\":\"00\",\"password\":\"\"}",
        "{\"ssid_hex\":\"abc\",\"password\":\"\"}", "{\"ssid_hex\":\"zx\",\"password\":\"\"}",
        "{\"ssid\":\"x\",\"use_saved\":true,\"password\":\"\"}", "{\"ssid\":\"x\",\"password\":\"1234567\"}",
        "{\"ssid\":\"x\",\"use_saved\":\"true\"}", "{\"ssid\":\"x\",\"use_saved\":false}",
        "{\"ssid\":\"x\",\"password\":null}", "{\"ssid\":\"x\",\"password\":\"\",}",
        "{\"ssid\":\"x\",\"password\":\"\"}extra", "{\"ssid\":\"x\",\"password\":\"\",\"nested\":{}}",
        "{\"ssid\":\"x\",\"ssid\":\"y\",\"password\":\"\"}", "{\"ssid\":\"\\u0000\",\"password\":\"\"}",
        "{\"ssid\":\"x\",\"password\":\"12345\\u0000abc\"}", "{\"ssid\":\"x\",\"password\":\"\\v\"}",
        "{\"ssid\":\"\\uD800\",\"password\":\"\"}", "{\"ssid\":\"\\uDC00\",\"password\":\"\"}",
        "{\"ssid\":\"\\uD800\\uABCD\",\"password\":\"\"}", "{\"ssid\":\"\xc0\xaf\",\"password\":\"\"}",
        "{\"ssid\":\"abc\n\",\"password\":\"\"}", "{\"ssid\":\"x\",\"password\":\"\",\"field\":trueX}",
        "{\"ssid\":\"123456789012345678901234567890123\",\"password\":\"\"}",
        "{\"ssid\":\"x\",\"password\":\"12345678901234567890123456789012345678901234567890123456789012345\"}",
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) reject(cases[i]);
    result = parse("{\"ssid\":\"123456789012345678901234567890123\",\"ssid_hex\":\"61\",\"password\":\"\"}");
    assert(!strcmp(result.credentials.ssid, "a"));
    char body[NET_PORTAL_BODY_MAX + 1];
    const char packet[] = "{\"ssid\":\"12345678901234567890123456789012\","
                          "\"password\":\"1234567890123456789012345678901234567890123456789012345678901234\"}";
    memset(body, ' ', sizeof(body));
    memcpy(body, packet, sizeof(packet) - 1);
    assert(net_portal_json_credentials((const uint8_t *)body, NET_PORTAL_BODY_MAX, &result));
    assert(strlen(result.credentials.ssid) == 32 && strlen(result.credentials.password) == 64);
    assert(!net_portal_json_credentials((const uint8_t *)body, sizeof(body), &result));

    const char ssid[] = {'"', '\\', '\n', '<', '>', '&', (char)0xff, (char)0xe4, (char)0xb8, (char)0xad, 0};
    char text[NET_PORTAL_TEXT_MAX], hex[NET_PORTAL_HEX_MAX];
    net_portal_json_ssid(ssid, text, hex);
    assert(!strcmp(text, "\"\\\"\\\\\\u000a\\u003c\\u003e\\u0026\\ufffd\xe4\xb8\xad\""));
    assert(!strcmp(hex, "225c0a3c3e26ffe4b8ad"));
    char worst[XIAOTAI_NET_SSID_MAX + 1];
    memset(worst, 0xff, XIAOTAI_NET_SSID_MAX);
    worst[XIAOTAI_NET_SSID_MAX] = 0;
    net_portal_json_ssid(worst, text, hex);
    assert(strlen(text) == NET_PORTAL_TEXT_MAX - 1 && strlen(hex) == NET_PORTAL_HEX_MAX - 1);
    puts("net_portal_json: ok");
    return 0;
}
