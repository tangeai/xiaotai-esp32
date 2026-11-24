// SPDX-License-Identifier: MIT
#include "xiaotai/port/sys.h"
#include "xiaotai/port/os.h"
#include "digest_limits.h"
#include <string.h>

#define SHA_BLOCK SHA256_BLOCK_BYTES

typedef struct {
    uint32_t h[8];
    uint64_t bytes;
    uint8_t tail[SHA_BLOCK];
    size_t used;
} sha256_t;

static const uint32_t rounds[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static void erase(void *memory, size_t size)
{
    volatile uint8_t *out = memory;
    while (size--) *out++ = 0;
}

static uint32_t rotate(uint32_t value, unsigned bits)
{
    return (value >> bits) | (value << (32U - bits));
}

static void compress(sha256_t *ctx, const uint8_t data[SHA_BLOCK])
{
    uint32_t words[64];
    for (unsigned i = 0; i < 16; ++i) {
        words[i] = ((uint32_t)data[4 * i] << 24) | ((uint32_t)data[4 * i + 1] << 16) |
                   ((uint32_t)data[4 * i + 2] << 8) | data[4 * i + 3];
    }
    for (unsigned i = 16; i < 64; ++i) {
        uint32_t x = words[i - 15], y = words[i - 2];
        words[i] = words[i - 16] + (rotate(x, 7) ^ rotate(x, 18) ^ (x >> 3)) +
                   words[i - 7] + (rotate(y, 17) ^ rotate(y, 19) ^ (y >> 10));
    }
    uint32_t a = ctx->h[0], b = ctx->h[1], c = ctx->h[2], d = ctx->h[3];
    uint32_t e = ctx->h[4], f = ctx->h[5], g = ctx->h[6], h = ctx->h[7];
    for (unsigned i = 0; i < 64; ++i) {
        uint32_t t1 = h + (rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25)) +
                      ((e & f) ^ (~e & g)) + rounds[i] + words[i];
        uint32_t t2 = (rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22)) +
                      ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d;
    ctx->h[4] += e; ctx->h[5] += f; ctx->h[6] += g; ctx->h[7] += h;
    erase(words, sizeof(words));
}

static void init(sha256_t *ctx)
{
    *ctx = (sha256_t){.h = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                           0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u}};
}

static void update(sha256_t *ctx, const void *data, size_t size)
{
    const uint8_t *next = data;
    ctx->bytes += size;
    unsigned blocks = 0;
    while (size) {
        size_t part = SHA_BLOCK - ctx->used;
        if (part > size) part = size;
        memcpy(ctx->tail + ctx->used, next, part);
        ctx->used += part;
        next += part;
        size -= part;
        if (ctx->used == SHA_BLOCK) {
            compress(ctx, ctx->tail);
            ctx->used = 0;
            if (++blocks == 64 && size) {
                xiaotai_delay_ms(1);
                blocks = 0;
            }
        }
    }
}

static void finish(sha256_t *ctx, uint8_t out[XIAOTAI_SHA256_BYTES])
{
    uint64_t bits = ctx->bytes * 8U;
    ctx->tail[ctx->used++] = 0x80;
    if (ctx->used > 56) {
        memset(ctx->tail + ctx->used, 0, SHA_BLOCK - ctx->used);
        compress(ctx, ctx->tail);
        ctx->used = 0;
    }
    memset(ctx->tail + ctx->used, 0, 56 - ctx->used);
    for (unsigned i = 0; i < 8; ++i) ctx->tail[63U - i] = (uint8_t)(bits >> (8U * i));
    compress(ctx, ctx->tail);
    for (unsigned i = 0; i < 32; ++i) out[i] = (uint8_t)(ctx->h[i / 4] >> (24U - 8U * (i % 4)));
    erase(ctx, sizeof(*ctx));
}

xiaotai_err_t xiaotai_sha256(const void *data, size_t size, uint8_t out[XIAOTAI_SHA256_BYTES])
{
    if (!out) return XIAOTAI_ERR_INVALID_ARG;
    if ((!data && size) || !digest_length_fits(size, 0)) {
        memset(out, 0, XIAOTAI_SHA256_BYTES);
        return XIAOTAI_ERR_INVALID_ARG;
    }
    sha256_t ctx;
    init(&ctx);
    update(&ctx, data, size);
    finish(&ctx, out);
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_hmac_sha256(const void *key, size_t key_size, const void *data, size_t size,
                                  uint8_t out[XIAOTAI_SHA256_BYTES])
{
    if (!out) return XIAOTAI_ERR_INVALID_ARG;
    if (!key || !key_size || !digest_length_fits(key_size, 0) ||
        (!data && size) || !digest_length_fits(size, SHA_BLOCK)) {
        memset(out, 0, XIAOTAI_SHA256_BYTES);
        return XIAOTAI_ERR_INVALID_ARG;
    }
    uint8_t block[SHA_BLOCK] = {0}, inner[XIAOTAI_SHA256_BYTES];
    sha256_t ctx;
    if (key_size > SHA_BLOCK) {
        init(&ctx);
        update(&ctx, key, key_size);
        finish(&ctx, block);
    } else {
        memcpy(block, key, key_size);
    }
    for (unsigned i = 0; i < SHA_BLOCK; ++i) block[i] ^= 0x36;
    init(&ctx);
    update(&ctx, block, sizeof(block));
    update(&ctx, data, size);
    finish(&ctx, inner);
    for (unsigned i = 0; i < SHA_BLOCK; ++i) block[i] ^= 0x36 ^ 0x5c;
    init(&ctx);
    update(&ctx, block, sizeof(block));
    update(&ctx, inner, sizeof(inner));
    finish(&ctx, out);
    erase(inner, sizeof(inner));
    erase(block, sizeof(block));
    return XIAOTAI_OK;
}
