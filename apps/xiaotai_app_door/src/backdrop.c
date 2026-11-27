// SPDX-License-Identifier: MIT
/* The pictures behind the door's home screens, drawn by this code once into
 * memory: a lit front door at night for the door station, a room with a
 * window at dusk for the indoor station. They need no picture in flash, fit
 * any wide display and stay dark where the words stand (the left side).
 *
 * One row at a time: every shape that crosses the row is blended into three
 * channels of 16 bits, then the row is dithered into RGB565. Lengths are in
 * units of a 600 rows high picture; x counts from the left edge, or from the
 * right one for what stands on the right. */
#include "backdrop.h"

#include <math.h>
#include <string.h>

enum { MAX_WIDTH = 1280, SCALE_BITS = 7 };

typedef struct {
    uint16_t *c[3];
    int width, height, y, first;
    float u;             /* pixels per unit */
    float yc;            /* centre of the row, pixels */
    uint16_t *left;      /* alpha of shade_left(), the same in every row; worked out in the first */
} row_t;

static int channel(uint32_t color, int i) { return (int)(color >> (16 - 8 * i)) & 0xff; }
static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static float fclamp(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

static uint32_t mix(uint32_t a, uint32_t b, float t)
{
    uint32_t out = 0;
    t = fclamp(t);
    for (int i = 0; i < 3; ++i) {
        const int v = (int)((float)channel(a, i) + (float)(channel(b, i) - channel(a, i)) * t + 0.5f);
        out = out << 8 | (uint32_t)v;
    }
    return out;
}

/* alpha 0..256 */
static inline void blend(row_t *r, int x, const int target[3], int alpha)
{
    for (int i = 0; i < 3; ++i) {
        const int now = r->c[i][x];
        r->c[i][x] = (uint16_t)(now + (((target[i] - now) * alpha) >> 8));
    }
}

/* The same over [from, to]. Most of a row is spans: one channel at a time, and
 * nothing to work out where the colour covers. */
static void blend_span(row_t *r, int from, int to, const int target[3], int alpha)
{
    if (alpha <= 0) return;
    for (int i = 0; i < 3; ++i) {
        const int colour = target[i];
        uint16_t *const c = r->c[i];
        if (alpha >= 256) for (int x = from; x <= to; ++x) c[x] = (uint16_t)colour;
        else for (int x = from; x <= to; ++x) c[x] = (uint16_t)(c[x] + (((colour - c[x]) * alpha) >> 8));
    }
}

static void set_target(int out[3], uint32_t color)
{
    for (int i = 0; i < 3; ++i) out[i] = channel(color, i) << SCALE_BITS;
}

/* Whole row, vertical gradient between two heights (units). */
static void band(row_t *r, float y0, float y1, uint32_t top, uint32_t bottom)
{
    const float a = y0 * r->u, b = y1 * r->u;
    if (r->yc < a || r->yc >= b) return;
    const float t = (r->yc - a) / (b - a);
    for (int i = 0; i < 3; ++i) {
        const int v = (int)(((float)channel(top, i) + (float)(channel(bottom, i) - channel(top, i)) * t) * (1 << SCALE_BITS));
        for (int x = 0; x < r->width; ++x) r->c[i][x] = (uint16_t)v;
    }
}

/* Rounded rectangle with a vertical gradient, smoothed edges. Pixels. */
static void rect_px(row_t *r, float x0, float y0, float x1, float y1, float radius, uint32_t top, uint32_t bottom, float opacity)
{
    if (r->yc < y0 - 1 || r->yc > y1 + 1 || x1 <= x0) return;
    const float cy = (y0 + y1) * 0.5f, hy = (y1 - y0) * 0.5f, cx = (x0 + x1) * 0.5f, hx = (x1 - x0) * 0.5f;
    radius = fminf(radius, fminf(hx, hy));
    const float qy = fabsf(r->yc - cy) - hy + radius;
    int target[3];
    set_target(target, mix(top, bottom, (r->yc - y0) / (y1 - y0)));
    const int from = imax(0, (int)(x0 - 1)), to = imin(r->width - 1, (int)(x1 + 1));
    const int full = (int)(opacity * 256);
    const int inner0 = (int)(x0 + radius + 1), inner1 = (int)(x1 - radius - 2);
    const float row_cover = fclamp(0.5f - (qy - radius));
    if (inner0 <= inner1) blend_span(r, imax(from, inner0), imin(to, inner1), target, (int)(row_cover * (float)full));
    for (int x = from; x <= to; ++x) {
        if (x >= inner0 && x <= inner1) {
            x = inner1;
            continue;
        }
        const float qx = fabsf((float)x + 0.5f - cx) - hx + radius;
        const float ox = fmaxf(qx, 0), oy = fmaxf(qy, 0);
        const float d = sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0) - radius;
        const float cover = fclamp(0.5f - d);
        if (cover > 0) blend(r, x, target, (int)(cover * (float)full));
    }
}

