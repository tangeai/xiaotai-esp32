// SPDX-License-Identifier: MIT
#include "chr_raster.h"

#include <limits.h>
#include <math.h>
#include <string.h>

static float minf(float a, float b) { return a < b ? a : b; }
static float maxf(float a, float b) { return a > b ? a : b; }

/* ------------------------------------------------------------------- spans */

static int one(float l, float r, float *out)
{
    if (!(r > l)) return 0;
    out[0] = l;
    out[1] = r;
    return 1;
}

/* [l, r] without the gap |x - centre| < half. */
static int split(float l, float r, float centre, float half, float *out)
{
    if (!(half > 0.0f)) return one(l, r, out);
    int count = one(l, minf(r, centre - half), out);
    count += one(maxf(l, centre + half), r, out + 2 * count);
    return count;
}

static int ellipse(const float *a, float y, float *out)
{
    const float d = (y - a[1]) / a[3];
    if (d <= -1.0f || d >= 1.0f) return 0;
    const float w = a[2] * sqrtf(1.0f - d * d);
    return one(a[0] - w, a[0] + w, out);
}

static int eye(const float *a, float y, float *out)
{
    const float cx = a[0], cy = a[1], hw = a[2], hh = a[3], lid = a[5], slant = a[6], arc = a[7];
    const float top = cy - hh, bottom = cy + hh;
    if (y <= top || y >= bottom || hw <= 0.0f) return 0;
    float radius = minf(a[4], minf(hw, hh));
    float inset = 0.0f;
    const float edge = minf(y - top, bottom - y);
    if (edge < radius) {
        const float d = radius - edge;
        inset = radius - sqrtf(radius * radius - d * d);
    }
    float l = cx - hw + inset, r = cx + hw - inset;
    const float open = top + lid;
    if (slant > 0.01f || slant < -0.01f) {
        const float x = cx + hw * (y - open) / slant;
        if (slant > 0.0f) r = minf(r, x);
        else l = maxf(l, x);
    } else if (y < open) {
        return 0;
    }
    float gap = 0.0f;
    if (arc > 0.01f) gap = 1.0f - (bottom - y) / arc;        /* the lower lid rises in the middle */
    else if (arc < -0.01f) gap = 1.0f + (y - open) / arc;    /* the upper lid sinks in the middle */
    return split(l, r, cx, gap > 0.0f ? hw * sqrtf(gap) : 0.0f, out);
}

static int blob(const float *a, float y, float *out)
{
    const float top = a[1], bottom = a[2], dome = a[5], base = a[6];
    if (y <= top || y >= bottom) return 0;
    float w;
    if (y < top + dome) {
        const float d = (top + dome - y) / dome;
        w = a[3] * sqrtf(1.0f - d * d);
    } else if (y > bottom - base) {
        const float d = (y - (bottom - base)) / base, q = d * d;
        w = a[4] * sqrtf(sqrtf(1.0f - q * q));
    } else {
        const float span = bottom - base - top - dome;
        const float v = span > 0.0f ? (y - top - dome) / span : 1.0f;
        w = a[3] + (a[4] - a[3]) * v * v * (3.0f - 2.0f * v);
    }
    const float cx = a[0] + a[7] * (bottom - y) / (bottom - top);
    return one(cx - w, cx + w, out);
}

static void edge_cross(float x0, float y0, float x1, float y1, float y, float *l, float *r)
{
    if ((y0 <= y) == (y1 <= y)) return;
    const float x = x0 + (x1 - x0) * (y - y0) / (y1 - y0);
    if (x < *l) *l = x;
    if (x > *r) *r = x;
}

static int stadium(const float *a, float y, float *out)
{
    const float radius = a[4];
    float l = 1e9f, r = -1e9f;
    for (int i = 0; i < 2; ++i) {
        const float d = y - a[1 + 2 * i];
        if (d > -radius && d < radius) {
            const float w = sqrtf(radius * radius - d * d);
            l = minf(l, a[2 * i] - w);
            r = maxf(r, a[2 * i] + w);
        }
    }
    const float dx = a[2] - a[0], dy = a[3] - a[1];
    const float length = sqrtf(dx * dx + dy * dy);
    if (length > 0.001f) {
        const float nx = -dy / length * radius, ny = dx / length * radius;
        const float px[4] = {a[0] + nx, a[2] + nx, a[2] - nx, a[0] - nx};
        const float py[4] = {a[1] + ny, a[3] + ny, a[3] - ny, a[1] - ny};
        for (int i = 0; i < 4; ++i) edge_cross(px[i], py[i], px[(i + 1) & 3], py[(i + 1) & 3], y, &l, &r);
    }
    return one(l, r, out);
}

