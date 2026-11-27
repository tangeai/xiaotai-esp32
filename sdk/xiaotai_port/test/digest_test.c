// SPDX-License-Identifier: MIT
// Replaces legacy: tests/host_crypto/run.py::platform_hmac
// Replaces legacy: tests/host_crypto/run.py::c61_image_sha256
#include "xiaotai/port/sys.h"
#include <assert.h>
#include <string.h>

static void check_hex(const uint8_t actual[XIAOTAI_SHA256_BYTES], const char *expected)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < XIAOTAI_SHA256_BYTES; ++i) {
        assert(expected[2 * i] == digits[actual[i] >> 4]);
        assert(expected[2 * i + 1] == digits[actual[i] & 15]);
    }
    assert(expected[2 * XIAOTAI_SHA256_BYTES] == '\0');
}

int main(void)
{
    uint8_t out[XIAOTAI_SHA256_BYTES];
    assert(xiaotai_sha256(NULL, 0, out) == XIAOTAI_OK);
    check_hex(out, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    assert(xiaotai_sha256("abc", 3, out) == XIAOTAI_OK);
    check_hex(out, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    static uint8_t data[8193];
    for (size_t i = 0; i < sizeof(data); ++i) data[i] = (uint8_t)(i % 251);
    static const struct { size_t size; const char *hex; } vectors[] = {
        {55, "463eb28e72f82e0a96c0a4cc53690c571281131f672aa229e0d45ae59b598b59"},
        {56, "da2ae4d6b36748f2a318f23e7ab1dfdf45acdc9d049bd80e59de82a60895f562"},
        {63, "29af2686fd53374a36b0846694cc342177e428d1647515f078784d69cdb9e488"},
        {64, "fdeab9acf3710362bd2658cdc9a29e8f9c757fcf9811603a8c447cd1d9151108"},
        {65, "4bfd2c8b6f1eec7a2afeb48b934ee4b2694182027e6d0fc075074f2fabb31781"},
        {4096, "d67c656e01756650d77717b0839985a056ec28ffe174601d690fc407a2ceffca"},
        {8193, "7e3691790cd64b19d4edb1a80e988214515abeb53aa0f34ffbfe4b4bf405d120"},
    };
    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
        assert(xiaotai_sha256(data, vectors[i].size, out) == XIAOTAI_OK);
        check_hex(out, vectors[i].hex);
    }

    // The other two fixed vectors from tests/host_crypto/run.py::c61_image_sha256.
    uint8_t c61_image[4097];
    for (size_t i = 0; i < 256; ++i) c61_image[i] = (uint8_t)i;
    assert(xiaotai_sha256(c61_image, 256, out) == XIAOTAI_OK);
    check_hex(out, "40aff2e9d2d8922e47afd4648e6967497158785fbd1da870e7110266bf944880");
    memset(c61_image, 'a', sizeof(c61_image));
    assert(xiaotai_sha256(c61_image, sizeof(c61_image), out) == XIAOTAI_OK);
    check_hex(out, "4e369b5618643c3abddd027b650bfa54810be3b418028a7c9d82299a59d008e8");

    // RFC 4231 cases 1 and 6 cover both the direct and hashed key paths.
    uint8_t key[131];
    memset(key, 0x0b, 20);
    assert(xiaotai_hmac_sha256(key, 20, "Hi There", 8, out) == XIAOTAI_OK);
    check_hex(out, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    memset(key, 0xaa, sizeof(key));
    static const char long_key_message[] = "Test Using Larger Than Block-Size Key - Hash Key First";
    assert(xiaotai_hmac_sha256(key, sizeof(key), long_key_message, sizeof(long_key_message) - 1, out) == XIAOTAI_OK);
    check_hex(out, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
    assert(xiaotai_hmac_sha256("key", 3, NULL, 0, out) == XIAOTAI_OK);
    check_hex(out, "5d5d139563c95b5967b9bd9a8c9b233a9dedb45072794cd232dc1b74832607d0");
    assert(xiaotai_hmac_sha256(data, 64, data, sizeof(data), out) == XIAOTAI_OK);
    check_hex(out, "dd6c7a30ab79f413a99bcbc1d60f5226a92a47d2cd3f69310863bc1a047a8158");
    assert(xiaotai_hmac_sha256(data, sizeof(data), "abc", 3, out) == XIAOTAI_OK);
    check_hex(out, "063388799772ae81d9544784dd5096959e3673b6144d62ae77ce337d7f36f582");

    const char hmac_message[] = "what do ya want for nothing?";
    assert(xiaotai_hmac_sha256("Jefe", 4, hmac_message, sizeof(hmac_message) - 1, out) == XIAOTAI_OK);
    check_hex(out, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    memset(key, 'a', sizeof(key));
    const char long_key[] = "key larger than one SHA256 block";
    assert(xiaotai_hmac_sha256(key, sizeof(key), long_key, sizeof(long_key) - 1, out) == XIAOTAI_OK);
    check_hex(out, "62a3ef3cc1c9370f30cbc51a29b3a752ac794420a6085ce80b994a57abd3c0bd");
    const char signed_text[] = "device-id17000000000000000000000000";
    assert(xiaotai_hmac_sha256("test-device-key", 15, signed_text, sizeof(signed_text) - 1, out) == XIAOTAI_OK);
    check_hex(out, "ffda1d5baa24a5a047b3cf12b55a447bc8caf4d63350598cac17a013526411ef");
    const char utf8_text[] = "device-\xe4\xb8\xad\xe6\x96\x87";
    assert(xiaotai_hmac_sha256("test-key", 8, utf8_text, sizeof(utf8_text) - 1, out) == XIAOTAI_OK);
    check_hex(out, "969470ac2b45550a93bb17033e3e2ed57fb56bca2094684f1d307361b864acdd");
    for (unsigned i = 0; i < 4096; ++i) {
        assert(xiaotai_hmac_sha256("Jefe", 4, hmac_message, sizeof(hmac_message) - 1, out) == XIAOTAI_OK);
        check_hex(out, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    }

    const uint8_t zero[XIAOTAI_SHA256_BYTES] = {0};
    memset(out, 0x5a, sizeof(out));
    assert(xiaotai_sha256(NULL, 1, out) == XIAOTAI_ERR_INVALID_ARG);
    assert(!memcmp(out, zero, sizeof(out)));
    assert(xiaotai_sha256("abc", 3, NULL) == XIAOTAI_ERR_INVALID_ARG);
    memset(out, 0x5a, sizeof(out));
    assert(xiaotai_hmac_sha256(NULL, 1, NULL, 0, out) == XIAOTAI_ERR_INVALID_ARG);
    assert(!memcmp(out, zero, sizeof(out)));
    memset(out, 0x5a, sizeof(out));
    assert(xiaotai_hmac_sha256(key, 0, NULL, 0, out) == XIAOTAI_ERR_INVALID_ARG);
    assert(!memcmp(out, zero, sizeof(out)));
    memset(out, 0x5a, sizeof(out));
    assert(xiaotai_hmac_sha256(key, 1, NULL, 1, out) == XIAOTAI_ERR_INVALID_ARG);
    assert(!memcmp(out, zero, sizeof(out)));
    assert(xiaotai_hmac_sha256(key, 1, NULL, 0, NULL) == XIAOTAI_ERR_INVALID_ARG);
#if SIZE_MAX > UINT64_MAX / 8U
    assert(xiaotai_sha256(data, SIZE_MAX, out) == XIAOTAI_ERR_INVALID_ARG);
    assert(!memcmp(out, zero, sizeof(out)));
    assert(xiaotai_hmac_sha256(key, 1, data, UINT64_MAX / 8U, out) == XIAOTAI_ERR_INVALID_ARG);
    assert(!memcmp(out, zero, sizeof(out)));
#endif
    return 0;
}
