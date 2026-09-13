#!/usr/bin/env python3
"""Actual profile, capture selection and scaler geometry for phone uplink."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
governor=(root/"main/media/media_governor.c").read_text()
scaler=(root/"main/media/video_yuv420_scaler.c").read_text()
def function(text, signature):
    start=text.index(signature)
    return text[start:text.index("\n}",start)+2]
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "media_tuning.h"
#define CONFIG_APP_RTC_VIDEO_AUTO_ADAPT_ENABLE 0
#define MEDIA_GOVERNOR_WEAK_NETWORK_OFF 0
#define MEDIA_GOVERNOR_WEAK_NETWORK_RESOLUTION_PRIORITY 1
#define MEDIA_GOVERNOR_COMPACT_CAPTURE_WIDTH 800U
#define MEDIA_GOVERNOR_COMPACT_CAPTURE_HEIGHT 640U
#define MEDIA_GOVERNOR_CAPTURE_WIDTH APP_MEDIA_CAMERA_CAPTURE_WIDTH
#define MEDIA_GOVERNOR_CAPTURE_HEIGHT APP_MEDIA_CAMERA_CAPTURE_HEIGHT
#define VIDEO_YUV420_SCALE_DENOMINATOR 16U
typedef struct {uint16_t width,height; uint8_t fps; uint32_t bitrate_bps; int weak_network_mode,weak_network_level,h264_min_qp,h264_max_qp;} media_governor_video_config_t;
typedef struct {uint16_t input_width,input_height,output_width,output_height; bool rotate_ccw90;} video_yuv420_scaler_config_t;
'''
code+=function(governor,"void media_governor_build_wechat_video_config(")
code+=function(governor,"static void media_governor_select_native_capture_size(")
code+=function(scaler,"static bool video_yuv420_select_geometry(")
code+=function(scaler,"static bool video_yuv420_config_valid(")
code+=r'''
int main(void) {
    media_governor_video_config_t config;
    media_governor_build_wechat_video_config(&config);
    assert(config.width==960 && config.height==1280 && config.fps==15 && config.bitrate_bps==2000000);
    uint16_t w=APP_MEDIA_CAMERA_CAPTURE_WIDTH,h=APP_MEDIA_CAMERA_CAPTURE_HEIGHT;
    media_governor_select_native_capture_size(&config,&w,&h);
    assert(w==1280 && h==960);
    video_yuv420_scaler_config_t scale={w,h,config.width,config.height,true};
    assert(video_yuv420_config_valid(&scale));
    uint16_t cw,ch,x,y; uint8_t step;
    assert(video_yuv420_select_geometry(&scale,&cw,&ch,&x,&y,&step));
    assert(cw==1280 && ch==960 && x==0 && y==0 && step==16);
    scale.rotate_ccw90=false;
    assert(!video_yuv420_config_valid(&scale));
    scale=(video_yuv420_scaler_config_t){800,640,384,256,false};
    assert(video_yuv420_config_valid(&scale));
    assert(video_yuv420_select_geometry(&scale,&cw,&ch,&x,&y,&step));
}
'''
with tempfile.TemporaryDirectory(prefix="full-frame-uplink-") as tmp:
    p=Path(tmp); (p/"test.c").write_text(code)
    subprocess.run(["cc","-Wall","-Wextra","-Werror","-I",str(root/"main/media"),str(p/"test.c"),"-o",str(p/"test")],check=True)
    subprocess.run([str(p/"test")],check=True)
video=(root/"components/p4_hardware/p4_video.c").read_text()
assert "mode == STARTER_TIRTC_VOIP || mode == STARTER_TIRTC_H5" in video
for config in ("sdkconfig.defaults", "sdkconfig"):
    assert "CONFIG_APP_RTC_H264_RESOURCE_FALLBACK_ENABLE=y" not in (root/config).read_text()
print("PASS: capture 1280x960 -> CCW90 full frame 960x1280@15 2Mbps; no crop/scale")
