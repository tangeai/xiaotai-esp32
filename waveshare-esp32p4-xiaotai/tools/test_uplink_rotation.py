#!/usr/bin/env python3
"""Exercise the actual PPA submission; no claim of hardware pixel verification."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "main/media/video_yuv420_scaler.c").read_text()
start = source.index("esp_err_t video_yuv420_scaler_process(")
function = source[start:source.index("\n}", start) + 2]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG -1
#define ESP_ERR_INVALID_SIZE -2
#define VIDEO_YUV420_SCALE_DENOMINATOR 16
#define PPA_SRM_COLOR_MODE_YUV420 1
#define PPA_SRM_ROTATION_ANGLE_0 0
#define PPA_SRM_ROTATION_ANGLE_90 1
#define PPA_TRANS_MODE_BLOCKING 0
typedef int esp_err_t;
typedef struct {uint16_t input_width,input_height,output_width,output_height; bool rotate_ccw90;} config_t;
typedef struct {config_t config; uint16_t crop_width,crop_height,crop_x,crop_y; unsigned scale_step;
 uint8_t *output_buffer; size_t output_buffer_size,output_data_len; int ppa_client;} scaler_t;
typedef scaler_t *video_yuv420_scaler_handle_t;
typedef struct {const void *buffer; size_t buffer_size; unsigned pic_w,pic_h,block_w,block_h,block_offset_x,block_offset_y,srm_cm;} picture_t;
typedef struct {picture_t in,out; unsigned rotation_angle,mode; float scale_x,scale_y;} ppa_srm_oper_config_t;
static bool rotated=true;
static int ppa_do_scale_rotate_mirror(int client,const ppa_srm_oper_config_t *op) {
 (void)client;
 assert(op->rotation_angle==(rotated?PPA_SRM_ROTATION_ANGLE_90:PPA_SRM_ROTATION_ANGLE_0));
 assert(op->scale_x==1 && op->scale_y==1);
 assert(op->in.block_w==1280 && op->in.block_h==960);
 assert(!op->in.block_offset_x && !op->in.block_offset_y);
 assert(op->out.pic_w==(rotated?960:1280) && op->out.pic_h==(rotated?1280:960));
 return 0;
}
static size_t video_yuv420_data_size(unsigned w,unsigned h) {return w*h*3/2;}
'''
code += function
code += r'''
int main(void) {
 uint8_t input=0,buf=0; const uint8_t *output=NULL; size_t length=0;
 scaler_t scaler={.config={1280,960,960,1280,true},.crop_width=1280,.crop_height=960,
 .scale_step=16,.output_buffer=&buf,.output_buffer_size=1843200,.output_data_len=1843200};
 assert(video_yuv420_scaler_process(&scaler,&input,1843200,&output,&length)==0);
 assert(output==&buf && length==1843200);
 rotated=false; scaler.config=(config_t){1280,960,1280,960,false};
 assert(video_yuv420_scaler_process(&scaler,&input,1843200,&output,&length)==0);
 assert(video_yuv420_scaler_process(&scaler,&input,10,&output,&length)==ESP_ERR_INVALID_SIZE);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="uplink-rotation-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(code)
    subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
pipeline = (root / "main/media/camera_pipeline.c").read_text()
assert ".rotate_ccw90 = rotate_ccw90" in pipeline
assert '"yuv420-ppa-ccw90"' in pipeline
print("PASS: actual PPA submission uses CCW90, full crop, 1:1 scale and exchanged axes; zero rotation preserved")
