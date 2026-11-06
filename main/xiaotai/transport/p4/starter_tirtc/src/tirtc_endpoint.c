#include "tirtc_endpoint.h"

#include <stddef.h>
#include <strings.h>

const char *starter_tirtc_resolve_service_endpoint(const char *endpoint,
                                                 bool supports_https)
{
    if (supports_https || endpoint == NULL) {
        return endpoint;
    }

    /* Keep discovery's environment; only official Mini EP aliases use HTTP.
     * Unknown HTTPS endpoints must retain the SDK's unsupported-protocol error. */
    static const struct {
        const char *https;
        const char *http;
    } aliases[] = {
        {"https://ep-tirtc.tange365.com", "http://ep-tirtc.tange365.com"},
        {"https://ep-tirtc.tange365.com/", "http://ep-tirtc.tange365.com/"},
        {"https://ep-pre-tirtc.tange365.com", "http://ep-pre-tirtc.tange365.com"},
        {"https://ep-pre-tirtc.tange365.com/", "http://ep-pre-tirtc.tange365.com/"},
    };
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); ++i) {
        if (strcasecmp(endpoint, aliases[i].https) == 0) {
            return aliases[i].http;
        }
    }
    return endpoint;
}
