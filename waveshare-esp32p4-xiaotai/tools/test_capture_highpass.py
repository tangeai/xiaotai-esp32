"""P4 high-pass arithmetic/continuity contracts; not an acoustic-quality test."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
src = root / 'components/starter_media/src'
aec = (src / 'starter_aec.c').read_text(encoding='utf-8')
media = (src / 'starter_media.c').read_text(encoding='utf-8')
process = aec[aec.index('esp_err_t starter_aec_process_capture('):]
assert process.index('aec_process(') < process.index('p4_capture_highpass_process(')
assert process.index('p4_capture_highpass_process(') < process.index('starter_agc_process(')
assert 's_aec.clean, s_aec.frame_samples, P4_CAPTURE_HIGHPASS_ENABLED != 0' in process
assert 'p4_capture_highpass_reset(' not in process
assert 'EXT_RAM_BSS_ATTR p4_capture_highpass_t s_highpass' in aec
assert '.level = {mic_level, ref_level, clean_level}' in process
assert '.post_level = {highpass_level, gain_level}' in process
assert 'EXT_RAM_BSS_ATTR capture_post_diagnostics_t s_capture_post_acc, s_capture_post_diag' in media
capture = media[media.index('static void audio_capture_task('):media.index('static bool buffer_audio_item(')]
assert '"CP ' not in capture  # Printing must never block the acquisition owner.

code = r'''
#include "p4_capture_highpass.h"
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

static int16_t input[16000], whole[16000], chunks[16000];
static uint32_t seed = 7;
static int16_t next_sample(void) {
    seed = seed * 1664525U + 1013904223U;
    return (int16_t)((int32_t)(seed >> 16) - 32768);
}
static void check_reference_and_chunks(void) {
    for (unsigned i = 0; i < 16000; ++i) input[i] = next_sample();
    memcpy(whole, input, sizeof(input));
    memcpy(chunks, input, sizeof(input));
    p4_capture_highpass_t a = {0}, b = {0};
    size_t clipped = p4_capture_highpass_process(&a, whole, 16000, true);
    const size_t sizes[] = {1, 512, 160, 320, 17};
    size_t chunk_clipped = 0;
    for (size_t pos = 0, n = 0; pos < 16000; ++n) {
        size_t count = sizes[n % 5];
        if (count > 16000 - pos) count = 16000 - pos;
        chunk_clipped += p4_capture_highpass_process(&b, chunks + pos, count, true);
        pos += count;
    }
    assert(clipped == chunk_clipped);
    assert(memcmp(whole, chunks, sizeof(whole)) == 0);
    assert(a.previous_input == b.previous_input && a.previous_output_q15 == b.previous_output_q15);
    double y = 0, prev = input[0], alpha = 31506.0 / 32768.0;
    for (unsigned i = 0; i < 16000; ++i) {
        y = alpha * (y + input[i] - prev);
        prev = input[i];
        double limited = fmin(32767.0, fmax(-32768.0, y));
        assert(fabs(whole[i] - limited) < 2.0);
    }
}
static void check_dc_bypass_and_saturation(void) {
    p4_capture_highpass_t f = {0};
    for (unsigned i = 0; i < 16000; ++i) whole[i] = 12345;
    assert(p4_capture_highpass_process(&f, whole, 16000, true) == 0);
    for (unsigned i = 0; i < 16000; ++i) assert(whole[i] == 0);
    memcpy(whole, input, sizeof(input));
    assert(p4_capture_highpass_process(&f, whole, 16000, false) == 0);
    assert(memcmp(whole, input, sizeof(input)) == 0 && !f.initialized);
    int16_t step[] = {INT16_MIN, INT16_MAX};
    assert(p4_capture_highpass_process(&f, step, 2, true) == 1);
    assert(step[0] == 0 && step[1] == INT16_MAX);
    /* Full scale switching stresses the 64-bit multiply before Q15 feedback. */
    for (unsigned n = 0; n < 100; ++n) {
        for (unsigned i = 0; i < 16000; ++i) whole[i] = i & 1 ? INT16_MIN : INT16_MAX;
        p4_capture_highpass_process(&f, whole, 16000, true);
    }
    p4_capture_highpass_reset(&f);
    int16_t start = 20000;
    p4_capture_highpass_process(&f, &start, 1, true);
    assert(start == 0);
}
static double response(double hz) {
    p4_capture_highpass_t f = {0};
    for (unsigned i = 0; i < 16000; ++i)
        whole[i] = input[i] = (int16_t)(8000 * sin(6.283185307179586 * hz * i / 16000));
    p4_capture_highpass_process(&f, whole, 16000, true);
    double in_energy = 0, out_energy = 0;
    for (unsigned i = 1600; i < 16000; ++i) {
        in_energy += (double)input[i] * input[i];
        out_energy += (double)whole[i] * whole[i];
    }
    return sqrt(out_energy / in_energy);
}
int main(void) {
    check_reference_and_chunks();
    check_dc_bypass_and_saturation();
    double low = response(10), knee = response(100), speech = response(1000);
    assert(low > 0.05 && low < 0.2);
    assert(knee > 0.6 && knee < 0.8);
    assert(speech > 0.9 && speech < 1.01);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='p4-highpass-') as tmp:
    p = Path(tmp)
    (p / 'test.c').write_text(code, encoding='utf-8')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I' + str(src),
                    str(p / 'test.c'), str(src / 'p4_capture_highpass.c'), '-lm',
                    '-o', str(p / 'test')], check=True)
    subprocess.run([str(p / 'test')], check=True)
print('PASS: P4 HPF continuity, DC rejection, bypass, bounded arithmetic and stage ownership')
