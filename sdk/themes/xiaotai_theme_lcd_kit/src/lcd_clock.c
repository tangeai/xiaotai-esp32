// SPDX-License-Identifier: MIT
#include "lcd_clock.h"

#include <stdio.h>
#include <string.h>
#include "lcd_almanac.h"
#include "xiaotai/port/mem.h"

enum { CARDS = 5, FACTS = 6 };

typedef struct {
    const char *icon;   /* LCD_ICON_*, or NULL */
    uint32_t tint;
    char label[40], detail[48];
} fact_t;

typedef struct { lv_obj_t *panel, *icon, *label, *detail; } card_t;

struct lcd_clock {
    lcd_state_t *s;
    lv_obj_t *hello_icon, *hello, *hour, *colon, *minute, *track, *fill, *date, *lunar, *calendar;
    card_t card[CARDS];
    fact_t facts[FACTS];
    size_t fact_count;
    xiaotai_ui_clock_t now;
    uint32_t stamp;             /* when `now` was taken */
    int32_t today;              /* days since 1970, valid with now.valid */
    lcd_box_t area, phone;
    const lv_font_t *digits;
    int32_t cell_w, cell_h, pad, digits_w, words_w;
    int second;                 /* the one the track shows, -1: none */
    bool month, rows_one, with_phone, merged, placed;
};

static int32_t mini(int32_t a, int32_t b) { return a < b ? a : b; }
static int32_t maxi(int32_t a, int32_t b) { return a > b ? a : b; }