/* Units; x from the right edge when `right`. */
static void rect(row_t *r, bool right, float x, float y, float w, float h, float radius, uint32_t top, uint32_t bottom,
                 float opacity)
{
    const float x0 = right ? (float)r->width - x * r->u : x * r->u;
    rect_px(r, x0, y * r->u, x0 + w * r->u, (y + h) * r->u, radius * r->u, top, bottom, opacity);
}

/* Soft light: full in the middle, nothing at the rim. */
static void glow(row_t *r, bool right, float x, float y, float rx, float ry, uint32_t color, float opacity)
{
    const float cx = right ? (float)r->width - x * r->u : x * r->u, cy = y * r->u;
    rx *= r->u;
    ry *= r->u;
    const float dy = (r->yc - cy) / ry;
    if (dy <= -1 || dy >= 1) return;
    const float half = rx * sqrtf(1 - dy * dy);
    const int from = imax(0, (int)(cx - half)), to = imin(r->width - 1, (int)(cx + half));
    int target[3];
    set_target(target, color);
    const float step = 1 / rx;
    float dx = ((float)from + 0.5f - cx) * step;
    for (int x = from; x <= to; ++x, dx += step) {
        const float left = 1 - dx * dx - dy * dy;
        if (left > 0) blend(r, x, target, (int)(left * left * opacity * 256));
    }
}

/* Ellipse, turned by `angle` (radians), smoothed edge. */
static void ellipse(row_t *r, bool right, float x, float y, float rx, float ry, float angle, uint32_t color, float opacity)
{
    const float cx = right ? (float)r->width - x * r->u : x * r->u, cy = y * r->u;
    rx *= r->u;
    ry *= r->u;
    const float reach = fmaxf(rx, ry) + 1;
    if (r->yc < cy - reach || r->yc > cy + reach) return;
    const float s = sinf(angle), c = cosf(angle), dy = r->yc - cy;
    int target[3];
    set_target(target, color);
    const int from = imax(0, (int)(cx - reach)), to = imin(r->width - 1, (int)(cx + reach));
    const float soft = fminf(rx, ry);
    for (int px = from; px <= to; ++px) {
        const float dx = (float)px + 0.5f - cx;
        const float a = (dx * c + dy * s) / rx, b = (dy * c - dx * s) / ry;
        const float cover = fclamp((1 - sqrtf(a * a + b * b)) * soft + 0.5f);
        if (cover > 0) blend(r, px, target, (int)(cover * opacity * 256));
    }
}

/* A cone of light between two heights, fading along its way. */
static void cone(row_t *r, bool right, float x, float y0, float w0, float y1, float w1, uint32_t color, float opacity)
{
    const float cx = right ? (float)r->width - x * r->u : x * r->u;
    const float t = (r->yc / r->u - y0) / (y1 - y0);
    if (t < 0 || t > 1) return;
    const float half = (w0 + (w1 - w0) * t) * 0.5f * r->u;
    int target[3];
    set_target(target, color);
    const int from = imax(0, (int)(cx - half)), to = imin(r->width - 1, (int)(cx + half));
    const float fade = (1 - t) * (1 - t) * opacity * 256;
    for (int px = from; px <= to; ++px) {
        const float d = fabsf((float)px + 0.5f - cx) / half;
        const float side = fclamp((1 - d) * 1.6f);
        blend(r, px, target, (int)(side * side * fade));
    }
}