static int band(const float *a, float y, float *out)
{
    const float cx = a[0], hw = a[1], at = a[2], bt = a[3], ab = a[4], bb = a[5];
    float lo = 0.0f, hi = 1.0f;
    bool empty = false;
    /* at + bt u^2 <= y */
    if (bt > 0.001f) hi = minf(hi, (y - at) / bt);
    else if (bt < -0.001f) lo = maxf(lo, (y - at) / bt);
    else if (y < at) empty = true;
    /* y <= ab + bb u^2 */
    if (bb > 0.001f) lo = maxf(lo, (y - ab) / bb);
    else if (bb < -0.001f) hi = minf(hi, (y - ab) / bb);
    else if (y > ab) empty = true;
    int count = 0;
    if (!empty && hi > lo && hi > 0.0f) {
        const float outer = hw * sqrtf(hi);
        count = split(cx - outer, cx + outer, cx, lo > 0.0f ? hw * sqrtf(lo) : 0.0f, out);
    }
    /* Round ends where the two curves meet the sides. */
    const float thick = (ab + bb) - (at + bt);
    if (thick > 0.0f) {
        const float radius = thick * 0.5f, d = y - (at + bt + radius);
        if (d > -radius && d < radius) {
            const float w = sqrtf(radius * radius - d * d);
            if (!count) {
                count = one(cx - hw - w, cx - hw + w, out);
                count += one(cx + hw - w, cx + hw + w, out + 2 * count);
            } else {
                out[0] = minf(out[0], cx - hw - w);
                out[2 * count - 1] = maxf(out[2 * count - 1], cx + hw + w);
                if (count == 2) {
                    out[1] = maxf(out[1], cx - hw + w);
                    out[2] = minf(out[2], cx + hw - w);
                    if (out[1] >= out[2]) {
                        out[1] = out[3];
                        count = 1;
                    }
                }
            }
        }
    }
    return count;
}

static int polygon(const chr_shape_t *s, float y, float *out)
{
    float l = 1e9f, r = -1e9f;
    const int n = s->points;
    for (int i = 0; i < n; ++i) {
        const int j = i + 1 == n ? 0 : i + 1;
        edge_cross(s->a[2 * i], s->a[2 * i + 1], s->a[2 * j], s->a[2 * j + 1], y, &l, &r);
    }
    return one(l, r, out);
}

int chr_shape_spans(const chr_shape_t *s, float y, float spans[4])
{
    switch (s->kind) {
    case CHR_ELLIPSE:
    case CHR_GLOW: return ellipse(s->a, y, spans);
    case CHR_EYE: return eye(s->a, y, spans);
    case CHR_BLOB: return blob(s->a, y, spans);
    case CHR_STADIUM: return stadium(s->a, y, spans);
    case CHR_BAND: return band(s->a, y, spans);
    case CHR_POLYGON: return polygon(s, y, spans);
    default: return 0;
    }
}

/* ------------------------------------------------------------------ bounds */

bool chr_shape_bounds(chr_shape_t *s)
{
    const float *a = s->a;
    float x0, y0, x1, y1;
    switch (s->kind) {
    case CHR_ELLIPSE:
    case CHR_GLOW:
    case CHR_EYE:
        x0 = a[0] - a[2]; x1 = a[0] + a[2]; y0 = a[1] - a[3]; y1 = a[1] + a[3];
        break;
    case CHR_BLOB: {
        const float w = maxf(a[3], a[4]);
        x0 = a[0] - w + minf(a[7], 0.0f); x1 = a[0] + w + maxf(a[7], 0.0f); y0 = a[1]; y1 = a[2];
        break;
    }
    case CHR_STADIUM:
        x0 = minf(a[0], a[2]) - a[4]; x1 = maxf(a[0], a[2]) + a[4];
        y0 = minf(a[1], a[3]) - a[4]; y1 = maxf(a[1], a[3]) + a[4];
        break;
    case CHR_BAND: {
        const float thick = maxf((a[4] + a[5]) - (a[2] + a[3]), 0.0f);
        x0 = a[0] - a[1] - thick * 0.5f; x1 = a[0] + a[1] + thick * 0.5f;
        y0 = minf(a[2], a[2] + a[3]); y1 = maxf(a[4], a[4] + a[5]);
        break;
    }
    case CHR_POLYGON:
        if (s->points < 3 || s->points > 4) return false;
        x0 = x1 = a[0]; y0 = y1 = a[1];
        for (int i = 1; i < s->points; ++i) {
            x0 = minf(x0, a[2 * i]); x1 = maxf(x1, a[2 * i]);
            y0 = minf(y0, a[2 * i + 1]); y1 = maxf(y1, a[2 * i + 1]);
        }
        break;
    default:
        return false;
    }
    if (!(x1 > x0) || !(y1 > y0) || x0 < -8192.0f || x1 > 8192.0f || y0 < -8192.0f || y1 > 8192.0f) return false;
    s->x0 = (int16_t)floorf(x0); s->y0 = (int16_t)floorf(y0);
    s->x1 = (int16_t)ceilf(x1); s->y1 = (int16_t)ceilf(y1);
    return true;
}

/* ------------------------------------------------------------------ pixels */

static uint16_t swap16(uint16_t v) { return (uint16_t)(v << 8 | v >> 8); }

static uint16_t mix565(uint32_t fg, uint32_t bg, uint32_t alpha)
{
    fg = (fg | fg << 16) & 0x07E0F81FU;
    bg = (bg | bg << 16) & 0x07E0F81FU;
    const uint32_t out = ((fg * alpha + bg * (32U - alpha)) >> 5) & 0x07E0F81FU;
    return (uint16_t)(out | out >> 16);
}

static void blend(uint8_t *p, uint16_t colour, uint32_t alpha, bool swap)
{
    uint16_t now;
    memcpy(&now, p, sizeof(now));
    if (swap) now = swap16(now);
    uint16_t out = alpha >= 32U ? colour : mix565(colour, now, alpha);
    if (swap) out = swap16(out);
    memcpy(p, &out, sizeof(out));
}

/* Two colours per row, taken by the parity of x: an ordered dither that keeps
 * a gradient smooth in sixteen bits and still fills in runs. */
