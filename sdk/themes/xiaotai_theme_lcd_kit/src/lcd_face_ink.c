// SPDX-License-Identifier: MIT
#include "lcd_face_ink.h"

#include <math.h>
#include <string.h>

enum { SUB = 16 };              /* parts of a row a column's edges are kept in */

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float mixf(float a, float b, float t) { return a + (b - a) * t; }
/* The larger and the smaller of two, without a corner where they meet. */
static float join_max(float a, float b, float k)
{
    const float h = clampf(0.5f + 0.5f * (a - b) / k, 0.0f, 1.0f);
    return mixf(b, a, h) + k * h * (1.0f - h);
}
static float join_min(float a, float b, float k) { return -join_max(-a, -b, k); }
static int floor_int(float v) { const int i = (int)v; return (float)i > v ? i - 1 : i; }

uint32_t ink_mix(uint32_t from, uint32_t to, unsigned amount)
{
    const uint32_t rb = from & 0xFF00FFU, g = from & 0x00FF00U;
    const uint32_t rb2 = to & 0xFF00FFU, g2 = to & 0x00FF00U;
    return ((((rb2 * amount + rb * (256U - amount)) >> 8) & 0xFF00FFU) | (((g2 * amount + g * (256U - amount)) >> 8) & 0x00FF00U));
}

ink_box_t ink_join(ink_box_t a, ink_box_t b)
{
    if (ink_empty(&a)) return b;
    if (ink_empty(&b)) return a;
    if (b.x0 < a.x0) a.x0 = b.x0;
    if (b.y0 < a.y0) a.y0 = b.y0;
    if (b.x1 > a.x1) a.x1 = b.x1;
    if (b.y1 > a.y1) a.y1 = b.y1;
    return a;
}

/* ------------------------------------------------------------------ shapes */

/* The upper and the lower edge at x; false outside. Edges of a glyph are its own. */
static bool edges(const ink_form_t *f, float x, float *top, float *bottom)
{
    const float dx = x - f->cx, ax = fabsf(dx);
    if (ax >= f->rx) return false;
    const float nx = dx / f->rx, n2 = nx * nx;
    const float centre = f->cy + f->shear * nx + f->bend * (n2 - 0.5f);
    if (f->glyph == INK_HEART) {
        const float an = fabsf(nx), c = an - 0.5f;
        *top = f->cy - f->ry * 0.5f - f->ry * sqrtf(fmaxf(0.0f, 0.25f - c * c));
        *bottom = f->cy + f->ry - f->ry * 1.45f * powf(an, 1.35f);
        return *bottom > *top;
    }
    if (f->glyph == INK_SPARK) {
        const float tip = 1.0f - fabsf(nx), half = f->ry * tip * tip * tip;
        *top = f->cy - half;
        *bottom = f->cy + half;
        return true;
    }
    if (f->glyph == INK_DROP) {
        const float r = sqrtf(1.0f - n2), an = fabsf(nx);
        *top = f->cy - f->ry + f->ry * 1.30f * powf(an, 0.8f);
        *bottom = f->cy + f->ry * 0.36f + f->ry * 0.64f * r;
        if (*top > f->cy + f->ry * 0.36f) *top = f->cy + f->ry * 0.36f - f->ry * 0.3f * r;
        return *bottom > *top;
    }
    float half = f->ry;
    const float corner = ax - (f->rx - f->round);
    if (corner > 0.0f && f->round > 0.0f) half += sqrtf(fmaxf(0.0f, f->round * f->round - corner * corner)) - f->round;
    if (f->soft > 0.0f) {
        /* Between an ellipse and its squarer cousin: no straight piece, no jump in curvature. */
        const float ellipse = sqrtf(1.0f - n2), square = sqrtf(sqrtf(1.0f - n2 * n2));
        half = mixf(half, f->ry * mixf(ellipse, square, 0.50f), f->soft);
    }
    /* A bar is as thick across where it runs downhill as where it runs level. */
    if (f->soft < 1.0f && (f->bend != 0.0f || f->wave != 0.0f)) {
        const float slope = (f->shear + 2.0f * f->bend * nx + f->wave * 4.712389f * cosf(nx * 4.712389f)) / f->rx;
        half *= mixf(fminf(sqrtf(1.0f + slope * slope), 1.28f), 1.0f, f->soft);
    }
    float middle = centre;
    if (f->wave != 0.0f) {
        middle += f->wave * sinf(nx * 4.712389f);
    }
    *top = middle - half * (1.0f - f->flat);
    *bottom = middle + half;
    if (f->lids) {
        const float arch = 1.0f - n2;
        const float upper = f->upper + f->upper_tilt * nx - f->upper_arch * arch;
        const float lower = f->lower + f->lower_tilt * nx - f->lower_arch * arch;
        *top = join_max(*top, upper, f->ease);
        *bottom = join_min(*bottom, lower, f->ease);
    }
    if (f->open < 1.0f) {
        const float sag = f->sag * (1.0f - f->open) * (n2 - 0.5f);
        *top = f->line + (*top - f->line) * f->open - sag;
        *bottom = f->line + (*bottom - f->line) * f->open - sag;
        /* A closed lid is a line with round ends, however thin the shape became. */
        const float least = f->least * (1.0f - f->open);
        const float end = f->rx - ax;
        const float half_line = end < least ? sqrtf(fmaxf(0.0f, least * least - (least - end) * (least - end))) : least;
        const float m = (*top + *bottom) * 0.5f;
        if (*bottom - *top < 2.0f * half_line) {
            *top = m - half_line;
            *bottom = m + half_line;
        }
    }
    return *bottom > *top;
}

