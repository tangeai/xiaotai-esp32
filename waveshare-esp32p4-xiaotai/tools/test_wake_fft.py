#!/usr/bin/env python3
"""Host checks for the real wake frontend and installed ESP-DSP C kernel.

Run after dependency resolution. Hardware preemption and timing require the
separate on-device soak; this test cannot prove interrupt-context behavior.
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SHARED = ROOT.parent / 'lckfb-szpi-esp32s3-tirtc/components/starter_voice/vendor'
DSP = ROOT / 'managed_components/espressif__esp-dsp/modules/fft/float/dsps_fft2r_fc32_ansi.c'


def function(source, name):
    match = re.search(r'^esp_err_t ' + name + r'\([^;]+?\)\s*\{', source, re.M)
    assert match, name
    depth, end = 1, match.end()
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


HEADER = r'''
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_DSP_INVALID_LENGTH 1
#define ESP_ERR_DSP_UNINITIALIZED 2
int dsps_fft2r_init_fc32(float *, int);
int selected_fft(float *, int, int);
int dsps_bit_rev2r_fc32(float *, int);
#define dsps_fft2r_fc32_ansi(d,n) selected_fft(d,n,1)
#define dsps_fft2r_fc32(d,n) selected_fft(d,n,0)
'''
BODY = r'''
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_dsp.h"
#include "mel_extractor.h"
static int dsps_fft2r_initialized = 1;
static int dsp_is_power_of_two(int n) { return n > 0 && !(n & (n-1)); }
static float twiddle[MEL_NFFT];
static int ansi_calls, optimized_calls, fail_fft, fail_reverse;
'''
TEST = r'''
int dsps_fft2r_init_fc32(float *unused, int n) {
    (void)unused;
    assert(n == MEL_NFFT);
    assert(dsps_gen_w_r2_fc32(twiddle, n) == 0);
    return dsps_bit_rev_fc32_ansi(twiddle, n/2);
}
int selected_fft(float *data, int n, int ansi) {
    if (ansi) ++ansi_calls; else ++optimized_calls;
    if (fail_fft) return 3;
    return dsps_fft2r_fc32_ansi_(data, n, twiddle);
}
int dsps_bit_rev2r_fc32(float *data, int n) {
    return fail_reverse ? 4 : dsps_bit_rev_fc32_ansi(data, n);
}
int main(void) {
    static int16_t pcm[MEL_TIME * MEL_HOP_LEN + MEL_NFFT];
    static float mel[MEL_TIME][MEL_N_MELS];
    static float input[2*MEL_NFFT], actual[2*MEL_NFFT];
    mel_extractor_init();
    for (int pattern=0; pattern<4; ++pattern) {
        uint32_t random = 129;
        for (int i=0; i<MEL_NFFT; ++i) {
            random = random * 1664525u + 1013904223u;
            input[2*i] = pattern == 0 ? (i==0) : pattern == 1 ? 1.0f :
                pattern == 2 ? sinf(2*M_PI*17*i/MEL_NFFT) :
                (float)((int32_t)(random >> 16)-32768)/32768;
        }
        memcpy(actual, input, sizeof(actual));
        assert(dsps_fft2r_fc32_ansi_(actual, MEL_NFFT, twiddle) == 0);
        assert(dsps_bit_rev_fc32_ansi(actual, MEL_NFFT) == 0);
        for (int k=0; k<MEL_NFFT; ++k) {
            double re=0, im=0;
            for (int i=0; i<MEL_NFFT; ++i) {
                double phase=2*M_PI*k*i/MEL_NFFT;
                re += input[2*i]*cos(phase);
                im -= input[2*i]*sin(phase);
            }
            assert(fabs(actual[2*k]-re) < .003);
            assert(fabs(actual[2*k+1]-im) < .003);
        }
    }
    assert(mel_extract(pcm, mel) == 0);
    for (int t=0; t<MEL_TIME; ++t)
        for (int m=0; m<MEL_N_MELS; ++m) assert(fabsf(mel[t][m]+8) < .0001f);
    for (unsigned i=0; i<sizeof(pcm)/sizeof(pcm[0]); ++i)
        pcm[i] = (int16_t)(12000*sinf(2*M_PI*997*i/16000));
    assert(mel_extract(pcm, mel) == 0);
    for (int t=0; t<MEL_TIME; ++t)
        for (int m=0; m<MEL_N_MELS; ++m) assert(isfinite(mel[t][m]));
    assert(mel_extract(NULL, mel) != 0);
    assert(mel_extract(pcm, NULL) != 0);
    fail_fft=1;
    assert(mel_extract(pcm, mel) != 0);
    fail_fft=0; fail_reverse=1;
    assert(mel_extract(pcm, mel) != 0);
#if CONFIG_IDF_TARGET_ESP32P4
    assert(ansi_calls == 2*MEL_TIME+2 && optimized_calls == 0);
#else
    assert(optimized_calls == 2*MEL_TIME+2 && ansi_calls == 0);
#endif
    puts("PASS: DFT reference, finite Mel, target selection, error propagation");
}
'''


def main():
    source = DSP.read_text(encoding='utf-8')
    kernels = '\n'.join(function(source, n) for n in (
        'dsps_fft2r_fc32_ansi_', 'dsps_bit_rev_fc32_ansi', 'dsps_gen_w_r2_fc32'))
    with tempfile.TemporaryDirectory(prefix='p4-wake-fft-') as directory:
        temp = Path(directory)
        (temp/'esp_dsp.h').write_text(HEADER)
        (temp/'esp_log.h').write_text('#define ESP_LOGI(tag,...) ((void)(tag))\n#define ESP_LOGE(tag,...) ((void)(tag))\n')
        (temp/'test.c').write_text(BODY + kernels + TEST)
        for p4 in (0, 1):
            subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', f'-DCONFIG_IDF_TARGET_ESP32P4={p4}',
                            '-I', str(temp), '-I', str(SHARED), str(temp/'test.c'),
                            str(SHARED/'mel_extractor.c'), '-lm', '-o', str(temp/'test')], check=True)
            subprocess.run([str(temp/'test')], check=True, timeout=20)


if __name__ == '__main__':
    main()
