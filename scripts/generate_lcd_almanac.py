#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Regenerate or check the colour theme kit's lunar calendar and solar terms.

    python -B scripts/generate_lcd_almanac.py --check
    python -B scripts/generate_lcd_almanac.py --write
    python -B scripts/generate_lcd_almanac.py --dump     # first day of every lunar month, for comparing

The table is computed, not copied: new moons and the sun's apparent longitude
from the series in Jean Meeus, "Astronomical Algorithms" (chapters 25 and 49),
in China Standard Time, and the calendar's rules (the month of the winter
solstice is the eleventh; of thirteen months between two solstices the first
without a principal term is the leap month). One word a lunar year, six bytes
of solar terms a calendar year.
"""

import argparse
import datetime
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TARGET = ROOT / "sdk/themes/xiaotai_theme_lcd_kit/src/lcd_almanac_data.inc"
FIRST, LAST = 2020, 2056      # from 2057 on a few new moons fall within seconds of midnight
EPOCH = datetime.date(2000, 1, 1)
JD_EPOCH = 2451544.5      # 2000-01-01 00:00


def rad(x):
    return math.radians(x % 360.0)


def delta_t_days(year):
    """Terrestrial minus universal time (Espenak and Meeus)."""
    if year < 2050:
        t = year - 2000
        seconds = 62.92 + 0.32217 * t + 0.005589 * t * t
    else:
        u = (year - 1820) / 100
        seconds = -20 + 32 * u * u - 0.5628 * (2150 - year)
    return seconds / 86400.0


def new_moon(k):
    """Julian ephemeris day of new moon number k (0 is the first of 2000)."""
    t = k / 1236.85
    jde = 2451550.09766 + 29.530588861 * k + 0.00015437 * t ** 2 - 0.000000150 * t ** 3 + 0.00000000073 * t ** 4
    e = 1 - 0.002516 * t - 0.0000074 * t ** 2
    m = rad(2.5534 + 29.10535670 * k - 0.0000014 * t ** 2 - 0.00000011 * t ** 3)
    n = rad(201.5643 + 385.81693528 * k + 0.0107582 * t ** 2 + 0.00001238 * t ** 3 - 0.000000058 * t ** 4)
    f = rad(160.7108 + 390.67050284 * k - 0.0016118 * t ** 2 - 0.00000227 * t ** 3 + 0.000000011 * t ** 4)
    o = rad(124.7746 - 1.56375588 * k + 0.0020672 * t ** 2 + 0.00000215 * t ** 3)
    terms = [(-0.40720, 0, n), (0.17241, 1, m), (0.01608, 0, 2 * n), (0.01039, 0, 2 * f), (0.00739, 1, n - m),
             (-0.00514, 1, n + m), (0.00208, 2, 2 * m), (-0.00111, 0, n - 2 * f), (-0.00057, 0, n + 2 * f),
             (0.00056, 1, 2 * n + m), (-0.00042, 0, 3 * n), (0.00042, 1, m + 2 * f), (0.00038, 1, m - 2 * f),
             (-0.00024, 1, 2 * n - m), (-0.00017, 0, o), (-0.00007, 0, n + 2 * m), (0.00004, 0, 2 * n - 2 * f),
             (0.00004, 0, 3 * m), (0.00003, 0, n + m - 2 * f), (0.00003, 0, 2 * n + 2 * f),
             (-0.00003, 0, n + m + 2 * f), (0.00003, 0, n - m + 2 * f), (-0.00002, 0, n - m - 2 * f),
             (-0.00002, 0, 3 * n + m), (0.00002, 0, 4 * n)]
    jde += sum(c * e ** p * math.sin(a) for c, p, a in terms)
    planets = [(0.000325, 299.77 + 0.107408 * k - 0.009173 * t * t), (0.000165, 251.88 + 0.016321 * k),
               (0.000164, 251.83 + 26.651886 * k), (0.000126, 349.42 + 36.412478 * k),
               (0.000110, 84.66 + 18.206239 * k), (0.000062, 141.74 + 53.303771 * k),
               (0.000060, 207.14 + 2.453732 * k), (0.000056, 154.84 + 7.306860 * k),
               (0.000047, 34.52 + 27.261239 * k), (0.000042, 207.19 + 0.121824 * k),
               (0.000040, 291.34 + 1.844379 * k), (0.000037, 161.72 + 24.198154 * k),
               (0.000035, 239.56 + 25.513099 * k), (0.000023, 331.55 + 3.592518 * k)]
    return jde + sum(c * math.sin(rad(a)) for c, a in planets)


def sun_longitude(jde):
    """Apparent longitude of the sun in degrees, with the main planetary perturbations."""
    t = (jde - 2451545.0) / 36525.0
    l0 = 280.46646 + 36000.76983 * t + 0.0003032 * t * t
    m = rad(357.52911 + 35999.05029 * t - 0.0001537 * t * t)
    c = ((1.914602 - 0.004817 * t - 0.000014 * t * t) * math.sin(m)
         + (0.019993 - 0.000101 * t) * math.sin(2 * m) + 0.000289 * math.sin(3 * m))
    c += (0.00134 * math.cos(rad(153.23 + 22518.7541 * t)) + 0.00154 * math.cos(rad(216.57 + 45037.5082 * t))
          + 0.00200 * math.cos(rad(312.69 + 32964.3577 * t))
          + 0.00179 * math.sin(rad(350.74 + 445267.1142 * t - 0.00144 * t * t))
          + 0.00178 * math.sin(rad(231.19 + 20.20 * t)))
    omega = rad(125.04 - 1934.136 * t)
    return (l0 + c - 0.00569 - 0.00478 * math.sin(omega)) % 360.0


def china(jde):
    """(date, fraction of the day) of a Julian ephemeris day in China Standard Time."""
    year = 2000 + (jde - 2451545.0) / 365.25
    days = jde - delta_t_days(year) + 8 / 24 - JD_EPOCH
    whole = math.floor(days)
    return EPOCH + datetime.timedelta(days=whole), days - whole


def solar_term(year, index):
    """Term `index` of a calendar year, 0 is xiaohan (285 degrees), in January."""
    target = (285 + 15 * index) % 360
    jde = JD_EPOCH + (datetime.date(year, 1, 5) - EPOCH).days + index * 15.2184
    for _ in range(12):
        off = (target - sun_longitude(jde) + 180) % 360 - 180
        jde += off * 365.2422 / 360
    return china(jde)


def months():
    """[(first day, month number, leap, days)] of the lunar months around FIRST..LAST."""
    first = round((FIRST - 2 - 2000) * 12.3685) - 2
    starts = [china(new_moon(k))[0] for k in range(first, first + (LAST - FIRST + 5) * 13)]
    out = []
    for year in range(FIRST - 2, LAST + 2):
        solstice, following = solar_term(year, 23)[0], solar_term(year + 1, 23)[0]
        a = max(i for i, s in enumerate(starts) if s <= solstice)
        b = max(i for i, s in enumerate(starts) if s <= following)
        principal = sorted(solar_term(y, i)[0] for y in (year, year + 1) for i in range(1, 24, 2))
        number, leap_left = 11, b - a == 13
        for i in range(a, b):
            leap = False
            if i > a:
                has = any(starts[i] <= d < starts[i + 1] for d in principal)
                if leap_left and not has:
                    leap, leap_left = True, False
                else:
                    number = number % 12 + 1
            out.append((starts[i], number, leap, (starts[i + 1] - starts[i]).days))
    return out


def generate():
    table = months()
    words = []
    for year in range(FIRST, LAST + 1):
        begin = next(i for i, (d, n, leap, _) in enumerate(table)
                     if n == 1 and not leap and d.year == year and d.month in (1, 2))
        end = next(i for i in range(begin + 1, len(table)) if table[i][1] == 1 and not table[i][2])
        run = table[begin:end]
        if len(run) not in (12, 13) or any(days not in (29, 30) for _, _, _, days in run):
            raise ValueError(f"{year}: {run}")
        leap_month = next((n for _, n, leap, _ in run if leap), 0)
        lengths = sum((days == 30) << i for i, (_, _, _, days) in enumerate(run))
        offset = (run[0][0] - datetime.date(year, 1, 1)).days
        words.append(offset | leap_month << 6 | lengths << 10)
    days, close = {}, []
    for year in range(FIRST, LAST + 1):
        for i in range(24):
            date, fraction = solar_term(year, i)
            if date.month != i // 2 + 1:
                raise ValueError(f"{year} term {i}: {date}")
            days[year, i] = date.day
            if min(fraction, 1 - fraction) < 4 / 1440:
                close.append((date.isoformat(), i, round((fraction if fraction < .5 else fraction - 1) * 1440, 1)))
    base = [min(days[y, i] for y in range(FIRST, LAST + 1)) for i in range(24)]
    terms = []
    for year in range(FIRST, LAST + 1):
        bits = 0
        for i in range(24):
            step = days[year, i] - base[i]
            if step > 3:
                raise ValueError(f"term {i}: more than two bits")
            bits |= step << (2 * i)
        terms.append(bits)
    lines = ["// SPDX-License-Identifier: MIT",
             "/* Generated by scripts/generate_lcd_almanac.py; do not edit. */",
             f"#define ALMANAC_FIRST {FIRST}", f"#define ALMANAC_LAST {LAST}",
             "/* bits 0-5 days from 1 January to the lunar new year, 6-9 the leap month (0 none),",
             " * 10-22 the months in order, set for thirty days */",
             "static const uint32_t lunar_years[] = {"]
    lines += ["    " + ", ".join(f"0x{w:06x}" for w in words[i:i + 8]) + "," for i in range(0, len(words), 8)]
    lines += ["};", "static const uint8_t term_base[24] = {" + ", ".join(map(str, base)) + "};",
              "/* two bits a term above its base day */", "static const uint8_t term_steps[][6] = {"]
    for i in range(0, len(terms), 3):
        lines.append("    " + ", ".join("{" + ", ".join(f"0x{b >> (8 * j) & 255:02x}" for j in range(6)) + "}"
                                         for b in terms[i:i + 3]) + ",")
    return "\n".join(lines + ["};", ""]), close


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    task = parser.add_mutually_exclusive_group(required=True)
    for name in ("--check", "--write", "--dump"):
        task.add_argument(name, action="store_true")
    args = parser.parse_args()
    if args.dump:
        for date, number, leap, days in months():
            if FIRST <= date.year <= LAST:
                print(date.isoformat(), number, int(leap), days)
        return
    text, close = generate()
    if args.write:
        TARGET.write_bytes(text.encode("utf-8"))
        for item in close:
            print("within four minutes of midnight:", *item)
    elif TARGET.read_bytes() != text.encode("utf-8"):
        raise SystemExit("lcd_almanac_data.inc differs: run --write")
    print(f"PASS almanac {FIRST}..{LAST}, {TARGET.stat().st_size} source bytes")


if __name__ == "__main__":
    main()
