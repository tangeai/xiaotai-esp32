"""Reproduce prompt/call scratch-buffer contention using the actual RX function."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import sys

root = Path(__file__).resolve().parents[1]
relative = 'components/starter_media/src/starter_media.c'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--baseline', type=Path, metavar='SOURCE_FILE',
                    help='Explicit historical source containing play_audio_item; never infer it from HEAD')
args = parser.parse_args()
if args.baseline is not None:
    try:
        source = args.baseline.read_text(encoding='utf-8')
    except (OSError, UnicodeError) as error:
        parser.error(f'cannot read baseline source: {error}')
    if 'static bool play_audio_item(' not in source:
        parser.error('baseline must contain the historical play_audio_item implementation')
else:
    source = (root/relative).read_text(encoding='utf-8')
    start = source.index('static esp_err_t play_audio_chunk(')
    function = source[start:source.index('\n}', start)+2]
    code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_TIMEOUT 1
#define ESP_CODEC_DEV_OK 0
#define pdTRUE 1
#define AUDIO_PLAYBACK_I2S_VALUES_PER_INPUT 4
static int16_t s_play_stereo[64],s_playback_previous;
static uint32_t s_playback_resampler_generation;
static struct {
    int mode; uint32_t generation,lock_busy,write_max_us;
    struct {struct {unsigned local_wait_ms;} window;} queue;
} s_playout={.mode=1,.generation=7};
static atomic_bool s_speaker_muted;
static atomic_bool s_active=true;
static atomic_uint s_audio_write_failed,s_audio_playback_blocked;
static bool s_amp_enabled=true,busy=true,locked;
static void *s_speaker_dev=(void*)1;
static int s_audio_output_mutex=1;
static unsigned writes;
static int64_t esp_timer_get_time(void) {return 1000;}
static bool same_session(int mode,uint32_t gen) {return mode==1 && gen==7;}
static int xSemaphoreTake(int mutex,int timeout) {
    assert(mutex==1 && timeout==0);
    if(busy) {
        for(unsigned i=0;i<64;i++) assert(s_play_stereo[i]==123);
        return 0;
    }
    locked=true; return 1;
}
static void xSemaphoreGive(int mutex) {assert(mutex==1 && locked);locked=false;}
/* Control policy is covered by test_speaker_controls.py; this suite checks
 * that applying it never escapes the existing PCM owner's lock. */
static void apply_speaker_controls_locked(bool active) {assert(locked && active);}
static int esp_codec_dev_write(void *dev,const void *data,size_t bytes) {
    assert(dev==(void*)1 && locked && data==s_play_stereo && bytes==32); ++writes; return 0;
}
'''+function+r'''
int main(void) {
    const int16_t pcm[4]={1000,1000,1000,1000};
    for(unsigned i=0;i<64;i++) s_play_stereo[i]=123;
    assert(play_audio_chunk(pcm,4)==ESP_ERR_TIMEOUT && writes==0);
    assert(s_playout.lock_busy==1 && s_playout.queue.window.local_wait_ms==2);
    busy=false; assert(play_audio_chunk(pcm,4)==ESP_OK && writes==1 && !locked);
    s_speaker_muted=true;
    assert(play_audio_chunk(pcm,4)==ESP_FAIL && writes==1 && !locked);
    s_speaker_muted=false; s_playout.generation=6;
    assert(play_audio_chunk(pcm,4)==ESP_FAIL && writes==1 && !locked);
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='p4-output-owner-') as tmp:
        p=Path(tmp)
        (p/'test.c').write_text(code,encoding='utf-8')
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                        str(p/'test.c'),'-o',str(p/'test')],check=True)
        subprocess.run([str(p/'test')],check=True)
    print('PASS: P4 output scratch ownership, nonblocking contention, mute and stale session')
    sys.exit(0)
start = source.index('static bool play_audio_item(')
function = source[start:source.index('\n}',start)+2]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#define AUDIO_RX_BYTES 16
#define AUDIO_PLAYBACK_I2S_VALUES_PER_INPUT 4
#define ESP_AUDIO_ERR_OK 0
#define ESP_CODEC_DEV_OK 0
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGW(...) ((void)0)
typedef struct {uint32_t length;} frame_t;
typedef struct {int mode;uint32_t generation;frame_t frame;uint8_t payload[16];} audio_rx_item_t;
typedef struct {uint8_t *buffer;size_t len;} esp_audio_dec_in_raw_t;
typedef struct {uint8_t *buffer;size_t len,decoded_size;} esp_audio_dec_out_frame_t;
typedef struct {int unused;} esp_audio_dec_info_t;
static int16_t s_decode_pcm[16],s_play_stereo[64],s_playback_previous;
static uint32_t s_playback_resampler_generation;
static atomic_uint s_audio_decode_failed,s_audio_decoded,s_audio_played,
                   s_audio_write_failed,s_audio_playback_blocked;
static atomic_bool s_speaker_muted;
static bool s_amp_enabled=true,busy=true,locked;
static void *s_speaker_dev=(void*)1;
static int s_g711_decoder=1,s_audio_output_mutex=1;
static unsigned writes;
static bool same_session(int mode,uint32_t gen) {return mode==1 && gen==7;}
static int esp_g711a_dec_decode(int decoder,esp_audio_dec_in_raw_t *in,
                                esp_audio_dec_out_frame_t *out,esp_audio_dec_info_t *info) {
    (void)decoder;(void)in;(void)info;
    out->decoded_size=8;
    for(unsigned i=0;i<4;i++) ((int16_t*)out->buffer)[i]=1000;
    return 0;
}
static int xSemaphoreTake(int mutex,int timeout) {
    assert(mutex==1 && timeout==500);
    if(busy) {
        /* The prompt/DMA still owns these samples until the output lock is available. */
        for(unsigned i=0;i<64;i++) assert(s_play_stereo[i]==123);
        return 0;
    }
    locked=true; return 1;
}
static void xSemaphoreGive(int mutex) {assert(mutex==1 && locked);locked=false;}
static int esp_codec_dev_write(void *dev,const void *data,size_t bytes) {
    assert(dev==(void*)1 && locked && data==s_play_stereo && bytes==32); ++writes; return 0;
}
'''+function+r'''
int main(void) {
    for(unsigned i=0;i<64;i++)s_play_stereo[i]=123;
    audio_rx_item_t item={.mode=1,.generation=7,.frame={4}};
    assert(!play_audio_item(&item) && writes==0);
    busy=false; assert(play_audio_item(&item) && writes==1 && !locked);
    s_speaker_muted=true; assert(!play_audio_item(&item) && writes==1 && !locked);
    item.generation=6; assert(!play_audio_item(&item) && !locked);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='p4-output-owner-') as tmp:
    p=Path(tmp)
    (p/'test.c').write_text(code,encoding='utf-8')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('PASS: prompt buffer retained during contention; playback, mute and stale generation release ownership')
