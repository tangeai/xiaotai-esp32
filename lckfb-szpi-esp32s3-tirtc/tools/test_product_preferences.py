"""Run the actual preference writer against failing NVS calls, without Flash."""
from pathlib import Path
import subprocess
import tempfile

from test_captive_portal_contract import function_body

root = Path(__file__).resolve().parents[1]
source = (root / "components/starter_product/src/starter_product.c").read_text(encoding="utf-8")
body = function_body(source, "preferences_write")
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
typedef int esp_err_t;
typedef unsigned nvs_handle_t;
#define ESP_OK 0
#define NVS_READWRITE 1
#define PRODUCT_NVS "product"
typedef struct {
    uint8_t volume;
    bool speaker_muted, microphone_muted, acknowledgement_male;
    uint8_t sleep_index, emoji_index;
} product_preferences_t;
static int open_error, fail_write, commit_error;
static unsigned writes, commits, closes;
static const char *keys[]={"volume","spk_mute","mic_mute","ack_voice","sleep","emoji"};
static const uint8_t values[]={8,1,0,1,2,4};
static int nvs_open(const char *name, int mode, nvs_handle_t *h) {
    assert(!strcmp(name,"product") && mode==NVS_READWRITE);
    *h=1; return open_error;
}
static int nvs_set_u8(nvs_handle_t h, const char *key, uint8_t value) {
    assert(h==1 && writes<6 && !strcmp(key,keys[writes]) && value==values[writes]);
    ++writes; return fail_write==(int)writes ? -2 : 0;
}
static int nvs_commit(nvs_handle_t h) { assert(h==1); ++commits; return commit_error; }
static void nvs_close(nvs_handle_t h) { assert(h==1); ++closes; }
static esp_err_t preferences_write(const product_preferences_t *snapshot) { BODY }
int main(void) {
    const product_preferences_t p={8,true,false,true,2,4};
    for (unsigned i=0;i<1000;++i) {
        writes=commits=closes=0;
        assert(preferences_write(&p)==0);
        assert(writes==6 && commits==1 && closes==1);
    }
    open_error=-1; writes=commits=closes=0;
    assert(preferences_write(&p)==-1 && !writes && !commits && !closes);
    open_error=0;
    for(int i=1;i<=6;++i) {
        writes=commits=closes=0; fail_write=i;
        assert(preferences_write(&p)==-2);
        assert(writes==(unsigned)i && commits==0 && closes==1);
    }
    fail_write=0; commit_error=-3; writes=commits=closes=0;
    assert(preferences_write(&p)==-3 && writes==6 && commits==1 && closes==1);
    puts("PASS: preference snapshots, six unchanged keys, 1000 saves, every NVS failure closes/preserves error");
}
'''.replace("BODY", body)
with tempfile.TemporaryDirectory(prefix="product-preferences-") as tmp:
    c = Path(tmp) / "test.c"
    exe = Path(tmp) / "test"
    c.write_text(harness, encoding="utf-8")
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
                    "-fsanitize=address,undefined", str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