static void row_colours(const chr_shape_t *s, int y, uint16_t pair[2])
{
    const int rows = s->y1 - s->y0;
    const int32_t g = rows > 0 ? (y - s->y0) * 256 / rows : 0;
    int32_t c[3];
    for (int i = 0; i < 3; ++i) {
        const int32_t from = (int32_t)(s->top >> (16 - 8 * i) & 0xFFU), to = (int32_t)(s->bottom >> (16 - 8 * i) & 0xFFU);
        c[i] = from * 256 + (to - from) * g;
    }
    const bool flat = s->top == s->bottom;
    for (int k = 0; k < 2; ++k) {
        static const uint8_t bayer[2][2] = {{0, 2}, {3, 1}};
        const int32_t threshold = flat ? 128 : (int32_t)bayer[y & 1][k] * 64 + 32;
        uint32_t out[3];
        for (int i = 0; i < 3; ++i) {
            const int32_t levels = i == 1 ? 63 : 31;
            const int32_t scaled = c[i] * levels / 255;
            int32_t q = (scaled >> 8) + ((scaled & 255) > threshold ? 1 : 0);
            if (q > levels) q = levels;
            out[i] = (uint32_t)q;
        }
        pair[k] = (uint16_t)(out[0] << 11 | out[1] << 5 | out[2]);
    }
}

static void run(uint8_t *p, int count, int x, const uint16_t pair[2], bool swap)
{
    uint16_t even = pair[0], odd = pair[1];
    if (swap) {
        even = swap16(even);
        odd = swap16(odd);
    }
    if (((uintptr_t)p & 1U) != 0U) {
        for (int i = 0; i < count; ++i, p += 2) memcpy(p, ((x + i) & 1) ? &odd : &even, 2);
        return;
    }
    uint16_t *d = (uint16_t *)(void *)p;
    if (count && (((uintptr_t)d & 2U) != 0U)) {
        *d++ = (x & 1) ? odd : even;
        ++x;
        --count;
    }
    const uint16_t first = (x & 1) ? odd : even, second = (x & 1) ? even : odd;
    uint32_t word;
    const uint16_t both[2] = {first, second};
    memcpy(&word, both, sizeof(word));
    uint32_t *w = (uint32_t *)(void *)d;
    for (int i = count >> 1; i > 0; --i) *w++ = word;
    if (count & 1) *(uint16_t *)(void *)w = first;
}

static uint32_t glow(const chr_target_t *t, const chr_shape_t *s, int x0, int y0, int x1, int y1)
{
    const float cx = s->a[0], cy = s->a[1], rx = s->a[2], ry = s->a[3];
    if (rx < 0.5f || ry < 0.5f) return 0;
    uint16_t pair[2];
    const int32_t step = (int32_t)(4096.0f / rx);
    for (int y = y0; y <= y1; ++y) {
        const float ny = ((float)y + 0.5f - cy) / ry;
        const int32_t rest = (int32_t)((1.0f - ny * ny) * 4096.0f);
        if (rest <= 0) continue;
        row_colours(s, y, pair);
        uint8_t *p = t->pixels + (size_t)(y - t->y0) * t->stride + 2U * (size_t)(x0 - t->x0);
        int32_t nx = (int32_t)(((float)x0 + 0.5f - cx) / rx * 4096.0f);
        for (int x = x0; x <= x1; ++x, p += 2, nx += step) {
            const int32_t f = rest - ((nx * nx) >> 12);
            if (f <= 0) continue;
            const uint32_t alpha = ((uint32_t)((f * f) >> 12) * s->alpha + 2048U) >> 12;
            if (alpha) blend(p, pair[x & 1], alpha, t->swap);
        }
    }
    return (uint32_t)(x1 - x0 + 1) * (uint32_t)(y1 - y0 + 1);
}

uint32_t chr_shape_fill(const chr_target_t *t, const chr_shape_t *s)
{
    if (!t || !s || !s->alpha || !t->pixels || !t->scratch) return 0;
    const int y0 = s->y0 > t->y0 ? s->y0 : t->y0, y1 = s->y1 < t->y1 ? s->y1 : t->y1;
    const int x0 = s->x0 > t->x0 ? s->x0 : t->x0, x1 = s->x1 < t->x1 ? s->x1 : t->x1;
    if (y0 > y1 || x0 > x1) return 0;
    if (s->kind == CHR_GLOW) return glow(t, s, x0, y0, x1, y1);
    const int width = x1 - x0 + 1;
    const float left = (float)x0, right = (float)(x1 + 1);
    const uint32_t alpha = s->alpha > 32U ? 32U : s->alpha;
    int16_t *acc = t->scratch;
    uint16_t pair[2];
    uint32_t written = 0;
    for (int y = y0; y <= y1; ++y) {
        int lo = INT_MAX, hi = INT_MIN;
        for (int k = 0; k < 4; ++k) {
            float spans[4];
            const int count = chr_shape_spans(s, (float)y + 0.125f + 0.25f * (float)k, spans);
            for (int i = 0; i < count; ++i) {
                const float l = maxf(spans[2 * i], left), r = minf(spans[2 * i + 1], right);
                if (!(r > l)) continue;
                const int32_t lf = (int32_t)((l - left) * 64.0f + 0.5f), rf = (int32_t)((r - left) * 64.0f + 0.5f);
                if (rf <= lf) continue;
                const int li = lf >> 6, ri = rf >> 6;
                acc[li] += (int16_t)(64 - (lf & 63));
                acc[li + 1] += (int16_t)(lf & 63);
                acc[ri] -= (int16_t)(64 - (rf & 63));
                acc[ri + 1] -= (int16_t)(rf & 63);
                if (li < lo) lo = li;
                if (ri + 1 > hi) hi = ri + 1;
            }
        }
        if (lo > hi) continue;
        row_colours(s, y, pair);
        uint8_t *row = t->pixels + (size_t)(y - t->y0) * t->stride + 2U * (size_t)(x0 - t->x0);
        int sum = 0;
        for (int i = lo; i <= hi; ++i) {
            sum += acc[i];
            acc[i] = 0;
            if (i >= width || sum <= 0) continue;
            if (sum >= 255 && alpha == 32U) {
                int end = i + 1;
                while (end <= hi && end < width && acc[end] == 0) ++end;
                run(row + 2 * i, end - i, x0 + i, pair, t->swap);
                written += (uint32_t)(end - i);
                i = end - 1;
                continue;
            }
            const uint32_t a = ((uint32_t)(sum > 256 ? 256 : sum) * alpha + 128U) >> 8;
            if (a) blend(row + 2 * i, pair[(x0 + i) & 1], a, t->swap);
            ++written;
        }
    }
    return written;
}

