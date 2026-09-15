"""Exercise the actual P4 PCM/controller; separate IDF test checks DMA pacing."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
src = root / 'components/starter_media/src'
code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "p4_audio_playout.h"
enum { N=P4_PLAYOUT_CHUNK, MS=N*1000/P4_PLAYOUT_RATE };
static p4_audio_playout_t q;
static int16_t input[P4_PLAYOUT_CAPACITY], out[N];
static p4_playout_block_t b;
static void assert_fade(const int16_t *pcm, size_t n) {
    for(size_t i=0;i<n;i++) {
        int expected=pcm[i];
        if(i<P4_PLAYOUT_FADE_SAMPLES)
            expected=expected*(int)(i+1)/(int)P4_PLAYOUT_FADE_SAMPLES;
        assert(out[i]==expected);
    }
}
static void stream_simulation(audio_playout_profile_t profile, bool jitter) {
    enum { PACKETS=1500, SAMPLES=320 };
    static uint32_t arrival[PACKETS];
    uint32_t random=12345;
    for(unsigned i=0;i<PACKETS;i++) {
        random=random*1664525U+1013904223U;
        uint32_t at=i*40U+80U+(jitter ? random%81U : 0U);
        if(jitter && at>=20000 && at<20350) at=20350;
        if(i && at<arrival[i-1]) at=arrival[i-1];
        arrival[i]=at;
    }
    p4_audio_playout_init(&q,profile);
    unsigned next=0, completed=0, consumed=0, emitted=0, target_max=0, dma_until=0;
    for(unsigned now=0;now<65000;now++) {
        while(next<PACKETS && arrival[next]<=now) {
            assert(p4_audio_playout_push(&q,input,SAMPLES,next*40U,arrival[next]));
            next++;
        }
        /* Bounded clock model; test_i2s_playback_cadence executes IDF write. */
        if((int)(dma_until-now)<=60 && p4_audio_playout_prepare(&q,now,out,&b)) {
            consumed+=b.consumed; emitted+=b.samples;
            completed+=p4_audio_playout_commit(&q,&b,now,true);
            if(dma_until<now) dma_until=now;
            dma_until+=(b.samples*1000+P4_PLAYOUT_RATE-1)/P4_PLAYOUT_RATE;
        }
        if(q.controller.target_delay_ms>target_max) target_max=q.controller.target_delay_ms;
        if(now && now%1000==0) p4_audio_playout_update_window(&q);
    }
    assert(next==PACKETS && completed==PACKETS && consumed==PACKETS*SAMPLES);
    assert(q.count==0 && q.overflow_samples==0);
    if(!jitter) assert(q.empty_events==1); /* final natural end only */
    printf("profile=%d jitter=%d in=%u consumed=%u out=%u empty=%u slow=%u fast=%u target_max=%u\n",
           profile,jitter,PACKETS*SAMPLES,consumed,emitted,q.empty_events,
           q.slow_blocks,q.fast_blocks,target_max);
}
int main(void) {
    for(unsigned i=0;i<P4_PLAYOUT_CAPACITY;i++) input[i]=(int16_t)i;
    p4_audio_playout_init(&q,AUDIO_PLAYOUT_PROFILE_ADAPTIVE_CALL);
    assert(p4_audio_playout_push(&q,input,100,0,0));
    assert(!p4_audio_playout_prepare(&q,0,out,&b));
    assert(p4_audio_playout_prepare(&q,120,out,&b) && b.samples==100);
    assert_fade(input,100);
    assert(p4_audio_playout_commit(&q,&b,120,true)==1 && q.count==0);

    /* Two short packets, non-consuming retries, fade and packet-end bitmap. */
    p4_audio_playout_init(&q,AUDIO_PLAYOUT_PROFILE_LOW_LATENCY);
    assert(p4_audio_playout_push(&q,input,37,1,1));
    assert(p4_audio_playout_push(&q,input+37,N-37,6,6));
    assert(p4_audio_playout_prepare(&q,126,out,&b));
    assert_fade(input,N);
    p4_playout_block_t again;
    assert(p4_audio_playout_prepare(&q,126,out,&again));
    assert(q.count==N && again.consumed==b.consumed && again.restart && b.restart);
    assert(q.fade_remaining==P4_PLAYOUT_FADE_SAMPLES);
    assert_fade(input,N);
    assert(p4_audio_playout_commit(&q,&b,126,true)==2 && q.count==0);
    assert(!p4_audio_playout_prepare(&q,130,out,&b) && q.empty_events==0);
    assert(!p4_audio_playout_prepare(&q,126+MS,out,&b) && q.empty_events==1);
    assert(!p4_audio_playout_prepare(&q,200,out,&b) && q.empty_events==1);

    p4_audio_playout_init(&q,AUDIO_PLAYOUT_PROFILE_LOW_LATENCY);
    for(unsigned i=0;i<10;i++) assert(p4_audio_playout_push(&q,input+i*160,160,i*20,0));
    unsigned packets=0,fast=0; int previous=-1;
    for(unsigned now=0;now<2000 && q.count;now+=MS) {
        if(!p4_audio_playout_prepare(&q,now,out,&b)) continue;
        if(b.rate_permille>0) fast++;
        for(size_t i=0;i<b.samples;i++) {
            assert(out[i]>=previous && out[i]-previous<=3); previous=out[i];
        }
        packets+=p4_audio_playout_commit(&q,&b,now,true);
    }
    assert(fast>0 && packets==10 && q.count==0);

    p4_audio_playout_init(&q,AUDIO_PLAYOUT_PROFILE_ADAPTIVE_CALL);
    assert(p4_audio_playout_push(&q,input,400,0,0)); q.started=true;
    assert(p4_audio_playout_prepare(&q,0,out,&b) && b.rate_permille==-12 && b.consumed<N);
    p4_audio_playout_commit(&q,&b,0,true); assert(q.slow_blocks==1);

    p4_audio_playout_init(&q,AUDIO_PLAYOUT_PROFILE_LOW_LATENCY);
    assert(p4_audio_playout_push(&q,input,P4_PLAYOUT_CAPACITY,0,0));
    assert(!p4_audio_playout_push(&q,input,1,0,0));
    assert(q.count==P4_PLAYOUT_CAPACITY && q.overflow_samples==1);

    /* Failed first half must not count the remainder as a successful packet. */
    p4_audio_playout_init(&q,AUDIO_PLAYOUT_PROFILE_LOW_LATENCY);
    assert(p4_audio_playout_push(&q,input,2*N,0,0));
    assert(p4_audio_playout_prepare(&q,0,out,&b));
    assert(p4_audio_playout_commit(&q,&b,0,false)==0);
    assert(p4_audio_playout_prepare(&q,MS,out,&b));
    assert(p4_audio_playout_commit(&q,&b,MS,true)==0 && q.count==0);
    assert(p4_audio_playout_push(&q,input,N,2*MS,2*MS));
    assert(p4_audio_playout_prepare(&q,2*MS,out,&b));
    assert(p4_audio_playout_commit(&q,&b,2*MS,true)==1);

    p4_audio_playout_init(&q,AUDIO_PLAYOUT_PROFILE_LOW_LATENCY); q.head=P4_PLAYOUT_CAPACITY-60;
    assert(p4_audio_playout_push(&q,input,N,UINT32_MAX-200U,UINT32_MAX-200U));
    assert(p4_audio_playout_prepare(&q,UINT32_MAX-10U,out,&b)); assert_fade(input,N);
    assert(p4_audio_playout_commit(&q,&b,UINT32_MAX-10U,true)==1 && q.count==0);
    assert(!p4_audio_playout_prepare(&q,0,out,&b) && q.empty_events==0);
    assert(!p4_audio_playout_prepare(&q,20,out,&b) && q.empty_events==1);

    p4_audio_playout_init(&q,AUDIO_PLAYOUT_PROFILE_ADAPTIVE_CALL);
    audio_playout_controller_note_underflow(&q.controller); assert(q.controller.target_delay_ms==180);
    for(unsigned i=0;i<64;i++) {
        q.window.received_ms=q.window.played_ms=1000; p4_audio_playout_update_window(&q);
    }
    assert(q.controller.target_delay_ms==140);
    q.window.local_wait_ms=2; p4_audio_playout_update_window(&q);
    assert(q.controller.condition==AUDIO_PLAYOUT_CONDITION_LOCAL_PRESSURE);

    /* Unity level during continuous speech, fade only at a real restart. */
    for(unsigned i=0;i<P4_PLAYOUT_CAPACITY;i++) input[i]=16000;
    p4_audio_playout_init(&q,AUDIO_PLAYOUT_PROFILE_LOW_LATENCY);
    assert(p4_audio_playout_push(&q,input,2*N,0,0));
    assert(p4_audio_playout_prepare(&q,0,out,&b) && b.restart); assert_fade(input,N);
    p4_audio_playout_commit(&q,&b,0,true);
    assert(p4_audio_playout_prepare(&q,MS,out,&b) && !b.restart);
    for(size_t i=0;i<b.samples;i++) assert(out[i]==16000);
    p4_audio_playout_commit(&q,&b,MS,true);
    assert(!p4_audio_playout_prepare(&q,200,out,&b));
    assert(p4_audio_playout_push(&q,input,2*N,300,300));
    assert(p4_audio_playout_prepare(&q,420,out,&b) && b.restart); assert_fade(input,N);
    p4_audio_playout_commit(&q,&b,420,false);
    assert(p4_audio_playout_prepare(&q,435,out,&b) && b.restart); assert_fade(input,b.samples);

    /* Tiny isolated speech is neither padded nor stranded by prebuffering. */
    p4_audio_playout_init(&q,AUDIO_PLAYOUT_PROFILE_LOW_LATENCY);
    assert(p4_audio_playout_push(&q,input,7,0,0));
    assert(p4_audio_playout_prepare(&q,120,out,&b) && b.samples==7 && b.consumed==7);
    assert_fade(input,7); assert(p4_audio_playout_commit(&q,&b,120,true)==1);

    p4_audio_playout_init(&q,AUDIO_PLAYOUT_PROFILE_LOW_LATENCY);
    assert(p4_audio_playout_push(&q,input,1600,0,0));
    for(unsigned i=0;i<6;i++) {
        assert(p4_audio_playout_prepare(&q,10,out,&b)); p4_audio_playout_commit(&q,&b,10,true);
    }
    assert(p4_audio_playout_pending_ms(&q,10)==90);
    assert(p4_audio_playout_prepare(&q,10,out,&b)); p4_audio_playout_commit(&q,&b,10,true);
    assert(p4_audio_playout_pending_ms(&q,10)==90 && p4_audio_playout_pending_ms(&q,100)==0);
    assert(p4_audio_playout_pending_ms(&q,UINT32_C(0x80000100))==0);
    assert(p4_audio_playout_prepare(&q,100,out,&b)); p4_audio_playout_commit(&q,&b,100,false);
    assert(!q.output_valid && p4_audio_playout_pending_ms(&q,100)==0);
    audio_playout_decision_t d;
    audio_playout_controller_decide(&q.controller,120,MS,true,&d); assert(d.rate_adjust_permille==0);
    for(unsigned profile=0;profile<3;profile++) {
        stream_simulation((audio_playout_profile_t)profile,false);
        stream_simulation((audio_playout_profile_t)profile,true);
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='p4-playout-') as tmp:
    p = Path(tmp)
    (p / 'test.c').write_text(code, encoding='utf-8')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I'+str(src), str(p/'test.c'),
                    str(src/'p4_audio_playout.c'), str(src/'audio_playout_controller.c'),
                    '-o', str(p/'test')], check=True)
    subprocess.run([str(p/'test')], check=True)
print('PASS: P4 tail, order, phase, packet counts, overflow, write failure, clock wrap and adaptation')
