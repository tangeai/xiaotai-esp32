#include "tirtc_endpoint.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    static const struct {
        const char *input;
        const char *expected;
    } aliases[] = {
        {"https://ep-tirtc.tange365.com", "http://ep-tirtc.tange365.com"},
        {"https://ep-tirtc.tange365.com/", "http://ep-tirtc.tange365.com/"},
        {"https://ep-pre-tirtc.tange365.com", "http://ep-pre-tirtc.tange365.com"},
        {"https://ep-pre-tirtc.tange365.com/", "http://ep-pre-tirtc.tange365.com/"},
        {"HTTPS://EP-TIRTC.TANGE365.COM", "http://ep-tirtc.tange365.com"},
    };
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); ++i) {
        assert(strcmp(starter_tirtc_resolve_service_endpoint(aliases[i].input, false),
                      aliases[i].expected) == 0);
        assert(starter_tirtc_resolve_service_endpoint(aliases[i].input, true) ==
               aliases[i].input);
    }
    static const char *unchanged[] = {
        "", "http://ep-tirtc.tange365.com", "http://ep-pre-tirtc.tange365.com/",
        "https://custom.example", "https://ep-tirtc.tange365.com:443",
        "https://ep-tirtc.tange365.com/api", "https://ep-tirtc.tange365.com?x=1",
        "https://ep-tirtc.tange365.com#x", "https://ep-tirtc.tange365.com.evil.example",
        "https://user@ep-tirtc.tange365.com", "https://ep-tirtc.tange365.com@evil.example",
        "ftp://ep-tirtc.tange365.com", " https://ep-tirtc.tange365.com",
    };
    for (size_t i = 0; i < sizeof(unchanged) / sizeof(unchanged[0]); ++i) {
        assert(starter_tirtc_resolve_service_endpoint(unchanged[i], false) == unchanged[i]);
        assert(starter_tirtc_resolve_service_endpoint(unchanged[i], true) == unchanged[i]);
    }
    assert(starter_tirtc_resolve_service_endpoint(NULL, false) == NULL);
    assert(starter_tirtc_resolve_service_endpoint(NULL, true) == NULL);
    puts("PASS: official EP aliases, TLS capability, environment and custom URL preservation");
    return 0;
}