/* ----------------------------------------------------------------- sprites */

/* Header: "XTCH", version, sprites, figure height, layout values, slots, clip words (all 16 bit), total bytes,
 * bytes of the head, its CRC-32 (reckoned with that field zero); then the layout (int16), the slots
 * (uint16, CHR_NO_SPRITE when empty) and the clips, each padded to four bytes, and the sprite table:
 * x, y, width, height, start, length, CRC-32. That is the head. The sprites follow. The clips, in
 * 16-bit words: how many clips, how many
 * frames; per clip its gesture, first frame, frames of the entry, of the loop, of the way out, bounce;
 * per frame its sprite, its milliseconds and the sprite it is a patch over (CHR_NO_SPRITE: none).
 *
 * A sprite is its row offsets and its rows. One whose length has SPRITE_PALETTE in its top byte keeps
 * 256 RGB565 colours between the two, and a pixel of its rows is one byte, a place in that palette:
 * half the size, for the pictures of the whole figure. One with SPRITE_SPANS is a patch over another
 * picture: each of its rows begins with two 16-bit numbers, the columns [from, to) of itself that take
 * the place of that picture's pixels, transparent ones included; the rows of the picture it leaves
 * alone say from == to. That is how a second picture of a pose is stored as what differs from the
 * first. */
enum { PACK_VERSION = 4, PACK_HEADER = CHR_PACK_FIXED, PACK_ENTRY = 20, CLIP_HEAD = 2, CLIP_ENTRY = 6, CLIP_FRAME = 3,
       SPRITE_PALETTE = 1, SPRITE_SPANS = 2, PALETTE_BYTES = 512 };

static uint32_t read32(const uint8_t *p) { uint32_t v; memcpy(&v, p, sizeof(v)); return v; }
static uint16_t read16(const uint8_t *p) { uint16_t v; memcpy(&v, p, sizeof(v)); return v; }

uint32_t chr_crc32(uint32_t crc, const void *data, size_t size)
{
    static const uint32_t nibble[16] = {
        0x00000000U, 0x1DB71064U, 0x3B6E20C8U, 0x26D930ACU, 0x76DC4190U, 0x6B6B51F4U, 0x4DB26158U, 0x5005713CU,
        0xEDB88320U, 0xF00F9344U, 0xD6D6A3E8U, 0xCB61B38CU, 0x9B64C2B0U, 0x86D3D2D4U, 0xA00AE278U, 0xBDBDF21CU};
    const uint8_t *p = data;
    crc = ~crc;
    for (; size; --size, ++p) {
        crc = nibble[(crc ^ *p) & 15U] ^ (crc >> 4);
        crc = nibble[(crc ^ (*p >> 4)) & 15U] ^ (crc >> 4);
    }
    return ~crc;
}

uint32_t chr_pack_head(const void *fixed, uint32_t *total)
{
    const uint8_t *bytes = fixed;
    if (!bytes || memcmp(bytes, "XTCH", 4) != 0 || read16(bytes + 4) != PACK_VERSION) return 0;
    const uint32_t head = read32(bytes + 20), all = read32(bytes + 16);
    if (head <= PACK_HEADER || head > CHR_PACK_HEAD_MAX || head > all) return 0;
    if (total) *total = all;
    return head;
}

/* What a sprite keeps before its rows: their offsets, and its palette when it has one. */
static uint32_t sprite_head(const uint8_t *entry)
{
    return 4U * ((uint32_t)read16(entry + 6) + 1U) + ((read32(entry + 12) >> 24) & SPRITE_PALETTE ? (uint32_t)PALETTE_BYTES : 0U);
}

/* Row offsets rise and stay inside the sprite. */
static bool rows_sound(const uint8_t *entry, const uint8_t *bytes)
{
    const uint32_t height = read16(entry + 6), length = read32(entry + 12) & 0xFFFFFFU;
    uint32_t before = sprite_head(entry);
    for (uint32_t row = 0; row <= height; ++row) {
        const uint32_t at = read32(bytes + 4U * row);
        if (at < before || at > length) return false;
        before = at;
    }
    return true;
}