/* Darker where the words stand. */
static void shade_left(row_t *r, float until, uint32_t color, float opacity)
{
    const int to = imin(r->width, (int)(until * (float)r->width));
    int target[3];
    set_target(target, color);
    for (int x = 0; r->y == r->first && x < to; ++x) {
        const float t = 1 - (float)x / (float)to;
        r->left[x] = (uint16_t)(t * t * (3 - 2 * t) * opacity * 256);
    }
    for (int i = 0; i < 3; ++i) {
        uint16_t *const c = r->c[i];
        for (int x = 0; x < to; ++x) c[x] = (uint16_t)(c[x] + (((target[i] - c[x]) * r->left[x]) >> 8));
    }
}

/* Darker under the marks and names at the top. */
static void shade_top(row_t *r, float until, uint32_t color, float opacity)
{
    const float t = 1 - r->yc / (until * r->u);
    if (t <= 0) return;
    int target[3];
    set_target(target, color);
    const int alpha = (int)(fminf(1, t * 1.8f) * opacity * 256);
    blend_span(r, 0, r->width - 1, target, alpha);
}

/* ------------------------------------------------- the front door at night */

static void door_scene(row_t *r)
{
    enum { WALL = 0x24303c, WALL_LOW = 0x161e27, WARM = 0xffc886, WARM_SOFT = 0xffa95a };
    band(r, 0, 506, WALL, WALL_LOW);
    band(r, 506, 601, 0x2a323a, 0x12171c);
    /* Boards of the wall: a dark gap with a faint edge above it. */
    for (int i = 1; i <= 10; ++i) {
        const float y = (float)i * 46.0f;
        if (y > 500) break;
        rect(r, false, -2, y, 1400, 2.2f, 0, 0x0c1218, 0x0c1218, 0.55f);
        rect(r, false, -2, y + 2.4f, 1400, 1.2f, 0, 0x3d4b59, 0x3d4b59, 0.40f);
    }
    rect(r, false, -2, 500, 1400, 6, 0, 0x0b1015, 0x0b1015, 0.9f);

    /* Light of the two lamps on the wall and on the ground. */
    glow(r, true, 415, 190, 190, 250, WARM_SOFT, 0.34f);
    glow(r, true, 58, 190, 150, 240, WARM_SOFT, 0.26f);
    glow(r, true, 240, 548, 330, 60, WARM_SOFT, 0.36f);

    /* Door: frame, glass above it, leaf. */
    rect(r, true, 360, 70, 246, 438, 10, 0x0c1116, 0x080b0f, 1);
    rect(r, true, 348, 82, 222, 46, 5, 0xffe2b4, 0xf4b366, 0.92f);
    rect(r, true, 238, 82, 3, 46, 0, 0x0c1116, 0x0c1116, 1);
    rect(r, true, 348, 140, 222, 366, 4, 0x6e4c34, 0x46301f, 1);
    for (int i = 1; i < 6; ++i) {
        rect(r, true, 348 - (float)i * 37.0f, 140, 1.6f, 366, 0, 0x2c1d13, 0x24170f, 0.8f);
        rect(r, true, 346 - (float)i * 37.0f, 140, 1.0f, 366, 0, 0x8a6446, 0x5a3f2a, 0.5f);
    }
    /* Its narrow pane, warm from inside, and the bars of it. */
    rect(r, true, 322, 168, 34, 250, 5, 0x22150c, 0x1c120a, 0.9f);
    rect(r, true, 318, 172, 26, 242, 3, 0xffdca6, 0xe99a4c, 0.95f);
    for (int i = 1; i < 4; ++i) rect(r, true, 318, 172 + (float)i * 60.0f, 26, 3, 0, 0x22150c, 0x22150c, 0.9f);
    glow(r, true, 305, 290, 90, 190, WARM, 0.16f);
    /* Handle and lock. */
    rect(r, true, 158, 262, 9, 150, 4.5f, 0xe4e8ec, 0x8b949d, 1);
    rect(r, true, 161, 262, 3, 150, 1.5f, 0x5a636b, 0x4a525a, 0.5f);
    ellipse(r, true, 153.5f, 440, 10, 10, 0, 0xb9c1c9, 1);
    ellipse(r, true, 153.5f, 440, 4, 4, 0, 0x20262c, 1);
    /* The shadow the frame throws on the leaf. */
    rect(r, true, 348, 140, 222, 10, 0, 0x000000, 0x2a1b10, 0.45f);

    /* Lamps: up and down. */
    for (int side = 0; side < 2; ++side) {
        const float x = side ? 58.0f : 415.0f;
        cone(r, true, x, 196, 30, 470, 200, WARM, side ? 0.36f : 0.50f);
        cone(r, true, x, 152, 30, 20, 150, WARM, side ? 0.28f : 0.40f);
        rect(r, true, x + 13, 148, 26, 52, 5, 0x10151a, 0x090c10, 1);
        rect(r, true, x + 10, 195, 20, 5, 2, 0xfff1d8, 0xffd9a3, 1);
        rect(r, true, x + 10, 148, 20, 5, 2, 0xfff1d8, 0xffd9a3, 1);
    }

    /* The bell panel beside the door, with the accent of the interface. */
    rect(r, true, 436, 262, 42, 96, 9, 0x2e3944, 0x1a222a, 1);
    rect(r, true, 434, 264, 38, 92, 7, 0x11171d, 0x0c1116, 1);
    ellipse(r, true, 415, 286, 9, 9, 0, 0x2a3641, 1);
    ellipse(r, true, 415, 286, 5, 5, 0, 0x0a0e12, 1);
    glow(r, true, 415, 328, 34, 34, 0x5ec8f2, 0.55f);
    ellipse(r, true, 415, 328, 13, 13, 0, 0x5ec8f2, 1);
    ellipse(r, true, 415, 328, 10, 10, 0, 0x123242, 1);

    /* Step, mat and the plant. */
    rect(r, true, 384, 506, 294, 22, 3, 0x59636c, 0x343c44, 1);
    rect(r, true, 384, 506, 294, 3, 1, 0xd9b184, 0xd9b184, 0.55f);
    rect(r, true, 316, 512, 158, 9, 4, 0x2a221c, 0x1a1511, 0.95f);
    rect(r, true, 390, 528, 306, 8, 2, 0x05070a, 0x05070a, 0.55f);
    static const struct { float x, y, rx, ry, angle; uint32_t color; } leaves[] = {
        {66, 420, 13, 46, -0.50f, 0x1f4a38}, {34, 424, 13, 46, 0.55f, 0x1b4233}, {50, 400, 14, 54, 0.04f, 0x2a6349},
        {78, 448, 11, 38, -1.00f, 0x18382c}, {22, 450, 11, 38, 1.00f, 0x16342a}, {58, 432, 11, 42, -0.22f, 0x2f6d50},
        {42, 434, 11, 42, 0.26f, 0x245840},
    };
    for (size_t i = 0; i < sizeof(leaves) / sizeof(*leaves); ++i)
        ellipse(r, true, leaves[i].x, leaves[i].y, leaves[i].rx, leaves[i].ry, leaves[i].angle, leaves[i].color, 1);
    rect(r, true, 84, 462, 68, 66, 8, 0x515b64, 0x2a3138, 1);
    rect(r, true, 88, 458, 76, 14, 4, 0x636e78, 0x414a52, 1);
    shade_left(r, 0.66f, 0x0b1117, 0.62f);
}

