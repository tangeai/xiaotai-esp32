"""Run actual P4 endpoint and WT camera layout helpers on the host."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
endpoint = ROOT / 'main/xiaotai/transport/p4/starter_tirtc/src'
cases = {
    'p4-endpoint': [ROOT / 'tests/native/p4/test_tirtc_endpoint.c', endpoint / 'tirtc_endpoint.c'],
    'wt99-camera-layout': [ROOT / 'tests/native/wt99/test_camera_frame_layout.c'],
}
with tempfile.TemporaryDirectory() as temp:
    board = (ROOT / 'main/boards/waveshare/esp32-p4-wifi6-touch-lcd-3.5/drivers/esp32_p4_wifi6_touch_lcd_35/esp32_p4_wifi6_touch_lcd_35.c').read_text()
    start = board.index('esp_err_t bsp_display_new(')
    end = board.index('\nesp_err_t bsp_display_new_with_handles(', start)
    wrapper = Path(temp) / 'display_output.c'
    wrapper.write_text('''
#include <assert.h>
#include <stddef.h>
typedef int esp_err_t, bsp_display_config_t;
typedef void *esp_lcd_panel_handle_t, *esp_lcd_panel_io_handle_t;
typedef struct { void *panel, *io; } bsp_lcd_handles_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 2
#define ESP_RETURN_ON_FALSE(ok, value, ...) do { if (!(ok)) return value; } while (0)
static int failure, called, marker;
static int bsp_display_new_with_handles(const bsp_display_config_t *cfg, bsp_lcd_handles_t *out) {
    (void)cfg; ++called;
    out->panel = &marker;
    if (failure) return 1;
    out->io = &marker; return 0;
}
''' + board[start:end] + '''
int main(void) {
    void *panel = &marker, *io = &marker;
    assert(bsp_display_new(NULL, NULL, &io) == ESP_ERR_INVALID_ARG && !called);
    failure = 1;
    assert(bsp_display_new(NULL, &panel, &io) == 1 && !panel && !io);
    failure = 0;
    assert(bsp_display_new(NULL, &panel, &io) == 0 && panel == &marker && io == &marker);
}
''')
    cases['p4-display-output'] = [wrapper]
    for name, sources in cases.items():
        for mode in ('normal', 'sanitized'):
            binary = Path(temp) / (name + '-' + mode)
            flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if mode == 'sanitized' else []
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-UNDEBUG', *flags, '-I', str(endpoint), *map(str, sources), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
print('P4/WT helpers: normal and ASan/UBSan passed')
