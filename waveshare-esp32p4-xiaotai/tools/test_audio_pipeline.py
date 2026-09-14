"""Exercise P4 sample chronology and allocation contracts, not acoustic quality."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
media = (root / 'components/starter_media/src/starter_media.c').read_text(encoding='utf-8')
aec = (root / 'components/starter_media/src/starter_aec.c').read_text(encoding='utf-8')
init = media[media.index('esp_err_t starter_media_init(void)'):media.index('i2c_master_bus_handle_t starter_media_i2c_bus')]
assert init.index('starter_aec_init()') < init.index('audio_codecs_init()') < init.index('xTaskCreateWithCaps(audio_sink_task')
assert '.no_dac_ref = false' in media
assert '.on_recv_q_ovf = capture_queue_overflow' in media
assert '.mic_num = 1' in aec and '.ref_num = 1' in aec
assert '.caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT' in aec
assert aec.index('aec_process(s_aec.handle') < aec.index('starter_agc_process(')
assert aec.index('aec_process(s_aec.handle') < aec.index('p4_capture_highpass_process(') < aec.index('starter_agc_process(')
assert '.level = {mic_level, ref_level, clean_level}' in aec

# This is a static synchronization contract; controlled device contention
# separately verifies that short snapshot ownership does not discard PCM.
capture = media[media.index('static void audio_capture_task('):media.index('static bool buffer_audio_item(')]
assert '#define PREROLL_CAPTURE_LOCK_WAIT_MS 2U' in media
assert 'xSemaphoreTake(s_preroll_mutex, 0)' not in capture
assert capture.count('xSemaphoreTake(s_preroll_mutex, pdMS_TO_TICKS(PREROLL_CAPTURE_LOCK_WAIT_MS))') == 2
assert capture.index('if (replay_interrupted) {') > capture.index('xSemaphoreGive(s_preroll_mutex);', capture.index('if (s_preroll.failed &&'))
assert capture.index('if (replay_complete) {') > capture.index('xSemaphoreGive(s_preroll_mutex);', capture.index('bool replay_complete'))
uplink = media[media.index('static void audio_uplink_task('):media.index('static void audio_capture_task(')]
assert media.count('starter_agc_boost_uplink(') == 1
assert 'starter_agc_boost_uplink(' not in capture and 'starter_agc_boost_uplink(' not in aec
assert uplink.index('bool ready = same_session(') < uplink.index('starter_agc_boost_uplink(') < uplink.index('esp_g711_enc_process(')
assert 'starter_agc_boost_uplink(item->pcm, AUDIO_PACKET_SAMPLES)' in uplink
assert 'TX gain failed: %s' in uplink and 'ready = false;' in uplink

# Reuse the dependency stub, but execute the P4 conditional branch of real AGC code.
from audio_agc_fixture import prefix
source = (root / 'components/starter_media/src/starter_agc.c').read_text(encoding='utf-8')
source = re.sub(r'^#include[^\n]*\n', '', source, flags=re.M)
test = r'''
int main(void) {
    assert(starter_agc_init()==ESP_OK && opened==2);
    assert(state[0].gain==9 && state[0].target==6 && state[0].limiter==1);
    assert(state[1].gain==6 && state[1].target==3 && state[1].limiter==1);
    assert(starter_agc_init()==ESP_OK && opened==2);
    int16_t in[512], out[512];
    for (unsigned f=0;f<200;f++) {
        for (unsigned i=0;i<512;i++) in[i]=(int16_t)(((f*512+i)%20000)-10000);
        assert(starter_agc_process(in,out,512)==ESP_OK);
        for (unsigned i=0;i<512;i++) {
            unsigned at=f*512+i;
            assert(out[i] == (at<160 ? 0 : (int)((at-160)%20000)-10000));
        }
    }
    int16_t pcm[160], expected[160];
    for (int i=0;i<160;i++) pcm[i]=expected[i]=(int16_t)(i*300-22000);
    for (int i=0;i<100;i++) {
        /* Stub verifies chronology only; real gain/limiter needs listening. */
        assert(starter_agc_boost_uplink(pcm,160)==ESP_OK);
        assert(memcmp(pcm,expected,sizeof(pcm))==0);
    }
    assert(starter_agc_boost_uplink(pcm,79)==ESP_ERR_INVALID_ARG);
    assert(starter_agc_boost_uplink(NULL,160)==ESP_ERR_INVALID_ARG);
    fail_process_mode=3;
    assert(starter_agc_boost_uplink(pcm,160)==ESP_FAIL);
    assert(starter_agc_process(in,out,1)==ESP_OK);
    fail_process_mode=0;
    starter_agc_deinit(); assert(closed==2);
    assert(starter_agc_boost_uplink(pcm,160)==ESP_ERR_INVALID_STATE);
    internal_mode=2;
    assert(starter_agc_init()==ESP_ERR_NOT_SUPPORTED && closed==3);
    internal_mode=3;
    assert(starter_agc_init()==ESP_ERR_NOT_SUPPORTED && closed==5);
    internal_mode=0; fail_allocation_at=opened+2;
    assert(starter_agc_init()==ESP_ERR_NO_MEM && closed==6);
    internal_mode=0; fail_allocation_at=opened+1;
    assert(starter_agc_init()==ESP_ERR_NO_MEM && closed==6);
    fail_allocation_at=0;
    assert(starter_agc_init()==ESP_OK);
    starter_agc_deinit(); assert(closed==8);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='p4-audio-') as tmp:
    p=Path(tmp)
    (p/'agc.c').write_text(prefix+source+test, encoding='utf-8')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    '-DCONFIG_IDF_TARGET_ESP32P4=1','-DCONFIG_XIAOTAI_CAPTURE_AGC_GAIN_DB=9',
                    '-I'+str(root/'components/starter_media/src'),str(p/'agc.c'),'-o',str(p/'agc')],check=True)
    subprocess.run([str(p/'agc')],check=True)

    loops = re.findall(r'    for \(size_t i = 0; i < (?:mono_samples|chunk_samples); \+\+i\) \{.*?\n    \}', media, re.S)
    # Local prompts have one extra indentation level.
    local = re.search(r'        for \(size_t i = 0; i < chunk_samples; \+\+i\) \{.*?\n        \}', media, re.S)
    assert loops and local
    pre = '#include <assert.h>\n#include <stdint.h>\n#include <stddef.h>\n#define AUDIO_PLAYBACK_I2S_VALUES_PER_INPUT 4\n'
    code = pre + 'static int16_t s_decode_pcm[17],s_play_stereo[68],s_playback_previous;\n'
    code += 'static void remote(size_t mono_samples){\nconst int16_t *pcm=s_decode_pcm;\n'+loops[0]+'\n}\n'
    code += 'static void prompt(const int16_t *pcm,size_t offset,size_t chunk_samples,int16_t *state){\nint16_t previous=*state;\n'+local[0]+'\n*state=previous;\n}\n'
    code += r'''
int main(void) {
    int16_t previous=0;
    for(int block=0;block<32;block++) {
        for(int i=0;i<17;i++)s_decode_pcm[i]=(int16_t)((block*17+i+1)*40);
        remote(17);
        for(int i=0;i<17;i++) {
            int curr=s_decode_pcm[i];
            assert(s_play_stereo[i*4]==curr-20 && s_play_stereo[i*4+2]==curr);
            assert(s_play_stereo[i*4]==s_play_stereo[i*4+1]);
            assert(s_play_stereo[i*4+2]==s_play_stereo[i*4+3]);
        }
        prompt(s_decode_pcm,0,17,&previous);
        for(int i=0;i<17;i++)assert(s_play_stereo[i*4]==s_decode_pcm[i]-20 && s_play_stereo[i*4+2]==s_decode_pcm[i]);
    }
}
'''
    (p/'order.c').write_text(code, encoding='utf-8')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'order.c'),'-o',str(p/'order')],check=True)
    subprocess.run([str(p/'order')],check=True)
print('PASS: P4 MR routing, ADC startup order, PSRAM AGC cleanup, 102400 samples, both playback interpolation paths')