/* ---------------------------------------------- the room, window at dusk */

static void room_scene(row_t *r)
{
    enum { WARM = 0xffcf96, WARM_SOFT = 0xffa960 };
    band(r, 0, 520, 0x2a2c35, 0x20212a);
    band(r, 520, 601, 0x33261e, 0x1a130f);
    /* Panelling on the lower wall, floor boards. */
    rect(r, false, -2, 356, 1400, 3, 0, 0x3a3c47, 0x3a3c47, 0.8f);
    rect(r, false, -2, 359, 1400, 161, 0, 0x23242d, 0x1b1c24, 0.9f);
    for (int i = 0; i < 12; ++i) {
        rect(r, true, 1050 - (float)i * 92.0f, 376, 72, 128, 3, 0x30323c, 0x292b34, 0.55f);
        rect(r, true, 1048 - (float)i * 92.0f, 378, 68, 124, 2, 0x23242d, 0x1e1f27, 0.75f);
    }
    rect(r, false, -2, 508, 1400, 12, 0, 0x3b3d48, 0x2a2b34, 0.9f);
    for (int i = 1; i < 4; ++i) rect(r, false, -2, 520 + (float)(i * i) * 7.0f, 1400, 1.4f, 0, 0x120c08, 0x120c08, 0.7f);

    /* Window: frame, sky, houses with lit windows, bars. */
    rect(r, true, 392, 54, 304, 268, 8, 0x4b4a52, 0x3b3a42, 1);
    rect(r, true, 382, 64, 284, 248, 3, 0x1a3358, 0x1a3358, 1);
    {
        rect(r, true, 382, 64, 284, 130, 3, 0x152c52, 0x2c4a76, 1);
        rect(r, true, 382, 194, 284, 70, 0, 0x2c4a76, 0x8a6a78, 1);
        rect(r, true, 382, 264, 284, 48, 0, 0x8a6a78, 0xe0945f, 1);
        glow(r, true, 240, 318, 200, 90, 0xffb37a, 0.55f);
        ellipse(r, true, 312, 112, 15, 15, 0, 0xf4efe2, 0.95f);
        ellipse(r, true, 305, 108, 13, 13, 0, 0x1f3c68, 1);
        static const struct { float x, y; } stars[] = {{352, 92}, {270, 84}, {210, 104}, {168, 82}, {134, 118}, {236, 140}};
        for (size_t i = 0; i < sizeof(stars) / sizeof(*stars); ++i)
            ellipse(r, true, stars[i].x, stars[i].y, 1.7f, 1.7f, 0, 0xe6ecf4, 0.85f);
        static const struct { float x, y, w; uint32_t color; } houses[] = {
            {382, 246, 60, 0x14202f}, {330, 222, 46, 0x1a2839}, {290, 258, 70, 0x111c29}, {226, 232, 40, 0x182636},
            {190, 252, 52, 0x121d2b}, {140, 226, 42, 0x17253a},
        };
        for (size_t i = 0; i < sizeof(houses) / sizeof(*houses); ++i) {
            rect(r, true, houses[i].x, houses[i].y, houses[i].w, 312 - houses[i].y, 1, houses[i].color, houses[i].color, 1);
            for (int k = 0; k < 2; ++k)
                rect(r, true, houses[i].x - 10 - (float)k * 18.0f, houses[i].y + 14 + (float)((i + (size_t)k) % 2) * 18.0f, 8, 10, 1,
                     0xffcf8a, 0xf3a65a, 0.9f);
        }
    }
    rect(r, true, 243, 60, 6, 256, 0, 0x4b4a52, 0x3b3a42, 1);
    rect(r, true, 386, 182, 292, 5, 0, 0x4b4a52, 0x46454d, 1);
    /* Curtains. */
    for (int side = 0; side < 2; ++side) {
        const float x = side ? 118.0f : 424.0f;
        rect(r, true, x, 40, 54, 316, 6, 0x5b5563, 0x3c3843, 1);
        for (int k = 1; k < 4; ++k) rect(r, true, x - (float)k * 13.0f, 40, 3.4f, 316, 1, 0x3b3742, 0x2a2730, 0.8f);
    }
    rect(r, true, 448, 32, 412, 9, 4.5f, 0x8d8a92, 0x5d5a63, 1);

    /* Console, lamp, and a panel of the door phone. */
    glow(r, true, 330, 396, 210, 170, WARM_SOFT, 0.42f);
    rect(r, true, 404, 452, 330, 12, 3, 0x8a6346, 0x5a3f2c, 1);
    rect(r, true, 392, 464, 10, 72, 1, 0x4a3424, 0x2e2016, 1);
    rect(r, true, 96, 464, 10, 72, 1, 0x4a3424, 0x2e2016, 1);
    rect(r, true, 388, 464, 298, 22, 1, 0x5f4431, 0x402d20, 1);
    rect(r, true, 333, 404, 6, 46, 2, 0x2b2420, 0x201a17, 1);
    rect(r, true, 348, 446, 36, 7, 3, 0x3a302a, 0x241d19, 1);
    ellipse(r, true, 330, 392, 35, 8, 0, 0xffe9c6, 1);
    rect(r, true, 364, 352, 68, 42, 12, 0xffe6bd, 0xffc27a, 1);
    glow(r, true, 330, 372, 70, 60, 0xffedd0, 0.45f);
    cone(r, true, 330, 396, 70, 452, 190, WARM, 0.45f);
    static const struct { float x, y, rx, ry, angle; uint32_t color; } leaves[] = {
        {150, 396, 8, 30, -0.45f, 0x2b5a45}, {126, 398, 8, 30, 0.50f, 0x244c3a}, {138, 386, 8, 34, 0.02f, 0x377257},
        {160, 412, 7, 24, -0.95f, 0x1f4033}, {116, 414, 7, 24, 0.95f, 0x1d3c30},
    };
    for (size_t i = 0; i < sizeof(leaves) / sizeof(*leaves); ++i)
        ellipse(r, true, leaves[i].x, leaves[i].y, leaves[i].rx, leaves[i].ry, leaves[i].angle, leaves[i].color, 1);
    rect(r, true, 157, 416, 38, 36, 6, 0x9a8f86, 0x665d56, 1);
    /* The door phone's handset panel on the wall. */
    rect(r, true, 54, 190, 44, 82, 8, 0x3b424a, 0x23292f, 1);
    rect(r, true, 50, 196, 36, 38, 4, 0x142028, 0x0e171d, 1);
    glow(r, true, 32, 215, 26, 24, 0x5ec8f2, 0.5f);
    ellipse(r, true, 32, 252, 7, 7, 0, 0x5ec8f2, 1);
    shade_left(r, 0.64f, 0x12131a, 0.66f);
    shade_top(r, 104, 0x14151c, 0.86f);
}

