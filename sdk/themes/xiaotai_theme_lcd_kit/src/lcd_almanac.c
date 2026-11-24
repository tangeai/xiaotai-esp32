// SPDX-License-Identifier: MIT
#include "lcd_almanac.h"

#include <stdio.h>

#include "lcd_almanac_data.inc"

enum { TERM_AHEAD = 20, HOLIDAY_AHEAD = 120 };

int32_t lcd_days(int year, int month, int day)
{
    const int y = year - (month <= 2);
    const int era = y / 400, yoe = y - era * 400;
    const int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int32_t)era * 146097 + doe - 719468;
}

void lcd_date(int32_t days, int *year, int *month, int *day)
{
    const int32_t z = days + 719468, era = z / 146097;
    const int32_t doe = z - era * 146097;
    const int32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    *day = (int)(doy - (153 * mp + 2) / 5 + 1);
    *month = (int)(mp < 10 ? mp + 3 : mp - 9);
    *year = (int)(yoe + era * 400) + (*month <= 2);
}

int lcd_weekday(int32_t days) { return (int)((days + 4) % 7); }

int lcd_month_days(int year, int month)
{
    return (int)(month == 12 ? 31 : lcd_days(year, month + 1, 1) - lcd_days(year, month, 1));
}

int lcd_week(int32_t days)
{
    /* The week belongs to the year its Thursday is in. */
    const int32_t thursday = days - (lcd_weekday(days) + 6) % 7 + 3;
    int year, month, day;
    lcd_date(thursday, &year, &month, &day);
    return (int)((thursday - lcd_days(year, 1, 1)) / 7) + 1;
}

bool lcd_lunar(int32_t days, lcd_lunar_t *out)
{
    int year, month, day;
    lcd_date(days, &year, &month, &day);
    if (year > ALMANAC_LAST) return false;
    for (int pass = 0; pass < 2; ++pass, --year) {
        if (year < ALMANAC_FIRST) return false;
        const uint32_t word = lunar_years[year - ALMANAC_FIRST];
        int32_t left = days - lcd_days(year, 1, 1) - (int32_t)(word & 0x3fU);
        if (left < 0) continue;   /* before the new year: still the year before */
        const unsigned leap = word >> 6 & 0xfU;
        for (unsigned i = 0; i < (leap ? 13U : 12U); ++i) {
            const int32_t length = word >> (10 + i) & 1U ? 30 : 29;
            if (left < length) {
                const bool past = leap && i >= leap;
                *out = (lcd_lunar_t){.year = (uint16_t)year, .month = (uint8_t)(past ? i : i + 1U),
                                     .day = (uint8_t)(left + 1), .leap = leap && i == leap};
                return true;
            }
            left -= length;
        }
        return false;
    }
    return false;
}

bool lcd_lunar_text(const lcd_lunar_t *lunar, char *out, size_t size)
{
    static const char *const months[] = {"正", "二", "三", "四", "五", "六", "七", "八", "九", "十", "冬", "腊"};
    static const char *const digits[] = {"十", "一", "二", "三", "四", "五", "六", "七", "八", "九"};
    const int d = lunar->day;
    const char *tens = d <= 10 ? "初" : d < 20 ? "十" : d == 20 ? "二" : d < 30 ? "廿" : "三";
    return snprintf(out, size, "%s%s月%s%s", lunar->leap ? "闰" : "", months[lunar->month - 1], tens,
                    digits[d % 10]) < (int)size;
}

bool lcd_lunar_year_text(const lcd_lunar_t *lunar, char *out, size_t size)
{
    static const char *const stems[] = {"甲", "乙", "丙", "丁", "戊", "己", "庚", "辛", "壬", "癸"};
    static const char *const branches[] = {"子", "丑", "寅", "卯", "辰", "巳", "午", "未", "申", "酉", "戌", "亥"};
    static const char *const animals[] = {"鼠", "牛", "虎", "兔", "龙", "蛇", "马", "羊", "猴", "鸡", "狗", "猪"};
    const int n = lunar->year - 4;
    return snprintf(out, size, "%s%s%s年", stems[n % 10], branches[n % 12], animals[n % 12]) < (int)size;
}

const char *lcd_solar_term(int32_t days)
{
    static const char *const names[24] = {
        "小寒", "大寒", "立春", "雨水", "惊蛰", "春分", "清明", "谷雨", "立夏", "小满", "芒种", "夏至",
        "小暑", "大暑", "立秋", "处暑", "白露", "秋分", "寒露", "霜降", "立冬", "小雪", "大雪", "冬至"};
    int year, month, day;
    lcd_date(days, &year, &month, &day);
    if (year < ALMANAC_FIRST || year > ALMANAC_LAST) return NULL;
    const uint8_t *steps = term_steps[year - ALMANAC_FIRST];
    for (int i = (month - 1) * 2; i < month * 2; ++i)
        if (term_base[i] + (steps[i / 4] >> (i % 4 * 2) & 3) == day) return names[i];
    return NULL;
}

const char *lcd_holiday(int32_t days)
{
    static const struct { uint8_t month, day; const char *name; } fixed[] = {
        {1, 1, "元旦"}, {5, 1, "劳动节"}, {6, 1, "儿童节"}, {9, 10, "教师节"}, {10, 1, "国庆节"},
    }, lunar_days[] = {
        {1, 1, "春节"}, {1, 15, "元宵节"}, {5, 5, "端午节"}, {7, 7, "七夕"}, {8, 15, "中秋节"}, {9, 9, "重阳节"},
        {12, 8, "腊八节"},
    };
    int year, month, day;
    lcd_date(days, &year, &month, &day);
    lcd_lunar_t lunar;
    if (lcd_lunar(days, &lunar) && !lunar.leap)
        for (size_t i = 0; i < sizeof(lunar_days) / sizeof(*lunar_days); ++i)
            if (lunar.month == lunar_days[i].month && lunar.day == lunar_days[i].day) return lunar_days[i].name;
    lcd_lunar_t after;
    if (lcd_lunar(days + 1, &after) && after.month == 1 && after.day == 1 && !after.leap) return "除夕";
    for (size_t i = 0; i < sizeof(fixed) / sizeof(*fixed); ++i)
        if (month == fixed[i].month && day == fixed[i].day) return fixed[i].name;
    const char *term = lcd_solar_term(days);
    return term && month == 4 && day < 10 ? "清明节" : NULL;
}

static const char *next(int32_t days, int limit, const char *(*named)(int32_t), int *ahead)
{
    for (int i = 1; i <= limit; ++i) {
        const char *name = named(days + i);
        if (!name) continue;
        *ahead = i;
        return name;
    }
    return NULL;
}

const char *lcd_next_solar_term(int32_t days, int *ahead) { return next(days, TERM_AHEAD, lcd_solar_term, ahead); }
const char *lcd_next_holiday(int32_t days, int *ahead) { return next(days, HOLIDAY_AHEAD, lcd_holiday, ahead); }
