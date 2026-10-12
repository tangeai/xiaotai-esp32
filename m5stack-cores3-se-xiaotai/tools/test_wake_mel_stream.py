#!/usr/bin/env python3
"""Compare production streaming Mel with full extraction using ESP-DSP C FFT.

Host numerical/window evidence only; this does not emulate S3 timing or prove
the custom model's acoustic acceptance.
"""
from pathlib import Path
import subprocess
import tempfile
from test_voice_reliability import function

ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT / "components/starter_voice/vendor"
DSP = "managed_components/espressif__esp-dsp/modules/fft/float/dsps_fft2r_fc32_ansi.c"
kernel = r'''
#include <math.h>
#include <stdint.h>
#include "esp_dsp.h"
#define M_PI 3.14159265358979323846
static float weights[512];
static int dsps_fft2r_initialized;
static int fft_calls, fail_next;
static int dsp_is_power_of_two(int n) { return n>0 && !(n&(n-1)); }
'''
kernel += "\n".join(function(DSP, name) for name in
                    ("dsps_fft2r_fc32_ansi_", "dsps_bit_rev_fc32_ansi", "dsps_gen_w_r2_fc32"))
kernel += r'''
int dsps_fft2r_init_fc32(float *unused,int n) {
    (void)unused;
    if(n!=512) return -1;
    dsps_gen_w_r2_fc32(weights,n);
    dsps_bit_rev_fc32_ansi(weights,n/2);
    dsps_fft2r_initialized=1;
    return 0;
}
int dsps_fft2r_fc32(float *p,int n) {
    ++fft_calls;
    if(fail_next) { fail_next=0;return -1; }
    return dsps_fft2r_fc32_ansi_(p,n,weights);
}
int dsps_bit_rev2r_fc32(float *p,int n) { return dsps_bit_rev_fc32_ansi(p,n); }
'''
kernel += r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mel_extractor.h"
#include "wake_window.h"
static mel_stream_t stream;
static float full[98][32], cached[98][32];
static int16_t audio[160000], pcm[WAKE_WINDOW_SAMPLES], ring[WAKE_RING_SAMPLES];
static unsigned comparisons, reused_total;
static void check(uint32_t epoch,unsigned end,int expected_reused) {
    memcpy(pcm,audio+end-WAKE_WINDOW_SAMPLES,sizeof(pcm));
    unsigned reused=999;
    fft_calls=0;
    assert(mel_stream_extract(&stream,pcm,epoch,end,cached,&reused)==0);
    assert(reused==(unsigned)expected_reused && fft_calls==98-expected_reused);
    assert(mel_extract(pcm,full)==0);
    assert(memcmp(full,cached,sizeof(full))==0);
    reused_total+=reused; ++comparisons;
}
int main(void) {
    uint32_t random=12;
    for(unsigned i=0;i<160000;++i) {
        random=random*1664525U+1013904223U;
        audio[i]=(int16_t)((int32_t)(random>>16)-32768);
        if(i<33000) audio[i]/=300; /* peak later expires from the clip window */
        if(i>72000 && i<98000) audio[i]=0;
    }
    unsigned end=WAKE_WINDOW_SAMPLES;
    check(1,end,0);
    for(unsigned n=0;n<42;++n) { end+=2560;check(1,end,82); }
    end+=160; check(2,end,0); /* new continuity epoch */
    check(2,end,0); /* duplicate sample position must not reuse */
    end+=512; check(2,end,0); /* unaligned caller falls back */
    end+=320; check(2,end,96);
    end-=160; check(2,end,0); /* reversed position */
    end+=98*160; check(2,end,0); /* no remaining overlap */
    fail_next=1;
    assert(mel_stream_extract(&stream,pcm,2,end+160,cached,NULL)==-1);
    assert(!stream.valid); /* no partially shifted cache after FFT failure */
    check(2,end+160,0);
    assert(mel_stream_extract(NULL,pcm,2,end,cached,NULL)==-1);
    assert(mel_stream_extract(&stream,NULL,2,end,cached,NULL)==-1);
    assert(mel_stream_extract(&stream,pcm,2,end,NULL,NULL)==-1);

    wake_window_t window={.pcm=ring};
    unsigned snapshots=0;
    for(unsigned total=0;total+512<120000;total+=512) {
        /* Variable drains emulate slower inference without losing samples. */
        wake_window_append(&window,audio+total,512,1000+(total+512)/16);
        if((total/512)%7!=0 || !wake_window_snapshot(&window,pcm)) continue;
        assert(window.snapshot_end<=window.total_samples);
        assert(window.total_samples-window.snapshot_end<160);
        assert((window.snapshot_end-WAKE_WINDOW_SAMPLES)%160==0);
        assert(memcmp(pcm,audio+window.snapshot_end-WAKE_WINDOW_SAMPLES,sizeof(pcm))==0);
        unsigned reuse=999;
        assert(mel_stream_extract(&stream,pcm,window.epoch,window.snapshot_end,cached,&reuse)==0);
        assert(mel_extract(pcm,full)==0 && !memcmp(full,cached,sizeof(full)));
        /* 7*512 samples alternate between 22/23 FFT hops after alignment. */
        if(snapshots) assert(reuse==75 || reuse==76);
        ++snapshots;
    }
    assert(snapshots>20 && comparisons>45 && reused_total>3000);
    wake_window_reset(&window);
    assert(!window.snapshot_end && !window.total_samples && !wake_window_snapshot(&window,pcm));
    printf("PASS: %u bit-identical Mel windows; %u cached frames; %u DMA/ring snapshots\n",
           comparisons,reused_total,snapshots);
}
'''
with tempfile.TemporaryDirectory(prefix="cores3-mel-stream-") as directory:
    target = Path(directory)
    (target / "esp_log.h").write_text("#define ESP_LOGI(tag,...) ((void)(tag))\n#define ESP_LOGE(tag,...) ((void)(tag))\n")
    (target / "esp_dsp.h").write_text("""
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_DSP_INVALID_LENGTH -2
#define ESP_ERR_DSP_UNINITIALIZED -3
int dsps_fft2r_init_fc32(float *,int);
int dsps_fft2r_fc32(float *,int);
int dsps_bit_rev2r_fc32(float *,int);
""")
    (target / "test.c").write_text(kernel)
    subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-I", str(target), "-I", str(VENDOR),
                    "-I", str(ROOT / "components/starter_voice/src"),
                    str(target / "test.c"), str(VENDOR / "mel_extractor.c"),
                    "-lm", "-o", str(target / "test")], check=True)
    subprocess.run([str(target / "test")], check=True, timeout=60)
print("PASS: cache invalidation, changing top_db, FFT errors, and bounded aligned windows")
