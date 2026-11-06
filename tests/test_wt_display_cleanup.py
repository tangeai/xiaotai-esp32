"""Exercise actual WT display start/rollback and JD9165 deletion with fake IO."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
board = (ROOT / 'main/boards/wt99p461-s1/drivers/wt99p461_s1/wt99p461_s1.c').read_text()
driver = (ROOT / 'main/boards/wt99p461-s1/drivers/esp_lcd_jd9165/esp_lcd_jd9165.c').read_text()
start = board.index('static esp_err_t display_cleanup(')
end = board.index('\nlv_indev_t *bsp_display_get_input_dev', start)
delete_start = driver.index('static esp_err_t panel_jd9165_del(esp_lcd_panel_t *panel)\n{')
delete_end = driver.index('\nstatic esp_err_t panel_jd9165_init(', delete_start)
with tempfile.TemporaryDirectory() as directory:
    temp = Path(directory)
    unit = temp / 'wt_cleanup.c'
    unit.write_text('#include "host.h"\n' + driver[delete_start:delete_end] + '\n' +
                    board[start:end] + '\n#include "cases.h"\n')
    for name, flags in [('normal', []), ('asan', ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie'])]:
        binary = temp / name
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                        '-I', str(ROOT / 'tests/wt_cleanup'), str(unit), '-o', str(binary), *flags], check=True)
        subprocess.run([str(binary)], check=True)