bool chr_pack_open_head(chr_pack_t *pack, const void *data, size_t size, const chr_shelf_t *shelf)
{
    if (!pack) return false;
    *pack = (chr_pack_t){0};
    const uint8_t *bytes = data;
    uint32_t total = 0;
    const uint32_t head = bytes && size >= PACK_HEADER ? chr_pack_head(bytes, &total) : 0;
    if (!head || head > size) return false;
    static const uint8_t none[4] = {0};
    if (chr_crc32(chr_crc32(chr_crc32(0, bytes, 24), none, 4), bytes + 28, head - 28U) != read32(bytes + 24)) return false;
    const uint16_t count = read16(bytes + 6), layout = read16(bytes + 10), slots = read16(bytes + 12);
    const uint16_t clip_words = read16(bytes + 14);
    const uint32_t layout_at = PACK_HEADER, slots_at = layout_at + ((2U * layout + 3U) & ~3U);
    const uint32_t clips_at = slots_at + ((2U * slots + 3U) & ~3U), table_at = clips_at + ((2U * clip_words + 3U) & ~3U);
    if (!count || table_at + (uint32_t)count * PACK_ENTRY != head) return false;
    if (clip_words) {
        /* Every clip lies inside the frames and has something to show; every frame is a sprite. */
        const uint8_t *words = bytes + clips_at;
        if (clip_words < CLIP_HEAD) return false;
        const uint32_t clips = read16(words), frames = read16(words + 2);
        if (CLIP_HEAD + CLIP_ENTRY * clips + CLIP_FRAME * frames > clip_words) return false;
        for (uint32_t i = 0; i < clips; ++i) {
            const uint8_t *clip = words + 2U * (CLIP_HEAD + CLIP_ENTRY * i);
            const uint32_t first = read16(clip + 2), enter = read16(clip + 4), loop = read16(clip + 6), leave = read16(clip + 8);
            if (!(enter + loop) || first + enter + loop + leave > frames) return false;
        }
        for (uint32_t i = 0; i < frames; ++i) {
            const uint8_t *frame = words + 2U * (CLIP_HEAD + CLIP_ENTRY * clips + CLIP_FRAME * i);
            if (read16(frame) >= count || (read16(frame + 4) != CHR_NO_SPRITE && read16(frame + 4) >= count)) return false;
        }
    }
    for (unsigned i = 0; i < slots; ++i) {
        const uint16_t sprite = read16(bytes + slots_at + 2U * i);
        if (sprite != CHR_NO_SPRITE && sprite >= count) return false;
    }
    for (unsigned i = 0; i < count; ++i) {
        const uint8_t *entry = bytes + table_at + i * PACK_ENTRY;
        const uint32_t start = read32(entry + 8), kind = read32(entry + 12) >> 24, length = read32(entry + 12) & 0xFFFFFFU;
        if (kind > (SPRITE_PALETTE | SPRITE_SPANS) || !read16(entry + 4) || !read16(entry + 6) || start < head || start > total ||
            length > total - start || length < sprite_head(entry)) return false;
    }
    *pack = (chr_pack_t){.data = bytes, .size = total, .head = head, .count = count, .figure_height = read16(bytes + 8),
        .layout_count = layout, .slot_count = slots, .clip_words = clip_words, .layout_at = layout_at, .slots_at = slots_at,
        .clips_at = clips_at, .table_at = table_at, .shelf = shelf};
    return true;
}

bool chr_pack_open(chr_pack_t *pack, const void *data, size_t size)
{
    if (!chr_pack_open_head(pack, data, size, NULL)) return false;
    bool sound = pack->size <= size;
    for (unsigned i = 0; sound && i < pack->count; ++i) {
        const uint8_t *entry = pack->data + pack->table_at + i * PACK_ENTRY;
        sound = rows_sound(entry, pack->data + read32(entry + 8));
    }
    if (!sound) *pack = (chr_pack_t){0};
    return sound;
}

bool chr_pack_place(const chr_pack_t *pack, unsigned id, uint32_t *start, uint32_t *length)
{
    if (!pack || !pack->data || id >= pack->count || !start || !length) return false;
    const uint8_t *entry = pack->data + pack->table_at + id * PACK_ENTRY;
    *start = read32(entry + 8);
    *length = read32(entry + 12) & 0xFFFFFFU;
    return true;
}

bool chr_pack_intact(const chr_pack_t *pack, unsigned id, const uint8_t *bytes)
{
    uint32_t start, length;
    if (!bytes || !chr_pack_place(pack, id, &start, &length)) return false;
    const uint8_t *entry = pack->data + pack->table_at + id * PACK_ENTRY;
    return chr_crc32(0, bytes, length) == read32(entry + 16) && rows_sound(entry, bytes);
}

bool chr_pack_clip(const chr_pack_t *pack, unsigned gesture, chr_clip_t *out)
{
    if (!pack || !pack->data || !pack->clip_words || !out) return false;
    const uint8_t *words = pack->data + pack->clips_at;
    const unsigned clips = read16(words);
    for (unsigned i = 0; i < clips; ++i) {
        const uint8_t *clip = words + 2U * (CLIP_HEAD + CLIP_ENTRY * i);
        if (read16(clip) != gesture) continue;
        *out = (chr_clip_t){read16(clip + 2), read16(clip + 4), read16(clip + 6), read16(clip + 8), read16(clip + 10)};
        return true;
    }
    return false;
}

