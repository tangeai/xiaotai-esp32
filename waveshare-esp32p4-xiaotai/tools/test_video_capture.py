#!/usr/bin/env python3
"""Actual capture boundary logic + serial evidence integrity (host, not HIL)."""
from pathlib import Path
import subprocess
import tempfile
from capture_uplink import Dump, fnv

root = Path(__file__).resolve().parents[1]
source = (root / "components/p4_hardware/p4_video_capture.c").read_text()
def function(signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2]

code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>
#define CAPTURE_CAP 64
#define pdTRUE 1
static int s_mutex, available = 1;
static atomic_bool s_active, s_missed;
static uint8_t storage[CAPTURE_CAP], *s_data=storage;
static size_t s_size;
static uint32_t s_generation=1, s_frames;
static uint16_t s_width, s_height;
static int64_t now, s_deadline=5000000;
static bool s_started;
static const char *s_reason;
static int xSemaphoreTake(int m, int ticks) { (void)m; assert(!ticks); return available; }
static void xSemaphoreGive(int m) { (void)m; }
static int64_t esp_timer_get_time(void) { return now; }
'''
code += function("static bool has_headers_and_idr(")
code += function("void p4_video_capture_offer(")
code += r'''
static void reset(void) {
 s_active=true; s_missed=false; s_started=false; s_size=s_frames=0;
 s_deadline=5000000; now=0; available=1;
}
int main(void) {
 const uint8_t idr[]={0,0,0,1,0x67,1,0,0,1,0x68,2,0,0,0,1,0x65,3};
 const uint8_t p[]={0,0,1,0x41,4};
 assert(has_headers_and_idr(idr,sizeof(idr)));
 for(size_t i=0;i<16;++i) assert(!has_headers_and_idr(idr,i));
 reset();
 p4_video_capture_offer(p,sizeof(p),1280,960,false,1); assert(!s_size);
 p4_video_capture_offer(p,sizeof(p),1280,960,true,1); assert(!s_size);
 p4_video_capture_offer(idr,sizeof(idr),1280,960,true,1);
 assert(s_size==sizeof(idr) && s_frames==1 && s_width==1280 && s_height==960);
 assert(!memcmp(s_data,idr,sizeof(idr)));
 p4_video_capture_offer(p,sizeof(p),1280,960,false,1); assert(s_frames==2);
 now=2000000;
 p4_video_capture_offer(p,sizeof(p),1280,960,false,1);
 assert(!s_active && s_frames==2 && !strcmp(s_reason,"time-limit"));
 reset(); p4_video_capture_offer(idr,sizeof(idr),1280,960,true,2);
 assert(!s_active && !s_size);
 reset(); available=0; p4_video_capture_offer(idr,sizeof(idr),1280,960,true,1);
 available=1; p4_video_capture_offer(idr,sizeof(idr),1280,960,true,1);
 assert(!s_active && !s_size && !strcmp(s_reason,"contention"));
 reset(); p4_video_capture_offer(idr,sizeof(idr),1280,960,true,1);
 while(s_active) p4_video_capture_offer(p,sizeof(p),1280,960,false,1);
 assert(s_size<=CAPTURE_CAP && !strcmp(s_reason,"capacity"));
 reset(); p4_video_capture_offer(idr,sizeof(idr),1280,960,true,1);
 p4_video_capture_offer(p,sizeof(p),640,480,false,1);
 assert(!s_active && s_frames==1 && !strcmp(s_reason,"size-changed"));
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="video-capture-test-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(code)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)

data = b"\x00\x00\x01\x67test"
header = f"VCAP BEGIN bytes={len(data)} fnv={fnv(data):08x} mode=3 generation=1 width=1280 height=960 frames=1"
dump = Dump()
assert not dump.feed("unrelated runtime log")
dump.feed(header)
dump.feed("VCAP DATA 00000000 " + data.hex())
assert dump.feed("VCAP END") and dump.data == data
for line in ["VCAP DATA 00000001 " + data.hex(), "VCAP END", "VCAP DATA 00000000 zz"]:
    dump = Dump(); dump.feed(header)
    try:
        dump.feed(line)
    except ValueError:
        pass
    else:
        raise AssertionError("corrupted evidence was accepted")
print("PASS: actual capture SPS/PPS/IDR admission, bounds, timeout, generation, contention, size changes; serial integrity")
