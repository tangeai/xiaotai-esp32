"""Run the real nonblocking speaker-control owner with injected codec failures."""
from test_runtime_resources import COMMON, ROOT, function, run_case

source = (ROOT / 'components/starter_media/src/starter_media.c').read_text(encoding='utf-8')
setters = '\n'.join(function(source, name) for name in (
    'starter_media_set_speaker_volume', 'starter_media_set_speaker_muted'))
assert 'xSemaphoreTake' not in setters
assert 'esp_codec_dev_set_' not in setters
assert 'heap_caps_' not in setters and 'xTaskCreate' not in setters
prompt = function(source, 'starter_media_play_pcm8k_at_epoch')
assert 'chunk_samples = AUDIO_PACKET_SAMPLES;' in prompt
assert 'apply_speaker_controls_locked(true)' in prompt
assert prompt.count('xSemaphoreGive(s_audio_output_mutex)') == 2
assert 'apply_speaker_controls_locked(atomic_load(&s_active))' in function(source, 'audio_sink_task')
assert 'apply_speaker_controls_locked(atomic_load(&s_active))' in function(source, 'play_audio_chunk')

body = COMMON + r'''
#define ESP_FAIL -1
#define ESP_CODEC_DEV_OK 0
static void *s_speaker_dev=(void*)1, *s_audio_output_mutex=(void*)2;
static atomic_uchar s_speaker_volume=7, s_speaker_volume_applied=7;
static atomic_bool s_speaker_muted, s_speaker_muted_applied, s_speaker_controls_pending;
static atomic_int s_speaker_control_error;
static unsigned vol_calls, mute_calls, pa_calls;
static bool owner, fail_volume, fail_mute, fail_pa, amp, inject_new;
static unsigned codec_volume=70;
static bool codec_mute;
static esp_err_t starter_media_set_speaker_volume(uint8_t volume);
static int esp_codec_dev_set_out_vol(void *dev,unsigned volume) {
    assert(owner && dev==s_speaker_dev); vol_calls++;
    if(inject_new) { inject_new=false; assert(starter_media_set_speaker_volume(9)==ESP_OK); }
    if(fail_volume) return -1;
    codec_volume=volume; return 0;
}
static int esp_codec_dev_set_out_mute(void *dev,bool muted) {
    assert(owner && dev==s_speaker_dev); mute_calls++;
    if(fail_mute) return -1;
    codec_mute=muted; return 0;
}
static esp_err_t audio_output_set_locked(bool enabled) {
    assert(owner); pa_calls++;
    if(fail_pa) return -1;
    amp=enabled; return 0;
}
''' + setters + function(source, 'apply_speaker_controls_locked') + r'''
static void apply(bool active) { owner=true; apply_speaker_controls_locked(active); owner=false; }
int main(void) {
    for(unsigned i=0;i<10000;i++) {
        assert(starter_media_set_speaker_volume(i%11)==ESP_OK);
        assert(starter_media_set_speaker_muted(i%2)==ESP_OK);
    }
    assert(vol_calls==0 && mute_calls==0 && pa_calls==0);
    assert(starter_media_set_speaker_volume(6)==ESP_OK);
    assert(starter_media_set_speaker_muted(false)==ESP_OK);
    apply(true);
    assert(vol_calls==1 && codec_volume==60 && !codec_mute && amp);
    assert(atomic_load(&s_speaker_volume_applied)==6 && !atomic_load(&s_speaker_muted_applied));
    assert(atomic_load(&s_speaker_control_error)==ESP_OK);
    apply(true); assert(vol_calls==1);
    assert(starter_media_set_speaker_volume(11)==ESP_ERR_INVALID_ARG);
    assert(starter_media_set_speaker_muted(true)==ESP_OK);
    apply(true); assert(codec_mute && !amp);
    assert(starter_media_set_speaker_muted(false)==ESP_OK);
    apply(false); assert(!codec_mute && !amp); /* idle never powers the PA */
    assert(starter_media_set_speaker_volume(8)==ESP_OK);
    inject_new=true; apply(true);
    assert(codec_volume==80 && atomic_load(&s_speaker_volume)==9 && atomic_load(&s_speaker_controls_pending));
    apply(true); assert(codec_volume==90 && !atomic_load(&s_speaker_controls_pending));
    fail_volume=true; assert(starter_media_set_speaker_volume(3)==ESP_OK); apply(true);
    assert(atomic_load(&s_speaker_control_error)==ESP_FAIL && codec_volume==90);
    assert(atomic_load(&s_speaker_volume_applied)==9 && atomic_load(&s_speaker_volume)==3);
    unsigned before=vol_calls; apply(true); assert(vol_calls==before); /* no hidden retries */
    fail_volume=false; assert(starter_media_set_speaker_volume(3)==ESP_OK); apply(true);
    assert(codec_volume==30 && atomic_load(&s_speaker_control_error)==ESP_OK);
    fail_mute=true; assert(starter_media_set_speaker_muted(true)==ESP_OK); apply(true);
    assert(!amp && !atomic_load(&s_speaker_muted_applied) && atomic_load(&s_speaker_control_error)==ESP_FAIL);
    fail_mute=false; fail_pa=true; assert(starter_media_set_speaker_muted(false)==ESP_OK); apply(true);
    assert(atomic_load(&s_speaker_control_error)==ESP_FAIL);
    s_speaker_dev=NULL;
    assert(starter_media_set_speaker_volume(7)==ESP_ERR_INVALID_ARG);
    assert(starter_media_set_speaker_muted(false)==ESP_ERR_INVALID_STATE);
    return 0;
}
'''
run_case('speaker controls: nonblocking/coalescing/ownership/concurrent request/errors', body)
