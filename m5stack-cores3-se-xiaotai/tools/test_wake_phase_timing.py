#!/usr/bin/env python3
"""Exercise production phase timing without changing model arithmetic."""
from pathlib import Path
import subprocess
import tempfile
from test_voice_reliability import function

ROOT = Path(__file__).resolve().parents[1]
source = r'''
#include <cassert>
#include <cstdint>
#include <cmath>
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_ERR_INVALID_STATE=1, ESP_FAIL=2, kTfLiteOk=0;
constexpr int MEL_TIME=98, MEL_N_MELS=32;
static int64_t clock_us;
static int64_t esp_timer_get_time() { return clock_us; }
struct wake_backend_timing_t { uint32_t mel_us, invoke_us, head_us, mel_reused_frames; };
static wake_backend_timing_t last_timing;
struct Tensor {
    struct { int8_t *int8; } data;
    struct { float scale; int zero_point; } params;
};
static int8_t input[3136], output[256];
static Tensor tensors[2]={{{input},{.1f,0}},{{output},{.2f,4}}};
static int invoke_result, mel_result, head_calls, quantize_calls;
static bool finite_result=true;
struct Interpreter {
    Tensor *input(int n) { assert(n==0); return &tensors[0]; }
    Tensor *output(int n) { assert(n==0); return &tensors[1]; }
    int Invoke() { clock_us+=200; return invoke_result; }
};
static Interpreter instance, *interpreter=&instance;
static float mel_data[98][32], (*mel)[32]=mel_data;
struct mel_stream_t { bool valid; };
static mel_stream_t stream_state, *mel_stream=&stream_state;
static int mel_extract(const int16_t *p, float (*)[32]) {
    assert(p); clock_us+=100; return mel_result;
}
static int mel_stream_extract(mel_stream_t *s,const int16_t *p,uint32_t e,uint64_t n,
                              float (*)[32],uint32_t *reused) {
    assert(s==mel_stream && p && e==3 && n==20000);
    *reused=82; clock_us+=100; return mel_result;
}
static void manbo_kws_quantize_mel(const float *,int8_t *p,int n,float s,int z) {
    assert(p==input && n==3136 && s==.1f && z==0);
    clock_us+=20; ++quantize_calls;
}
static float kws_postprocess(int8_t *p,float s,int z) {
    assert(p==output && s==.2f && z==4);
    clock_us+=50; ++head_calls;
    return finite_result ? .75f : NAN;
}
'''
source += "\n".join(function("components/starter_voice/src/wake_backend.cpp", name) for name in
                    ("wake_backend_infer", "wake_backend_run", "wake_backend_run_window"))
source += r'''
int main() {
    int16_t pcm[16192]={}; float probability=0;
    assert(wake_backend_run(pcm,&probability)==ESP_OK && probability==.75f);
    assert(clock_us==370 && quantize_calls==1 && head_calls==1);
    assert(last_timing.mel_us==100 && last_timing.invoke_us==220 && last_timing.head_us==50);
    assert(!last_timing.mel_reused_frames);
    mel_result=-1;
    assert(wake_backend_run(pcm,&probability)==ESP_FAIL);
    assert(last_timing.mel_us==100 && !last_timing.invoke_us && !last_timing.head_us);
    assert(quantize_calls==1 && head_calls==1);
    mel_result=0; invoke_result=-1;
    assert(wake_backend_run(pcm,&probability)==ESP_FAIL);
    assert(last_timing.mel_us==100 && last_timing.invoke_us==220 && !last_timing.head_us);
    assert(head_calls==1);
    invoke_result=0; finite_result=false;
    assert(wake_backend_run(pcm,&probability)==ESP_FAIL && !std::isfinite(probability));
    assert(last_timing.head_us==50 && head_calls==2);
    assert(wake_backend_run(nullptr,&probability)==ESP_ERR_INVALID_STATE);
    assert(!last_timing.mel_us && !last_timing.invoke_us && !last_timing.head_us);
    assert(wake_backend_run(pcm,nullptr)==ESP_ERR_INVALID_STATE);
    finite_result=true;
    assert(wake_backend_run_window(pcm,3,20000,&probability)==ESP_OK);
    assert(last_timing.mel_reused_frames==82 && last_timing.invoke_us==220);
    interpreter=nullptr;
    assert(wake_backend_run(pcm,&probability)==ESP_ERR_INVALID_STATE);
}
'''
with tempfile.TemporaryDirectory(prefix="cores3-wake-timing-") as directory:
    target = Path(directory)
    (target / "test.cpp").write_text(source)
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(target / "test.cpp"),
                    "-o", str(target / "test")], check=True)
    subprocess.run([str(target / "test")], check=True, timeout=10)
print("PASS: real wake phase timing preserves success, failure, and arithmetic paths")