/* A lid is along the top only: where the edge runs down a side, the shadow keeps on at the
 * slope of a round corner instead of following it. */
static void find_lid(ink_shape_t *shape)
{
    enum { SLOPE = SUB * 7 / 8 };
    ink_column_t *c = shape->column;
    const int count = shape->count;
    for (int i = 1; i < count; ++i) if (c[i].lid > c[i - 1].lid + SLOPE) c[i].lid = (int16_t)(c[i - 1].lid + SLOPE);
    for (int i = count - 2; i >= 0; --i) if (c[i].lid > c[i + 1].lid + SLOPE) c[i].lid = (int16_t)(c[i + 1].lid + SLOPE);
    for (int i = 0; i < count; ++i) if (c[i].lid != INT16_MAX && c[i].lid > shape->lid) shape->lid = c[i].lid;
}

void ink_form(ink_shape_t *shape, const ink_form_t *f, ink_column_t *store, size_t room, int16_t width, int16_t height)
{
    shape->column = store;
    shape->count = 0;
    shape->box = (ink_box_t){0, 0, -1, -1};
    shape->lid = 0;
    if (f->rx <= 0.0f || f->ry <= 0.0f) return;
    int x0 = floor_int(f->cx - f->rx), x1 = floor_int(f->cx + f->rx);
    if (x0 < 0) x0 = 0;
    if (x1 >= width) x1 = width - 1;
    if (x1 < x0) return;
    if ((size_t)(x1 - x0 + 1) > room) x1 = x0 + (int)room - 1;
    shape->x0 = (int16_t)x0;
    shape->count = (int16_t)(x1 - x0 + 1);
    const float high = (float)height * SUB;
    /* The edges where two columns meet serve both; between them they run straight. Where the shape
     * begins and ends they do not, and every slice is looked up. */
    float left_top = 0, left_bottom = 0, right_top = 0, right_bottom = 0;
    bool left = edges(f, (float)x0, &left_top, &left_bottom), right = false;
    for (int x = x0; x <= x1; ++x, left = right, left_top = right_top, left_bottom = right_bottom) {
        ink_column_t *c = &store[x - x0];
        int first = INT16_MAX, last = -1, full0 = 0, full1 = INT16_MAX, lid = INT16_MAX;
        right = edges(f, (float)(x + 1), &right_top, &right_bottom);
        const bool straight = left && right && x > x0 + 1 && x < x1 - 1;
        for (int s = 0; s < INK_SLICES; ++s) {
            const float part = ((float)s + 0.5f) * (1.0f / INK_SLICES);
            float top, bottom;
            bool inside = true;
            if (straight) {
                top = left_top + (right_top - left_top) * part;
                bottom = left_bottom + (right_bottom - left_bottom) * part;
            } else {
                inside = edges(f, (float)x + part, &top, &bottom);
            }
            if (!inside) {
                c->top[s] = c->bottom[s] = 0;
                full1 = -1;
                continue;
            }
            const int t = (int)clampf(top * SUB + 0.5f, 0.0f, high), b = (int)clampf(bottom * SUB + 0.5f, 0.0f, high);
            c->top[s] = (int16_t)t;
            c->bottom[s] = (int16_t)b;
            if (b <= t) {
                full1 = -1;
                continue;
            }
            if (t < lid) lid = t;
            if (t / SUB < first) first = t / SUB;
            if ((b - 1) / SUB > last) last = (b - 1) / SUB;
            if ((t + SUB - 1) / SUB > full0) full0 = (t + SUB - 1) / SUB;
            if (b / SUB - 1 < full1) full1 = b / SUB - 1;
        }
        c->first = (int16_t)first;
        c->last = (int16_t)last;
        c->lid = (int16_t)lid;
        c->full0 = (int16_t)full0;
        c->full1 = (int16_t)(full1 == INT16_MAX ? -1 : full1);
        if (last < first) continue;
        shape->box = ink_join(shape->box, (ink_box_t){(int16_t)x, (int16_t)first, (int16_t)x, (int16_t)last});
    }
    if (f->shade) find_lid(shape);
    if (ink_empty(&shape->box)) shape->count = 0;
    shape->whole = f->glyph == INK_BAR && f->bend == 0.0f && f->wave == 0.0f && f->sag == 0.0f &&
                   (!f->lids || (f->upper_arch >= 0.0f && f->lower_arch <= 0.0f));
}

