// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Classification of a refused fragmented envelope, without retaining payload.
 * Initialize to zero. Feed exactly the declared payload, even across escapes. */
typedef struct {
    uint32_t objects;
    uint16_t code_unit;
    uint8_t grammar, lex, depth;
    uint8_t key_candidates, key_length, field, seen, hex_left, literal_at;
    bool top_key, selected_string, high_surrogate, pair_required, type_ok;
    bool first_byte;
} platform_signal_scan_t;

void platform_signal_scan_feed(platform_signal_scan_t *s, const uint8_t *data, size_t size);
bool platform_signal_scan_valid(const platform_signal_scan_t *s);
bool platform_signal_scan_unbind(const platform_signal_scan_t *s);
