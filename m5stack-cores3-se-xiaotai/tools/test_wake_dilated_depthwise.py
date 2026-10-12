"""Exact dilated-row transform, actual vendor quantization, and bounds."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
NN = ROOT / "managed_components/espressif__esp-nn"
TFLM = ROOT / "managed_components/espressif__esp-tflite-micro"
PATCH = ROOT / "cmake/tflm_dilated_depthwise.cmake"
SOURCE = TFLM / "tensorflow/lite/micro/kernels/esp_nn/depthwise_conv.cc"

HARNESS = r'''
#include <cassert>
#include <cstdio>
#include <random>
#include <vector>
#include "wake_dilated_depthwise.h"
#include "tensorflow/lite/kernels/internal/reference/integer_ops/depthwise_conv.h"

int main() {
  std::mt19937 rng(250930);
  size_t checked=0;
  for (int trial=0; trial<1200; ++trial) {
    const int ch=trial%3==0 ? 24 : (trial%3==1 ? 32 : 48);
    const int d=trial%4==0 ? 2 : (trial%4==1 ? 4 : (trial%4==2 ? 8 : 32));
    const int rows=trial%2 ? 98 : 1+rng()%128;
    const int in_rows=rows+2*d;
    std::vector<int8_t> input(in_rows*ch), weights(3*ch), out(rows*ch+32, 71), ref(rows*ch);
    std::vector<int32_t> bias(ch), mult(ch), shift(ch);
    for (auto& v:input) v=static_cast<int8_t>(rng());
    for (auto& v:weights) v=static_cast<int8_t>(rng());
    for (int c=0;c<ch;++c) {
      bias[c]=static_cast<int>(rng()%20001)-10000;
      mult[c]=1073741824+rng()%1073741823;
      shift[c]=-1-static_cast<int>(rng()%12);
    }
    const int in_offset=static_cast<int>(rng()%256)-127;
    const int out_offset=static_cast<int>(rng()%256)-128;
    const int amin=trial%2 ? -128 : -35;
    const int amax=trial%2 ? 127 : 81;
    const size_t bytes=starter_wake::DilatedWorkspaceBytes(in_rows,rows,ch,d);
    assert(bytes>0 && bytes<20000);
    std::vector<unsigned char> workspace(bytes+48,0xab);
    const int align=trial%16;
    assert(!starter_wake::DilatedDepthwise(input.data(),weights.data(),bias.data(),
        mult.data(),shift.data(),out.data(),in_rows,rows,ch,d,in_offset,out_offset,
        amin,amax,workspace.data()+16+align,bytes-1));
    for (auto v:out) assert(v==71);
    assert(starter_wake::DilatedDepthwise(input.data(),weights.data(),bias.data(),
        mult.data(),shift.data(),out.data(),in_rows,rows,ch,d,in_offset,out_offset,
        amin,amax,workspace.data()+16+align,bytes));
    tflite::DepthwiseParams p={};
    p.stride_width=p.stride_height=p.dilation_width_factor=p.depth_multiplier=1;
    p.dilation_height_factor=d; p.input_offset=in_offset; p.output_offset=out_offset;
    p.quantized_activation_min=amin; p.quantized_activation_max=amax;
    const int32_t is[]={1,in_rows,1,ch}, fs[]={1,3,1,ch}, bs[]={ch}, os[]={1,rows,1,ch};
    tflite::reference_integer_ops::DepthwiseConvPerChannel(p,mult.data(),shift.data(),
        tflite::RuntimeShape(4,is),input.data(),tflite::RuntimeShape(4,fs),weights.data(),
        tflite::RuntimeShape(1,bs),bias.data(),tflite::RuntimeShape(4,os),ref.data());
    assert(!memcmp(out.data(),ref.data(),ref.size())); checked+=ref.size();
    for (size_t i=0;i<16+align;++i) assert(workspace[i]==0xab);
    for (size_t i=16+align+bytes;i<workspace.size();++i) assert(workspace[i]==0xab);
    for (size_t i=ref.size();i<out.size();++i) assert(out[i]==71);
  }
  assert(!starter_wake::DilatedWorkspaceBytes(102,98,24,1));
  assert(!starter_wake::DilatedWorkspaceBytes(102,98,65,2));
  assert(!starter_wake::DilatedWorkspaceBytes(103,98,24,2));
  printf("PASS: 1200 actual TFLM/ESP-NN comparisons, %zu bytes, all alignments, row order and guards\n",checked);
}
'''

with tempfile.TemporaryDirectory(prefix="wake-dilated-") as directory:
    path = Path(directory)
    generated = path / "depthwise_conv.cc"
    script = path / "generate.cmake"
    script.write_text(f'include("{PATCH.as_posix()}")\nstarter_patch_tflm_depthwise("{SOURCE.as_posix()}" "{generated.as_posix()}")\n')
    before = SOURCE.read_bytes()
    subprocess.run(["cmake", "-P", str(script)], check=True)
    adapted = generated.read_text()
    assert adapted.count("starter_wake::DilatedDepthwise(") == 1
    assert adapted.count("starter_wake::DilatedWorkspaceBytes(") == 1
    assert "reference_integer_ops::DepthwiseConvPerChannel(" in adapted
    assert "params.depth_multiplier == 1" in adapted
    assert "params.dilation_width_factor == 1" in adapted
    assert SOURCE.read_bytes() == before
    mutated = path / "changed.cc"
    mutated.write_bytes(before + b"\n// changed\n")
    script.write_text(f'include("{PATCH.as_posix()}")\nstarter_patch_tflm_depthwise("{mutated.as_posix()}" "{generated.as_posix()}")\n')
    drift = subprocess.run(["cmake", "-P", str(script)], capture_output=True, text=True)
    assert drift.returncode != 0 and "TFLM depthwise adapter changed" in drift.stderr
    (path / "test.cpp").write_text(HARNESS)
    flags = ["-O2", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    subprocess.run([os.environ.get("CC", "cc"), *flags,
                    "-I" + str(NN / "include"), "-I" + str(NN / "src/common"),
                    "-c", str(NN / "src/convolution/esp_nn_depthwise_conv_ansi.c"),
                    "-o", str(path / "nn.o")], check=True)
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++14", *flags,
                    "-DTF_LITE_STATIC_MEMORY", "-DTF_LITE_DISABLE_X86_NEON",
                    "-I" + str(NN / "include"), "-I" + str(TFLM),
                    "-I" + str(TFLM / "third_party/gemmlowp"),
                    "-I" + str(ROOT / "components/starter_voice/src"),
                    str(path / "test.cpp"), str(path / "nn.o"),
                    str(TFLM / "tensorflow/lite/kernels/internal/common.cc"),
                    "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
