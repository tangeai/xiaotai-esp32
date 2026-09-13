"""Exercise the real P4 SDK feedback bridge and media-owner scheduling."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
sdk = (root.parent / 'lckfb-szpi-esp32s3-tirtc/components/starter_tirtc/src/starter_tirtc.c').read_text(encoding='utf-8')
video = (root / 'components/p4_hardware/p4_video.c').read_text(encoding='utf-8')

def function(text, signature):
    start = text.index(signature)
    return text[start:text.index('\n}', start) + 2]

assert '.on_update_bitrate = on_update_bitrate,' in sdk
poll = function(video, 'void p4_video_poll(')
assert poll.index('configure_bitrate(generation)') < poll.index('camera_pipeline_set_rtc_video_enabled(true)')
assert poll.index('media_governor_reset_transport_adaptation(true') < poll.index('media_governor_set_rtc_video_config(&config)')
assert 'maintain_bitrate();' in poll
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#define CONFIG_IDF_TARGET_ESP32P4 1
#define CONFIG_APP_RTC_SDK_VIDEO_ADAPT_ENABLE 1
#define H5_VIDEO_STREAM 11
#define TIRTC_E_INVALID_PARAMETER -1
#define ESP_OK 0
#define taskENTER_CRITICAL(x) ((void)(x))
#define taskEXIT_CRITICAL(x) ((void)(x))
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
typedef void *tirtc_conn_t;
typedef int esp_err_t;
typedef enum {STARTER_TIRTC_NONE,STARTER_TIRTC_H5,STARTER_TIRTC_AI,
              STARTER_TIRTC_VOIP,STARTER_TIRTC_CALL} starter_tirtc_mode_t;
static atomic_uintptr_t s_connection;
static atomic_uint s_active_generation, s_live_generation;
static atomic_bool s_audio_subscribed,s_video_subscribed;
static int s_bitrate_lock;
static uint32_t s_bitrate_generation,s_bitrate_target,s_bitrate_registered_generation;
static int64_t s_bitrate_step_us,clock_us;
static starter_tirtc_mode_t mode;
static uint32_t starter_tirtc_generation(void) {return s_active_generation;}
static starter_tirtc_mode_t starter_tirtc_mode(void) {return mode;}
static int64_t esp_timer_get_time(void) {return clock_us;}
static unsigned registrations,applies,steps,reconfigures;
static int sdk_result;
static bool sdk_initial_callback, change=true;
static uint32_t applied;
static void on_update_bitrate(tirtc_conn_t,uint8_t,uint32_t);
static int TiRtcConnSetVideoBitrateParams(tirtc_conn_t conn,uint8_t stream,
                                         uint32_t min,uint32_t max,uint32_t start) {
    assert(conn==(void*)1 && stream==11 && min==64000 && start==200000 && max==256000);
    ++registrations;
    if(sdk_initial_callback) on_update_bitrate(conn,stream,start);
    return sdk_result;
}
typedef struct {uint32_t min_bitrate_bps,max_bitrate_bps,start_bitrate_bps;} media_governor_transport_bitrate_range_t;
static void media_governor_get_transport_bitrate_range(media_governor_transport_bitrate_range_t *r) {
    *r=(media_governor_transport_bitrate_range_t){64000,256000,200000};
}
static int media_governor_apply_transport_bitrate_target(uint32_t b,bool *changed) {
    applied=b; ++applies; *changed=change; return 0;
}
static int media_governor_step_transport_adaptation(bool *changed) {
    ++steps; *changed=change; return 0;
}
static void camera_pipeline_on_rtc_video_config_changed(void) {++reconfigures;}
'''
for name in ['static bool connection_matches(', 'static void clear_subscriptions(',
             'static void on_update_bitrate(', 'int starter_tirtc_set_video_bitrate(',
             'bool starter_tirtc_take_video_bitrate(']:
    code += function(sdk, name)
code += function(video, 'static void configure_bitrate(')
code += function(video, 'static void maintain_bitrate(')
code += r'''
int main(void) {
    assert(!connection_matches(NULL));
    mode=STARTER_TIRTC_CALL; s_connection=1; s_active_generation=7;
    assert(starter_tirtc_set_video_bitrate(6,64000,256000,200000)==-1);
    assert(starter_tirtc_set_video_bitrate(7,0,256000,200000)==-1);
    assert(starter_tirtc_set_video_bitrate(7,64000,256000,256000)==-1);
    mode=STARTER_TIRTC_AI;
    assert(starter_tirtc_set_video_bitrate(7,64000,256000,200000)==-1);
    assert(registrations==0);
    mode=STARTER_TIRTC_CALL; s_live_generation=7; sdk_initial_callback=true;
    configure_bitrate(7); assert(registrations==1 && s_bitrate_registered_generation==7);
    uint32_t target=0;
    assert(starter_tirtc_take_video_bitrate(7,&target) && target==200000);
    assert(!starter_tirtc_take_video_bitrate(7,&target));
    assert(!starter_tirtc_take_video_bitrate(7,NULL));
    on_update_bitrate(NULL,11,1); on_update_bitrate((void*)2,11,1);
    on_update_bitrate((void*)1,10,1); on_update_bitrate((void*)1,11,0);
    assert(!starter_tirtc_take_video_bitrate(7,&target));
    for(unsigned i=0;i<10000;i++) on_update_bitrate((void*)1,11,64000+i);
    assert(applies==0 && reconfigures==0); /* Callback must not run the encoder. */
    maintain_bitrate(); assert(applies==1 && applied==73999 && reconfigures==1);
    maintain_bitrate(); assert(steps==0);
    clock_us=200000; maintain_bitrate(); assert(steps==1 && reconfigures==2);
    change=false; clock_us+=200000; maintain_bitrate(); assert(reconfigures==2);
    on_update_bitrate((void*)1,11,90000);
    s_active_generation=8; maintain_bitrate(); assert(applies==1);
    assert(!starter_tirtc_take_video_bitrate(8,&target));
    clear_subscriptions(); assert(s_bitrate_generation==0 && s_bitrate_target==0);
    on_update_bitrate((void*)1,11,90000);
    assert(!starter_tirtc_take_video_bitrate(8,&target));
    sdk_result=-1; configure_bitrate(8);
    assert(!s_bitrate_registered_generation && !s_bitrate_generation && !s_bitrate_target);
    s_live_generation=8; maintain_bitrate(); assert(applies==1);
    sdk_result=0; configure_bitrate(8); maintain_bitrate(); assert(applies==2);
    puts("PASS: P4 bitrate registration, 10000 coalesced callbacks, owner-only apply, recovery cadence, stale/unsupported/AI rejection");
}
'''
with tempfile.TemporaryDirectory(prefix='p4-bitrate-') as tmp:
    path = Path(tmp)
    (path/'test.c').write_text(code, encoding='utf-8')
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