bool chr_pack_frame(const chr_pack_t *pack, unsigned index, chr_frame_t *out)
{
    if (!pack || !pack->data || !pack->clip_words || !out) return false;
    const uint8_t *words = pack->data + pack->clips_at;
    const unsigned clips = read16(words), frames = read16(words + 2);
    if (index >= frames) return false;
    const uint8_t *frame = words + 2U * (CLIP_HEAD + CLIP_ENTRY * clips + CLIP_FRAME * index);
    *out = (chr_frame_t){read16(frame), read16(frame + 2), read16(frame + 4)};
    return true;
}

bool chr_pack_sprite(const chr_pack_t *pack, unsigned id, chr_sprite_t *out)
{
    if (!pack || !pack->data || id >= pack->count || !out) return false;
    const uint8_t *entry = pack->data + pack->table_at + id * PACK_ENTRY;
    *out = (chr_sprite_t){(int16_t)read16(entry), (int16_t)read16(entry + 2), read16(entry + 4), read16(entry + 6)};
    return true;
}

unsigned chr_pack_slot(const chr_pack_t *pack, unsigned slot)
{
    if (!pack || !pack->data || slot >= pack->slot_count) return CHR_NO_SPRITE;
    return read16(pack->data + pack->slots_at + 2U * slot);
}

int chr_pack_layout(const chr_pack_t *pack, unsigned index)
{
    if (!pack || !pack->data || index >= pack->layout_count) return 0;
    return (int16_t)read16(pack->data + pack->layout_at + 2U * index);
}

static float row_shift(const chr_blit_t *b, float y)
{
    float shift = b->tilt * (b->tilt_y - y);
    if (b->sway != 0.0f && b->sway_to != b->sway_from) {
        float t = (y - b->sway_from) / (b->sway_to - b->sway_from);
        t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
        shift += b->sway * t * t;
    }
    return shift;
}

static bool grows(const chr_blit_t *b) { return b->grow_x != 0.0f || b->grow_y != 0.0f; }

bool chr_blit_bounds(const chr_pack_t *pack, chr_blit_t *b)
{
    chr_sprite_t sprite;
    if (!b || !b->alpha || !chr_pack_sprite(pack, b->id, &sprite)) return false;
    if (grows(b)) {
        const float sx = 1.0f + b->grow_x, sy = 1.0f + b->grow_y;
        if (sx < 0.25f || sy < 0.25f || sx > 4.0f || sy > 4.0f) return false;
        b->x0 = (int16_t)((int)floorf(b->pin_x + ((float)b->x - b->pin_x) * sx) - 1);
        b->x1 = (int16_t)((int)ceilf(b->pin_x + ((float)(b->x + sprite.width) - b->pin_x) * sx) + 1);
        b->y0 = (int16_t)((int)floorf(b->pin_y + ((float)b->y - b->pin_y) * sy) - 1);
        b->y1 = (int16_t)((int)ceilf(b->pin_y + ((float)(b->y + sprite.height) - b->pin_y) * sy) + 1);
        return true;
    }
    const float top = row_shift(b, (float)b->y), bottom = row_shift(b, (float)(b->y + sprite.height - 1));
    const float low = minf(minf(top, bottom), 0.0f), high = maxf(maxf(top, bottom), 0.0f);
    const float turn = (b->turn < 0.0f ? -b->turn : b->turn) * (float)sprite.width;
    b->x0 = (int16_t)(b->x + (int)floorf(low) - 1);
    b->x1 = (int16_t)(b->x + sprite.width + (int)ceilf(high) + 1);
    b->y0 = (int16_t)(b->y - (int)ceilf(turn) - 1);
    b->y1 = (int16_t)(b->y + sprite.height + (int)ceilf(turn) + 1);
    return true;
}

static uint16_t washed(uint16_t colour, uint32_t grey)
{
    if (!grey) return colour;
    const uint32_t r = colour >> 11, g = colour >> 5 & 63U, b = colour & 31U;
    const uint32_t level = (r * 2U * 30U + g * 59U + b * 2U * 11U) * 86U / 10000U;   /* 0..63, a little darker */
    return mix565((uint16_t)((level >> 1) << 11 | level << 5 | level >> 1), colour, grey);
}

/* A sprite as it lies in the pack: where its rows and its palette are, and whether its rows are
 * spans (each begins with the columns it replaces in the picture under it). */
typedef struct {
    const uint8_t *base, *palette;
    chr_sprite_t box;
    bool spans;
} source_t;

static bool source_of(const chr_pack_t *pack, unsigned id, source_t *out)
{
    if (!chr_pack_sprite(pack, id, &out->box)) return false;
    const uint8_t *entry = pack->data + pack->table_at + id * PACK_ENTRY;
    const uint32_t kind = read32(entry + 12) >> 24;
    out->base = pack->shelf ? pack->shelf->find(pack->shelf->ctx, id) : pack->data + read32(entry + 8);
    if (!out->base) return false;
    out->palette = kind & SPRITE_PALETTE ? out->base + 4U * ((uint32_t)out->box.height + 1U) : NULL;
    out->spans = (kind & SPRITE_SPANS) != 0;
    return true;
}

/* The bytes of one row; false when the sprite has no such row. A row of spans is entered past its
 * two column numbers, which `from` and `to` receive. */
static bool row_of(const source_t *s, int row, const uint8_t **p, const uint8_t **end, int *from, int *to)
{
    if (row < 0 || row >= (int)s->box.height) return false;
    *p = s->base + read32(s->base + 4U * (uint32_t)row);
    *end = s->base + read32(s->base + 4U * (uint32_t)(row + 1));
    if (!s->spans) return true;
    if (*p + 4 > *end) return false;
    *from = read16(*p);
    *to = read16(*p + 2);
    *p += 4;
    return *to > *from && *to <= (int)s->box.width;
}

