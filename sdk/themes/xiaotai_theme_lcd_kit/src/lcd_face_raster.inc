// SPDX-License-Identifier: MIT
/* Fixed-point contours and decorative layers shared by clipped RGB565 and
 * the software canvas. The same traversal also records per-frame damage. */
static void face_column(face_raster_t *raster, int x, int32_t top, int32_t bottom, int y0, int y1,
                        unsigned opacity, bool overlay)
{
    int a0 = top / 256, a1 = (bottom + 255) / 256;
    if (a0 < y0) a0 = y0;
    if (a1 > y1) a1 = y1;
    for (int y = a0; y <= a1; ++y) {
        int32_t a = top > y * 256 ? top : y * 256;
        int32_t b = bottom < (y + 1) * 256 ? bottom : (y + 1) * 256;
        if (b > a) face_pixel(raster, x, y, ((unsigned)(b - a) * opacity + 2048U) / 4096U, overlay);
    }
}
typedef enum { FACE_SHAPE_ROUND, FACE_SHAPE_HEART, FACE_SHAPE_SPARK, FACE_SHAPE_TEAR } face_shape_t;
typedef struct {
    int32_t fold, wave, pucker; /* Q8 strength for folds/lips; Q8 pixels for wave. */
    bool lower_half;
} face_profile_t;
static void face_contour(face_raster_t *raster, const int32_t *p, unsigned opacity, face_shape_t glyph,
                         bool overlay, const face_profile_t *profile)
{
    if (opacity == 0 || p[2] <= 0 || p[3] <= 0) return;
    if (opacity > 256) opacity = 256;
    int32_t rx = p[2] / 2, ry = p[3] / 2, radius = p[4];
    if (radius > rx) radius = rx;
    if (radius > ry) radius = ry;
    if (radius < 0) radius = 0;
    int x0 = (p[0] - rx) / 256 - 1, x1 = (p[0] + rx) / 256 + 1;
    int extent = (abs(p[5]) + abs(p[6]) + (profile ? abs(profile->wave) : 0)) / 256 + 2;
    int y0 = (p[1] - ry) / 256 - extent, y1 = (p[1] + ry) / 256 + extent;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= LCD_FACE_WIDTH) x1 = LCD_FACE_WIDTH - 1;
    if (y1 >= LCD_FACE_HEIGHT) y1 = LCD_FACE_HEIGHT - 1;
    if (x0 > x1 || y0 > y1) return;
    const face_bounds_t bounds = {x0, y0, x1, y1};
    face_record_bounds(raster, &bounds);
    if (!raster->surface) return;
    const face_bounds_t *clip = &raster->clip;
    if (x0 < clip->x0) x0 = clip->x0;
    if (x1 > clip->x1) x1 = clip->x1;
    if (y0 < clip->y0) y0 = clip->y0;
    if (y1 > clip->y1) y1 = clip->y1;
    if (x0 > x1 || y0 > y1) return;
    for (int x = x0; x <= x1; ++x) {
        int32_t dx = x * 256 + 128 - p[0];
        if (abs(dx) > rx) continue;
        int32_t half = ry, corner = abs(dx) - (rx - radius);
        if (corner > 0 && radius > 0) {
            lv_sqrt_res_t root;
            /* Q4 before squaring: bounded integer rounded-rectangle raster. */
            int32_t r = radius / 16, c = corner / 16;
            lv_sqrt((uint32_t)(r * r - c * c), &root, 0x8000);
            half = ry - radius + root.i * 16;
        }
        int32_t nx = dx * 256 / rx;
        int32_t centre = p[1] + p[5] * nx / 256 + p[6] * (nx * nx - 32768) / 65536;
        int32_t gap = 0;
        if (profile) {
            int32_t q = 2 * abs(nx) - 256;
            centre += profile->wave * (q * q - 32768) / 65536;
            if (profile->fold) {
                /* Two thick, round-ended arms meeting at the inner corner. */
                int32_t stroke = ry * 7 / 16;
                if (stroke > rx) stroke = rx;
                int32_t run = rx - stroke, cap = stroke;
                int32_t edge = abs(dx) - run;
                if (edge > 0) {
                    lv_sqrt_res_t root;
                    int32_t r = stroke / 16, c = edge / 16;
                    lv_sqrt((uint32_t)(r * r - c * c), &root, 0x8000);
                    cap = root.i * 16;
                }
                int32_t along = run - (profile->fold > 0 ? dx : -dx);
                if (along < 0) along = 0;
                if (along > run * 2) along = run * 2;
                int32_t offset = run > 0 ? (ry - stroke) * along / (run * 2) : 0;
                int32_t folded_half = offset + cap;
                int32_t folded_gap = offset > cap ? offset - cap : 0;
                int32_t t = abs(profile->fold) / 64;
                half = face_lerp(half, folded_half, t);
                gap = folded_gap * t / 1024;
            }
            if (profile->pucker) {
                lv_sqrt_res_t root;
                lv_sqrt((uint32_t)(65536 - nx * nx), &root, 0x8000);
                int32_t lobe = ry * root.i / 512, offset = ry * 7 / 16;
                int32_t t = profile->pucker / 64;
                half = face_lerp(half, offset + lobe, t);
                gap = face_lerp(gap, offset > lobe ? offset - lobe : 0, t);
            }
        }
        int32_t top = centre - half, bottom = centre + half;
        if (glyph == FACE_SHAPE_HEART) {
            int32_t ax = abs(nx), c = ax - 128;
            lv_sqrt_res_t root;
            lv_sqrt((uint32_t)(128 * 128 - c * c), &root, 0x8000);
            top = centre - ry / 2 - ry * root.i / 256;
            bottom = centre + ry - 3 * ry * ax / 512;
        } else if (glyph == FACE_SHAPE_SPARK) {
            int32_t tip = 256 - abs(nx);
            half = ry * tip * tip / 65536;
            top = centre - half; bottom = centre + half;
        } else if (glyph == FACE_SHAPE_TEAR) {
            lv_sqrt_res_t root;
            lv_sqrt((uint32_t)(65536 - nx * nx), &root, 0x8000);
            top = centre - ry + 4 * ry * abs(nx) / 768;
            bottom = centre + ry / 3 + 2 * ry * root.i / 768;
        }
        if (profile && profile->lower_half) {
            if (top < centre + gap) top = centre + gap;
        } else if (gap > 0) {
            face_column(raster, x, top, centre - gap, y0, y1, opacity, overlay);
            top = centre + gap;
        }
        face_column(raster, x, top, bottom, y0, y1, opacity, overlay);
    }
}
static void face_shape(face_raster_t *raster, const int32_t *p, unsigned opacity, face_shape_t glyph, bool overlay)
{
    face_contour(raster, p, opacity, glyph, overlay, NULL);
}
static uint32_t face_tint(uint32_t rgb, uint32_t tint, unsigned amount)
{
    uint32_t result = 0;
    for (unsigned shift = 0; shift <= 16; shift += 8) {
        unsigned a = (rgb >> shift) & 255U, b = (tint >> shift) & 255U;
        result |= ((a * (256U - amount) + b * amount) / 256U) << shift;
    }
    return result;
}
static void face_mark(face_raster_t *raster, int x, int y, int w, int h, int slope, int bend, unsigned opacity)
{
    int32_t p[7] = {x, y, w, h, h / 2, slope, bend};
    face_shape(raster, p, opacity, 0, true);
}
static void face_heart(face_raster_t *raster, int x, int y, int size, unsigned opacity)
{
    const int32_t p[7] = {x, y, size, size, size / 2, 0, 0};
    face_shape(raster, p, opacity, FACE_SHAPE_HEART, true);
}
static void face_line(face_raster_t *raster, int ax, int ay, int bx, int by, int width, unsigned opacity)
{
    if (opacity == 0) return;
    if (opacity > 256) opacity = 256;
    int radius = width / 2;
    face_bounds_t b = {
        ((ax < bx ? ax : bx) - radius) / 256 - 1,
        ((ay < by ? ay : by) - radius) / 256 - 1,
        ((ax > bx ? ax : bx) + radius) / 256 + 1,
        ((ay > by ? ay : by) + radius) / 256 + 1,
    };
    if (b.x0 < 0) b.x0 = 0;
    if (b.y0 < 0) b.y0 = 0;
    if (b.x1 >= LCD_FACE_WIDTH) b.x1 = LCD_FACE_WIDTH - 1;
    if (b.y1 >= LCD_FACE_HEIGHT) b.y1 = LCD_FACE_HEIGHT - 1;
    if (b.x0 > b.x1 || b.y0 > b.y1) return;
    face_record_bounds(raster, &b);
    if (!raster->surface) return;
    const face_bounds_t *clip = &raster->clip;
    if (b.x0 < clip->x0) b.x0 = clip->x0;
    if (b.x1 > clip->x1) b.x1 = clip->x1;
    if (b.y0 < clip->y0) b.y0 = clip->y0;
    if (b.y1 > clip->y1) b.y1 = clip->y1;
    if (b.x0 > b.x1 || b.y0 > b.y1) return;
    int dx = bx - ax, dy = by - ay, length2 = dx * dx + dy * dy;
    int outer = radius + 128, inner = radius > 128 ? radius - 128 : 0;
    for (int y = b.y0; y <= b.y1; ++y) {
        for (int x = b.x0; x <= b.x1; ++x) {
            int px = x * 256 + 128 - ax, py = y * 256 + 128 - ay;
            int t = length2 ? (int)(((int64_t)px * dx + (int64_t)py * dy) * 256 / length2) : 0;
            if (t < 0) t = 0;
            if (t > 256) t = 256;
            px -= dx * t / 256; py -= dy * t / 256;
            int distance2 = px * px + py * py;
            if (distance2 >= outer * outer) continue;
            unsigned coverage = 256;
            if (distance2 > inner * inner) {
                lv_sqrt_res_t root;
                lv_sqrt((uint32_t)distance2, &root, 0x8000);
                coverage = (unsigned)(outer - root.i);
            }
            face_pixel(raster, x, y, (coverage * opacity + 2048U) / 4096U, true);
        }
    }
}
static void face_path(face_raster_t *raster, const int8_t (*points)[2], unsigned count,
                      int x, int y, int scale, int width, unsigned opacity)
{
    for (unsigned i = 1; i < count; ++i)
        face_line(raster, x + points[i - 1][0] * scale, y + points[i - 1][1] * scale,
                  x + points[i][0] * scale, y + points[i][1] * scale, width, opacity);
}
static void face_note(face_raster_t *raster, int x, int y, int scale, unsigned opacity)
{
    face_mark(raster, x - 2 * scale, y + 5 * scale, 6 * scale, 4 * scale, -scale, 0, opacity);
    face_line(raster, x, y + 4 * scale, x, y - 6 * scale, 2 * scale, opacity);
    face_line(raster, x, y - 6 * scale, x + 4 * scale, y - 4 * scale, 2 * scale, opacity);
}
static void face_render(face_raster_t *raster, const int32_t *pose, int64_t now)
{
    uint32_t rgb = ((uint32_t)(pose[FACE_RED] / 256) << 16) |
                   ((uint32_t)(pose[FACE_GREEN] / 256) << 8) | (uint32_t)(pose[FACE_BLUE] / 256);
    face_ink(raster, rgb);
    face_clear(raster);
    unsigned style = (unsigned)pose[FACE_STYLE_B] / 256;
    unsigned glasses = (unsigned)pose[FACE_GLASSES] / 256;
    for (unsigned i = 0; i < 2; ++i) {
        const face_profile_t eye = {
            .fold = i == 0 ? pose[FACE_FOLD_LEFT] : -pose[FACE_FOLD_RIGHT],
        };
        unsigned visible = 256 - glasses + glasses * style / 256;
        face_contour(raster, pose + i * 7, visible, FACE_SHAPE_ROUND, false, &eye);
    }
    const int32_t *mouth = pose + 14;
    const int32_t *right = pose + 7;
    face_profile_t lips = {.wave = pose[FACE_MOUTH_WAVE], .pucker = pose[FACE_MOUTH_PUCKER]};
    face_ink(raster, face_tint(rgb, 0xF8DDF1, (unsigned)pose[FACE_MOUTH_PINK] / 256));
    face_contour(raster, mouth, 256, FACE_SHAPE_ROUND, false, &lips);
    if (pose[FACE_MOUTH_TWO_TONE]) {
        lips.lower_half = true;
        face_ink(raster, 0xF4C9E4);
        face_contour(raster, mouth, (unsigned)pose[FACE_MOUTH_TWO_TONE] / 256,
                     FACE_SHAPE_ROUND, true, &lips);
    }

    unsigned blush = (unsigned)pose[FACE_BLUSH] / 256;
    unsigned tongue = (unsigned)pose[FACE_TONGUE] / 256;
    unsigned heart = (unsigned)pose[FACE_HEART] / 256;
    unsigned kiss = (unsigned)pose[FACE_KISS] / 256;
    if (blush || tongue || heart || kiss) {
        face_ink(raster, 0xF3BDD4);
        for (unsigned i = 0; i < 2; ++i) {
            int x = pose[i * 7], y = pose[i * 7 + 1] + 25 * 256;
            face_mark(raster, x, y, 13 * 256, 5 * 256, 0, 0, blush * (256 - style) / 256);
            for (int j = -1; j <= 1; j += 2)
                face_line(raster, x + j * 6 * 256 - 2 * 256, y + 3 * 256,
                             x + j * 6 * 256 + 2 * 256, y - 3 * 256,
                             5 * 256, blush * style / 256);
        }
        int32_t tx = pose[FACE_TONGUE_X], nx = tx * 512 / mouth[2];
        int32_t lip = mouth[1] + mouth[3] / 2 + mouth[5] * nx / 256 +
                      mouth[6] * (nx * nx - 32768) / 65536;
        face_mark(raster, mouth[0] + tx, lip + 256, 9 * 256 + (int)style * 3,
                     9 * 256 + (int)style * 5, (int)style, 0, tongue);
        int32_t lift = face_pulse(face_render_cue_age(raster, FACE_HEART, now) - 450, 600, 200, 900) / 4;
        face_heart(raster, right[0] + 24 * 256 - (int)style * 44, 16 * 256 - lift,
                      14 * 256 + (int)style * 4, heart);
        face_heart(raster, right[0] - 44 * 256, 25 * 256 - lift / 2,
                      12 * 256, heart * style / 256);
        int32_t drift = face_pulse(face_render_cue_age(raster, FACE_KISS, now) - 400, 800, 200, 1000) / 2;
        face_heart(raster, mouth[0] + 42 * 256 + drift * (256 + (int)style * 2) / 256,
                      mouth[1] - 256, 14 * 256 + (int)style * 6, kiss);
    }

    unsigned tears = (unsigned)pose[FACE_TEARS] / 256;
    if (tears) {
        face_ink(raster, 0xA8DDF2);
        int32_t fall = face_ease(face_render_cue_age(raster, FACE_TEARS, now) - 550, 1000) * 3 / 4;
        for (unsigned i = 0; i < 2; ++i) {
            const int32_t *eye = pose + i * 7;
            const int32_t drop[7] = {eye[0] + (i ? 1 : -1) * (17 * 256 + (int)style * 7),
                eye[1] + eye[3] / 2 + 8 * 256 + (int)style + fall,
                7 * 256 + (int)style * 3, 11 * 256 + (int)style * 5, 0, 0, 0};
            face_shape(raster, drop, i ? tears : tears * style / 256, FACE_SHAPE_TEAR, true);
        }
    }

    if (glasses) {
        int32_t settle = (1024 - face_ease(face_render_cue_age(raster, FACE_GLASSES, now), 800)) / 2;
        settle = settle * (256 + (int)style * 7) / 256;
        int32_t low = (int)style * 20 - settle;
        face_ink(raster, 0xA6CBE9);
        face_mark(raster, (pose[0] + right[0]) / 2, (pose[1] + right[1]) / 2 - 12 * 256 + low,
                     60 * 256, 5 * 256, 0, 0, glasses);
        for (unsigned i = 0; i < 2; ++i) {
            const int32_t *eye = pose + i * 7;
            int32_t frame[7] = {eye[0], eye[1] + low, 68 * 256 + (int)style * 8,
                40 * 256 + (int)style * 4, 14 * 256 + (int)style * 4, -(int)style, 0};
            face_ink(raster, 0xA6CBE9);
            face_shape(raster, frame, glasses, 0, true);
            frame[1] += 2 * 256; frame[2] -= 4 * 256; frame[3] -= 4 * 256;
            face_ink(raster, 0x344350);
            face_shape(raster, frame, glasses, 0, true);
            face_ink(raster, 0xB8D9F3);
            face_line(raster, eye[0] - 19 * 256, eye[1] + 2 * 256 + low,
                         eye[0] - 9 * 256, eye[1] - 8 * 256 + low, 3 * 256, glasses * 3 / 4);
        }
    }

    unsigned music = (unsigned)pose[FACE_MUSIC] / 256;
    if (music) {
        int32_t lift = face_pulse(face_render_cue_age(raster, FACE_MUSIC, now) % 7200 - 1000, 700, 300, 1200) / 2;
        lift = lift * (256 + (int)style) / 256;
        face_ink(raster, face_tint(0xF2E8CE, 0xC9F8EA, style));
        face_note(raster, right[0] + 12 * 256 - (int)style * 5, 15 * 256 + (int)style - lift,
                     256 + (int)style / 2, music);
        face_ink(raster, face_tint(0xF2E8CE, 0xF0CAEA, style));
        face_note(raster, right[0] + 28 * 256, 23 * 256 - lift / 2,
                     256 + (int)style / 4, music * 4 / 5);
    }
    unsigned zzz = (unsigned)pose[FACE_ZZZ] / 256;
    if (zzz) {
        static const int8_t z_path[][2] = {{0,0},{6,0},{0,7},{6,7}};
        face_ink(raster, 0xCAC6EB);
        int32_t lift = face_pulse(face_render_cue_age(raster, FACE_ZZZ, now) % 8800 - 1200, 1400, 400, 1800) / 2;
        for (unsigned i = 0; i < 3; ++i) {
            int scale = 192 + (int)i * 48 + (int)style / 2;
            face_path(raster, z_path, 4, right[0] + ((int)i * 14 - 10) * 256,
                         (24 - (int)i * 8) * 256 - lift * (int)(i + 1) / 3,
                         scale, 2 * 256 + (int)style, zzz);
        }
    }

    unsigned question = (unsigned)pose[FACE_QUESTION] / 256;
    if (question) {
        static const int8_t question_path[][2] = {{0,0},{2,-3},{6,-3},{8,0},{7,3},{4,5},{4,7}};
        face_ink(raster, 0xADD6EF);
        int x = right[0] + 32 * 256, y = 13 * 256;
        face_path(raster, question_path, 7, x, y, 256 + (int)style / 2, 3 * 256 + (int)style, question);
        face_mark(raster, x + 4 * 256 + (int)style * 2, y + 12 * 256 + (int)style * 5,
                     3 * 256 + (int)style, 3 * 256 + (int)style, 0, 0, question);
    }
    unsigned exclaim = (unsigned)pose[FACE_EXCLAIM] / 256;
    if (exclaim) {
        face_ink(raster, 0xF4E5AE);
        face_mark(raster, right[0] + 37 * 256, 16 * 256, 4 * 256 + (int)style * 2, 10 * 256 + (int)style * 3, 0, 0, exclaim);
        face_mark(raster, right[0] + 37 * 256, 27 * 256, 4 * 256 + (int)style, 4 * 256 + (int)style, 0, 0, exclaim);
    }
    unsigned emphasis = (unsigned)pose[FACE_EMPHASIS] / 256;
    if (emphasis) {
        face_ink(raster, 0xDDEAF7);
        face_line(raster, 102 * 256, 12 * 256, 104 * 256, 19 * 256, 4 * 256, emphasis);
        face_line(raster, 118 * 256, 12 * 256, 116 * 256, 19 * 256, 4 * 256, emphasis);
        face_line(raster, 130 * 256, 20 * 256, 127 * 256, 24 * 256, 4 * 256, emphasis * style / 256);
    }
    unsigned sparkle = (unsigned)pose[FACE_SPARKLE] / 256;
    if (sparkle) {
        face_ink(raster, 0xF4E7B6);
        int x = right[0] + 32 * 256, y = 15 * 256;
        const int32_t star[7] = {x, y, 13 * 256 + (int)style * 3, 15 * 256 + (int)style * 5, 0, 0, 0};
        face_shape(raster, star, sparkle, FACE_SHAPE_SPARK, true);
    }

    unsigned listen = (unsigned)pose[FACE_LISTEN] / 256;
    if (listen) {
        static const int8_t outer[][2] = {{5,-14},{2,-8},{1,0},{2,8},{5,14}};
        static const int8_t inner[][2] = {{11,-7},{9,-3},{9,3},{11,7}};
        int x = pose[0] - pose[2] / 2 - 14 * 256, y = pose[1];
        int wave = face_wave(face_render_cue_age(raster, FACE_LISTEN, now), 2400);
        unsigned alpha = listen * (unsigned)(232 + wave * 24 / 32768) / 256;
        face_ink(raster, 0xA8D4EF);
        face_path(raster, outer, 5, x, y, 256, 2 * 256 + (int)style * 2, alpha * 4 / 5);
        face_path(raster, inner, 4, x, y, 256, 2 * 256 + (int)style * 2, alpha);
    }
    unsigned thought = (unsigned)pose[FACE_THOUGHT] / 256;
    if (thought) {
        face_ink(raster, rgb);
        for (unsigned i = 0; i < 3; ++i) {
            int wave = face_wave(face_render_cue_age(raster, FACE_THOUGHT, now) + i * 300, 2400);
            unsigned alpha = thought * (unsigned)(216 + wave * 32 / 32768) / 256;
            int size = (i == 1 ? 6 : 4) * 256 + (int)style * (int)(i + 1);
            face_mark(raster, right[0] + ((int)i - 1) * 12 * 256,
                         14 * 256 + (1 - (int)i) * (int)style * 4,
                         size, size, 0, 0, alpha);
        }
    }
    unsigned steam = (unsigned)pose[FACE_STEAM] / 256;
    if (steam) {
        int32_t lift = face_pulse(face_render_cue_age(raster, FACE_STEAM, now) - 320, 400, 300, 1000);
        int x = right[0] + 29 * 256, y = 16 * 256 - lift / 2;
        face_ink(raster, 0xF5DAF0);
        face_mark(raster, x, y, 15 * 256, 12 * 256, 0, 0, steam);
        face_mark(raster, x + 6 * 256, y - 6 * 256, 12 * 256, 12 * 256, 0, 0, steam);
        face_mark(raster, x + 11 * 256, y + 256, 12 * 256, 11 * 256, 0, 0, steam);
        face_line(raster, x - 2 * 256, y + 4 * 256, x - 5 * 256, y + 11 * 256, 4 * 256, steam);
    }
}