static unsigned cover(const ink_column_t *c, int y)
{
    if (y < c->first || y > c->last) return 0;
    if (y >= c->full0 && y <= c->full1) return INK_SOLID;
    const int a = y * SUB, b = a + SUB;
    int sum = 0;
    for (int s = 0; s < INK_SLICES; ++s) {
        const int t = c->top[s] > a ? c->top[s] : a, e = c->bottom[s] < b ? c->bottom[s] : b;
        if (e > t) sum += e - t;
    }
    return (unsigned)sum * (INK_SOLID / (SUB * INK_SLICES));
}

/* -------------------------------------------------------------------- rows */

static inline uint16_t pack(uint32_t rgb) { return (uint16_t)((rgb >> 8 & 0xF800U) | (rgb >> 5 & 0x07E0U) | (rgb >> 3 & 0x001FU)); }

void ink_row(ink_t *ink, int16_t y)
{
    ink->y = y;
    ink->masked = NULL;
    const int n = ink->clip.x1 - ink->clip.x0 + 1;
    for (int i = 0; i < n; ++i) ink->line[i] = ink->background;
}

void ink_row_end(ink_t *ink)
{
    /* Ordered dither, in eighths of a step of red and blue: a ramp of colours
     * shows no bands, flat paper stays flat. */
    static const uint8_t pattern[4][4] = {{0, 4, 1, 5}, {6, 2, 7, 3}, {1, 5, 0, 4}, {7, 3, 6, 2}};
    const uint8_t *d = pattern[ink->y & 3];
    const int n = ink->clip.x1 - ink->clip.x0 + 1;
    uint8_t *out = ink->surface->pixels + (size_t)(ink->y - ink->top) * ink->surface->stride + 2U * (size_t)(ink->clip.x0 - ink->left);
    const uint32_t *line = ink->line;
    const bool swap = ink->surface->swap_bytes;
    /* Neighbours are mostly of one colour: its four shades are worked out once and laid in pairs. */
    for (int i = 0; i < n;) {
        const uint32_t c = line[i];
        int end = i + 1;
        while (end < n && line[end] == c) ++end;
        uint16_t shade[4];
        if (c == ink->background) {
            shade[0] = swap ? (uint16_t)(ink->paper << 8 | ink->paper >> 8) : ink->paper;
            shade[1] = shade[2] = shade[3] = shade[0];
        } else {
            const unsigned red = c >> 16 & 255U, green = c >> 8 & 255U, blue = c & 255U;
            /* Only the shades this run gets to. */
            for (int k = i; k < end && k < i + 4; ++k) {
                const unsigned at = (unsigned)(ink->clip.x0 + k) & 3U;
                unsigned r = red + d[at], g = green + (d[at] >> 1), b = blue + d[at];
                if (r > 255U) r = 255U;
                if (g > 255U) g = 255U;
                if (b > 255U) b = 255U;
                const unsigned v = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3;
                shade[at] = (uint16_t)(swap ? v << 8 | v >> 8 : v);
            }
        }
        uint16_t *p = (uint16_t *)(void *)(out + 2 * i);
        int x = ink->clip.x0 + i;
        const int stop = ink->clip.x0 + end;
        if (stop - x >= 8) {
            for (; (uintptr_t)p & 3U; ++x) *p++ = shade[x & 3];
            const uint32_t one = shade[x & 3] | (uint32_t)shade[(x + 1) & 3] << 16, two = shade[(x + 2) & 3] | (uint32_t)shade[(x + 3) & 3] << 16;
            uint32_t *w = (uint32_t *)(void *)p;
            for (; x + 4 <= stop; x += 4) {
                *w++ = one;
                *w++ = two;
            }
            p = (uint16_t *)(void *)w;
        }
        for (; x < stop; ++x) *p++ = shade[x & 3];
        i = end;
    }
}

