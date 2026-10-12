"""Development gestures exercise LVGL's input path and the dark-screen wake rule."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "components/starter_product/src/starter_product.c").read_text()
start = source.index("EXT_RAM_BSS_ATTR static struct {\n    lv_point_t from, to;")
end = source.index("esp_err_t starter_product_debug_state", start)
gesture = source[start:end]
start = source.index("/* Per pointer device, LVGL task only.")
wake = source[start:source.index("static esp_err_t preferences_write(", start)]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define EXT_RAM_BSS_ATTR
#define LCD_H_RES 320
#define LCD_V_RES 240
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_TIMEOUT 3
#define LV_INDEV_STATE_RELEASED 0
#define LV_INDEV_STATE_PRESSED 1
typedef int esp_err_t;
typedef struct { int16_t x, y; } lv_point_t;
typedef struct { unsigned unused; } lv_indev_drv_t;
typedef struct { lv_point_t point; int state; } lv_indev_data_t;
static bool s_started = true, lock_ok = true, locked;
static int64_t clock_us;
static unsigned unlocks;
static int64_t esp_timer_get_time(void) { return clock_us; }
static bool lvgl_port_lock(unsigned timeout) {
    assert(timeout == 100 && !locked);
    return locked = lock_ok;
}
static void lvgl_port_unlock(void) { assert(locked); locked = false; ++unlocks; }
#define ESP_LOGI(...) ((void)0)
static bool s_display_awake = true, wake_succeeds = true;
static unsigned wakes;
static void note_interaction(void) { ++wakes; if (wake_succeeds) s_display_awake = true; }
'''
test = r'''
int main(void) {
    lv_indev_data_t data;
    debug_touch_read(NULL, &data);
    assert(data.state == LV_INDEV_STATE_RELEASED);
    assert(starter_product_debug_tap(320, 0) == ESP_ERR_INVALID_ARG);
    assert(starter_product_debug_tap(0, 240) == ESP_ERR_INVALID_ARG);
    assert(starter_product_debug_swipe(0,0,320,0,100) == ESP_ERR_INVALID_ARG);
    assert(starter_product_debug_swipe(0,0,0,240,100) == ESP_ERR_INVALID_ARG);
    assert(starter_product_debug_swipe(0,0,1,1,99) == ESP_ERR_INVALID_ARG);
    assert(starter_product_debug_swipe(0,0,1,1,2001) == ESP_ERR_INVALID_ARG);
    assert(!unlocks && !s_debug_touch.until);
    s_started = false;
    assert(starter_product_debug_tap(1,1) == ESP_ERR_INVALID_STATE);
    s_started = true; lock_ok = false;
    assert(starter_product_debug_tap(1,1) == ESP_ERR_TIMEOUT);
    assert(!unlocks && !s_debug_touch.until); lock_ok = true;
    clock_us = INT64_C(10000000000);
    assert(starter_product_debug_tap(319,239) == ESP_OK);
    for (unsigned i=0; i<120; ++i) {
        debug_touch_read(NULL, &data);
        assert(data.state == LV_INDEV_STATE_PRESSED);
        assert(data.point.x == 319 && data.point.y == 239);
        ++clock_us;
    }
    assert(starter_product_debug_tap(2,2) == ESP_ERR_INVALID_STATE);
    assert(s_debug_touch.to.x == 319 && !locked);
    clock_us = s_debug_touch.until;
    debug_touch_read(NULL, &data);
    assert(data.state == LV_INDEV_STATE_RELEASED);
    unsigned samples = 0;
    for (unsigned direction=0; direction<4; ++direction) {
        int x0=direction&1?319:0, y0=direction&2?239:0;
        int x1=319-x0, y1=239-y0;
        assert(starter_product_debug_swipe(x0,y0,x1,y1,2000) == ESP_OK);
        int previous_x=x0, previous_y=y0;
        int64_t begin=s_debug_touch.started;
        for (unsigned i=0; i<=2000; ++i) {
            clock_us=begin+(int64_t)i*1000;
            debug_touch_read(NULL, &data);
            assert(data.point.x >= 0 && data.point.x <= 319);
            assert(data.point.y >= 0 && data.point.y <= 239);
            assert(x1>x0?data.point.x>=previous_x:data.point.x<=previous_x);
            assert(y1>y0?data.point.y>=previous_y:data.point.y<=previous_y);
            assert(data.state == (i<2000?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED));
            previous_x=data.point.x; previous_y=data.point.y; ++samples;
        }
        assert(data.point.x==x1 && data.point.y==y1);
        clock_us+=INT64_C(4000000000);
        debug_touch_read(NULL,&data);
        assert(data.state==LV_INDEV_STATE_RELEASED && data.point.x==x1 && data.point.y==y1);
    }
    assert(samples==8004 && !locked && !wakes);
    /* Dark screen: the whole injected contact only wakes, like a finger. */
    s_display_awake = false;
    assert(starter_product_debug_tap(10,20) == ESP_OK);
    for (int64_t t = s_debug_touch.started; t < s_debug_touch.until; t += 1000) {
        clock_us = t; debug_touch_read(NULL, &data);
        assert(data.state == LV_INDEV_STATE_RELEASED && wakes == 1 && s_display_awake);
    }
    clock_us = s_debug_touch.until; debug_touch_read(NULL, &data);
    assert(data.state == LV_INDEV_STATE_RELEASED && wakes == 1);
    /* Awake again: the next AT+TAP is delivered for its full duration. */
    assert(starter_product_debug_tap(10,20) == ESP_OK);
    for (int64_t t = s_debug_touch.started; t < s_debug_touch.until; t += 1000) {
        clock_us = t; debug_touch_read(NULL, &data);
        assert(data.state == LV_INDEV_STATE_PRESSED && data.point.x == 10 && data.point.y == 20);
    }
    clock_us = s_debug_touch.until; debug_touch_read(NULL, &data);
    assert(data.state == LV_INDEV_STATE_RELEASED && wakes == 1);
    /* A swipe that starts dark is consumed end to end, with one wake. */
    s_display_awake = false;
    assert(starter_product_debug_swipe(0,0,319,239,500) == ESP_OK);
    for (int64_t t = s_debug_touch.started; t <= s_debug_touch.until; t += 1000) {
        clock_us = t; debug_touch_read(NULL, &data);
        assert(data.state == LV_INDEV_STATE_RELEASED);
    }
    assert(wakes == 2 && s_display_awake);
    /* A contact already delivered stays delivered if the screen sleeps. */
    assert(starter_product_debug_swipe(5,5,6,6,500) == ESP_OK);
    clock_us = s_debug_touch.started; debug_touch_read(NULL, &data);
    assert(data.state == LV_INDEV_STATE_PRESSED);
    s_display_awake = false;
    for (int64_t t = s_debug_touch.started; t < s_debug_touch.until; t += 1000) {
        clock_us = t; debug_touch_read(NULL, &data);
        assert(data.state == LV_INDEV_STATE_PRESSED && wakes == 2);
    }
    clock_us = s_debug_touch.until; debug_touch_read(NULL, &data);
    assert(data.state == LV_INDEV_STATE_RELEASED);
    /* A failed backlight write keeps every new contact consumed. */
    wake_succeeds = false;
    for (unsigned round = 0; round < 3; ++round) {
        assert(starter_product_debug_tap(7,7) == ESP_OK);
        for (int64_t t = s_debug_touch.started; t <= s_debug_touch.until; t += 1000) {
            clock_us = t; debug_touch_read(NULL, &data);
            assert(data.state == LV_INDEV_STATE_RELEASED);
        }
        assert(wakes == 3 + round && !s_display_awake);
    }
    assert(!locked);
    puts("PASS: LVGL gesture bounds, tap timing, 8004 monotonic samples, long uptime, overlap, lock failures, dark-screen wake-only injection");
}
'''
with tempfile.TemporaryDirectory(prefix="cores3-gesture-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(prefix + wake + gesture + test)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
