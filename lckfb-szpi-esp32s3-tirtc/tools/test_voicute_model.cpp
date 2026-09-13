// Host structural validation using the exact managed TFLM FlatBuffer schema.
// Does not execute inference or claim acoustic accuracy.
#include "tensorflow/lite/schema/schema_generated.h"
#include "head.h"
#include "kws_postprocess.h"
#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>
int main(int argc, char **argv) {
    assert(argc == 2);
    std::ifstream stream(argv[1], std::ios::binary);
    assert(stream.good());
    std::vector<uint8_t> data{std::istreambuf_iterator<char>(stream), {}};
    flatbuffers::Verifier verifier(data.data(), data.size());
    assert(tflite::VerifyModelBuffer(verifier));
    const auto *m = tflite::GetModel(data.data());
    assert(m->version() == 3 && m->subgraphs()->size() == 1);
    const auto *g = m->subgraphs()->Get(0);
    assert(g->inputs()->size() == 1 && g->outputs()->size() == 1);
    const auto *in = g->tensors()->Get(g->inputs()->Get(0));
    const auto *out = g->tensors()->Get(g->outputs()->Get(0));
    assert(in->type() == tflite::TensorType_INT8 && out->type() == tflite::TensorType_INT8);
    assert(in->shape()->size() == 3 && in->shape()->Get(0) == 1 &&
           in->shape()->Get(1) == 98 && in->shape()->Get(2) == 32);
    assert(out->shape()->size() == 2 && out->shape()->Get(0) == 1 && out->shape()->Get(1) == KWS_HEAD_D);
    for (const auto *t : {in, out}) {
        assert(t->quantization() && t->quantization()->scale()->size() == 1);
        assert(std::isfinite(t->quantization()->scale()->Get(0)) && t->quantization()->scale()->Get(0) > 0);
    }
    for (const auto *op : *m->operator_codes()) {
        auto code = op->builtin_code();
        switch (code) {
        case tflite::BuiltinOperator_CONV_2D: case tflite::BuiltinOperator_DEPTHWISE_CONV_2D:
        case tflite::BuiltinOperator_PAD: case tflite::BuiltinOperator_ADD:
        case tflite::BuiltinOperator_RESHAPE: case tflite::BuiltinOperator_STRIDED_SLICE:
        case tflite::BuiltinOperator_MEAN: case tflite::BuiltinOperator_CONCATENATION: break;
        default: std::cerr << "unsupported operator " << code << '\n'; return 1;
        }
        std::cout << tflite::EnumNameBuiltinOperator(code) << " version=" << op->version() << '\n';
    }
    // Numeric smoke tests for the real model-specific head and quantization helper.
    int8_t embedding[KWS_HEAD_D] = {};
    float p = kws_postprocess(embedding, .1f, 0);
    assert(std::isfinite(p) && p >= 0 && p <= 1);
    for (int i = 0; i < KWS_HEAD_D; ++i) embedding[i] = (i % 255) - 128;
    p = kws_postprocess(embedding, .1f, -4);
    assert(std::isfinite(p) && p >= 0 && p <= 1);
    float values[] = {-1000, 0, 1000}; int8_t quantized[3];
    manbo_kws_quantize_mel(values, quantized, 3, .1f, 4);
    assert(quantized[0] == -128 && quantized[1] == 4 && quantized[2] == 127);
    data[4] = 0; flatbuffers::Verifier broken(data.data(), data.size());
    assert(!tflite::VerifyModelBuffer(broken));
    std::cout << "PASS: model schema/tensors/operator allowlist, corrupt-buffer rejection, head numeric smoke tests\n";
}