static inline void put(uint32_t *p, uint32_t rgb, unsigned a)
{
    *p = a >= INK_SOLID ? rgb : ink_mix(*p, rgb, a);
}

/* The coverage of a shape on this row, once per row and shape. */
static const uint16_t *mask_of(ink_t *ink, const ink_shape_t *shape)
{
    if (!shape) return NULL;
    if (ink->masked == shape) return ink->mask;
    ink->masked = shape;
    ink->from = ink->full0 = 0;
    ink->to = ink->full1 = -1;
    int x0 = shape->x0 > ink->clip.x0 ? shape->x0 : ink->clip.x0;
    int x1 = shape->x0 + shape->count - 1;
    if (x1 > ink->clip.x1) x1 = ink->clip.x1;
    if (ink->y < shape->box.y0 || ink->y > shape->box.y1 || x1 < x0) return ink->mask;
    uint16_t *mask = ink->mask - ink->clip.x0;
    const ink_column_t *column = shape->column - shape->x0;
    int from = x0, to = x1;
    /* From both ends to where it covers everything; what lies between is whole, or is looked at too. */
    while (from <= to && !(mask[from] = (uint16_t)cover(&column[from], ink->y))) ++from;
    if (from > to) return ink->mask;
    while (to > from && !(mask[to] = (uint16_t)cover(&column[to], ink->y))) --to;
    int full0 = from, full1 = to;
    while (full0 < full1 && mask[full0] < INK_SOLID) {
        ++full0;
        mask[full0] = (uint16_t)cover(&column[full0], ink->y);
    }
    while (full1 > full0 && mask[full1] < INK_SOLID) {
        --full1;
        if (full1 > full0) mask[full1] = (uint16_t)cover(&column[full1], ink->y);
    }
    if (shape->whole || full1 - full0 < 2) {
        for (int x = full0 + 1; x < full1; ++x) mask[x] = INK_SOLID;
        ink->full0 = (int16_t)full0;
        ink->full1 = (int16_t)(mask[full0] < INK_SOLID ? full0 - 1 : full1);
    } else {
        for (int x = full0 + 1; x < full1; ++x) mask[x] = (uint16_t)cover(&column[x], ink->y);
        ink->full0 = (int16_t)full0;
        ink->full1 = (int16_t)(full0 - 1);
    }
    ink->from = (int16_t)from;
    ink->to = (int16_t)to;
    return ink->mask;
}

/* Keeps a span to where the clipping shape is. */
static bool within(const ink_t *ink, const uint16_t *mask, int *x0, int *x1)
{
    if (mask) {
        if (*x0 < ink->from) *x0 = ink->from;
        if (*x1 > ink->to) *x1 = ink->to;
    }
    return *x1 >= *x0;
}

/* The pixels of the row a thing from cx - rx to cx + rx touches; false for none. */
static bool span(const ink_t *ink, float cx, float rx, int *from, int *to)
{
    int x0 = floor_int(cx - rx), x1 = floor_int(cx + rx);
    if (x0 < ink->clip.x0) x0 = ink->clip.x0;
    if (x1 > ink->clip.x1) x1 = ink->clip.x1;
    *from = x0;
    *to = x1;
    return x1 >= x0;
}