/* One row of a sprite onto stage row `y`, its first column on stage column `x`. */
static uint32_t blit_row(const chr_target_t *t, const chr_blit_t *b, const source_t *s, int x, int row, int y, bool plain)
{
    const uint8_t *p, *end, *const palette = s->palette;
    int from, to;
    if (!row_of(s, row, &p, &end, &from, &to)) return 0;
    uint32_t written = 0;
    while (p < end) {
        const unsigned token = *p++;
        const int count = (int)(token & (token < 0x80U ? 0x7FU : 0x3FU)) + 1;
        if (token < 0x80U) {
            x += count;
            continue;
        }
        const bool solid = token < 0xC0U;
        const int step = (solid ? 2 : 3) - (palette ? 1 : 0);
        if (p + step * count > end) break;   /* a damaged row: stop rather than read past it */
        if (plain && solid) {
            /* The common case: a run of opaque pixels copied as it is, or looked up one by one. */
            const int left = x < t->x0 ? t->x0 : x, right = x + count - 1 > t->x1 ? t->x1 : x + count - 1;
            if (right >= left) {
                uint8_t *out = t->pixels + (size_t)(y - t->y0) * t->stride + 2 * (left - t->x0);
                if (!palette) memcpy(out, p + 2 * (left - x), 2U * (size_t)(right - left + 1));
                else {
                    const uint8_t *in = p + (left - x);
                    for (int i = 0; i <= right - left; ++i, out += 2) {
                        const uint8_t *colour = palette + 2U * in[i];
                        out[0] = colour[0];
                        out[1] = colour[1];
                    }
                }
                written += (uint32_t)(right - left + 1);
            }
        } else {
            for (int i = 0; i < count; ++i) {
                int px = x + i, py = y;
                if (b->turn != 0.0f) py += (int)floorf(b->turn * ((float)px - b->turn_x) + 0.5f);
                if (px < t->x0 || px > t->x1 || py < t->y0 || py > t->y1) continue;
                const uint8_t *source = p + step * i;
                const uint32_t alpha = (solid ? 32U : source[0]) * b->alpha / 32U;
                const uint8_t *pixel = source + (solid ? 0 : 1);
                const uint16_t colour = washed(read16(palette ? palette + 2U * pixel[0] : pixel), b->grey);
                blend(t->pixels + (size_t)(py - t->y0) * t->stride + 2 * (px - t->x0), colour, alpha, t->swap);
                ++written;
            }
        }
        p += step * count;
        x += count;
    }
    return written;
}

/* The part of a target between two stage columns; false when there is none. */
static bool narrowed(const chr_target_t *t, int x0, int x1, chr_target_t *part)
{
    if (x0 < t->x0) x0 = t->x0;
    if (x1 > t->x1) x1 = t->x1;
    if (x1 < x0) return false;
    *part = *t;
    part->pixels = t->pixels + 2 * (x0 - t->x0);
    part->x0 = (int16_t)x0;
    part->x1 = (int16_t)x1;
    return true;
}

/* One row of a picture, and of the patch over it when there is one: where the patch has a span the
 * picture is not drawn at all, so what the patch leaves empty there shows the stage behind her (a
 * hand that has moved away is gone, not painted over). */
static uint32_t blit_rows(const chr_target_t *t, const chr_blit_t *b, const source_t *s, const source_t *patch, int row, int y,
                          bool plain)
{
    const int x = b->x + (int)floorf(row_shift(b, (float)y) + 0.5f);
    const uint8_t *p, *end;
    int from = 0, to = 0;
    const int dx = patch ? patch->box.x - s->box.x : 0, patch_row = patch ? row - (patch->box.y - s->box.y) : 0;
    if (!patch || !row_of(patch, patch_row, &p, &end, &from, &to)) return blit_row(t, b, s, x, row, y, plain);
    const int first = x + dx + from, last = x + dx + to - 1;
    chr_target_t part;
    uint32_t written = 0;
    if (narrowed(t, t->x0, first - 1, &part)) written += blit_row(&part, b, s, x, row, y, plain);
    if (narrowed(t, last + 1, t->x1, &part)) written += blit_row(&part, b, s, x, row, y, plain);
    if (narrowed(t, first, last, &part)) written += blit_row(&part, b, patch, x + dx, patch_row, y, plain);
    return written;
}

/* One row of a sprite as pixels: a colour and an alpha (0..32) for each of its columns. */
static void unpack_row(const source_t *s, int row, int width, uint16_t *colour, uint8_t *alpha)
{
    const uint8_t *p, *end, *const palette = s->palette;
    int from, to;
    memset(alpha, 0, (size_t)width);
    if (!row_of(s, row, &p, &end, &from, &to)) return;
    int x = 0;
    while (p < end) {
        const unsigned token = *p++;
        const int count = (int)(token & (token < 0x80U ? 0x7FU : 0x3FU)) + 1;
        if (token < 0x80U) {
            x += count;
            continue;
        }
        const bool solid = token < 0xC0U;
        const int step = (solid ? 2 : 3) - (palette ? 1 : 0);
        if (p + step * count > end || x + count > width) break;   /* a damaged row */
        if (solid && !palette) memcpy(colour + x, p, 2U * (size_t)count);
        for (int i = 0; i < count; ++i) {
            const uint8_t *source = p + step * i;
            alpha[x + i] = solid ? 32U : source[0] > 32U ? 32U : source[0];
            if (solid && !palette) continue;
            const uint8_t *pixel = source + (solid ? 0 : 1);
            memcpy(colour + x + i, palette ? palette + 2U * pixel[0] : pixel, 2);
        }
        p += step * count;
        x += count;
    }
}

