// SPDX-License-Identifier: MIT
// Pictures of the character for design review, drawn by the same code the
// device runs: every mood, every state, and the reactions as frame sequences.
// tools/character_preview.py runs this and turns the frames into PNG files and
// an overview page. Usage: chr_gallery <out-dir> <character.pack> [width height]
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chr_internal.h"

enum { FRAME_MS = 40 };

static uint16_t width = 760, height = 600;
static uint16_t *frame;
static int16_t *scratch;
static chr_t *c;
static FILE *index_file, *sequence;
static const char *out_dir;
static uint8_t *pack;
static size_t pack_size;
static unsigned frames;

static void background(void)
{
    /* The home page's backdrop: a flat centre that darkens towards the edge. */
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float nx = 2.0f * (float)x / (float)(width - 1) - 1.0f, ny = 2.0f * (float)y / (float)(height - 1) - 1.0f;
            float t = (nx * nx + ny * ny - 0.6f) / 0.4f;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            const int r = (int)(0x1c - t * (0x1c - 0x10)), g = (int)(0x1f - t * (0x1f - 0x13)), b = (int)(0x22 - t * (0x22 - 0x15));
            frame[y * width + x] = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
        }
    }
}

static void paint(void)
{
    background();
    const chr_target_t target = {.pixels = (uint8_t *)frame, .stride = (uint32_t)width * 2U, .x0 = 0, .y0 = 0,
        .x1 = (int16_t)(width - 1), .y1 = (int16_t)(height - 1), .scratch = scratch, .lines = (uint16_t)width};
    (void)chr_paint(c, &target);
}

static void rgb_of(uint16_t v, unsigned *sum)
{
    sum[0] += (unsigned)(v >> 11) * 255U / 31U;
    sum[1] += (unsigned)(v >> 5 & 63U) * 255U / 63U;
    sum[2] += (unsigned)(v & 31U) * 255U / 31U;
}

/* Stills at full size; sequences at half size (2x2 averaged) to keep them small. */
static void write_rgb(FILE *file, int divide)
{
    for (int y = 0; y + divide <= height; y += divide) {
        for (int x = 0; x + divide <= width; x += divide) {
            unsigned sum[3] = {0, 0, 0};
            for (int j = 0; j < divide; ++j)
                for (int i = 0; i < divide; ++i) rgb_of(frame[(y + j) * width + x + i], sum);
            const unsigned n = (unsigned)(divide * divide);
            const uint8_t rgb[3] = {(uint8_t)(sum[0] / n), (uint8_t)(sum[1] / n), (uint8_t)(sum[2] / n)};
            fwrite(rgb, 1, 3, file);
        }
    }
}

static void step(unsigned ms)
{
    chr_rect_t damage[CHR_DAMAGE_MAX];
    for (unsigned t = 0; t < ms; t += FRAME_MS) {
        (void)chr_step(c, FRAME_MS, damage);
        if (sequence) {
            paint();
            write_rgb(sequence, 2);
            ++frames;
        }
    }
}

static void fresh(void)
{
    if (!chr_init(c, width, height, 20261124U, pack, pack_size)) {
        fprintf(stderr, "chr_gallery: not a sprite pack\n");
        exit(3);
    }
    step(400);
}

static void still(const char *group, const char *name, const char *title)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s-%s.rgb", out_dir, group, name);
    FILE *file = fopen(path, "wb");
    if (!file) exit(2);
    paint();
    write_rgb(file, 1);
    fclose(file);
    fprintf(index_file, "%s\t%s\t%s\t%u\t%u\t1\n", group, name, title, width, height);
}

static void begin(const char *group, const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s-%s.rgb", out_dir, group, name);
    sequence = fopen(path, "wb");
    if (!sequence) exit(2);
    frames = 0;
}

static void end(const char *group, const char *name, const char *title)
{
    fclose(sequence);
    sequence = NULL;
    fprintf(index_file, "%s\t%s\t%s\t%u\t%u\t%u\n", group, name, title, width / 2U, height / 2U, frames);
}