static int32_t text_width(const char *text, const lv_font_t *font)
{
    lv_point_t size;
    lv_text_get_size(&size, text ? text : "", font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

/* ----------------------------------------------------------------- calendar */

static void draw_text(lv_layer_t *layer, const lcd_clock_t *c, const char *text, lcd_box_t box, uint32_t color,
                      lv_text_align_t align)
{
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.font = c->s->m.text;
    dsc.color = lv_color_hex(color);
    dsc.text = text;
    dsc.text_local = 1;
    dsc.align = align;
    const int32_t line = lcd_line_height(c->s), y = box.y + (box.height - line) / 2;
    const lv_area_t area = {box.x, y, box.x + box.width - 1, y + line - 1};
    lv_draw_label(layer, &dsc, &area);
}

static void draw_fill(lv_layer_t *layer, lcd_box_t box, uint32_t color, int32_t radius)
{
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_color_hex(color);
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = radius;
    const lv_area_t area = {box.x, box.y, box.x + box.width - 1, box.y + box.height - 1};
    lv_draw_rect(layer, &dsc, &area);
}

static const char *const columns[7] = {"一", "二", "三", "四", "五", "六", "日"};

/* A day with a holiday or a solar term has a dot under its number. */
static void draw_dot(lv_layer_t *layer, const lcd_clock_t *c, int32_t day, lcd_box_t cell, bool today)
{
    const bool holiday = lcd_holiday(day) != NULL;
    if (!holiday && !lcd_solar_term(day)) return;
    const int32_t side = maxi(3, lcd_dp(c->s, 3)), line = lcd_line_height(c->s);
    /* Under the number, clear of the circle around the day below. */
    const int32_t dot = c->month ? mini((cell.height + line) / 2 - side / 2, cell.height - side) :
                        cell.height - side - 1;
    draw_fill(layer, (lcd_box_t){cell.x + (cell.width - side) / 2, cell.y + dot, side, side},
              today ? LCD_ACCENT_INK : holiday ? LCD_WARNING : LCD_ACCENT, LV_RADIUS_CIRCLE);
}

static void draw_month(lv_layer_t *layer, const lcd_clock_t *c, lcd_box_t box)
{
    const int32_t line = lcd_line_height(c->s);
    const lcd_box_t head = {box.x + c->pad, box.y + c->pad, box.width - 2 * c->pad, line};
    char text[24];
    (void)snprintf(text, sizeof(text), "%u年%u月", (unsigned)c->now.year, (unsigned)c->now.month);
    draw_text(layer, c, text, head, LCD_TEXT, LV_TEXT_ALIGN_LEFT);
    (void)snprintf(text, sizeof(text), "第%d周", lcd_week(c->today));
    draw_text(layer, c, text, head, LCD_MUTED, LV_TEXT_ALIGN_RIGHT);
    const int32_t top = head.y + line + lcd_dp(c->s, 6);
    for (int i = 0; i < 7; ++i)
        draw_text(layer, c, columns[i], (lcd_box_t){head.x + i * c->cell_w, top, c->cell_w, line},
                  i < 5 ? LCD_MUTED : LCD_FAINT, LV_TEXT_ALIGN_CENTER);
    const int32_t first = lcd_days(c->now.year, c->now.month, 1);
    const int lead = (lcd_weekday(first) + 6) % 7, count = lcd_month_days(c->now.year, c->now.month);
    for (int i = 0; i < count; ++i) {
        const int slot = lead + i;
        const lcd_box_t cell = {head.x + slot % 7 * c->cell_w, top + line + lcd_dp(c->s, 2) + slot / 7 * c->cell_h,
                                c->cell_w, c->cell_h};
        const bool today = first + i == c->today;
        if (today) {
            const int32_t side = mini(mini(c->cell_w, c->cell_h), line + lcd_dp(c->s, 6));
            draw_fill(layer, (lcd_box_t){cell.x + (cell.width - side) / 2, cell.y + (cell.height - side) / 2, side, side},
                      LCD_ACCENT, LV_RADIUS_CIRCLE);
        }
        (void)snprintf(text, sizeof(text), "%d", i + 1);
        draw_text(layer, c, text, cell, today ? LCD_ACCENT_INK : first + i < c->today ? LCD_FAINT : LCD_TEXT,
                  LV_TEXT_ALIGN_CENTER);
        if (!today) draw_dot(layer, c, first + i, cell, false);
    }
}

static void draw_week(lv_layer_t *layer, const lcd_clock_t *c, lcd_box_t box)
{
    const int32_t line = lcd_line_height(c->s), monday = c->today - (lcd_weekday(c->today) + 6) % 7;
    for (int i = 0; i < 7; ++i) {
        const lcd_box_t column = {box.x + box.width * i / 7, box.y, box.width / 7, box.height};
        const bool today = monday + i == c->today;
        if (today) draw_fill(layer, (lcd_box_t){column.x + lcd_dp(c->s, 1), column.y, column.width - lcd_dp(c->s, 2),
                                                column.height}, LCD_ACCENT, lcd_dp(c->s, 8));
        int year, month, day;
        lcd_date(monday + i, &year, &month, &day);
        char text[12];
        (void)snprintf(text, sizeof(text), "%d", day);
        const int32_t rows = (column.height - lcd_dp(c->s, 4)) / 2;
        draw_text(layer, c, columns[i], (lcd_box_t){column.x, column.y + lcd_dp(c->s, 2), column.width, rows},
                  today ? LCD_ACCENT_INK : LCD_FAINT, LV_TEXT_ALIGN_CENTER);
        const lcd_box_t number = {column.x, column.y + rows, column.width, rows > line ? rows : line};
        draw_text(layer, c, text, number, today ? LCD_ACCENT_INK : monday + i < c->today ? LCD_MUTED : LCD_TEXT,
                  LV_TEXT_ALIGN_CENTER);
        draw_dot(layer, c, monday + i, column, today);
    }
}

static void draw_calendar(lv_event_t *event)
{
    const lcd_clock_t *c = lv_event_get_user_data(event);
    if (!c->now.valid) return;
    lv_area_t at;
    lv_obj_get_coords(lv_event_get_target_obj(event), &at);
    const lcd_box_t box = {at.x1, at.y1, lv_area_get_width(&at), lv_area_get_height(&at)};
    if (c->month) draw_month(lv_event_get_layer(event), c, box);
    else draw_week(lv_event_get_layer(event), c, box);
}

/* -------------------------------------------------------------------- facts */

static fact_t *fact(lcd_clock_t *c)
{
    if (c->fact_count == FACTS) return NULL;
    fact_t *f = &c->facts[c->fact_count++];
    *f = (fact_t){.tint = LCD_ACCENT};
    return f;
}

static void copy(char *out, size_t size, const char *text) { (void)snprintf(out, size, "%s", text ? text : ""); }

/* What the application says comes next, then what the calendar says. */
static void collect(lcd_clock_t *c, const xiaotai_view_t *view)
{
    c->fact_count = 0;
    for (size_t i = 0; i < view->item_count && i < 2; ++i) {
        const xiaotai_view_item_t *item = &view->items[i];
        fact_t *f = item->state & XIAOTAI_STATE_HIDDEN ? NULL : fact(c);
        if (!f) continue;
        f->icon = lcd_icon(item->icon);
        copy(f->label, sizeof(f->label), item->label);
        copy(f->detail, sizeof(f->detail), item->detail);
    }
    if (!view->item_count && view->text && view->text[0]) {
        fact_t *f = fact(c);
        if (f) copy(f->detail, sizeof(f->detail), view->text);
    }
    if (!c->now.valid) return;
    int ahead = 0;
    const char *name = lcd_holiday(c->today);
    fact_t *f = NULL;
    if (name && (f = fact(c))) {
        copy(f->label, sizeof(f->label), "今天是");
        copy(f->detail, sizeof(f->detail), name);
    } else if (!name && (name = lcd_next_holiday(c->today, &ahead)) && (f = fact(c))) {
        (void)snprintf(f->label, sizeof(f->label), "距%s", name);
        (void)snprintf(f->detail, sizeof(f->detail), "%d 天", ahead);
    }
    if (f) {
        f->icon = LCD_ICON_CALENDAR;
        f->tint = LCD_WARNING;
    }
    f = NULL;
    name = lcd_solar_term(c->today);
    if (name && (f = fact(c))) {
        copy(f->label, sizeof(f->label), "今日节气");
        copy(f->detail, sizeof(f->detail), name);
    } else if (!name && (name = lcd_next_solar_term(c->today, &ahead)) && (f = fact(c))) {
        (void)snprintf(f->label, sizeof(f->label), "距%s", name);
        (void)snprintf(f->detail, sizeof(f->detail), "%d 天", ahead);
    }
    if (f) {
        f->icon = LCD_ICON_LEAF;
        f->tint = LCD_CAPTION_USER;
    }
    if ((f = fact(c))) {
        const int32_t first = lcd_days(c->now.year, 1, 1), count = lcd_days(c->now.year + 1, 1, 1) - first;
        f->icon = LCD_ICON_PULSE;
        (void)snprintf(f->label, sizeof(f->label), "今年第%d天", (int)(c->today - first + 1));
        (void)snprintf(f->detail, sizeof(f->detail), "已过 %d%%", (int)((c->today - first + 1) * 100 / count));
    }
    lcd_lunar_t moon;
    if (lcd_lunar(c->today, &moon) && (f = fact(c))) {
        const int d = moon.day;
        f->icon = LCD_ICON_MOON;
        f->tint = LCD_CAPTION_AI;
        copy(f->label, sizeof(f->label), "月相");
        copy(f->detail, sizeof(f->detail), d == 1 ? "新月" : d < 7 ? "蛾眉月" : d < 10 ? "上弦月" : d < 15 ? "盈凸月" :
             d < 18 ? "满月" : d < 22 ? "亏凸月" : d < 25 ? "下弦月" : "残月");
    }
}

/* One line of a fact, for a card that has only one. */
static void joined(const fact_t *f, char *out, size_t size)
{
    (void)snprintf(out, size, "%s%s%s", f->label, f->label[0] && f->detail[0] ? " " : "", f->detail);
}

static void cards_text(lcd_clock_t *c)
{
    for (size_t i = 0; i < c->fact_count && i < CARDS; ++i) {
        char text[96];
        joined(&c->facts[i], text, sizeof(text));
        lcd_text(c->card[i].label, c->facts[i].label);
        lcd_text(c->card[i].detail, c->rows_one ? text : c->facts[i].detail);
    }
}

static int32_t fact_width(const lcd_clock_t *c, const fact_t *f)
{
    char text[96];
    joined(f, text, sizeof(text));
    const lv_font_t *font = c->s->m.text;
    const int32_t words = c->rows_one ? text_width(text, font) :
                          maxi(text_width(f->label, font), text_width(f->detail, font));
    return words + 2 * lcd_dp(c->s, 12) + (f->icon ? lv_font_get_line_height(c->s->m.icons) + lcd_dp(c->s, 10) : 0);
}

/* One row of cards from fact `first` on; returns the fact after the last one placed. */
static size_t place_cards(lcd_clock_t *c, size_t first, lcd_box_t row)
{
    lcd_state_t *s = c->s;
    const int32_t gap = lcd_dp(s, 8), line = lcd_line_height(s), icon = lv_font_get_line_height(s->m.icons);
    int32_t sum = 0;
    size_t end = first;
    for (; end < c->fact_count && end < CARDS; ++end) {
        const int32_t more = fact_width(c, &c->facts[end]) + (end > first ? gap : 0);
        if (end > first && sum + more > row.width) break;
        sum += more;
    }
    const int32_t n = (int32_t)(end - first), share = n ? (row.width - (n - 1) * gap) / n : 0;
    /* Equal widths when every card can do with one. */
    bool equal = true;
    for (size_t i = first; i < end; ++i) equal &= fact_width(c, &c->facts[i]) <= share;
    const int32_t spare = n ? maxi(0, row.width - sum) / n : 0;
    int32_t x = row.x;
    for (size_t i = first; i < end; ++i) {
        card_t *card = &c->card[i];
        const fact_t *f = &c->facts[i];
        const int32_t need = fact_width(c, f), width = equal ? share : mini(need + spare, row.width);
        lcd_visible(card->panel, true);
        lcd_place(card->panel, (lcd_box_t){x, row.y, width, row.height});
        x += width + gap;
        const bool drawn = f->icon && need <= width;   /* the words come before the picture */
        const int32_t lead = lcd_dp(s, 12), left = drawn ? lead + icon + lcd_dp(s, 10) : lead;
        const bool two = !c->rows_one && f->label[0];
        lcd_visible(card->icon, drawn);
        if (drawn) {
            lcd_place(card->icon, (lcd_box_t){lead, (row.height - icon) / 2, icon, icon});
            lv_obj_set_style_text_color(card->icon, lv_color_hex(f->tint), 0);
            lcd_text(card->icon, f->icon);
        }
        lcd_visible(card->label, two);
        const int32_t room = maxi(1, width - left - lead / 2), top = (row.height - (two ? 2 : 1) * line) / 2;
        if (two) lcd_place(card->label, (lcd_box_t){left, top, room, line});
        lcd_place(card->detail, (lcd_box_t){left, top + (two ? line : 0), room, line});
    }
    return end;
}

/* ------------------------------------------------------------------- layout */

/* The digits' own height and where it begins in their line. */
static int32_t digits_height(const lv_font_t *font) { return font->line_height * 74 / 100; }
static int32_t digits_top(const lv_font_t *font) { return font->line_height * 12 / 100; }

static void place_digits(lcd_clock_t *c, int32_t x, int32_t y)
{
    const int32_t two = text_width("00", c->digits), colon = text_width(":", c->digits), h = c->digits->line_height;
    const int32_t top = y - digits_top(c->digits);
    lv_obj_t *const parts[] = {c->hour, c->colon, c->minute};
    const int32_t widths[] = {two, colon, two};
    for (int i = 0; i < 3; ++i) {
        lv_obj_set_style_text_font(parts[i], c->digits, 0);
        lcd_place(parts[i], (lcd_box_t){x, top, widths[i], h});
        x += widths[i];
    }
}

static int month_rows(const lcd_clock_t *c)
{
    if (!c->now.valid) return 6;
    const int lead = (lcd_weekday(lcd_days(c->now.year, c->now.month, 1)) + 6) % 7;
    return (lead + lcd_month_days(c->now.year, c->now.month) + 6) / 7;
}

typedef enum { HELLO, DIGITS, TRACK, DATE, LUNAR, WEEK, FACT, MORE, BLOCKS } block_t;

static lcd_box_t beside_phone(lcd_clock_t *c, lcd_box_t row)
{
    if (!c->with_phone) return row;
    lcd_state_t *s = c->s;
    const int32_t side = mini(maxi(row.height, lcd_dp(s, 46)), 3 * lcd_line_height(s));
    c->phone = (lcd_box_t){row.x + row.width - side, row.y + (row.height - side) / 2, side, side};
    row.width -= side + lcd_dp(s, 10);
    return row;
}

static void layout(lcd_clock_t *c)
{
    lcd_state_t *s = c->s;
    const int32_t line = lcd_line_height(s), two = text_width("88", s->m.text);
    const lcd_box_t area = c->area;
    c->digits = s->m.clock;
    c->digits_w = text_width("00:00", c->digits);
    c->pad = lcd_dp(s, 10);

    /* The month beside the time, when it leaves the time its width. */
    const int32_t head = 2 * line + lcd_dp(s, 8) + 2 * c->pad;
    const int32_t cell_min = two + lcd_dp(s, 7), between = lcd_dp(s, 16);
    const int32_t smallest = text_width("00:00", s->m.number);
    c->month = area.height >= head + 6 * line && area.width - between - 7 * cell_min - 2 * c->pad >= smallest + lcd_dp(s, 30);
    lcd_box_t left = area;
    if (c->month) {
        c->cell_w = maxi(cell_min, mini((area.width * 42 / 100 - 2 * c->pad) / 7, two + lcd_dp(s, 18)));
        left.width = area.width - 7 * c->cell_w - 2 * c->pad - between;
    }
    lv_obj_set_style_bg_opa(c->calendar, c->month ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    if (c->digits_w > left.width) {
        c->digits = s->m.number;
        c->digits_w = text_width("00:00", c->digits);
    }

    /* Blocks from top to bottom; the least needed ones go while it is too tall. */
    const int32_t card_h = 2 * line + lcd_dp(s, 14);
    int32_t height[BLOCKS] = {[HELLO] = line, [DIGITS] = digits_height(c->digits), [TRACK] = lcd_dp(s, 3),
                              [DATE] = line, [LUNAR] = c->month ? line : 0,
                              [WEEK] = c->month ? 0 : 2 * line + lcd_dp(s, 4),
                              [FACT] = c->fact_count || c->with_phone ? card_h : 0,
                              [MORE] = c->fact_count > 2 ? card_h : 0};
    c->rows_one = false;
    int32_t sum = 0, blocks = 0;
    static const uint8_t spend[] = {MORE, HELLO, FACT, TRACK, LUNAR};
    /* What a block keeps to the one below it, at least and at most. */
    const int32_t tight = lcd_dp(s, 4), wide = lcd_dp(s, 16);
    for (size_t step = 0; ; ++step) {
        sum = blocks = 0;
        for (int i = 0; i < BLOCKS; ++i) {
            sum += height[i];
            blocks += height[i] > 0;
        }
        /* A second row of cards is for a page that has space to spare. */
        if (sum + (blocks - 1) * (height[MORE] ? lcd_dp(s, 10) : tight) <= left.height || step == sizeof(spend)) break;
        if (spend[step] == FACT && height[FACT]) {
            height[FACT] = line + lcd_dp(s, 4);   /* one line instead of a card */
            c->rows_one = true;
        } else {
            height[spend[step]] = 0;
        }
    }
    c->merged = !height[LUNAR];
    /* Date and lunar date belong together, and so do the rows of cards. */
    const int32_t pairs = (height[LUNAR] ? 1 : 0) + (height[MORE] ? 1 : 0), free = blocks - 1 - pairs;
    const int32_t gap = free > 0 ? mini(maxi(0, left.height - sum - pairs * tight) / free, wide) : 0;
    const int32_t group = sum + free * gap + (height[LUNAR] ? mini(gap, tight) : 0) + (height[MORE] ? lcd_dp(s, 8) : 0);
    int32_t y = left.y + maxi(0, left.height - group) / 2;
    if (c->month) {   /* as tall as what stands beside it */
        const int rows = month_rows(c);
        c->cell_h = maxi(line, mini((maxi(group, mini(left.height, head + rows * (line + lcd_dp(s, 5)))) - head) / rows,
                                    line + lcd_dp(s, 18)));
        const int32_t width = 7 * c->cell_w + 2 * c->pad, tall = head + rows * c->cell_h;
        lcd_place(c->calendar, (lcd_box_t){area.x + area.width - width, area.y + (area.height - tall) / 2, width, tall});
    }
    for (size_t k = 0; k < CARDS; ++k) {
        lcd_visible(c->card[k].panel, false);
        lv_obj_set_style_bg_opa(c->card[k].panel, c->rows_one ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    }
    c->phone = (lcd_box_t){0};
    const bool centred = !c->month;
    const int32_t icon = lv_font_get_line_height(s->m.icons), space = lcd_dp(s, 9);
    size_t placed = 0;
    for (int i = 0; i < BLOCKS; ++i) {
        const int32_t h = height[i];
        const lcd_box_t row = {left.x, y, left.width, h};
        if (h) y += h + (i == DATE && height[LUNAR] ? mini(gap, tight) : i == FACT ? lcd_dp(s, 8) : gap);
        switch ((block_t)i) {
        case HELLO: {
            const int32_t w = icon + space + text_width("晚上好", s->m.text);
            const int32_t x = centred ? row.x + (row.width - w) / 2 : row.x;
            lcd_visible(c->hello, h > 0);
            lcd_visible(c->hello_icon, h > 0);
            lcd_place(c->hello_icon, (lcd_box_t){x, row.y + (line - icon) / 2, icon, icon});
            lcd_place(c->hello, (lcd_box_t){x + icon + space, row.y, maxi(1, row.x + row.width - x - icon - space), line});
            break;
        }
        case DIGITS:
            place_digits(c, centred ? row.x + (row.width - c->digits_w) / 2 : row.x, row.y);
            break;
        case TRACK:
            lcd_visible(c->track, h > 0);
            lcd_place(c->track, (lcd_box_t){centred ? row.x + (row.width - c->digits_w) / 2 : row.x, row.y, c->digits_w, maxi(1, h)});
            lv_obj_set_height(c->fill, maxi(1, h));
            c->second = -1;
            break;
        case DATE:
        case LUNAR: {
            lv_obj_t *obj = i == DATE ? c->date : c->lunar;
            lcd_visible(obj, h > 0);
            lv_obj_set_style_text_align(obj, centred ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT, 0);
            lcd_place(obj, (lcd_box_t){row.x, row.y, row.width, line});
            c->words_w = row.width;
            break;
        }
        case WEEK:
            if (!c->month) lcd_place(c->calendar, row);
            break;
        case FACT:
        case MORE: {
            if (!h) break;
            /* The call shortcut ends the last row. */
            lcd_box_t cards = i == MORE || !height[MORE] ? beside_phone(c, row) : row;
            if (c->rows_one && c->fact_count && centred) {   /* a centred line of text */
                const int32_t w = mini(fact_width(c, &c->facts[0]), cards.width);
                cards.x += (cards.width - w) / 2;
                cards.width = w;
            }
            placed = place_cards(c, placed, cards);
            break;
        }
        case BLOCKS:
            break;
        }
    }
    cards_text(c);
    lv_obj_invalidate(c->calendar);
    c->placed = true;
}

/* -------------------------------------------------------------------- texts */

static void texts(lcd_clock_t *c)
{
    static const char *const weekdays[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
    const xiaotai_ui_clock_t *now = &c->now;
    char hour[8] = "--", minute[8] = "--", date[96] = "等待时间同步", lunar[96] = "";
    const char *hello = "你好";
    bool day = true;
    if (now->valid) {
        (void)snprintf(hour, sizeof(hour), "%02u", (unsigned)now->hour % 24U);
        (void)snprintf(minute, sizeof(minute), "%02u", (unsigned)now->minute % 60U);
        const unsigned h = now->hour;
        hello = h < 5 || h >= 23 ? "夜深了" : h < 9 ? "早上好" : h < 11 ? "上午好" : h < 13 ? "中午好" :
                h < 18 ? "下午好" : "晚上好";
        day = h >= 6 && h < 18;
        lcd_lunar_t moon;
        char words[32] = "", year[24] = "";
        if (lcd_lunar(c->today, &moon)) {
            (void)lcd_lunar_text(&moon, words, sizeof(words));
            (void)lcd_lunar_year_text(&moon, year, sizeof(year));
            (void)snprintf(lunar, sizeof(lunar), "农历%s · %s", words, year);
            if (text_width(lunar, c->s->m.text) > c->words_w) (void)snprintf(lunar, sizeof(lunar), "农历%s", words);
        }
        (void)snprintf(date, sizeof(date), "%u月%u日 %s%s%s", (unsigned)now->month, (unsigned)now->day,
                       weekdays[now->weekday % 7U], c->merged && words[0] ? " · " : "", c->merged ? words : "");
    }
    lcd_text(c->hour, hour);
    lcd_text(c->minute, minute);
    lcd_text(c->date, date);
    lcd_text(c->lunar, lunar);
    lcd_text(c->hello, hello);
    lcd_text(c->hello_icon, day ? LCD_ICON_SUN : LCD_ICON_MOON);
    lv_obj_set_style_text_color(c->hello_icon, lv_color_hex(day ? LCD_WARNING : LCD_CAPTION_AI), 0);
}

/* ---------------------------------------------------------------- interface */

static lv_obj_t *line_of(lcd_clock_t *c, lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *obj = lcd_label(parent, font, color);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_DOT);
    (void)c;
    return obj;
}

lcd_clock_t *lcd_clock_open(lcd_state_t *s, lv_obj_t *root)
{
    lcd_clock_t *c = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*c));
    if (!c) return NULL;
    c->s = s;
    c->second = -1;
    c->hello_icon = lcd_label(root, s->m.icons, LCD_WARNING);
    c->hello = line_of(c, root, s->m.text, LCD_MUTED);
    c->hour = lcd_label(root, s->m.clock, LCD_TEXT);
    c->colon = lcd_label(root, s->m.clock, LCD_FAINT);
    c->minute = lcd_label(root, s->m.clock, LCD_ACCENT);
    lcd_text(c->colon, ":");
    c->track = lcd_panel(root, (lcd_box_t){0}, LCD_SURFACE_HIGH, LV_OPA_COVER, LV_RADIUS_CIRCLE);
    c->fill = lcd_panel(c->track, (lcd_box_t){0}, LCD_ACCENT, LV_OPA_COVER, LV_RADIUS_CIRCLE);
    c->date = line_of(c, root, s->m.text, LCD_TEXT);
    c->lunar = line_of(c, root, s->m.text, LCD_MUTED);
    c->calendar = lcd_panel(root, (lcd_box_t){0}, LCD_SURFACE, LV_OPA_COVER, lcd_dp(s, LCD_RADIUS_CARD));
    lv_obj_add_event_cb(c->calendar, draw_calendar, LV_EVENT_DRAW_MAIN, c);
    for (size_t i = 0; i < CARDS; ++i) {
        card_t *card = &c->card[i];
        card->panel = lcd_panel(root, (lcd_box_t){0}, LCD_SURFACE, LV_OPA_COVER, lcd_dp(s, LCD_RADIUS));
        card->icon = lcd_label(card->panel, s->m.icons, LCD_ACCENT);
        card->label = line_of(c, card->panel, s->m.text, LCD_MUTED);
        card->detail = line_of(c, card->panel, s->m.text, LCD_TEXT);
        lcd_visible(card->panel, false);
    }
    return c;
}

lcd_box_t lcd_clock_place(lcd_clock_t *c, const xiaotai_view_t *view, lcd_box_t area, bool phone)
{
    c->area = area;
    c->with_phone = phone;
    collect(c, view);
    layout(c);
    texts(c);
    return c->phone;
}

bool lcd_clock_update(lcd_clock_t *c, const xiaotai_view_t *view, const xiaotai_ui_scene_t *scene)
{
    const xiaotai_ui_clock_t *now = &scene->clock;
    const int32_t today = now->valid ? lcd_days(now->year, now->month, now->day) : 0;
    const bool another_day = now->valid != c->now.valid || today != c->today;
    const bool time = another_day || now->hour != c->now.hour || now->minute != c->now.minute;
    if (time) {   /* the seconds run on from here; another render does not know better */
        c->now = *now;
        c->stamp = scene->now_ms;
    }
    c->today = today;
    fact_t before[FACTS];
    const size_t count = c->fact_count;
    memcpy(before, c->facts, sizeof(before));
    collect(c, view);
    /* A countdown that only moved on keeps its place; the page is not laid
     * out again every second for it. */
    bool same = !another_day && count == c->fact_count, words = false;
    for (size_t i = 0; same && i < count; ++i) {
        same = before[i].icon == c->facts[i].icon && !strcmp(before[i].label, c->facts[i].label) &&
               strlen(before[i].detail) == strlen(c->facts[i].detail);
        words |= strcmp(before[i].detail, c->facts[i].detail) != 0;
    }
    if (c->placed && same && words) cards_text(c);
    if (time) texts(c);
    (void)lcd_clock_tick(c, scene->now_ms);
    return !c->placed || !same;
}

uint32_t lcd_clock_tick(lcd_clock_t *c, uint32_t now_ms)
{
    if (!c || !c->now.valid || !c->placed) return 0;
    const uint32_t passed = now_ms - c->stamp;
    const int second = (int)((c->now.second + passed / 1000U) % 60U);
    if (second != c->second) {
        c->second = second;
        lv_obj_set_width(c->fill, c->digits_w * second / 59);
    }
    return 1000U - passed % 1000U;
}

void lcd_clock_free(lcd_clock_t *c) { xiaotai_mem_free(c); }