void ink_shape(ink_t *ink, const ink_shape_t *shape, uint32_t top, uint32_t bottom, unsigned alpha, const ink_shape_t *inside)
{
    if (!alpha || !shape->count || ink->y < shape->box.y0 || ink->y > shape->box.y1) return;
    const int high = shape->box.y1 - shape->box.y0;
    const uint32_t rgb = top == bottom || !high ? top : ink_mix(top, bottom, (unsigned)((ink->y - shape->box.y0) * 256 / high));
    uint32_t *line = ink->line - ink->clip.x0;
    if (!inside) {
        const uint16_t *mask = mask_of(ink, shape) - ink->clip.x0;
        const int full0 = ink->full0, full1 = ink->full1;
        int x = ink->from;
        for (; x <= ink->to && (full1 < full0 || x < full0); ++x) put(&line[x], rgb, mask[x] * alpha >> 8);
        if (full1 < full0) return;
        if (alpha >= INK_SOLID) for (; x <= full1; ++x) line[x] = rgb;
        else for (; x <= full1; ++x) line[x] = ink_mix(line[x], rgb, alpha);
        for (; x <= ink->to; ++x) put(&line[x], rgb, mask[x] * alpha >> 8);
        return;
    }
    const uint16_t *mask = mask_of(ink, inside) - ink->clip.x0;
    int x0 = shape->x0, x1 = shape->x0 + shape->count - 1;
    if (!within(ink, mask, &x0, &x1)) return;
    const ink_column_t *column = shape->column - shape->x0;
    for (int x = x0; x <= x1; ++x) {
        const unsigned a = cover(&column[x], ink->y);
        if (a) put(&line[x], rgb, (a * mask[x] >> 8) * alpha >> 8);
    }
}

void ink_lid(ink_t *ink, const ink_shape_t *shape, uint32_t rgb, float depth, unsigned alpha)
{
    const int reach = (int)(depth * SUB);
    if (!alpha || reach <= 0 || !shape->count || ink->y < shape->box.y0 || ink->y > shape->box.y1) return;
    if (ink->y * SUB > shape->lid + reach) return;
    const uint16_t *mask = mask_of(ink, shape) - ink->clip.x0;
    const ink_column_t *column = shape->column - shape->x0;
    uint32_t *line = ink->line - ink->clip.x0;
    const int row = ink->y * SUB + SUB / 2;
    for (int x = ink->from; x <= ink->to; ++x) {
        const int below = row - column[x].lid;
        if (below >= reach) continue;
        const unsigned fade = below <= 0 ? 256U : (unsigned)((reach - below) * 256 / reach);
        put(&line[x], rgb, (mask[x] * alpha >> 8) * (fade * fade >> 8) >> 8);
    }
}

void ink_disc(ink_t *ink, float cx, float cy, float rx, float ry, uint32_t rgb, unsigned alpha, const ink_shape_t *inside)
{
    int x0, x1;
    if (!alpha || rx <= 0.0f || ry <= 0.0f) return;
    const float dy = ((float)ink->y + 0.5f - cy) / ry;
    const float edge = (rx < ry ? rx : ry) * 0.5f;
    const float far2 = 1.0f + 1.0f / edge;       /* beyond it nothing is covered */
    if (dy * dy > far2 || !span(ink, cx, rx + 1.0f, &x0, &x1)) return;
    const uint16_t *mask = mask_of(ink, inside);
    if (!within(ink, mask, &x0, &x1)) return;
    const float step = 1.0f / rx;
    float nx = ((float)x0 + 0.5f - cx) * step;
    for (int x = x0; x <= x1; ++x, nx += step) {
        const float c = (1.0f - nx * nx - dy * dy) * edge + 0.5f;
        if (c <= 0.0f) continue;
        unsigned a = c >= 1.0f ? INK_SOLID : (unsigned)(c * 256.0f);
        if (mask) a = a * mask[x - ink->clip.x0] >> 8;
        put(&ink->line[x - ink->clip.x0], rgb, a * alpha >> 8);
    }
}

void ink_soft(ink_t *ink, float cx, float cy, float rx, float ry, uint32_t rgb, unsigned alpha, const ink_shape_t *inside)
{
    int x0, x1;
    if (!alpha || rx <= 0.0f || ry <= 0.0f) return;
    const float dy = ((float)ink->y + 0.5f - cy) / ry, room = 1.0f - dy * dy;
    if (room <= 0.0f || !span(ink, cx, rx, &x0, &x1)) return;
    const uint16_t *mask = mask_of(ink, inside);
    if (!within(ink, mask, &x0, &x1)) return;
    const float step = 1.0f / rx;
    float nx = ((float)x0 + 0.5f - cx) * step;
    for (int x = x0; x <= x1; ++x, nx += step) {
        const float v = room - nx * nx;
        if (v <= 0.0f) continue;
        unsigned a = (unsigned)(v * v * 256.0f) * alpha >> 8;
        if (mask) a = a * mask[x - ink->clip.x0] >> 8;
        put(&ink->line[x - ink->clip.x0], rgb, a);
    }
}