/* A voice: syllables with gaps, as an amplitude. */
static uint16_t voice(unsigned ms, unsigned peak)
{
    const unsigned syllable = ms % 260U;
    const unsigned gap = (ms / 1300U) % 3U == 2U && ms % 1300U > 900U;
    if (gap || syllable > 190U) return 0;
    const unsigned shape = syllable < 60U ? syllable * peak / 60U : peak - (syllable - 60U) * peak / 200U;
    return (uint16_t)(shape * (70U + (ms / 260U * 37U) % 30U) / 100U);
}

static void talk(unsigned ms, bool speaking)
{
    for (unsigned t = 0; t < ms; t += FRAME_MS) {
        if (speaking) chr_set_envelope(c, 0, voice(t, 9000));
        else chr_set_envelope(c, voice(t, 1500), 0);
        chr_set_level(c, speaking ? 0 : 2);
        step(FRAME_MS);
    }
    chr_set_envelope(c, 0, 0);
    chr_set_level(c, 0);
}

/* Touch points are given from the figure's anchor: bottom centre of the stage. */
static int16_t at_x(int x) { return (int16_t)(width / 2 + x); }
static int16_t at_y(int y) { return (int16_t)(height + y); }

static void drag(int x0, int y0, int x1, int y1, unsigned ms)
{
    for (unsigned t = 0; t <= ms; t += FRAME_MS) {
        (void)chr_pointer(c, true, at_x(x0 + (x1 - x0) * (int)t / (int)ms), at_y(y0 + (y1 - y0) * (int)t / (int)ms));
        step(FRAME_MS);
    }
}

static void lift(void)
{
    (void)chr_pointer(c, false, 0, 0);
    step(FRAME_MS);
}

