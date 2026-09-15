"""Compile the actual ESP-DSP FIR kernel and P4 adapter on the host."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def compile_test(directory, source, name='test'):
    p = Path(directory)
    dsp = ROOT / 'managed_components/espressif__esp-dsp/modules'
    if not (dsp / 'fir/float/dsps_firmr_f32_ansi.c').is_file():
        raise SystemExit('ESP-DSP sources missing. Run idf.py reconfigure before this test.')
    (p / 'esp_err.h').write_text('#pragma once\ntypedef int esp_err_t;\n'
        '#define ESP_OK 0\n#define ESP_ERR_INVALID_ARG 0x102\n', encoding='ascii')
    (p / 'esp_idf_version.h').write_text('#define ESP_IDF_VERSION 0\n'
        '#define ESP_IDF_VERSION_VAL(a,b,c) 1\n', encoding='ascii')
    (p / 'sdkconfig.h').write_text('', encoding='ascii')
    (p / 'dsp_tests.h').write_text('', encoding='ascii')
    (p / f'{name}.c').write_text(source, encoding='utf-8')
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
        '-fsanitize=address,undefined,float-cast-overflow', '-I' + str(p),
        '-I' + str(ROOT / 'components/starter_media/src'),
        '-I' + str(dsp / 'fir/include'), '-I' + str(dsp / 'common/include'),
        str(p / f'{name}.c'),
        str(ROOT / 'components/starter_media/src/p4_playback_resampler.c'),
        str(dsp / 'fir/float/dsps_firmr_init_f32.c'),
        str(dsp / 'fir/float/dsps_firmr_f32_ansi.c'),
        '-lm', '-o', str(p / name)], check=True)
    return p / name
