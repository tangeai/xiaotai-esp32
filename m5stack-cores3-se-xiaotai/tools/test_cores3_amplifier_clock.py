"""Exercise the actual duplex-clock adapter; hardware/audio proof is separate."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
media = (root / "components/starter_media/src/starter_media.c").read_text()

def body(name):
    return re.search(r"static esp_err_t " + name + r"\(void\)\n\{.*?\n\}", media, re.S)[0]

adapter = body("audio_amplifier_clock_init")
codecs = body("audio_codecs_init")
assert codecs.index("esp_codec_dev_open(s_microphone_dev") < codecs.index("audio_amplifier_clock_init()")
assert "return audio_amplifier_clock_init();" in codecs
assert not re.search(r"malloc|calloc|realloc|vTaskDelay", adapter)

prefix = r'''
#include <assert.h>
#include <stddef.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE -2
#define ESP_CODEC_DEV_OK 0
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
typedef struct codec codec_t;
struct codec {
    int (*get_reg)(const codec_t *, int, int *);
    int (*set_reg)(const codec_t *, int, int);
};
static int value, reads, writes, fail_read_at, fail_write, corrupt_readback;
static int get_reg(const codec_t *c, int reg, int *v) {
    (void)c; assert(reg == 6); ++reads;
    if (reads == fail_read_at) return -1;
    *v = value ^ ((corrupt_readback && reads == 2) ? 1 : 0);
    return 0;
}
static int set_reg(const codec_t *c, int reg, int v) {
    (void)c; assert(reg == 6); ++writes;
    if (fail_write) return -1;
    value = v; return 0;
}
static codec_t codec = {get_reg, set_reg};
static const codec_t *s_amp_codec_if = &codec;
static void reset(int v) {
    value=v; reads=writes=fail_read_at=fail_write=corrupt_readback=0;
    s_amp_codec_if=&codec; codec.get_reg=get_reg; codec.set_reg=set_reg;
}
'''
test = r'''
int main(void) {
    for (int v=0;v<=0xffff;++v) {
        reset(v);
        assert(audio_amplifier_clock_init()==ESP_OK);
        assert(value==((v & ~0x30) | 0x20));
        assert(reads==2 && writes==1);
    }
    reset(0x3c03);
    assert(audio_amplifier_clock_init()==ESP_OK && value==0x3c23);
    reset(0x3c03); fail_read_at=1;
    assert(audio_amplifier_clock_init()==ESP_FAIL && writes==0 && value==0x3c03);
    reset(0x3c03); fail_write=1;
    assert(audio_amplifier_clock_init()==ESP_FAIL && reads==1 && value==0x3c03);
    reset(0x3c03); fail_read_at=2;
    assert(audio_amplifier_clock_init()==ESP_FAIL);
    reset(0x3c03); corrupt_readback=1;
    assert(audio_amplifier_clock_init()==ESP_FAIL);
    reset(0x3c03); s_amp_codec_if=NULL;
    assert(audio_amplifier_clock_init()==ESP_ERR_INVALID_STATE && reads==0);
    reset(0x3c03); codec.get_reg=NULL;
    assert(audio_amplifier_clock_init()==ESP_ERR_INVALID_STATE && reads==0);
    reset(0x3c03); codec.set_reg=NULL;
    assert(audio_amplifier_clock_init()==ESP_ERR_INVALID_STATE && reads==0);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="core-amp-clock-") as temporary:
    path = Path(temporary)
    source = path / "test.c"
    source.write_text(prefix + adapter + test)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
                    str(source), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
print("PASS: CoreS3-SE 64fs amp clock, all 65536 register values preserved outside BCK, failures surfaced")
