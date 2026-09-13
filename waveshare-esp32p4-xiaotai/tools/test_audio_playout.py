"""Host checks for the actual P4 PCM/controller; no hardware or acoustic claim."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
src = root / 'components/starter_media/src'
code = r'''
#include <assert.h>
#include <string.h>
#include "p4_audio_playout.h"
static p4_audio_playout_t q;
static int16_t input[P4_PLAYOUT_CAPACITY], out[P4_PLAYOUT_CHUNK];
static p4_playout_block_t b;
int main(void) {
    for (unsigned i=0;i<P4_PLAYOUT_CAPACITY;i++) input[i]=(int16_t)i;
    p4_audio_playout_init(&q, AUDIO_PLAYOUT_PROFILE_ADAPTIVE_CALL);
    assert(p4_audio_playout_push(&q,input,100,0,0));
    assert(!p4_audio_playout_prepare(&q,0,out,&b));
    assert(p4_audio_playout_prepare(&q,120,out,&b) && b.samples==100);
    assert(memcmp(input,out,200)==0);
    assert(p4_audio_playout_commit(&q,&b,120,true)==1 && q.count==0);

    p4_audio_playout_init(&q, AUDIO_PLAYOUT_PROFILE_LOW_LATENCY);
    assert(p4_audio_playout_push(&q,input,37,1,1));
    assert(p4_audio_playout_push(&q,input+37,123,6,6));
    assert(p4_audio_playout_prepare(&q,6,out,&b));
    assert(memcmp(input,out,320)==0);
    size_t before=q.count;
    p4_playout_block_t again;
    assert(p4_audio_playout_prepare(&q,6,out,&again));
    assert(q.count==before && again.consumed==b.consumed); /* lock retry owns PCM */
    assert(p4_audio_playout_commit(&q,&b,6,true)==2 && q.count==0);
    assert(!p4_audio_playout_prepare(&q,10,out,&b) && q.empty_events==0);
    assert(!p4_audio_playout_prepare(&q,26,out,&b) && q.empty_events==1);
    assert(!p4_audio_playout_prepare(&q,60,out,&b) && q.empty_events==1);

    p4_audio_playout_init(&q, AUDIO_PLAYOUT_PROFILE_LOW_LATENCY);
    for(unsigned i=0;i<8;i++) assert(p4_audio_playout_push(&q,input+i*160,160,i*20,0));
    unsigned packets=0, fast=0;
    int previous=-1;
    for(unsigned now=0;now<2000 && q.count;now+=20) {
        if(!p4_audio_playout_prepare(&q,now,out,&b)) continue;
        if(b.rate_permille>0) fast++;
        for(size_t i=0;i<b.samples;i++) {
            assert(out[i]>=previous && out[i]-previous<=3); previous=out[i];
        }
        packets+=p4_audio_playout_commit(&q,&b,now,true);
    }
    assert(fast>0 && packets==8 && q.count==0);

    p4_audio_playout_init(&q, AUDIO_PLAYOUT_PROFILE_ADAPTIVE_CALL);
    assert(p4_audio_playout_push(&q,input,400,0,0));
    q.started=true; q.next_due_ms=0;
    assert(p4_audio_playout_prepare(&q,0,out,&b) && b.rate_permille==-12);
    assert(b.consumed<160);
    p4_audio_playout_commit(&q,&b,0,true);
    assert(q.slow_blocks==1);

    p4_audio_playout_init(&q,AUDIO_PLAYOUT_PROFILE_LOW_LATENCY);
    assert(p4_audio_playout_push(&q,input,P4_PLAYOUT_CAPACITY,0,0));
    assert(!p4_audio_playout_push(&q,input,1,0,0));
    assert(q.count==P4_PLAYOUT_CAPACITY && q.overflow_samples==1);

    p4_audio_playout_init(&q,AUDIO_PLAYOUT_PROFILE_LOW_LATENCY);
    assert(p4_audio_playout_push(&q,input,320,0,0));
    assert(p4_audio_playout_prepare(&q,0,out,&b));
    assert(p4_audio_playout_commit(&q,&b,0,false)==0);
    assert(p4_audio_playout_prepare(&q,20,out,&b));
    assert(p4_audio_playout_commit(&q,&b,20,true)==0); /* partially failed packet */
    assert(q.count==0);
    assert(p4_audio_playout_push(&q,input,160,40,40));
    assert(p4_audio_playout_prepare(&q,40,out,&b));
    assert(p4_audio_playout_commit(&q,&b,40,true)==1);

    p4_audio_playout_init(&q,AUDIO_PLAYOUT_PROFILE_LOW_LATENCY);
    q.head=P4_PLAYOUT_CAPACITY-60;
    assert(p4_audio_playout_push(&q,input,160,UINT32_MAX-10U,UINT32_MAX-10U));
    assert(p4_audio_playout_prepare(&q,UINT32_MAX-10U,out,&b));
    assert(memcmp(input,out,320)==0);
    assert(p4_audio_playout_commit(&q,&b,UINT32_MAX-10U,true)==1);
    assert(!p4_audio_playout_prepare(&q,0,out,&b) && q.empty_events==0);
    assert(!p4_audio_playout_prepare(&q,10,out,&b) && q.empty_events==1);

    p4_audio_playout_init(&q,AUDIO_PLAYOUT_PROFILE_ADAPTIVE_CALL);
    audio_playout_controller_note_underflow(&q.controller);
    assert(q.controller.target_delay_ms==180);
    for(unsigned i=0;i<64;i++) {
        q.window.received_ms=q.window.played_ms=1000;
        p4_audio_playout_update_window(&q);
    }
    assert(q.controller.target_delay_ms==140);
    q.window.local_wait_ms=2;
    p4_audio_playout_update_window(&q);
    assert(q.controller.condition==AUDIO_PLAYOUT_CONDITION_LOCAL_PRESSURE);
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
