"""Guard boot-owned capture and exercise the actual AGC adapter with API stubs.

This checks ownership, configuration and sample chronology, not DSP acoustics.
"""
from pathlib import Path
import math
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
media = (root / "components/starter_media/src/starter_media.c").read_text()
aec = (root / "components/starter_media/src/starter_aec.c").read_text()
app = (root / "main/app_main.c").read_text()

def body(source, name):
    return re.search(r"\b" + name + r"\([^\n]*\)\n\{.*?\n\}", source, re.S)[0]

boot = body(app, "app_main")
assert boot.index("starter_media_init()") < boot.index("wifi_manager_start()")
init = body(media, "starter_media_init")
assert init.index("starter_aec_init()") < init.index("audio_codecs_init()")
assert "starter_agc_init()" in body(aec, "starter_aec_init")
for worker in ("audio_input_task", "audio_capture_task", "audio_sink_task", "audio_uplink_task"):
    assert worker in init
assert "void starter_media_stop(void) { (void)stop_media(0); }" in media
stop = body(media, "stop_media")
assert not re.search(r"esp_codec_dev_close|i2s_channel_disable|starter_aec_(release|deinit)", stop)
assert "AFE_MEMORY_ALLOC_INTERNAL_PSRAM_BALANCE" in aec

# ADC headroom and post-AEC compensation form one gain budget, not a mute.
aec_header = (root / "components/starter_media/src/starter_aec.h").read_text()
pga = float(re.search(r"#define STARTER_AEC_MIC_PGA_DB ([\d.]+)f", aec_header)[1])
post = float(re.search(r"#define STARTER_AEC_OUTPUT_GAIN ([\d.]+)f", aec_header)[1])
assert pga == 24 and abs(pga + 20 * math.log10(post) - 30) < .001
codec = body(media, "audio_codecs_init")
assert len(re.findall(r"ESP_CODEC_DEV_MAKE_CHANNEL_MASK\([01]\),\s*STARTER_AEC_MIC_PGA_DB", codec)) == 2
assert re.search(r"ESP_CODEC_DEV_MAKE_CHANNEL_MASK\(2\),\s*0\.0f", codec)
assert "config->afe_linear_gain = STARTER_AEC_OUTPUT_GAIN" in aec
assert "config->aec_nlp_level = AEC_NLP_LEVEL_AGGR" in aec
assert "config->afe_perferred_core = 0" in aec