/* A sprite that grows about its pin. Every stage pixel takes the picture's pixel that the growth
 * brings there: the nearest row and the nearest column. Where a stage pixel looks is worked out
 * from the pin and only then moved to this picture's own corner, in whole steps, so pictures that
 * share a pin and a growth take the same rows and columns and stay exactly on top of each other.
 * A picture that only grows taller or shorter is still copied in runs, row by row. */
static uint32_t blit_grown(const chr_target_t *t, const chr_blit_t *b, const source_t *s, const source_t *patch, bool plain)
{
    const int width = s->box.width, height = s->box.height;
    const float sx = 1.0f + b->grow_x, sy = 1.0f + b->grow_y;
    const int top = b->y0 > t->y0 ? b->y0 : t->y0, bottom = b->y1 < t->y1 ? b->y1 : t->y1;
    const int left = b->x0 > t->x0 ? b->x0 : t->x0, right = b->x1 < t->x1 ? b->x1 : t->x1;
    /* Columns in 1/65536, counted from the stage column the pin is in; rows in 1/32. */
    const int pin_column = (int)floorf(b->pin_x);
    const int32_t step = (int32_t)(65536.0f / sx + 0.5f);
    const int32_t at_pin = (int32_t)floorf(((float)pin_column + 0.5f - b->pin_x) / sx * 65536.0f) +
                           (int32_t)floorf((b->pin_x - (float)b->x) * 65536.0f + 0.5f);
    const int32_t own_row = (int32_t)floorf((b->pin_y - (float)b->y) * 32.0f + 0.5f);
    const bool straight = b->grow_x == 0.0f && b->pin_x == floorf(b->pin_x);
    enum { BIAS = 4096 };       /* keeps the row count positive while it is rounded */
    uint16_t *colours = NULL, *patch_colours = NULL;
    uint8_t *alphas = NULL, *patch_alphas = NULL;
    if (!straight) {
        if (!t->scratch || width > (int)t->lines || (patch && patch->box.width > t->lines)) return 0;
        int16_t *room = t->scratch + (t->x1 - t->x0 + 1) + 4;
        colours = (uint16_t *)(void *)room;
        patch_colours = (uint16_t *)(void *)(room + t->lines);
        alphas = (uint8_t *)(void *)(room + 2 * t->lines);
        patch_alphas = alphas + t->lines;
    }
    int held = -1;
    uint32_t written = 0;
    for (int y = top; y <= bottom; ++y) {
        const int32_t v = (int32_t)floorf(((float)y + 0.5f - b->pin_y) / sy * 32.0f) + own_row + 32 * BIAS;
        if (v < 0) continue;
        const int row = (v >> 5) - BIAS;
        if (row < 0 || row >= height) continue;
        if (straight) {
            written += blit_rows(t, b, s, patch, row, y, plain);
            continue;
        }
        if (held != row) {
            unpack_row(s, row, width, colours, alphas);
            const uint8_t *p, *end;
            int from, to;
            const int patch_row = patch ? row - (patch->box.y - s->box.y) : 0;
            if (patch && row_of(patch, patch_row, &p, &end, &from, &to)) {
                /* The patch's span takes the place of the picture's pixels, empty ones included. */
                const int dx = patch->box.x - s->box.x;
                unpack_row(patch, patch_row, patch->box.width, patch_colours, patch_alphas);
                for (int column = from; column < to; ++column) {
                    if (column + dx < 0 || column + dx >= width) continue;
                    colours[column + dx] = patch_colours[column];
                    alphas[column + dx] = patch_alphas[column];
                }
            }
            held = row;
        }
        uint8_t *out = t->pixels + (size_t)(y - t->y0) * t->stride + 2U * (size_t)(left - t->x0);
        int32_t u = at_pin + (left - pin_column) * step;
        for (int x = left; x <= right; ++x, out += 2, u += step) {
            if (u < 0) continue;
            const int column = u >> 16;
            if (column >= width) break;
            const uint32_t alpha = (uint32_t)alphas[column] * b->alpha / 32U;
            if (!alpha) continue;
            blend(out, washed(colours[column], b->grey), alpha, t->swap);
            ++written;
        }
    }
    return written;
}

uint32_t chr_blit_draw(const chr_target_t *t, const chr_pack_t *pack, const chr_blit_t *b)
{
    source_t sprite, over;
    if (!t || !t->pixels || !b || !b->alpha || !source_of(pack, b->id, &sprite) || sprite.spans) return 0;
    if (b->x1 < t->x0 || b->x0 > t->x1 || b->y1 < t->y0 || b->y0 > t->y1) return 0;
    const source_t *patch = b->patch && source_of(pack, b->patch - 1U, &over) && over.spans ? &over : NULL;
    const bool plain = b->alpha >= 32U && !b->grey && !t->swap && b->turn == 0.0f;
    if (grows(b)) return blit_grown(t, b, &sprite, patch, plain);
    uint32_t written = 0;
    for (int row = 0; row < (int)sprite.box.height; ++row) {
        const int y = b->y + row;
        if (b->turn == 0.0f && (y < t->y0 || y > t->y1)) continue;
        written += blit_rows(t, b, &sprite, patch, row, y, plain);
    }
    return written;
}
