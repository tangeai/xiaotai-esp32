"""Run the installed IDF write loop against a deterministic DMA clock.

This checks the software/driver contract, not actual hardware timing or sound.
"""
from pathlib import Path
import os
import subprocess
import tempfile

idf = Path(os.environ.get('IDF_PATH', '/mnt/c/esp/v5.5.4/esp-idf'))
source = (idf / 'components/esp_driver_i2s/i2s_common.c').read_text(encoding='utf-8')
write = source[source.index('esp_err_t i2s_channel_write('):
               source.index('esp_err_t i2s_channel_read(')]
root = Path(__file__).resolve().parents[1]
media = (root / 'components/starter_media/src/starter_media.c').read_text(encoding='utf-8')
sink = media[media.index('static void audio_sink_task('):media.index('static void camera_task(')]
assert 'audio_channel.dma_desc_num = 6;' in media
assert 'audio_channel.dma_frame_num = 240;' in media
assert 'next_due_ms' not in sink
assert 's_playout.queue.started && s_playout.queue.count ? 0 : 20' in sink
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "p4_audio_playout.h"
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_TIMEOUT 3
#define pdTRUE 1
#define pdFALSE 0
#define I2S_DIR_TX 0
#define I2S_CHAN_STATE_RUNNING 1
#define SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE 0
#define pdMS_TO_TICKS(ms) (ms)
#define I2S_NULL_POINTER_CHECK(tag,p) assert(p)
#define ESP_RETURN_ON_FALSE(cond,err,tag,msg) do { if (!(cond)) return err; } while (0)
typedef int esp_err_t;
typedef struct {
    int dir, state, binary;
    void *msg_queue;
    struct { unsigned rw_pos, buf_size, desc_num; void *curr_ptr; } dma;
} channel_t;
typedef channel_t *i2s_chan_handle_t;
enum { DESCS=6, FRAMES=240, VALUES=FRAMES*2, QUEUE=DESCS-1, BLOCK=320 };
static int16_t buffers[DESCS][VALUES], pcm[BLOCK*2];
static unsigned available[QUEUE], head, count, next_desc, now_us, eof_us;
static unsigned received, holes, zero_run, max_hole, wrong_order, previous, first_hole;
static bool started;
static int xSemaphoreTake(int s, unsigned wait) { (void)s; (void)wait; return pdTRUE; }
static void xSemaphoreGive(int s) { (void)s; }
static unsigned uxQueueSpacesAvailable(void *q) { (void)q; return QUEUE-count; }
static void dma_eof(void) {
    int16_t *p=buffers[next_desc];
    for(unsigned i=0;i<FRAMES;i++) {
        assert(p[i*2]==p[i*2+1]);
        unsigned sample=(uint16_t)p[i*2];
        if(sample) {
            if(started) {
                if(zero_run && !first_hole) first_hole=received;
                holes+=zero_run; if(zero_run>max_hole) max_hole=zero_run;
                if(sample!=previous%30000+1) wrong_order++;
            }
            started=true; zero_run=0; previous=sample; received++;
        } else if(started) zero_run++;
    }
    /* IDF TX EOF clears the completed DMA buffer, then queues it for refill. */
    memset(p,0,sizeof(buffers[0]));
    if(count==QUEUE) { head=(head+1)%QUEUE; count--; }
    available[(head+count)%QUEUE]=next_desc; count++;
    next_desc=(next_desc+1)%DESCS; now_us=eof_us; eof_us+=15000;
}
static void advance(unsigned target) {
    while(eof_us<=target) dma_eof();
    if(now_us<target) now_us=target;
}
static int xQueueReceive(void *q, void *dest, unsigned timeout) {
    (void)q;
    if(!count) { assert(timeout>=15); dma_eof(); }
    *(void **)dest=buffers[available[head]];
    head=(head+1)%QUEUE; count--; return pdTRUE;
}
'''
test = r'''
static void run(bool software_paced, unsigned stall_us) {
    memset(buffers,0,sizeof(buffers)); head=count=next_desc=now_us=0;
    eof_us=15000; received=holes=zero_run=max_hole=wrong_order=previous=first_hole=0; started=false;
    channel_t c={.dir=I2S_DIR_TX,.state=I2S_CHAN_STATE_RUNNING,
        .dma={.buf_size=sizeof(buffers[0]),.desc_num=DESCS}};
    advance(300000); /* Capture/codec runs before remote speech arrives. */
    unsigned due=now_us, input=0;
    for(unsigned n=0;n<300;n++) {
        if(software_paced) advance(due);
        if(n && n%17==0) advance(now_us+stall_us);
        for(unsigned i=0;i<BLOCK;i++) {
            pcm[i*2]=pcm[i*2+1]=(int16_t)(input++%30000+1);
        }
        size_t written=0;
        assert(i2s_channel_write(&c,pcm,sizeof(pcm),&written,1000)==ESP_OK);
        assert(written==sizeof(pcm));
        due+=20000;
        if(now_us>due) due=now_us;
        if(!software_paced) advance(now_us+1000); /* Product writer yields one tick. */
    }
    advance(now_us+180000);
    printf("pacing=%s stall_us=%u input=%u sent=%u inserted_zero=%u max_zero_us=%u order_errors=%u\n",
           software_paced?"software-20ms":"DMA",stall_us,input,received,holes,max_hole*1000000/16000,wrong_order);
    assert(received==input && wrong_order==0);
    if(software_paced || stall_us>90000) assert(holes>0);
    else assert(holes==0);
}
static void product_stream(audio_playout_profile_t profile) {
    static p4_audio_playout_t q;
    static int16_t source_pcm[320], output_pcm[P4_PLAYOUT_CHUNK];
    memset(buffers,0,sizeof(buffers)); head=count=next_desc=now_us=0;
    eof_us=15000; received=holes=zero_run=max_hole=wrong_order=previous=first_hole=0; started=false;
    channel_t c={.dir=I2S_DIR_TX,.state=I2S_CHAN_STATE_RUNNING,
        .dma={.buf_size=sizeof(buffers[0]),.desc_num=DESCS}};
    advance(300000);
    p4_audio_playout_init(&q,profile);
    unsigned packets=0, consumed=0, output=0, complete=0;
    for(unsigned i=0;i<320;i++) source_pcm[i]=1000;
    while(now_us<15500000) {
        while(packets<300 && 300000+packets*40000<=now_us) {
            assert(p4_audio_playout_push(&q,source_pcm,320,packets*40,300+packets*40));
            packets++;
        }
        p4_playout_block_t block;
        if(p4_audio_playout_prepare(&q,now_us/1000,output_pcm,&block)) {
            /* Monotonic output markers isolate DMA continuity from DSP.
             * The PCM/controller test separately verifies source consumption. */
            for(unsigned i=0;i<block.samples*2;i++) {
                pcm[i*2]=pcm[i*2+1]=(int16_t)(output++%30000+1);
            }
            size_t written=0, bytes=block.samples*4*sizeof(int16_t);
            assert(i2s_channel_write(&c,pcm,bytes,&written,1000)==ESP_OK && written==bytes);
            consumed+=block.consumed;
            complete+=p4_audio_playout_commit(&q,&block,now_us/1000,true);
        }
        advance(now_us+1000);
    }
    printf("product profile=%d packets=%u consumed=%u output=%u sent=%u zero=%u max_zero_us=%u first_hole_at=%u\n",
           profile,complete,consumed,output,received,holes,max_hole*1000000/16000,first_hole);
    assert(complete==300 && consumed==96000 && received==output && wrong_order==0);
    assert(holes==0);
}
int main(void) {
    setbuf(stdout,NULL);
    run(true,0); run(false,0);
    run(true,8000); run(false,8000);
    run(false,130000); /* Exceeding DMA capacity must remain a visible limitation. */
    for(unsigned p=0;p<3;p++) product_stream((audio_playout_profile_t)p);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='p4-i2s-cadence-') as tmp:
    p = Path(tmp)
    (p / 'test.c').write_text(prefix + write + test, encoding='utf-8')
    subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I'+str(root/'components/starter_media/src'),
                    str(p / 'test.c'), str(root/'components/starter_media/src/p4_audio_playout.c'),
                    str(root/'components/starter_media/src/audio_playout_controller.c'),
                    '-o', str(p / 'test')], check=True)
    subprocess.run([str(p / 'test')], check=True)
print('PASS: actual IDF write loop, successful-byte accounting and DMA continuity model')
