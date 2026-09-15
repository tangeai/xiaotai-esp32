"""Check the shipped KWS pair and tensor contract; not a wake-rate test.

Run in Linux/WSL with a C++17 compiler after ESP-IDF has downloaded TFLM.
"""
import hashlib
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
VOICE = ROOT / "components/starter_voice"
TFLM = ROOT / "managed_components/espressif__esp-tflite-micro"
docs = (ROOT / "docs/DEPENDENCIES.md").read_text(encoding="utf-8")
for name in ("head.h", "nihaoxiaotai.tflite"):
    path = VOICE / "model" / name
    expected = re.search(r"`" + re.escape(path.relative_to(ROOT).as_posix())
                         + r"` \| `([0-9a-f]{64})`", docs)
    assert expected, f"Missing documented hash: {name}"
    assert hashlib.sha256(path.read_bytes()).hexdigest() == expected[1], name

SOURCE = r'''
#include "tensorflow/lite/schema/schema_generated.h"
#include "head.h"
#include "kws_postprocess.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <vector>

static void shape(const tflite::Tensor *t, std::initializer_list<int> dims) {
    assert(t && t->type() == tflite::TensorType_INT8 && t->shape());
    assert(t->shape()->size() == dims.size());
    assert(std::equal(dims.begin(), dims.end(), t->shape()->begin()));
    auto q = t->quantization();
    assert(q && q->scale() && q->zero_point());
    assert(q->scale()->size() == 1 && q->zero_point()->size() == 1);
    assert(std::isfinite(q->scale()->Get(0)) && q->scale()->Get(0) > 0);
    assert(q->zero_point()->Get(0) >= -128 && q->zero_point()->Get(0) <= 127);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    std::ifstream file(argv[1], std::ios::binary);
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file), {}};
    flatbuffers::Verifier verifier(bytes.data(), bytes.size());
    assert(tflite::VerifyModelBuffer(verifier));
    auto m = tflite::GetModel(bytes.data());
    assert(m->version() == 3 && m->subgraphs()->size() == 1);
    auto g = m->subgraphs()->Get(0);
    assert(g->inputs()->size() == 1 && g->outputs()->size() == 1);
    auto in = g->tensors()->Get(g->inputs()->Get(0));
    auto out = g->tensors()->Get(g->outputs()->Get(0));
    shape(in, {1, 98, 32});
    shape(out, {1, KWS_HEAD_D});
    auto first = g->operators()->Get(0);
    assert(m->operator_codes()->Get(first->opcode_index())->builtin_code() == tflite::BuiltinOperator_RESHAPE);
    shape(g->tensors()->Get(first->outputs()->Get(0)), {1, 98, 1, 32});
    for (auto op : *m->operator_codes()) {
        switch (op->builtin_code()) {
            case tflite::BuiltinOperator_CONV_2D:
            case tflite::BuiltinOperator_DEPTHWISE_CONV_2D: assert(op->version() == 3); break;
            case tflite::BuiltinOperator_PAD:
            case tflite::BuiltinOperator_ADD:
            case tflite::BuiltinOperator_MEAN:
            case tflite::BuiltinOperator_STRIDED_SLICE:
            case tflite::BuiltinOperator_CONCATENATION: assert(op->version() == 2); break;
            case tflite::BuiltinOperator_RESHAPE: assert(op->version() == 1); break;
            default: assert(false && "Update and verify the firmware resolver");
        }
    }
    static_assert(KWS_HEAD_K == 5 && KWS_HEAD_D == 256);
    assert(std::isfinite(KWS_ABS_TEMP) && KWS_ABS_TEMP > 0 && std::isfinite(KWS_FC_B));
    for (int k = 0; k < KWS_HEAD_K; ++k) {
        assert(std::isfinite(KWS_FC_W[k]));
        double norm = 0;
        for (float v : KWS_PROTO_NORM[k]) { assert(std::isfinite(v)); norm += v * v; }
        assert(std::abs(norm - 1.0) < 0.0001);
    }
    float scale = in->quantization()->scale()->Get(0);
    int zero = in->quantization()->zero_point()->Get(0);
    float mel[98 * 32];
    int8_t quantized[98 * 32];
    for (int i = 0; i < 98 * 32; ++i) mel[i] = (i % 200 - 100) * 0.1f;
    manbo_kws_quantize_mel(mel, quantized, 98 * 32, scale, zero);
    for (int i = 0; i < 98 * 32; ++i)
        assert(quantized[i] == std::clamp(int(std::lrint(mel[i] / scale)) + zero, -128, 127));
    int8_t embedding[KWS_HEAD_D];
    scale = out->quantization()->scale()->Get(0);
    zero = out->quantization()->zero_point()->Get(0);
    std::fill_n(embedding, KWS_HEAD_D, zero);
    assert(std::abs(kws_postprocess(embedding, scale, zero) - 1.f/(1.f+std::exp(-KWS_FC_B))) < 1e-6f);
    for (int seed = 0; seed < 256; ++seed) {
        for (int i = 0; i < KWS_HEAD_D; ++i) embedding[i] = ((i + seed) % 256) - 128;
        float probability = kws_postprocess(embedding, scale, zero);
        assert(std::isfinite(probability) && probability >= 0 && probability <= 1);
    }
}
'''

with tempfile.TemporaryDirectory(prefix="p4-wake-model-") as directory:
    tmp = Path(directory)
    source = tmp / "check.cpp"
    source.write_text(SOURCE, encoding="utf-8")
    binary = tmp / "check"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-O1", "-UNDEBUG",
                    "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-I" + str(TFLM), "-I" + str(TFLM / "third_party/flatbuffers/include"),
                    "-I" + str(VOICE / "model"), "-I" + str(VOICE / "vendor"),
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary), str(VOICE / "model/nihaoxiaotai.tflite")], check=True)
print("PASS: model/head hashes, INT8 layout, resolver ops, quantization, head numerics")
