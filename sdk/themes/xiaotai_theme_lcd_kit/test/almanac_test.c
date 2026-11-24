// SPDX-License-Identifier: MIT
// Dates of the clock page against published calendars.
#include "lcd_almanac.h"

#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

static bool lunar_is(int y, int m, int d, const char *text)
{
    lcd_lunar_t lunar;
    char out[32];
    return lcd_lunar(lcd_days(y, m, d), &lunar) && lcd_lunar_text(&lunar, out, sizeof(out)) && !strcmp(out, text);
}

static bool named(const char *name, const char *text) { return name ? text && !strcmp(name, text) : !text; }

int main(void)
{
    /* The civil calendar. */
    CHECK(lcd_days(1970, 1, 1) == 0 && lcd_days(2000, 3, 1) == 11017);
    for (int32_t day = lcd_days(2019, 12, 1); day < lcd_days(2058, 1, 1); ++day) {
        int y, m, d;
        lcd_date(day, &y, &m, &d);
        CHECK(lcd_days(y, m, d) == day && d >= 1 && d <= lcd_month_days(y, m));
    }
    CHECK(lcd_weekday(lcd_days(2026, 11, 24)) == 2 && lcd_month_days(2028, 2) == 29 && lcd_month_days(2026, 12) == 31);
    CHECK(lcd_week(lcd_days(2026, 11, 24)) == 48 && lcd_week(lcd_days(2027, 1, 1)) == 53 &&
          lcd_week(lcd_days(2024, 12, 30)) == 1);

    /* The lunar calendar: new years, leap months, both ends of the table. */
    CHECK(lunar_is(2026, 11, 24, "十月十六") && lunar_is(2026, 2, 17, "正月初一") && lunar_is(2026, 2, 16, "腊月廿九"));
    CHECK(lunar_is(2025, 7, 24, "六月三十") && lunar_is(2025, 7, 25, "闰六月初一") && lunar_is(2025, 8, 23, "七月初一"));
    CHECK(lunar_is(2028, 6, 23, "闰五月初一") && lunar_is(2033, 12, 22, "闰冬月初一") && lunar_is(2023, 11, 15, "十月初三"));
    CHECK(lunar_is(2026, 9, 25, "八月十五") && lunar_is(2027, 1, 1, "冬月廿四") && lunar_is(2026, 12, 18, "冬月初十"));
    lcd_lunar_t lunar;
    char text[32];
    CHECK(lcd_lunar(lcd_days(2026, 11, 24), &lunar) && lcd_lunar_year_text(&lunar, text, sizeof(text)) && !strcmp(text, "丙午马年"));
    CHECK(lcd_lunar(lcd_days(2026, 2, 16), &lunar) && lcd_lunar_year_text(&lunar, text, sizeof(text)) && !strcmp(text, "乙巳蛇年"));
    CHECK(!lcd_lunar(lcd_days(2019, 6, 1), &lunar) && !lcd_lunar(lcd_days(2020, 1, 1), &lunar) && !lcd_lunar(lcd_days(2057, 3, 1), &lunar));
    CHECK(lcd_lunar(lcd_days(2056, 12, 31), &lunar));

    /* Solar terms and holidays. */
    CHECK(named(lcd_solar_term(lcd_days(2026, 11, 22)), "小雪") && named(lcd_solar_term(lcd_days(2026, 12, 7)), "大雪"));
    CHECK(named(lcd_solar_term(lcd_days(2026, 2, 4)), "立春") && named(lcd_solar_term(lcd_days(2026, 4, 5)), "清明"));
    CHECK(named(lcd_solar_term(lcd_days(2025, 2, 3)), "立春") && named(lcd_solar_term(lcd_days(2026, 12, 22)), "冬至"));
    CHECK(named(lcd_solar_term(lcd_days(2026, 6, 21)), "夏至") && named(lcd_solar_term(lcd_days(2026, 11, 24)), NULL));
    CHECK(named(lcd_holiday(lcd_days(2026, 2, 16)), "除夕") && named(lcd_holiday(lcd_days(2026, 2, 17)), "春节"));
    CHECK(named(lcd_holiday(lcd_days(2026, 6, 19)), "端午节") && named(lcd_holiday(lcd_days(2026, 9, 25)), "中秋节"));
    CHECK(named(lcd_holiday(lcd_days(2026, 4, 5)), "清明节") && named(lcd_holiday(lcd_days(2025, 10, 6)), "中秋节"));
    CHECK(named(lcd_holiday(lcd_days(2026, 10, 1)), "国庆节") && named(lcd_holiday(lcd_days(2026, 11, 24)), NULL));
    int ahead = 0;
    CHECK(named(lcd_next_holiday(lcd_days(2026, 11, 24), &ahead), "元旦") && ahead == 38);
    CHECK(named(lcd_next_solar_term(lcd_days(2026, 11, 24), &ahead), "大雪") && ahead == 13);
    /* Outside the table only the fixed dates remain. */
    CHECK(named(lcd_next_holiday(lcd_days(2060, 9, 20), &ahead), "国庆节") && ahead == 11);
    CHECK(named(lcd_next_solar_term(lcd_days(2060, 9, 20), &ahead), NULL));
    return 0;
}