void ink_stroke(ink_t *ink, float ax, float ay, float bx, float by, float radius, uint32_t rgb, unsigned alpha,
                const ink_shape_t *inside)
{
    if (!alpha || radius <= 0.0f) return;
    const float y = (float)ink->y + 0.5f;
    const float lo = (ay < by ? ay : by) - radius - 0.5f, hi = (ay > by ? ay : by) + radius + 0.5f;
    if (y < lo || y > hi) return;
    int x0, x1;
    const float left = ax < bx ? ax : bx, right = ax > bx ? ax : bx;
    if (!span(ink, (left + right) * 0.5f, (right - left) * 0.5f + radius + 1.0f, &x0, &x1)) return;
    const uint16_t *mask = mask_of(ink, inside);
    if (!within(ink, mask, &x0, &x1)) return;
    const float dx = bx - ax, dy = by - ay, length2 = dx * dx + dy * dy, inverse = length2 > 0.0f ? 1.0f / length2 : 0.0f;
    const float outer = radius + 0.5f, inner = radius > 0.5f ? radius - 0.5f : 0.0f;
    for (int x = x0; x <= x1; ++x) {
        float px = (float)x + 0.5f - ax, py = y - ay;
        const float t = clampf((px * dx + py * dy) * inverse, 0.0f, 1.0f);
        px -= dx * t;
        py -= dy * t;
        const float d2 = px * px + py * py;
        if (d2 >= outer * outer) continue;
        unsigned a = d2 <= inner * inner ? INK_SOLID : (unsigned)((outer - sqrtf(d2)) * 256.0f);
        if (a > INK_SOLID) a = INK_SOLID;
        if (mask) a = a * mask[x - ink->clip.x0] >> 8;
        put(&ink->line[x - ink->clip.x0], rgb, a * alpha >> 8);
    }
}

void ink_iris(ink_t *ink, float cx, float cy, float rx, float ry, const uint32_t ramp[INK_TIERS][INK_RAMP], unsigned alpha,
              const ink_shape_t *inside)
{
    int x0, x1;
    if (!alpha || rx <= 0.0f || ry <= 0.0f) return;
    const float dy = ((float)ink->y + 0.5f - cy) / ry, dy2 = dy * dy;
    const float edge = (rx < ry ? rx : ry) * 0.5f, far2 = 1.0f + 0.5f / edge, near2 = 1.0f - 0.5f / edge;
    if (dy2 > far2 || !span(ink, cx, rx + 1.0f, &x0, &x1)) return;
    const uint16_t *mask = mask_of(ink, inside);
    if (!within(ink, mask, &x0, &x1)) return;
    /* Between two tiers; where the iris is small, the nearer one. */
    const float at = clampf(dy * 0.5f + 0.5f, 0.0f, 0.999f) * (INK_TIERS - 1);
    const unsigned tier = (unsigned)at, part = (unsigned)((at - (float)tier) * 256.0f);
    const uint32_t *row = ramp[part < 128U ? tier : tier + 1U];
    uint32_t between[INK_RAMP];
    const bool fine = ry > 20.0f;
    if (fine) {
        unsigned i = (unsigned)(dy2 * (INK_RAMP - 1));
        if (i > INK_RAMP - 1) i = INK_RAMP - 1;
        for (; i < INK_RAMP; ++i) between[i] = ink_mix(ramp[tier][i], ramp[tier + 1U][i], part);
        row = between;
    }
    const float step = 1.0f / rx;
    float nx = ((float)x0 + 0.5f - cx) * step;
    uint32_t *line = ink->line - ink->clip.x0;
    for (int x = x0; x <= x1; ++x, nx += step) {
        const float r2 = nx * nx + dy2;
        if (r2 >= far2) continue;
        unsigned a = INK_SOLID;
        uint32_t rgb = row[INK_RAMP - 1];
        if (r2 > near2) a = (unsigned)(((1.0f - r2) * edge + 0.5f) * 256.0f);
        if (r2 < 1.0f) {
            const float r = r2 * (INK_RAMP - 1);
            const unsigned i = (unsigned)r;
            rgb = fine && i < INK_RAMP - 1 ? ink_mix(row[i], row[i + 1], (unsigned)((r - (float)i) * 256.0f)) : row[i];
        }
        if (mask) a = a * mask[x - ink->clip.x0] >> 8;
        put(&line[x], rgb, a * alpha >> 8);
    }
}
