"""Fault-inject actual S3 board ownership code, with hardware calls stubbed."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'main/boards/lckfb/szpi-esp32s3/xiaotai_hardware.c'
HEADERS = ['driver/gpio.h', 'driver/spi_master.h', 'driver/i2s_std.h', 'driver/i2s_tdm.h',
           'driver/i2c_master.h', 'esp_check.h', 'esp_codec_dev_defaults.h', 'esp_codec_dev.h',
           'es7210_adc.h', 'es8311_codec.h', 'esp_lcd_io_i2c.h', 'esp_lcd_panel_vendor.h',
           'esp_lcd_panel_ops.h', 'esp_lcd_touch_ft5x06.h', 'esp_log.h', 'freertos/semphr.h']
with tempfile.TemporaryDirectory() as temporary:
    temp = Path(temporary)
    for header in HEADERS:
        path = temp / header
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "board_host.h"\n')
    for mode in ('normal', 'sanitized'):
        flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if mode == 'sanitized' else []
        binary = temp / ('rollback-' + mode)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror', '-UNDEBUG',
                        *flags, '-I', str(temp), '-I', str(ROOT / 'tests/board_rollback'),
                        '-I', str(ROOT / 'main/boards/common'), '-DBOARD_SOURCE="' + str(SOURCE) + '"',
                        str(ROOT / 'tests/board_rollback/test_rollback.c'), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
