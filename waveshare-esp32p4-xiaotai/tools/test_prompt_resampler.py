"""Run real prompt ownership, cancellation and FIR-tail completion together."""
import subprocess
import tempfile
from playback_resampler_fixture import ROOT, compile_test

media = (ROOT / 'components/starter_media/src/starter_media.c').read_text()
start = media.index('esp_err_t starter_media_play_pcm8k_at_epoch(')
function = media[start:media.index('\n}', start) + 2]
code = r'''
#include <assert.h>
#include <stdatomic.h>
#include <string.h>
#include "p4_playback_resampler.h"
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_TIMEOUT 0x107
#define ESP_FAIL -1
#define ESP_CODEC_DEV_OK 0
#define AUDIO_PACKET_SAMPLES 160U
#define AUDIO_TRANSPORT_SAMPLE_RATE_HZ 8000U
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGI(...) ((void)0)
static atomic_bool s_ready=true,s_active,s_speaker_muted;
static atomic_uint s_pcm8k_cancel_sequence;
static void *s_speaker_dev=(void*)1;
static void *s_audio_output_mutex=(void*)2;
static int16_t s_play_stereo[6000], recorded[4096];
static p4_playback_resampler_t s_prompt_resampler;
static bool locked,amp;
static unsigned writes,used,interrupt_at,fail_at,mute_at,waited;
static int xSemaphoreTake(void *mutex,int ticks) {assert(mutex==(void*)2&&ticks==500&&!locked);locked=true;return 1;}
static void xSemaphoreGive(void *mutex) {assert(mutex==(void*)2&&locked);locked=false;}
static void apply_speaker_controls_locked(bool active) {assert(locked&&active);}
static esp_err_t audio_output_set_locked(bool enabled) {assert(locked);amp=enabled;return ESP_OK;}
static void vTaskDelay(int ticks) {assert(ticks==40&&locked);waited++;}
static int esp_codec_dev_write(void *dev, const void *data, size_t bytes) {
    assert(locked&&amp&&dev==s_speaker_dev&&data==s_play_stereo);
    assert(bytes<=1280&&used+bytes/2<=4096);
    memcpy(recorded+used,data,bytes); used+=bytes/2; writes++;
    if(writes==interrupt_at) atomic_fetch_add(&s_pcm8k_cancel_sequence,1);
    if(writes==mute_at) s_speaker_muted=true;
    return writes==fail_at ? -1 : 0;
}
''' + function + r'''
int main(void) {
    int16_t pcm[333],expected[1456];
    for(unsigned i=0;i<333;++i)pcm[i]=(int16_t)(i*30-5000);
    p4_playback_resampler_t reference={0};
    assert(p4_playback_resampler_reset(&reference,pcm[0])==ESP_OK);
    assert(p4_playback_resampler_process(&reference,pcm,333,expected,1456)==1332);
    assert(p4_playback_resampler_finish(&reference,expected+1332,124)==124);
    assert(starter_media_play_pcm8k_at_epoch(pcm,333,0)==ESP_OK);
    assert(used==1456&&writes==4&&waited==1&&!locked&&!amp);
    assert(!memcmp(expected,recorded,sizeof(expected)));
    writes=used=waited=0; interrupt_at=2;
    assert(starter_media_play_pcm8k_at_epoch(pcm,333,0)==ESP_ERR_INVALID_STATE);
    assert(writes==2&&used==1280&&!waited&&!locked&&!amp);
    writes=used=0; interrupt_at=0; mute_at=1;
    assert(starter_media_play_pcm8k_at_epoch(pcm,333,1)==ESP_ERR_INVALID_STATE);
    assert(writes==1&&used==640&&!locked&&!amp);
    s_speaker_muted=false; writes=used=0; mute_at=3;
    assert(starter_media_play_pcm8k_at_epoch(pcm,333,1)==ESP_ERR_INVALID_STATE);
    assert(writes==3&&used==1332&&!waited&&!locked&&!amp); /* last-block mute, no FIR drain */
    s_speaker_muted=false; writes=used=0; mute_at=0; fail_at=4;
    assert(starter_media_play_pcm8k_at_epoch(pcm,333,1)==ESP_FAIL);
    assert(writes==4&&!waited&&!locked&&!amp);
}
'''
with tempfile.TemporaryDirectory(prefix='p4-prompt-fir-') as p:
    subprocess.run([str(compile_test(p, code))], check=True)
print('PASS: real prompt chunks, exact FIR tail, mute/cancel/failure, output ownership')