/* ------------------------------------------------------------------------- */

/* 4x4 ordered dither, so the soft gradients do not band in 5 and 6 bits. */
static void put_row(const row_t *r, uint16_t *out)
{
    static const uint8_t order[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    const uint8_t *line = order[r->y & 3];
    for (int x = 0; x < r->width; ++x) {
        const unsigned d = line[x & 3];
        /* 15 bits of channel; 5 and 6 of them are kept. */
        unsigned red = r->c[0][x] + d * ((1u << (SCALE_BITS + 3)) / 16u);
        unsigned green = r->c[1][x] + d * ((1u << (SCALE_BITS + 2)) / 16u);
        unsigned blue = r->c[2][x] + d * ((1u << (SCALE_BITS + 3)) / 16u);
        red >>= SCALE_BITS + 3;
        green >>= SCALE_BITS + 2;
        blue >>= SCALE_BITS + 3;
        out[x] = (uint16_t)((red > 31 ? 31 : red) << 11 | (green > 63 ? 63 : green) << 5 | (blue > 31 ? 31 : blue));
    }
}

bool door_backdrop_rows(uint16_t *pixels, uint16_t width, uint16_t height, door_backdrop_t kind, uint16_t *work,
                        uint16_t from, uint16_t rows)
{
    if (!pixels || !work || !width || width > MAX_WIDTH || !height || from + (uint32_t)rows > height) return false;
    row_t r = {.c = {work, work + width, work + 2 * (size_t)width}, .left = work + 3 * (size_t)width, .width = width,
               .height = height, .u = (float)height / 600.0f};
    r.first = from;
    for (r.y = from; r.y < from + rows; ++r.y) {
        r.yc = (float)r.y + 0.5f;
        if (kind == DOOR_BACKDROP_DOOR) door_scene(&r);
        else room_scene(&r);
        put_row(&r, pixels + (size_t)(r.y - from) * width);
    }
    return true;
}

bool door_backdrop_draw(uint16_t *pixels, uint16_t width, uint16_t height, door_backdrop_t kind, uint16_t *work)
{
    return door_backdrop_rows(pixels, width, height, kind, work, 0, height);
}
