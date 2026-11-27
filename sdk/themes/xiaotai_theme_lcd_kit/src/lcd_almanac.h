// SPDX-License-Identifier: MIT
#pragma once
/* What the clock page says about a date: the lunar date, the solar term, the
 * next holiday. Computed on the device from a small table
 * (scripts/generate_lcd_almanac.py); a date outside it has none of these. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint16_t year;        /* the calendar year the lunar year began in */
    uint8_t month, day;   /* 1..12, 1..30 */
    bool leap;
} lcd_lunar_t;

/* Days since 1970-01-01, and back. */
int32_t lcd_days(int year, int month, int day);
void lcd_date(int32_t days, int *year, int *month, int *day);
/* 0 Sunday .. 6. */
int lcd_weekday(int32_t days);
int lcd_month_days(int year, int month);
/* ISO 8601 week of the year, 1..53. */
int lcd_week(int32_t days);

bool lcd_lunar(int32_t days, lcd_lunar_t *out);
/* "十月十六", "闰六月初一". */
bool lcd_lunar_text(const lcd_lunar_t *lunar, char *out, size_t size);
/* "丙午马年". */
bool lcd_lunar_year_text(const lcd_lunar_t *lunar, char *out, size_t size);

/* The solar term that begins on this day, or NULL. */
const char *lcd_solar_term(int32_t days);
/* The holiday on this day, or NULL. */
const char *lcd_holiday(int32_t days);
/* The first one after this day, and how many days ahead; NULL beyond the table. */
const char *lcd_next_solar_term(int32_t days, int *ahead);
const char *lcd_next_holiday(int32_t days, int *ahead);
