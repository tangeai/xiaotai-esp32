"""Compile the actual subscription callbacks with S3 and P4 feature guards."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "components/starter_tirtc/src/starter_tirtc.c").read_text()
callbacks = []
for name in ("on_subscribe_audio", "on_subscribe_video"):
    match = re.search(r"static int " + name + r"\([^\n]*\)\n\{.*?\n\}", source, re.S)
    assert match, name
    callbacks.append(match[0])
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#define ESP_LOGI(...) ((void)0)
#define H5_AUDIO_STREAM 10U
#define H5_VIDEO_STREAM 11U
#define AI_AUDIO_STREAM 1U
#define CALL_AUDIO_STREAM 10U
typedef uintptr_t tirtc_conn_t;
typedef enum { STARTER_TIRTC_NONE, STARTER_TIRTC_H5, STARTER_TIRTC_AI,
               STARTER_TIRTC_VOIP, STARTER_TIRTC_CALL, STARTER_TIRTC_ROOM } starter_tirtc_mode_t;
static atomic_int s_mode;
static atomic_bool s_audio_subscribed, s_video_subscribed;
static unsigned key_requests;
static bool connection_matches(tirtc_conn_t c) { return c == 7; }
static starter_tirtc_mode_t starter_tirtc_mode(void) { return atomic_load(&s_mode); }
static void on_request_key_frame(tirtc_conn_t c, uint8_t id) { (void)c; (void)id; ++key_requests; }
'''
main = r'''
int main(void) {
    for (int mode=STARTER_TIRTC_NONE; mode<=STARTER_TIRTC_ROOM; ++mode) {
        atomic_store(&s_mode, mode);
        for (unsigned id=0; id<256; ++id) {
            s_audio_subscribed=false;
            bool audio=((mode==STARTER_TIRTC_AI || mode==STARTER_TIRTC_ROOM) && id==AI_AUDIO_STREAM) ||
                ((mode==STARTER_TIRTC_H5 || mode==STARTER_TIRTC_CALL ||
                  mode==STARTER_TIRTC_VOIP) && id==H5_AUDIO_STREAM);
            assert(on_subscribe_audio(7,id)==(audio?0:-1));
            assert(s_audio_subscribed==audio);
            assert(on_subscribe_audio(8,id)==-1);
            s_video_subscribed=false;
            unsigned before=key_requests;
            bool video=CONFIG_IDF_TARGET_ESP32P4 && id==H5_VIDEO_STREAM &&
                (mode==STARTER_TIRTC_H5 || mode==STARTER_TIRTC_CALL || mode==STARTER_TIRTC_VOIP);
            assert(on_subscribe_video(7,id)==(video?0:-1));
            assert(s_video_subscribed==video);
            assert(key_requests==before+(video?1:0));
            assert(on_subscribe_video(8,id)==-1);
        }
    }
}
'''
with tempfile.TemporaryDirectory(prefix="s3-audio-only-") as tmp:
    path = Path(tmp)
    (path / "test.c").write_text(prefix + "\n".join(callbacks) + main)
    for p4 in (0, 1):
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                        "-Werror", "-Wno-unused-function", "-fsanitize=address,undefined",
                        f"-DCONFIG_IDF_TARGET_ESP32P4={p4}", str(path / "test.c"),
                        "-o", str(path / "test")], check=True)
        subprocess.run([str(path / "test")], check=True)
print("PASS: audio/video subscription matrix, stale connection rejection, P4 guard compatibility")
