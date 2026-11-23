# SPDX-License-Identifier: MIT
# Replaces legacy: tests/test_wt_display_cleanup.py
"""Exercise the native vendor wrapper separately from the panel owner's latch."""
from pathlib import Path
import subprocess
import sys
import tempfile

PREFIX = r'''
#include <assert.h>
typedef int esp_err_t;
enum { ESP_OK, ESP_FAIL };
typedef struct { void *user_data; } esp_lcd_panel_t;
typedef struct { int reset_gpio_num; esp_err_t (*del)(esp_lcd_panel_t *); } jd9165_panel_t;
static unsigned frees, deletes;
static jd9165_panel_t wrapper;
static esp_err_t del_dpi(esp_lcd_panel_t *panel)
{
    assert(panel->user_data == &wrapper);
    return ++deletes == 1 ? ESP_FAIL : ESP_OK;
}
static void free(void *ptr) { assert(ptr == &wrapper); ++frees; }
static void gpio_reset_pin(int pin) { assert(pin < 0); }
#define ESP_LOGD(...) ((void)0)
#define ESP_RETURN_ON_ERROR(call, ...) do { const esp_err_t err = (call); if (err) return err; } while (0)
'''

CASE = r'''
int main(void)
{
    wrapper = (jd9165_panel_t){.reset_gpio_num = -1, .del = del_dpi};
    esp_lcd_panel_t panel = {.user_data = &wrapper};
    assert(panel_jd9165_del(&panel) == ESP_FAIL && deletes == 1);
    assert(frees == 0 && panel.user_data == &wrapper);
    assert(panel_jd9165_del(&panel) == ESP_OK && deletes == 2 && frees == 1);
    return 0;
}
'''

source = Path(__file__).resolve().parents[1] / "esp-idf/esp_lcd_jd9165.c"
body = source.read_text(encoding="utf-8")
start = body.index("static esp_err_t panel_jd9165_del(esp_lcd_panel_t *panel)\n{")
end = body.index("\n}\n", start) + 2
with tempfile.TemporaryDirectory() as directory:
    unit = Path(directory) / "vendor_delete.c"
    unit.write_text(PREFIX + body[start:end] + "\n" + CASE, encoding="utf-8")
    binary = unit.with_suffix("")
    subprocess.run([sys.argv[1], "-std=c11", "-Wall", "-Wextra", "-Werror", "-fno-builtin-free",
                    "-fsanitize=address,undefined", "-no-pie", unit, "-o", binary], check=True)
    subprocess.run([binary], check=True)
