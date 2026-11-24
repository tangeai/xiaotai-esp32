// SPDX-License-Identifier: MIT
// Replaces legacy: tests/host_crypto/run.py::platform_hmac
#include <assert.h>
#include <string.h>
#include "platform_internal.h"

static xiaotai_identity_t identity;
static xiaotai_err_t identity_result;
static int64_t unix_ms;
static uint32_t nonce_parts[2];
static unsigned random_count;

xiaotai_err_t __wrap_xiaotai_identity_get(xiaotai_identity_t *out)
{
    if (identity_result == XIAOTAI_OK) {
        assert(xiaotai_identity_valid(&identity));
        *out = identity;
    }
    return identity_result;
}

int64_t __wrap_xiaotai_clock_unix_ms(void)
{
    return unix_ms;
}

uint32_t __wrap_xiaotai_random(void)
{
    assert(random_count < 2);
    return nonce_parts[random_count++];
}

static void check(const char *stamp, const char *nonce, const char *expected)
{
    _Static_assert(sizeof(((platform_signature_t *)0)->signature) == 45, "SHA256 Base64 and terminator fit");
    platform_signature_t signature;
    memset(&signature, 'x', sizeof(signature));
    random_count = 0;
    assert(platform_signature_make(&signature) == XIAOTAI_OK);
    assert(random_count == 2);
    assert(!strcmp(signature.device, identity.device_id));
    assert(!strcmp(signature.stamp, stamp));
    assert(!strcmp(signature.nonce, nonce));
    assert(!strcmp(signature.signature, expected) && signature.signature[44] == '\0');

    static const char *const names[] = {"X-Device-Id", "X-Timestamp", "X-Nonce", "X-Signature"};
    const char *const values[] = {identity.device_id, stamp, nonce, expected};
    xiaotai_http_header_t headers[4];
    assert(platform_signature_headers(&signature, headers) == 4);
    for (unsigned i = 0; i < 4; ++i) {
        assert(!strcmp(headers[i].name, names[i]));
        assert(!strcmp(headers[i].value, values[i]));
    }
}

int main(void)
{
    strcpy(identity.device_id, "device-id");
    strcpy(identity.device_key, "Jefe");
    unix_ms = INT64_C(1700000000000);
    check("1700000000", "0000000000000000", "DRRUUDDAt7qDBHZVajyFkgHSz5D+pqbEdlt/Pe115Gw=");

    strcpy(identity.device_id, "fixture-bound");
    memset(identity.device_key, 'a', 131);
    unix_ms = INT64_C(1700000100000);
    nonce_parts[0] = UINT32_C(0xaabbccdd);
    nonce_parts[1] = UINT32_C(0x12345678);
    check("1700000100", "aabbccdd12345678", "J8Br0NJ24YQsqFmBft7yWHj89cIcvvjSmx+MvJuZPTs=");

    memset(identity.device_id, 'Q', XIAOTAI_IDENTITY_DEVICE_ID_MAX);
    memset(identity.device_key, 'z', XIAOTAI_IDENTITY_DEVICE_KEY_MAX);
    unix_ms = INT64_C(1700000200999);
    nonce_parts[0] = UINT32_MAX;
    nonce_parts[1] = 1;
    check("1700000200", "ffffffff00000001", "nD5YQaH6RXKexKk2Kskduam4lU/o5zcHOCf10ItrO7w=");

    memset(&identity, 0, sizeof(identity));
    strcpy(identity.device_id, "device-\xe4\xb8\xad\xe6\x96\x87");
    strcpy(identity.device_key, "test-key");
    unix_ms = INT64_C(1700000300000);
    nonce_parts[0] = UINT32_C(0x01020304);
    nonce_parts[1] = UINT32_C(0x0f0e0d0c);
    check("1700000300", "010203040f0e0d0c", "9bv1JsYGK88Dk+BQeeAu95c85qa8T2eEceU9r+KQF9A=");

    platform_signature_t no_identity = {0};
    random_count = 0;
    identity_result = XIAOTAI_ERR_NOT_FOUND;
    assert(platform_signature_make(&no_identity) == XIAOTAI_ERR_NOT_FOUND);
    assert(!random_count && !no_identity.signature[0]);
    return 0;
}