static void tap(int x, int y)
{
    drag(x, y, x, y, 80);
    lift();
}

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    out_dir = argv[1];
    FILE *asset = fopen(argv[2], "rb");
    if (!asset) return 2;
    fseek(asset, 0, SEEK_END);
    pack_size = (size_t)ftell(asset);
    fseek(asset, 0, SEEK_SET);
    pack = malloc(pack_size);
    if (!pack || fread(pack, 1, pack_size, asset) != pack_size) return 2;
    fclose(asset);
    if (argc >= 5) {
        width = (uint16_t)atoi(argv[3]);
        height = (uint16_t)atoi(argv[4]);
    }
    frame = malloc((size_t)width * height * 2U);
    scratch = calloc(chr_scratch_entries(width), sizeof(*scratch));
    c = malloc(chr_size());
    char path[512];
    snprintf(path, sizeof(path), "%s/index.tsv", out_dir);
    index_file = fopen(path, "w");
    if (!frame || !scratch || !c || !index_file) return 2;

    for (size_t i = 0; chr_mood_name(i); ++i) {
        fresh();
        chr_set_mood(c, i);
        step(1700);
        still("mood", chr_mood_name(i), chr_mood_name(i));
    }

    /* The ten words of a conversation, in a pause of her answer, a moment after each has come. */
    static const struct { const char *mood, *title; unsigned ms; } words[] = {
        {"neutral", "中性", 1100}, {"happy", "开心", 1100}, {"excited", "兴奋", 1100}, {"gentle", "温和", 1100}, {"comfort", "安慰", 1100},
        {"thinking", "思考", 1100}, {"surprised", "惊讶", 600}, {"surprised", "惊讶（一秒后）", 1600}, {"serious", "严肃", 1100},
        {"sleepy", "困倦", 1100}, {"confused", "困惑", 1100},
    };
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i) {
        char name[32];
        snprintf(name, sizeof(name), "%02u-%s", (unsigned)i, words[i].mood);
        fresh(); chr_set_phase(c, CHR_PHASE_SPEAKING); talk(1800, true);
        chr_set_mood(c, chr_mood_find(words[i].mood));
        step(words[i].ms);
        still("word", name, words[i].title);
    }

    fresh(); chr_set_phase(c, CHR_PHASE_LISTENING); step(1000); talk(700, false); still("state", "listening", "在听");
    fresh(); chr_set_phase(c, CHR_PHASE_LISTENING); step(8000); still("state", "listening-long", "在听，很久没人说话");
    fresh(); chr_set_phase(c, CHR_PHASE_THINKING); step(1000); still("state", "thinking", "在想");
    fresh(); chr_set_phase(c, CHR_PHASE_THINKING); step(2400); still("state", "thinking-long", "在想，想了一会儿");
    fresh(); chr_set_phase(c, CHR_PHASE_SPEAKING); step(1000); talk(340, true); still("state", "speaking", "在说");
    static const struct { uint32_t bit; const char *name, *title; } conditions[] = {
        {CHR_OFFLINE, "offline", "断网"}, {CHR_CONNECTING, "connecting", "联网中"}, {CHR_RINGING, "ringing", "来电"},
        {CHR_IN_CALL, "call", "通话中"}, {CHR_ALARM, "alarm", "闹钟响"}, {CHR_MUTED, "muted", "麦克风关闭"},
    };
    for (size_t i = 0; i < sizeof(conditions) / sizeof(conditions[0]); ++i) {
        fresh();
        chr_set_conditions(c, conditions[i].bit);
        step(2300);
        still("state", conditions[i].name, conditions[i].title);
    }

    /* A whole conversation. */
    fresh();
    begin("talk", "conversation");
    step(600);
    chr_set_phase(c, CHR_PHASE_LISTENING); talk(2400, false);
    chr_set_phase(c, CHR_PHASE_THINKING); step(1800);
    chr_set_mood(c, chr_mood_find("happy"));
    chr_set_phase(c, CHR_PHASE_SPEAKING); talk(3200, true);
    chr_set_envelope(c, 0, 8000); step(FRAME_MS);
    chr_set_phase(c, CHR_PHASE_LISTENING); talk(1800, false);     /* barged in */
    chr_set_phase(c, CHR_PHASE_SPEAKING); talk(1600, true);
    chr_set_phase(c, CHR_PHASE_IDLE); chr_set_mood(c, 0); step(2200);
    end("talk", "conversation", "一次对讲：被唤醒、在听、在想、在说、被打断、结束");

    static const struct { chr_cue_t cue; int32_t value; const char *title; unsigned ms; } cues[] = {
        {CHR_CUE_GREET, 0, "开机问候", 3000}, {CHR_CUE_WAKE, 0, "被唤醒", 1500}, {CHR_CUE_STARTLE, 0, "被打断", 1500},
        {CHR_CUE_BYE, 0, "对讲结束", 2200}, {CHR_CUE_VOLUME, 80, "音量变化", 1800}, {CHR_CUE_ERROR, 0, "出错", 1800},
        {CHR_CUE_NOD, 0, "点头", 1200}, {CHR_CUE_STRETCH, 0, "被摸醒伸懒腰", 2500}, {CHR_CUE_CHEER, 0, "欢呼（网络恢复）", 2000},
        {CHR_CUE_SHIVER, 0, "一哆嗦（网络断开）", 1500}, {CHR_CUE_HOP, 0, "待机：蹦一下", 1200},
        {CHR_CUE_HUM, 0, "待机：哼歌", 3200}, {CHR_CUE_YAWN, 0, "待机：打哈欠", 2600},
        {CHR_CUE_GLANCE, 1, "待机：往旁边看", 1900}, {CHR_CUE_NOD_OFF, 0, "犯困：打瞌睡，惊醒", 3000},
        {CHR_CUE_WAVE, 0, "没人理：探头看你，挥手", 2700}, {CHR_CUE_OFFER, -1, "伸手等你击掌", 3000},
        {CHR_CUE_DANCE, 0, "跳几拍舞", 6600},
        {CHR_CUE_EARS, 1, "待机：竖耳朵听", 2000}, {CHR_CUE_WAG, 0, "待机：摇尾巴", 2100}, {CHR_CUE_TWITCH, 0, "待机：耳朵一抖", 1400},
    };
    const uint32_t has = chr_features(c);
    for (size_t i = 0; i < sizeof(cues) / sizeof(cues[0]); ++i) {
        /* What is done with ears or a tail is shown for a figure that has them. */
        if ((cues[i].cue == CHR_CUE_EARS || cues[i].cue == CHR_CUE_TWITCH) && !(has & CHR_HAS_EARS)) continue;
        if (cues[i].cue == CHR_CUE_WAG && !(has & CHR_HAS_TAIL)) continue;
        fresh();
        begin("cue", chr_cue_name(cues[i].cue));
        step(240);
        chr_cue(c, cues[i].cue, cues[i].value);
        step(cues[i].ms);
        end("cue", chr_cue_name(cues[i].cue), cues[i].title);
    }

    /* Where to touch her: from her own layout. */
    fresh();
    const chr_figure_t f = c->fig;
    const int cheek_x = f.eye_lx + f.cheek_dx, cheek_y = f.cheek_y, crown = (f.hair_top + f.brow_y) / 2, half = f.head_half * 2 / 3;
    fresh(); begin("touch", "poke"); step(200); tap(cheek_x, cheek_y); step(1300); end("touch", "poke", "戳一下脸");
    fresh(); begin("touch", "dizzy"); step(200);
    for (int i = 0; i < 3; ++i) { tap(cheek_x - 30 + 15 * i, cheek_y); step(160); }
    step(2600); end("touch", "dizzy", "连戳三下：晕");
    fresh(); begin("touch", "pout"); step(200);
    for (int i = 0; i < 6; ++i) { tap(cheek_x - 30 + 6 * i, cheek_y); step(120); }
    step(3000); end("touch", "pout", "连戳六下：赌气不理人");
    fresh(); begin("touch", "pet"); step(200);
    for (int i = 0; i < 3; ++i) { drag(f.face_x - half, crown, f.face_x + half, crown, 420); drag(f.face_x + half, crown, f.face_x - half, crown, 420); }
    lift(); step(1800); end("touch", "pet", "摸头");
    fresh(); begin("touch", "forgive"); step(200);
    for (int i = 0; i < 6; ++i) { tap(cheek_x - 30 + 6 * i, cheek_y); step(120); }
    step(700);
    for (int i = 0; i < 2; ++i) { drag(f.face_x - half, crown, f.face_x + half, crown, 420); drag(f.face_x + half, crown, f.face_x - half, crown, 420); }
    lift(); step(1500); end("touch", "forgive", "赌气时摸头：马上哄好");
    fresh(); begin("touch", "squish"); step(200); drag(cheek_x, cheek_y, cheek_x, cheek_y, 1700); lift(); step(1400);
    end("touch", "squish", "按住脸不放");
    if (f.arm_top) {
        const int arm_y = f.arm_top + 60;
        fresh(); begin("touch", "highfive"); step(200); tap(f.arm_rx, arm_y); step(900);
        if (f.hand_rx) tap(f.hand_rx, f.hand_ry);
        else tap(f.arm_rx, arm_y);
        step(1800); end("touch", "highfive", "点她的手臂：伸手；再点她的手：击掌");
        fresh(); begin("touch", "hold"); step(200); drag(f.arm_lx, arm_y, f.arm_lx, arm_y, 3200); lift(); step(1500);
        end("touch", "hold", "按住她的手臂：牵手");
    }
    if (has & CHR_HAS_EARS) {
        const int ear_x = (f.ear_lx0 + f.ear_lx1) / 2, ear_y = f.ears_top + 30;
        fresh(); begin("touch", "ear"); step(200); drag(ear_x, ear_y, ear_x + 60, ear_y + 20, 600);
        drag(ear_x + 60, ear_y + 20, ear_x - 40, ear_y - 10, 700);
        lift(); step(1800); end("touch", "ear", "揪耳朵");
    }
    if (has & CHR_HAS_TAIL) {
        fresh(); begin("touch", "tail"); step(200); tap((f.tail_x0 + f.tail_x1) / 2, f.tail_top / 2); step(2000);
        end("touch", "tail", "摸尾巴");
    }
    fresh(); begin("touch", "follow"); step(200); drag(-330, -80, 330, -480, 1500); drag(330, -480, -300, -430, 1100);
    lift(); step(1200); end("touch", "follow", "视线跟着手指");
    fresh(); begin("touch", "sway"); step(200); drag(-330, -60, 330, -60, 240); lift(); step(1500);
    end("touch", "sway", "横划：跟着晃");
    fresh(); begin("touch", "jump"); step(200); drag(320, -60, 320, -330, 240); lift(); step(1500);
    end("touch", "jump", "上划：跳起来");
    fresh(); begin("touch", "duck"); step(200); drag(320, -330, 320, -60, 240); lift(); step(1700);
    end("touch", "duck", "下划：蹲下躲起来");
    fresh(); begin("touch", "look"); step(200); tap(350, f.hair_top + 60); step(1500); end("touch", "look", "点旁边：看过去");

    fresh(); begin("life", "idle"); step(16000); end("life", "idle", "待机 16 秒：眨眼、张望、小动作");
    fresh(); chr_set_mood(c, chr_mood_find("sleepy")); begin("life", "sleepy"); step(7000);
    chr_set_mood(c, 0); chr_cue(c, CHR_CUE_STRETCH, 0); step(2600); end("life", "sleepy", "犯困睡着，被摸醒");
    /* Half an hour alone, not drawn: she has grown tired and dozed off. Then someone talks to her. */
    fresh(); for (int i = 0; i < 52; ++i) step(60000);
    begin("life", "doze"); step(3000); chr_set_phase(c, CHR_PHASE_LISTENING); step(2600); chr_set_phase(c, CHR_PHASE_IDLE); step(2400);
    end("life", "doze", "很久没人理：睡着了；被叫醒，伸懒腰");
    /* A talk ends on a surprised answer: the mood stays for two seconds, then she is herself again. */
    fresh(); chr_set_phase(c, CHR_PHASE_SPEAKING); chr_set_mood(c, chr_mood_find("surprised")); talk(1200, true);
    begin("life", "after"); chr_set_phase(c, CHR_PHASE_IDLE); step(5000);
    end("life", "after", "对讲结束：表情保持两秒，再回到平常");
    fresh(); begin("life", "network"); chr_set_conditions(c, CHR_OFFLINE); chr_cue(c, CHR_CUE_SHIVER, 0); step(4000);
    chr_set_conditions(c, CHR_CONNECTING); step(3000); chr_set_conditions(c, 0); chr_cue(c, CHR_CUE_CHEER, 0); step(2400);
    end("life", "network", "断网、联网中、恢复");
    fresh(); begin("life", "call"); step(400); chr_set_conditions(c, CHR_RINGING); step(3000);
    chr_set_conditions(c, CHR_IN_CALL); talk(2400, false); chr_set_conditions(c, 0); chr_cue(c, CHR_CUE_BYE, 0); step(2000);
    end("life", "call", "来电、通话、挂断");
    fresh(); begin("life", "alarm"); step(400); chr_set_conditions(c, CHR_ALARM); step(3200); chr_set_conditions(c, 0);
    chr_cue(c, CHR_CUE_NOD, 0); step(1500); end("life", "alarm", "闹钟响，关掉");

    fclose(index_file);
    chr_info_t info;
    chr_inspect(c, &info);
    printf("GALLERY items=%u steps=%u\n", (unsigned)info.items, (unsigned)info.steps);
    return 0;
}
