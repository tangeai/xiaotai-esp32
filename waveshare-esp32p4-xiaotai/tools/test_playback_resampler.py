"""Numerical playback tests; no claim about speaker/acoustic acceptance."""
import math
import re
import subprocess
import tempfile
from playback_resampler_fixture import ROOT, compile_test

src = (ROOT / 'components/starter_media/src/p4_playback_resampler.c').read_text()
coefficients = [float(x) for x in re.findall(r'(-?[\d.]+(?:e-?\d+)?)f',
    src.split('= {', 1)[1].split('};', 1)[0])]
ideal = []
for n in range(63):
    x = math.pi * (n - 31) / 2
    ideal.append((math.sin(x) / x if x else 1) *
                 (0.42 - 0.5 * math.cos(2 * math.pi * n / 62) +
                  0.08 * math.cos(4 * math.pi * n / 62)))
ideal.append(0)
ideal = [v / sum(ideal[i % 2::2]) for i, v in enumerate(ideal)]
assert len(coefficients) == 64 and max(abs(a-b) for a,b in zip(ideal, coefficients)) < 1e-9
media = (ROOT / 'components/starter_media/src/starter_media.c').read_text()
assert 'EXT_RAM_BSS_ATTR static p4_playback_resampler_t s_playback_resampler;' in media
assert 'EXT_RAM_BSS_ATTR static p4_playback_resampler_t s_prompt_resampler;' in media
assert 'p4_playback_resampler_process(&s_prompt_resampler,' in media
assert 'p4_playback_resampler_finish(&s_prompt_resampler,' in media
assert '!s_playout.queue.started && !s_playout.queue.count' in media
assert 'play_audio_chunk(NULL, 0, false)' in media

code = r'''
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "p4_playback_resampler.h"
#define PI 3.14159265358979323846
static int16_t input[20003], whole[80200], split[80200];

static double amplitude(const int16_t *s, unsigned start, unsigned n, double f) {
    double re=0, im=0;
    for (unsigned i=0;i<n;++i) {
        double phase=2*PI*f*i/16000;
        re+=s[(start+i)*2]*cos(phase); im+=s[(start+i)*2]*sin(phase);
    }
    return 2*hypot(re,im)/n;
}

int main(void) {
    p4_playback_resampler_t a={0}, b={0};
    assert(!p4_playback_resampler_process(&a,input,10,whole,40));
    assert(p4_playback_resampler_reset(NULL,0)==ESP_ERR_INVALID_ARG);
    for (int dc=-32000;dc<=32000;dc+=1000) {
        assert(p4_playback_resampler_reset(&a,dc)==ESP_OK);
        for (int i=0;i<160;++i) input[i]=dc;
        assert(p4_playback_resampler_process(&a,input,160,whole,640)==640);
        for (int i=0;i<640;++i) assert(abs(whole[i]-dc)<=1);
        assert(!a.clipped);
    }
    for (int sign=0;sign<2;++sign) {
        int dc=sign?32767:-32768;
        assert(p4_playback_resampler_reset(&a,dc)==ESP_OK);
        for(int i=0;i<160;++i)input[i]=dc;
        assert(p4_playback_resampler_process(&a,input,160,whole,640)==640);
        for(int i=0;i<640;++i)assert(whole[i]==dc);
        assert(!a.clipped);
    }
    /* All irregular splits must match one continuous stream, including tail. */
    unsigned rng=17;
    for (int i=0;i<20003;++i) {rng=rng*1664525U+1013904223U; input[i]=(int16_t)(rng>>16);}
    assert(p4_playback_resampler_reset(&a,input[0])==ESP_OK);
    assert(p4_playback_resampler_reset(&b,input[0])==ESP_OK);
    assert(p4_playback_resampler_process(&a,input,20003,whole,80200)==80012);
    for (size_t at=0;at<20003;) {
        size_t n=(at*13+17)%173+1;
        if (n>20003-at) n=20003-at;
        assert(p4_playback_resampler_process(&b,input+at,n,split+at*4,80200-at*4)==n*4);
        at+=n;
    }
    assert(p4_playback_resampler_finish(&a,whole+80012,188)==124);
    assert(p4_playback_resampler_finish(&b,split+80012,188)==124);
    assert(!memcmp(whole,split,80136*sizeof(int16_t)) && a.clipped==b.clipped);
    assert(a.clipped>0); /* Reconstruction peak protection is exercised. */
    assert(!p4_playback_resampler_finish(&a,whole,80200));
    for (int i=0;i<80136;i+=2) assert(whole[i]==whole[i+1]);

    /* Invalid capacity does not consume history or a pending tail. */
    assert(p4_playback_resampler_reset(&a,0)==ESP_OK);
    assert(!p4_playback_resampler_process(&a,input,160,whole,639));
    assert(!a.pending_tail && a.fir.pos==0);
    memset(input,0,sizeof(input)); input[159]=12000;
    assert(p4_playback_resampler_process(&a,input,160,whole,640)==640);
    assert(!p4_playback_resampler_finish(&a,whole+640,123) && a.pending_tail);
    assert(p4_playback_resampler_finish(&a,whole+640,124)==124);
    /* The final impulse survives after the input packet ended. */
    assert(whole[(159*2+31)*2]==12000);
    assert(!a.pending_tail);
    for (int i=0;i<160;++i) input[i]=-2000;
    assert(p4_playback_resampler_reset(&a,-2000)==ESP_OK);
    assert(p4_playback_resampler_process(&a,input,160,whole,640)==640);
    for(int i=0;i<640;++i) assert(whole[i]==-2000); /* no previous conversation */

    puts("Hz old_gain_dB fir_gain_dB old_image_dBc fir_image_dBc");
    const int frequencies[]={300,1000,2000,3000,3400};
    for(unsigned t=0;t<sizeof(frequencies)/sizeof(frequencies[0]);++t) {
        int f=frequencies[t];
        for(int i=0;i<10000;++i) input[i]=(int16_t)lround(12000*sin(2*PI*f*i/8000));
        assert(p4_playback_resampler_reset(&a,0)==ESP_OK);
        assert(p4_playback_resampler_process(&a,input,10000,whole,40000)==40000);
        int16_t previous=0;
        for(int i=0;i<10000;++i) {
            split[i*4]=split[i*4+1]=((int)previous+input[i])/2;
            split[i*4+2]=split[i*4+3]=input[i]; previous=input[i];
        }
        double old=amplitude(split,1024,16000,f), actual=amplitude(whole,1024,16000,f);
        double image=amplitude(whole,1024,16000,8000-f);
        double old_image=amplitude(split,1024,16000,8000-f);
        double gain=20*log10(actual/12000), rejection=20*log10(fmax(image,1e-8)/actual);
        assert(fabs(gain)<0.04 && rejection < -50 && !a.clipped);
        printf("%d %.3f %.3f %.2f %.2f\n",f,20*log10(old/12000),gain,
               20*log10(old_image/old),rejection);
    }
    puts("PASS: vendor FIR, coefficients, unity gain, spectrum, split/order, final impulse, clipping, reset");
}
'''
with tempfile.TemporaryDirectory(prefix='p4-src-') as p:
    subprocess.run([str(compile_test(p, code))], check=True)