source = (root / "components/starter_media/src/starter_agc.c").read_text()
source = re.sub(r'^#include[^\n]*\n', '', source, flags=re.M)
prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "starter_agc_stream.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_NOT_SUPPORTED 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_INVALID_ARG 4
#define ESP_AGC_SUCCESS 0
#define AGC_MODE_2 2
#define AGC_MODE_3 3
#define EXT_RAM_BSS_ATTR
#define ESP_LOGI(...) ((void)0)
static struct { int mode, rate, gain, limiter, target; } state[2];
static int opened, closed, fail_allocation_at, internal_mode, fail_process_mode;
static void *esp_agc_open(int mode, int rate) {
    assert((mode==2 && rate==16000) || (mode==3 && rate==8000));
    ++opened;
    if (opened == fail_allocation_at) return NULL;
    state[mode-2].mode=mode; state[mode-2].rate=rate;
    return &state[mode-2];
}
static bool esp_ptr_external_ram(void *p) { return p != &state[internal_mode==3 ? 1 : 0] || internal_mode==0; }
static void esp_agc_close(void *p) { assert(p==&state[0] || p==&state[1]); ++closed; }
static void set_agc_config(void *p, int g, int l, int t) {
    int i=p==&state[0] ? 0 : 1;
    assert(p==&state[i]); state[i].gain=g; state[i].limiter=l; state[i].target=t;
}
static int esp_agc_process(void *p, int16_t *in, int16_t *out, int n, int rate) {
    int i=p==&state[0] ? 0 : 1;
    assert(p==&state[i] && rate==state[i].rate && n==rate/100 && in!=out);
    if (fail_process_mode==state[i].mode) return -1;
    for(int k=0;k<n;k++) out[k]=in[k];
    return 0;
}
void starter_agc_deinit(void);
'''
test = r'''
int main(void) {
    assert(starter_agc_init() == ESP_OK);
    assert(state[0].gain==CONFIG_XIAOTAI_CAPTURE_AGC_GAIN_DB && state[0].limiter==1 && state[0].target==6);
    assert(state[1].gain==6 && state[1].limiter==1 && state[1].target==3);
    assert(starter_agc_init() == ESP_OK && opened == 2);
    int16_t in[512], out[512];
    for (unsigned frame=0; frame<100; ++frame) {
        for (unsigned i=0; i<512; ++i) in[i]=(int16_t)(((frame*512+i)%40000)-20000);
        assert(starter_agc_process(in,out,512)==ESP_OK);
        for (unsigned i=0; i<512; ++i) {
            unsigned pos=frame*512+i;
            int expected=pos<160 ? 0 : (int)((pos-160)%40000)-20000;
            assert(out[i] == expected);
        }
    }
    starter_agc_discard_pending();
    assert(starter_agc_process(in,out,1)==ESP_OK && out[0]==0);
    assert(starter_agc_process(NULL,out,1)==ESP_ERR_INVALID_ARG);
    int16_t pcm[160], expected[160];
    for(int i=0;i<160;i++) pcm[i]=expected[i]=(int16_t)(i*300-22000);
    for(int i=0;i<100;i++) {
        assert(starter_agc_boost_uplink(pcm,160)==ESP_OK);
        assert(memcmp(pcm,expected,sizeof(pcm))==0);
    }
    assert(starter_agc_boost_uplink(pcm,79)==ESP_ERR_INVALID_ARG);
    assert(starter_agc_boost_uplink(NULL,160)==ESP_ERR_INVALID_ARG);
    assert(starter_agc_boost_uplink(pcm,0)==ESP_ERR_INVALID_ARG);
    fail_process_mode=3;
    assert(starter_agc_boost_uplink(pcm,160)==ESP_FAIL);
    assert(starter_agc_process(in,out,1)==ESP_OK);
    fail_process_mode=0;
    starter_agc_deinit();
    assert(closed==2);
    assert(starter_agc_process(in,out,1)==ESP_ERR_INVALID_STATE);
    assert(starter_agc_boost_uplink(pcm,160)==ESP_ERR_INVALID_STATE);
    internal_mode=2;
    assert(starter_agc_init()==ESP_ERR_NOT_SUPPORTED && closed==3);
    internal_mode=3;
    assert(starter_agc_init()==ESP_ERR_NOT_SUPPORTED && closed==5);
    internal_mode=0;
    fail_allocation_at=opened+2;
    assert(starter_agc_init()==ESP_ERR_NO_MEM && closed==6);
    fail_allocation_at=opened+1;
    assert(starter_agc_init()==ESP_ERR_NO_MEM && closed==6);
    fail_allocation_at=0;
    assert(starter_agc_init()==ESP_OK);
    starter_agc_deinit();
    assert(closed==8);
}
'''
with tempfile.TemporaryDirectory(prefix="audio-boot-gain-") as tmp:
    path=Path(tmp)
    (path/"test.c").write_text(prefix + source + test)
    for gain in (9, 15):
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-DCONFIG_IDF_TARGET_ESP32S3=1", f"-DCONFIG_XIAOTAI_CAPTURE_AGC_GAIN_DB={gain}",
                        "-I"+str(root/"components/starter_media/src"), str(path/"test.c"),
                        "-o", str(path/"test")], check=True)
        subprocess.run([str(path/"test")], check=True)
print("PASS: boot audio, balanced AFE, independent TX limiter, PSRAM failure cleanup and sample chronology")
