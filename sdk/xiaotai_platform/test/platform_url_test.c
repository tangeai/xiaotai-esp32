// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_s3_discovered_endpoint.py
// Replaces legacy: tests/regression/test_service_http_policy.py
/* Address admission (src/url.c), ported from the deployed checks and their
 * tests (tests/regression/test_service_http_policy.py, the URL part of
 * tests/environment/test_environment.c): http(s) service addresses written
 * back as http://, trailing slashes of bases, user information, ports, IPv6
 * literals, forbidden bytes, the strict parser's host and path alphabets,
 * mqtt(s) broker addresses checked and unchecked, capacity limits measured
 * on the input, in-place rewriting, request paths and joining. Every input is
 * a terminated copy of exactly its own size. No port allocation. */
#include <stdbool.h>
#include "platform_internal.h"
#include "platform_test.h"
#include "xiaotai_mem_host.h"

#define CAPACITY 256

static xiaotai_err_t http(const char *url, bool trim, char *out, size_t capacity)
{
    char *copy = url ? platform_test_exact(url, strlen(url) + 1) : NULL;
    xiaotai_err_t err = platform_url_http(copy, trim, out, capacity);
    free(copy);
    return err;
}

static bool mqtt_valid(const char *url)
{
    char *copy = url ? platform_test_exact(url, strlen(url) + 1) : NULL;
    bool valid = platform_url_mqtt_valid(copy);
    free(copy);
    return valid;
}

