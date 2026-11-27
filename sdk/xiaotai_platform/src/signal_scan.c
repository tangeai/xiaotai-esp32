// SPDX-License-Identifier: MIT
/* Streaming counterpart of platform_json_envelope for packets whose payload
 * cannot enter the pool. Match the top-level fields and the unbind type
 * without storing the payload or decoding nested values. */
#include "signal_scan.h"
#include <string.h>
#include "xiaotai/json.h"

enum { LEAD, BOM2, BOM3, KEY_OR_END, KEY, COLON, VALUE_OR_END, VALUE, AFTER, DONE, BAD };
enum {
    TEXT, STRING, ESCAPE, HEX, ZERO, SIGN, INTEGER, DOT, FRACTION,
    EXP, EXP_SIGN, EXP_DIGITS, TRUE_WORD, FALSE_WORD, NULL_WORD
};
enum { TYPE = 1U, CHANNEL = 2U, UNBIND = 4U };

static bool blank(uint8_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static bool digit(uint8_t c) { return c >= '0' && c <= '9'; }
static int hex(uint8_t c)
{
    if (digit(c)) return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool object(const platform_signal_scan_t *s)
{
    return (s->objects & ((uint32_t)1U << (s->depth - 1U))) != 0U;
}

static void push(platform_signal_scan_t *s, bool is_object)
{
    if (s->depth == XIAOTAI_JSON_DEPTH_MAX) {
        s->grammar = BAD;
        return;
    }
    const uint32_t bit = (uint32_t)1U << s->depth++;
    if (is_object) s->objects |= bit;
    else s->objects &= ~bit;
    s->grammar = is_object ? KEY_OR_END : VALUE_OR_END;
}

static void pop(platform_signal_scan_t *s) { s->grammar = --s->depth ? AFTER : DONE; }

static void string_unit(platform_signal_scan_t *s, uint16_t c)
{
    if (!s->key_candidates) return;
    const unsigned i = s->key_length++;
    if (s->key_candidates & UNBIND) {
        if (i >= sizeof("unbind") - 1U || c != (uint8_t)"unbind"[i]) s->key_candidates = 0;
        return;
    }
    if (i >= sizeof("type") - 1U || c != (uint8_t)"type"[i]) s->key_candidates &= (uint8_t)~TYPE;
    if (i >= sizeof("channel") - 1U || c != (uint8_t)"channel"[i]) s->key_candidates &= (uint8_t)~CHANNEL;
}

static void start_string(platform_signal_scan_t *s, bool top_key, bool selected)
{
    s->lex = STRING;
    s->top_key = top_key;
    s->selected_string = selected;
    s->key_length = 0;
    s->key_candidates = top_key ? TYPE | CHANNEL : selected && s->field == TYPE ? UNBIND : 0;
    s->high_surrogate = s->pair_required = false;
}

static void end_string(platform_signal_scan_t *s)
{
    if (s->grammar == KEY || s->grammar == KEY_OR_END) {
        if (s->top_key) {
            uint8_t field = 0;
            if ((s->key_candidates & TYPE) && s->key_length == sizeof("type") - 1U) field = TYPE;
            if ((s->key_candidates & CHANNEL) && s->key_length == sizeof("channel") - 1U) field = CHANNEL;
            s->field = field & (uint8_t)~s->seen;
            s->seen |= field;
        }
        s->grammar = COLON;
    } else {
        if (s->selected_string && s->field == TYPE) {
            s->type_ok = true;
            if ((s->key_candidates & UNBIND) && s->key_length == sizeof("unbind") - 1U) s->seen |= UNBIND;
        }
        s->grammar = AFTER;
    }
    s->lex = TEXT;
}

static bool string_byte(platform_signal_scan_t *s, uint8_t c)
{
    if (s->lex == HEX) {
        const int v = hex(c);
        if (v < 0) return false;
        s->code_unit = (uint16_t)((s->code_unit << 4) | (unsigned)v);
        if (--s->hex_left != 0) return true;
        const uint16_t unit = s->code_unit;
        if (s->selected_string) {
            const bool low = unit >= 0xdc00 && unit <= 0xdfff;
            if (!unit || low != s->pair_required) return false;
            s->high_surrogate = unit >= 0xd800 && unit <= 0xdbff;
            s->pair_required = false;
        }
        string_unit(s, unit);
        s->lex = STRING;
        return true;
    }
    if (s->lex == ESCAPE) {
        if (c == 'u') {
            s->lex = HEX;
            s->hex_left = 4;
            s->code_unit = 0;
        } else {
            if (s->pair_required || !strchr("\"\\/bfnrt", c)) return false;
            /* No simple JSON escape decodes to a letter of the matched strings. */
            s->key_candidates = 0;
            s->lex = STRING;
        }
        return true;
    }
    if (c < 0x20U) return false;
    if (s->high_surrogate && c != '\\') return false;
    if (c == '"') end_string(s);
    else if (c == '\\') {
        s->pair_required = s->high_surrogate;
        s->high_surrogate = false;
        s->lex = ESCAPE;
    } else string_unit(s, c);
    return true;
}

static bool lex_byte(platform_signal_scan_t *s, uint8_t c, bool *used)
{
    *used = true;
    if (s->lex <= HEX) return string_byte(s, c);
    if (s->lex >= TRUE_WORD) {
        const char *word = s->lex == TRUE_WORD ? "true" : s->lex == FALSE_WORD ? "false" : "null";
        if (c != (uint8_t)word[s->literal_at++]) return false;
        if (!word[s->literal_at]) { s->lex = TEXT; s->grammar = AFTER; }
        return true;
    }
    if (digit(c)) {
        switch (s->lex) {
        case ZERO: return false;  /* a leading zero is only valid by itself */
        case SIGN: s->lex = c == '0' ? ZERO : INTEGER; break;
        case DOT: s->lex = FRACTION; break;
        case EXP:
        case EXP_SIGN: s->lex = EXP_DIGITS; break;
        default: break;
        }
        return true;
    }
    if ((s->lex == ZERO || s->lex == INTEGER) && c == '.') { s->lex = DOT; return true; }
    if ((s->lex == ZERO || s->lex == INTEGER || s->lex == FRACTION) && (c == 'e' || c == 'E')) {
        s->lex = EXP;
        return true;
    }
    if (s->lex == EXP && (c == '+' || c == '-')) { s->lex = EXP_SIGN; return true; }
    if (s->lex == SIGN || s->lex == DOT || s->lex == EXP || s->lex == EXP_SIGN) return false;
    s->lex = TEXT;
    s->grammar = AFTER;
    *used = false; /* the delimiter belongs to the surrounding container */
    return true;
}

static bool grammar_byte(platform_signal_scan_t *s, uint8_t c)
{
    if (s->grammar == LEAD) {
        const bool first = !s->first_byte;
        s->first_byte = true;
        if (first && c == 0xefU) { s->grammar = BOM2; return true; }
        if (c <= 0x20U) return true;
        if (c != '{') return false;
        push(s, true);
        return true;
    }
    if (s->grammar == BOM2) { s->grammar = BOM3; return c == 0xbbU; }
    if (s->grammar == BOM3) { s->grammar = LEAD; return c == 0xbfU; }
    if (s->grammar == DONE) return c <= 0x20U;
    if (blank(c)) return true;
    switch (s->grammar) {
    case KEY_OR_END:
        if (c == '}') { pop(s); return true; }
        /* fall through */
    case KEY:
        if (c != '"') return false;
        start_string(s, s->depth == 1, false);
        return true;
    case COLON:
        s->grammar = VALUE;
        return c == ':';
    case VALUE_OR_END:
        if (c == ']') { pop(s); return true; }
        /* fall through */
    case VALUE:
        if (c == '{' || c == '[') { push(s, c == '{'); return true; }
        if (c == '"') start_string(s, false, s->depth == 1 && s->field);
        else if (c == 't' || c == 'f' || c == 'n') {
            s->lex = c == 't' ? TRUE_WORD : c == 'f' ? FALSE_WORD : NULL_WORD;
            s->literal_at = 1;
        } else if (c == '-' || digit(c)) s->lex = c == '-' ? SIGN : c == '0' ? ZERO : INTEGER;
        else return false;
        return true;
    case AFTER: {
        const bool is_object = object(s);
        if (c == ',') { s->grammar = is_object ? KEY : VALUE; return true; }
        if (c != (is_object ? '}' : ']')) return false;
        pop(s);
        return true;
    }
    default: return false;
    }
}

void platform_signal_scan_feed(platform_signal_scan_t *s, const uint8_t *data, size_t size)
{
    for (size_t i = 0; i < size && s->grammar != BAD; ++i) {
        const uint8_t c = data[i];
        bool used = false;
        if (!c || (s->lex != TEXT && !lex_byte(s, c, &used)) ||
            (!used && !grammar_byte(s, c))) s->grammar = BAD;
    }
}

bool platform_signal_scan_valid(const platform_signal_scan_t *s)
{
    return s->grammar == DONE && s->type_ok;
}

bool platform_signal_scan_unbind(const platform_signal_scan_t *s)
{
    return platform_signal_scan_valid(s) && (s->seen & UNBIND);
}
