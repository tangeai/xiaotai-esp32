// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SHA256_BLOCK_BYTES 64U

// SHA-256 stores the bit length in 64 bits; HMAC adds one full block to the inner hash.
static inline bool digest_length_fits(uint64_t bytes, uint64_t prefix)
{
    return bytes <= UINT64_MAX / 8U - prefix;
}