static void http_accepted(void)
{
    static const char *const cases[][3] = {
        /* input, as is, trimmed */
        {"HTTPS://[::1]:8080/tenant/", "http://[::1]:8080/tenant/", "http://[::1]:8080/tenant"},
        {"https://service.test:8080/tenant/", "http://service.test:8080/tenant/", "http://service.test:8080/tenant"},
        {"http://host.test", "http://host.test", "http://host.test"},
        {"http://host.test/", "http://host.test/", "http://host.test"},
        {"HTTPS://host.test:8443/pre///", "http://host.test:8443/pre///", "http://host.test:8443/pre"},
        {"http://127.0.0.1:8080/pre", "http://127.0.0.1:8080/pre", "http://127.0.0.1:8080/pre"},
        {"http://[::1]:8080/tenant/v2", "http://[::1]:8080/tenant/v2", "http://[::1]:8080/tenant/v2"},
        {"http://host.test/pre%20two", "http://host.test/pre%20two", "http://host.test/pre%20two"},
        /* Host case is kept; only the scheme is rewritten. */
        {"HtTp://Host.Test", "http://Host.Test", "http://Host.Test"},
        /* The strict parser's edges that deployed firmware accepted. */
        {"http://host:65535", "http://host:65535", "http://host:65535"},
        {"http://host:00080/x", "http://host:00080/x", "http://host:00080/x"},
        {"http://[fe80::1%25eth0]:8080/x", "http://[fe80::1%25eth0]:8080/x", "http://[fe80::1%25eth0]:8080/x"},
        {"http://[::ffff:10.0.0.1]", "http://[::ffff:10.0.0.1]", "http://[::ffff:10.0.0.1]"},
        {"http://a-b.c-d.example", "http://a-b.c-d.example", "http://a-b.c-d.example"},
        {"http://host/a\"b<c>d{e}f|g^h`i~j!k$l&m'n(o)p*q+r,s;t=u:v@w[x]y", "",
         ""}, /* filled below: every printable path byte the parser allows */
        /* An empty user information passed the deployed check (the parser
         * records no user field for it) and still does. */
        {"http://@host", "http://@host", "http://@host"},
        {"http://host//", "http://host//", "http://host"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const char *input = cases[i][0];
        const char *plain = cases[i][1][0] ? cases[i][1] : NULL;
        char expected[CAPACITY];
        if (!plain) {
            /* The path case: same text with the scheme already http. */
            snprintf(expected, sizeof(expected), "%s", input);
            plain = expected;
        }
        const char *trimmed = cases[i][2][0] ? cases[i][2] : plain;
        char out[CAPACITY];
        memset(out, 'x', sizeof(out));
        CHECK_TEXT(http(input, false, out, sizeof(out)) == XIAOTAI_OK, input);
        CHECK_TEXT(strcmp(out, plain) == 0, input);
        memset(out, 'x', sizeof(out));
        CHECK_TEXT(http(input, true, out, sizeof(out)) == XIAOTAI_OK, input);
        CHECK_TEXT(strcmp(out, trimmed) == 0, input);
    }
}

static void http_refused(void)
{
    static const char *const cases[] = {
        /* tests/regression/test_service_http_policy.py */
        "ftp://host", "http://", "http://user@host", "http://host:0", "http://host:65536", "http://host?secret",
        "http://host#fragment", "http://host\\path", "http://host/path\t", "http://host name", "http://host/\177",
        "http://host/path\r\n",
        /* tests/environment/test_environment.c */
        "", "mqtt://host", "http://user:password@host", "http://host:port", "http://[::1",
        "http://host/pre?token=secret", "http://host/pre#part", "http://host\\pre", "http://host/pre\n",
        "http://host/pre\177", "http://host/pre\t",
        /* The strict parser's grammar. */
        "http:///path", "http://host:", "http://:80", "http://ho_st", "http://h\xC3\xA9st", "http://host/\xC3\xA9",
        "http://[]", "http://[::1]x", "http://host:80:90", "http://a@@b", "http://[fe80::1%eth0",
        "http://host:99999999999999999999", "http://user@", "http://[g::1]", "http://host:-1", "http://ho%st",
        "http:/host", "http:host", "https//host", "httpx://host", " http://host", "http://host ",
        "http://a@b@c", "http://[::1]:", "http://h,st", "http://host:8080x",
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        char out[CAPACITY];
        memset(out, 'x', sizeof(out));
        CHECK_TEXT(http(cases[i], false, out, sizeof(out)) == XIAOTAI_ERR_INVALID_ARG, cases[i]);
        CHECK_TEXT(out[0] == '\0', cases[i]);
        memset(out, 'x', sizeof(out));
        CHECK_TEXT(http(cases[i], true, out, sizeof(out)) == XIAOTAI_ERR_INVALID_ARG, cases[i]);
        CHECK_TEXT(out[0] == '\0', cases[i]);
    }
    /* Every control byte, DEL and the four refused printable bytes, at the
     * end of the path and inside the host. */
    for (unsigned c = 1; c < 256; ++c) {
        bool refused = c <= 32U || c == 127U || c >= 128U || c == '?' || c == '#' || c == '\\';
        char path[32];
        snprintf(path, sizeof(path), "http://host/p%c", (char)c);
        char out[CAPACITY];
        xiaotai_err_t err = http(path, false, out, sizeof(out));
        CHECK_TEXT(refused ? err == XIAOTAI_ERR_INVALID_ARG : err == XIAOTAI_OK, path);
    }
}

static void http_limits(void)
{
    char out[CAPACITY];
    /* NULL input or output. */
    memset(out, 'x', sizeof(out));
    CHECK(platform_url_http(NULL, false, out, sizeof(out)) == XIAOTAI_ERR_INVALID_ARG && out[0] == '\0');
    CHECK(platform_url_http("http://host", false, NULL, 0) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(platform_url_http("http://host", false, out, 0) == XIAOTAI_ERR_INVALID_ARG);
    /* The input must fit as given, as the deployed copy into a fixed buffer
     * required, even when the rewritten text would be shorter. */
    memset(out, 'x', sizeof(out));
    CHECK(http("http://host", false, out, 4) == XIAOTAI_ERR_INVALID_SIZE && out[0] == '\0');
    CHECK(http("http://host", false, out, 11) == XIAOTAI_ERR_INVALID_SIZE && out[0] == '\0');
    CHECK(http("http://host", false, out, 12) == XIAOTAI_OK && strcmp(out, "http://host") == 0);
    CHECK(http("https://host", false, out, 12) == XIAOTAI_ERR_INVALID_SIZE);
    CHECK(http("https://host", false, out, 13) == XIAOTAI_OK && strcmp(out, "http://host") == 0);
    /* In place, as deployed firmware rewrote its buffers. */
    char url[CAPACITY] = "HTTPS://[::1]:8080/tenant/";
    CHECK(platform_url_http(url, false, url, sizeof(url)) == XIAOTAI_OK);
    CHECK(strcmp(url, "http://[::1]:8080/tenant/") == 0);
    CHECK(platform_url_http(url, true, url, sizeof(url)) == XIAOTAI_OK);
    CHECK(strcmp(url, "http://[::1]:8080/tenant") == 0);
    CHECK(platform_url_http(url, false, url, sizeof(url)) == XIAOTAI_OK);
    CHECK(strcmp(url, "http://[::1]:8080/tenant") == 0);
    char bad[CAPACITY] = "http://user@host";
    CHECK(platform_url_http(bad, false, bad, sizeof(bad)) == XIAOTAI_ERR_INVALID_ARG && bad[0] == '\0');
    /* The longest admissible service address: 255 characters in 256 bytes. */
    char longest[CAPACITY + 1];
    memset(longest, 'a', sizeof(longest));
    memcpy(longest, "http://host/", 12);
    longest[CAPACITY - 1] = '\0';
    CHECK(http(longest, true, out, CAPACITY) == XIAOTAI_OK && strlen(out) == CAPACITY - 1);
    longest[CAPACITY - 1] = 'a';
    longest[CAPACITY] = '\0';
    CHECK(http(longest, true, out, CAPACITY) == XIAOTAI_ERR_INVALID_SIZE && out[0] == '\0');
}

static void mqtt(void)
{
    static const char *const good[] = {
        "mqtt://broker.test", "mqtts://broker.test", "mqtt://broker.test:1884", "mqtts://broker.test:8884",
        "MQTT://broker.test:1884/", "mqtt://127.0.0.1:1883", "mqtts://[::1]:8883",
    };
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); ++i) {
        CHECK_TEXT(mqtt_valid(good[i]), good[i]);
        /* Handed over unchanged, with or without the check. */
        char out[CAPACITY];
        CHECK_TEXT(platform_url_mqtt(good[i], false, out, sizeof(out)) == XIAOTAI_OK && !strcmp(out, good[i]),
                   good[i]);
        CHECK_TEXT(platform_url_mqtt(good[i], true, out, sizeof(out)) == XIAOTAI_OK && !strcmp(out, good[i]),
                   good[i]);
    }
    CHECK(!platform_url_mqtt_valid(NULL));
    static const char *const bad[] = {
        "", "mqtt://", "mqtts://", "http://broker.test", "ws://broker.test", "mqtt://user@broker.test",
        "mqtts://user:pass@broker.test", "mqtt://broker.test:0", "mqtts://broker.test:65536",
        "mqtt://broker.test:abc", "mqtt://:1883", "mqtt://broker.test?token", "mqtts://broker.test#fragment",
        "mqtt://broker.test\\path", "mqtt://broker name", "mqtt://broker.test/\177", "mqtts://broker.test/\r\n",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        CHECK_TEXT(!mqtt_valid(bad[i]), bad[i]);
        char out[CAPACITY];
        memset(out, 'x', sizeof(out));
        CHECK_TEXT(platform_url_mqtt(bad[i], false, out, sizeof(out)) == XIAOTAI_ERR_INVALID_ARG, bad[i]);
        CHECK_TEXT(out[0] == '\0', bad[i]);
    }
    /* Unchecked (mqtt_address_unchecked): any non-empty text that fits,
     * verbatim. */
    char out[CAPACITY];
    CHECK(platform_url_mqtt("ws://h", true, out, sizeof(out)) == XIAOTAI_OK && strcmp(out, "ws://h") == 0);
    CHECK(platform_url_mqtt("ws://h", false, out, sizeof(out)) == XIAOTAI_ERR_INVALID_ARG && out[0] == '\0');
    CHECK(platform_url_mqtt("", true, out, sizeof(out)) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(platform_url_mqtt(NULL, true, out, sizeof(out)) == XIAOTAI_ERR_INVALID_ARG);
    char text[CAPACITY + 1];
    memset(text, 'm', sizeof(text));
    text[CAPACITY - 1] = '\0';
    CHECK(platform_url_mqtt(text, true, out, CAPACITY) == XIAOTAI_OK && strlen(out) == CAPACITY - 1);
    text[CAPACITY - 1] = 'm';
    text[CAPACITY] = '\0';
    memset(out, 'x', sizeof(out));
    CHECK(platform_url_mqtt(text, true, out, CAPACITY) == XIAOTAI_ERR_INVALID_SIZE && out[0] == '\0');
}

static void paths(void)
{
    CHECK(platform_url_path_valid("/v1/device/token"));
    CHECK(platform_url_path_valid("/"));
    CHECK(platform_url_path_valid("/v1/voip/device/contacts?page=1&size=20"));
    /* The longest path in use: a speech download, 160 characters. */
    char path[XIAOTAI_PLATFORM_PATH_MAX + 2];
    snprintf(path, sizeof(path), "/v1/recognition/requests/%064d/audio/%064d", 0, 0);
    CHECK(strlen(path) == 160 && platform_url_path_valid(path));
    memset(path, 'p', sizeof(path));
    path[0] = '/';
    path[XIAOTAI_PLATFORM_PATH_MAX - 1] = '\0'; /* 191 characters */
    CHECK(platform_url_path_valid(path));
    path[XIAOTAI_PLATFORM_PATH_MAX - 1] = 'p';
    path[XIAOTAI_PLATFORM_PATH_MAX] = '\0'; /* 192 characters */
    CHECK(!platform_url_path_valid(path));
    static const char *const bad[] = {"", "v1/device", "/a b", "/a\r\nHost: x", "/a\tb", "/a#b", "/\x7f",
                                      "/\xC3\xA9", "http://host/a"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        CHECK_TEXT(!platform_url_path_valid(bad[i]), bad[i]);
    }
    CHECK(!platform_url_path_valid(NULL));
}

static void join(void)
{
    /* 16 + 16 characters: 33 bytes with the terminator. */
    char out[33];
    CHECK(platform_url_join("http://host.test", "/v1/device/token", out, 33) == XIAOTAI_OK);
    CHECK(strcmp(out, "http://host.test/v1/device/token") == 0);
    memset(out, 'x', sizeof(out));
    CHECK(platform_url_join("http://host.test", "/v1/device/token", out, 32) == XIAOTAI_ERR_INVALID_SIZE);
    CHECK(out[0] == '\0');
    CHECK(platform_url_join(NULL, "/", out, sizeof(out)) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(platform_url_join("http://h", NULL, out, sizeof(out)) == XIAOTAI_ERR_INVALID_ARG);
}

int main(void)
{
    xiaotai_mem_host_clear();
    http_accepted();
    http_refused();
    http_limits();
    mqtt();
    paths();
    join();
    CHECK(xiaotai_mem_host_allocations() == 0);
    return platform_test_result("platform_url");
}
