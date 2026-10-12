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
    unsigned convolutions = 0;
    for (const auto *op : *g->operators()) {
        const auto code = m->operator_codes()->Get(op->opcode_index())->builtin_code();
        if (code != tflite::BuiltinOperator_CONV_2D &&
            code != tflite::BuiltinOperator_DEPTHWISE_CONV_2D) continue;
        assert(op->inputs()->size() >= 2 && op->outputs()->size() == 1);
        const auto *input = g->tensors()->Get(op->inputs()->Get(0));
        const auto *filter = g->tensors()->Get(op->inputs()->Get(1));
        const auto *output = g->tensors()->Get(op->outputs()->Get(0));
        assert(input->shape()->size() == 4 && filter->shape()->size() == 4 &&
               output->shape()->size() == 4);
        assert(input->type() == tflite::TensorType_INT8 &&
               filter->type() == tflite::TensorType_INT8 &&
               output->type() == tflite::TensorType_INT8);
        const int in_channels = input->shape()->Get(3);
        const int out_channels = output->shape()->Get(3);
        if (code == tflite::BuiltinOperator_CONV_2D) {
            assert(filter->shape()->Get(3) == in_channels);
            assert(filter->shape()->Get(0) == out_channels);
        } else {
            assert(filter->shape()->Get(0) == 1);
            assert(filter->shape()->Get(3) == out_channels);
            assert(out_channels == in_channels *
                op->builtin_options_as_DepthwiseConv2DOptions()->depth_multiplier());
        }
        ++convolutions;
    }
    assert(convolutions > 0);
    std::cout << "NHWC convolution channel contracts=" << convolutions
              << " model-bytes=" << data.size() << '\n';
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
    assert(std::isfinite(KWS_ABS_TEMP) && KWS_ABS_TEMP > 0 && std::isfinite(KWS_FC_B));
    for (int k = 0; k < KWS_HEAD_K; ++k) {
        assert(std::isfinite(KWS_FC_W[k]));
        float norm = 0;
        for (int i = 0; i < KWS_HEAD_D; ++i) {
            assert(std::isfinite(KWS_PROTO_NORM[k][i]));
            norm += KWS_PROTO_NORM[k][i] * KWS_PROTO_NORM[k][i];
        }
        assert(std::fabs(norm - 1.0f) < 0.001f);
    }
    data[4] = 0; flatbuffers::Verifier broken(data.data(), data.size());
    assert(!tflite::VerifyModelBuffer(broken));
    std::cout << "PASS: model schema/tensors/operator allowlist, corrupt-buffer rejection, head numeric smoke tests\n";
}
