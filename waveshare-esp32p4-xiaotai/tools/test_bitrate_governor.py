"""Run the actual P4 bitrate governor and product tuning with a virtual clock."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "media_governor.h"
#include "media_tuning.h"
uint32_t now_tick;
static media_governor_video_config_t current(void) {
    media_governor_video_config_t c={0}; media_governor_get_rtc_video_config(&c); return c;
}
static void test_profile(bool phone, uint32_t start_tick) {
    bool changed=false;
    media_governor_video_config_t c={0};
    if(phone) media_governor_build_wechat_video_config(&c);
    else media_governor_build_device_call_video_config(&c);
    assert(media_governor_set_rtc_video_config(&c)==ESP_OK);
    media_governor_transport_bitrate_range_t r={0};
    media_governor_get_transport_bitrate_range(&r);
    assert(r.min_bitrate_bps < r.start_bitrate_bps && r.start_bitrate_bps < r.max_bitrate_bps);
    assert(r.max_bitrate_bps==c.bitrate_bps);
    assert(media_governor_apply_transport_bitrate_target(0,&changed)==ESP_ERR_INVALID_ARG);
    now_tick=start_tick;
    assert(media_governor_apply_transport_bitrate_target(1,&changed)==ESP_OK && changed);
    assert(current().bitrate_bps==r.min_bitrate_bps);
    assert(current().width==c.width && current().height==c.height && current().fps==c.fps);
    assert(media_governor_apply_transport_bitrate_target(UINT32_MAX,&changed)==ESP_OK && !changed);
    now_tick+=APP_MEDIA_TGMP_RECOVERY_HOLD_MS-1;
    assert(media_governor_step_transport_adaptation(&changed)==ESP_OK && !changed);
    now_tick++;
    assert(media_governor_step_transport_adaptation(&changed)==ESP_OK && changed);
    uint32_t previous=current().bitrate_bps;
    assert(previous>r.min_bitrate_bps && previous<r.max_bitrate_bps);
    for(unsigned i=0;current().bitrate_bps!=r.max_bitrate_bps && i<100;i++) {
        now_tick+=APP_MEDIA_TGMP_RECOVERY_INTERVAL_MS-1;
        assert(media_governor_step_transport_adaptation(&changed)==ESP_OK && !changed);
        now_tick++;
        assert(media_governor_step_transport_adaptation(&changed)==ESP_OK && changed);
        assert(current().bitrate_bps>previous && current().bitrate_bps<=r.max_bitrate_bps);
        previous=current().bitrate_bps;
    }
    assert(current().bitrate_bps==r.max_bitrate_bps);
    assert(current().width==c.width && current().height==c.height && current().fps==c.fps);
    assert(media_governor_apply_transport_bitrate_target(r.min_bitrate_bps,&changed)==ESP_OK && changed);
    assert(media_governor_reset_transport_adaptation(true,&changed)==ESP_OK && changed);
    assert(!media_governor_transport_adaptation_active() && current().bitrate_bps==c.bitrate_bps);
    now_tick+=60000;
    assert(media_governor_step_transport_adaptation(&changed)==ESP_OK && !changed);
}
int main(void) {
    assert(media_governor_init()==ESP_OK && !media_governor_auto_adaptation_enabled());
    test_profile(false,10000);
    test_profile(true,10000);
    test_profile(false,UINT32_MAX-1000);
    puts("PASS: real governor floors, strict ranges, immediate protection, 5s hold, 3s recovery steps, clock wrap, session reset; dimensions/fps preserved");
}
'''
with tempfile.TemporaryDirectory(prefix='p4-governor-') as tmp:
    p=Path(tmp)
    (p/'freertos').mkdir()
    (p/'esp_err.h').write_text('#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_ERR_INVALID_ARG -1\nstatic inline const char *esp_err_to_name(int e){(void)e;return "stub";}\n')
    (p/'esp_check.h').write_text('#define ESP_RETURN_ON_FALSE(c,e,...) do {if(!(c))return (e);}while(0)\n')
    (p/'esp_log.h').write_text('#include <stdio.h>\n#define ESP_LOGI(tag,...) do{(void)(tag);if(0)printf(__VA_ARGS__);}while(0)\n#define ESP_LOGW ESP_LOGI\n')
    (p/'sdkconfig.h').write_text('#define CONFIG_APP_RTC_VIDEO_AUTO_ADAPT_ENABLE 0\n')
    (p/'freertos/FreeRTOS.h').write_text('#pragma once\n#include <stdint.h>\ntypedef uint32_t TickType_t;\ntypedef int portMUX_TYPE;\n#define portMUX_INITIALIZER_UNLOCKED 0\n#define taskENTER_CRITICAL(x) ((void)(x))\n#define taskEXIT_CRITICAL(x) ((void)(x))\n#define pdMS_TO_TICKS(x) (x)\n')
    (p/'freertos/portmacro.h').write_text('#include "FreeRTOS.h"\n')
    (p/'freertos/task.h').write_text('#include "FreeRTOS.h"\nextern uint32_t now_tick;\nstatic inline TickType_t xTaskGetTickCount(void){return now_tick;}\n')
    (p/'test.c').write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    '-I'+str(p),'-I'+str(root/'main/media'),str(root/'main/media/media_governor.c'),
                    str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
