// SPDX-License-Identifier: MIT
#include "fft.h"
#include "internal.h"
#include "model/postprocess.h"
#include "xiaotai/port/mem.h"

#include <cmath>
#include <cstring>
#include <new>
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

extern const uint8_t model_start[] asm("_binary_nihaoxiaotai_tflite_start");
extern const uint8_t model_end[] asm("_binary_nihaoxiaotai_tflite_end");

namespace {

constexpr size_t arena_bytes = 128U * 1024U;
constexpr size_t tensor_alignment = 16U;
constexpr size_t feature_count = WAKE_MEL_FRAMES * WAKE_MEL_BINS;
static_assert(WAKE_MEL_RATE == 16000U && WAKE_MEL_HOP == 160U && WAKE_MEL_FFT_SIZE == 512U);
static_assert(WAKE_MEL_SAMPLES == 16192U && WAKE_MEL_FRAMES == 98U && WAKE_MEL_BINS == 32U);
static_assert(WAKE_MODEL_EMBEDDING == 256U);

using Features = float[WAKE_MEL_BINS];

struct Model {
    uint8_t *bytes{};
    uint8_t *arena{};
    float *scratch{};
    Features *features{};
    wake_mel_stream_t *stream{};
    wake_mel_t mel{};
    tflite::MicroMutableOpResolver<8> ops;
    alignas(tflite::MicroInterpreter) unsigned char storage[sizeof(tflite::MicroInterpreter)];
    tflite::MicroInterpreter *interpreter{};
};

/* ESP-NN's depthwise scratch and ESP-DSP's plan are process-global. The wake
 * task is their single inference owner; do not add a second interpreter here. */
Model *active;

bool register_ops(tflite::MicroMutableOpResolver<8> &ops)
{
    return ops.AddConv2D() == kTfLiteOk && ops.AddDepthwiseConv2D() == kTfLiteOk &&
           ops.AddPad() == kTfLiteOk && ops.AddAdd() == kTfLiteOk && ops.AddReshape() == kTfLiteOk &&
           ops.AddStridedSlice() == kTfLiteOk && ops.AddMean() == kTfLiteOk && ops.AddConcatenation() == kTfLiteOk;
}

void destroy(Model *state)
{
    if (!state) return;
    if (state->interpreter) state->interpreter->~MicroInterpreter();
    xiaotai_mem_free(state->scratch);
    xiaotai_mem_free(state->stream);
    xiaotai_mem_free(state->features);
    xiaotai_mem_free(state->arena);
    xiaotai_mem_free(state->bytes);
    state->~Model();
    xiaotai_mem_free(state);
}

void model_close(void *handle)
{
    if (!handle || active != handle) return;
    Model *state = active;
    active = nullptr;
    destroy(state);
}

bool valid_tensor(const TfLiteTensor *tensor, size_t count, int dims, int dim1, int dim2)
{
    return tensor && tensor->data.int8 && tensor->type == kTfLiteInt8 && tensor->bytes == count &&
           tensor->dims && tensor->dims->size == dims && tensor->dims->data[0] == 1 &&
           tensor->dims->data[1] == dim1 && (dims == 2 || tensor->dims->data[2] == dim2) &&
           std::isfinite(tensor->params.scale) && tensor->params.scale > 0 &&
           tensor->params.zero_point >= INT8_MIN && tensor->params.zero_point <= INT8_MAX;
}

xiaotai_err_t initialize(Model *state, size_t size)
{
    state->bytes = static_cast<uint8_t *>(xiaotai_mem_aligned(XIAOTAI_MEM_BULK, tensor_alignment, size));
    if (!state->bytes) return XIAOTAI_ERR_NO_MEM;
    std::memcpy(state->bytes, model_start, size);
    flatbuffers::Verifier verifier(state->bytes, size);
    if (!tflite::VerifyModelBuffer(verifier)) return XIAOTAI_ERR_INVALID_ARG;
    const auto *model = tflite::GetModel(state->bytes);
    if (model->version() != TFLITE_SCHEMA_VERSION) return XIAOTAI_ERR_NOT_SUPPORTED;
    const auto *graphs = model->subgraphs();
    if (!graphs || !graphs->size() || !graphs->Get(0)->inputs() || graphs->Get(0)->inputs()->size() != 1 ||
        !graphs->Get(0)->outputs() || graphs->Get(0)->outputs()->size() != 1) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (!register_ops(state->ops)) return XIAOTAI_FAIL;

    state->arena = static_cast<uint8_t *>(xiaotai_mem_aligned(XIAOTAI_MEM_BULK, tensor_alignment, arena_bytes));
    state->features = static_cast<Features *>(xiaotai_mem_alloc(XIAOTAI_MEM_BULK, feature_count * sizeof(float)));
    state->stream = static_cast<wake_mel_stream_t *>(xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*state->stream)));
    state->scratch = static_cast<float *>(xiaotai_mem_aligned(XIAOTAI_MEM_BULK, WAKE_MEL_FFT_ALIGN,
                                                            WAKE_MEL_FFT_FLOATS * sizeof(float)));
    if (!state->arena || !state->features || !state->stream || !state->scratch) return XIAOTAI_ERR_NO_MEM;
    state->interpreter = new (state->storage) tflite::MicroInterpreter(model, state->ops, state->arena, arena_bytes);
    if (state->interpreter->AllocateTensors() != kTfLiteOk) return XIAOTAI_FAIL;
    if (!valid_tensor(state->interpreter->input(0), feature_count, 3, WAKE_MEL_FRAMES, WAKE_MEL_BINS) ||
        !valid_tensor(state->interpreter->output(0), WAKE_MODEL_EMBEDDING, 2, WAKE_MODEL_EMBEDDING, 0))
        return XIAOTAI_ERR_NOT_SUPPORTED;

    const xiaotai_err_t result = wake_platform_fft_prepare();
    if (result != XIAOTAI_OK) return result;
    return wake_mel_init(&state->mel, state->scratch, wake_platform_fft, nullptr) == 0 ? XIAOTAI_OK : XIAOTAI_FAIL;
}

xiaotai_err_t model_open(void **out)
{
    if (!out) return XIAOTAI_ERR_INVALID_ARG;
    if (active) return XIAOTAI_ERR_INVALID_STATE;
    *out = nullptr;
    const auto start = reinterpret_cast<uintptr_t>(model_start), end = reinterpret_cast<uintptr_t>(model_end);
    if (end <= start) return XIAOTAI_ERR_INVALID_ARG;
    /* This model was validated in PSRAM; don't consume contiguous internal
     * heap needed by transport on a product misconfigured without it. */
    if (!xiaotai_mem_has_external()) return XIAOTAI_ERR_NO_MEM;
    auto *storage = xiaotai_mem_aligned(XIAOTAI_MEM_BULK, alignof(Model), sizeof(Model));
    if (!storage) return XIAOTAI_ERR_NO_MEM;
    auto *state = new (storage) Model{};
    const auto result = initialize(state, end - start);
    if (result != XIAOTAI_OK) {
        destroy(state);
        return result;
    }
    active = state;
    *out = state;
    return XIAOTAI_OK;
}

xiaotai_err_t model_detect(void *handle, const int16_t *pcm, uint32_t stream, uint64_t end_sample,
                          float *probability, xiaotai_wake_timing_t *timing)
{
    if (timing) *timing = {};
    if (!handle || active != handle) return XIAOTAI_ERR_INVALID_STATE;
    if (!pcm || !probability) return XIAOTAI_ERR_INVALID_ARG;
    Model &state = *active;
    auto started = xiaotai_time_us();
    uint32_t reused = 0;
    if (!stream) state.stream->valid = false;
    const int transform = stream ? wake_mel_stream(&state.mel, state.stream, pcm, stream, end_sample,
                                                   state.features, &reused)
                                 : wake_mel_full(&state.mel, pcm, state.features);
    if (timing) timing->features_us = static_cast<uint32_t>(xiaotai_time_us() - started);
    if (transform != 0) return XIAOTAI_FAIL;
    if (timing) {
        timing->reused_frames = static_cast<uint16_t>(reused);
        timing->computed_frames = static_cast<uint16_t>(WAKE_MEL_FRAMES - reused);
    }
    started = xiaotai_time_us();
    const auto *in = state.interpreter->input(0);
    if (wake_model_quantize(&state.features[0][0], in->data.int8, feature_count, in->params.scale,
                            in->params.zero_point) != 0) return XIAOTAI_FAIL;
    const auto invoked = state.interpreter->Invoke();
    if (timing) timing->inference_us = static_cast<uint32_t>(xiaotai_time_us() - started);
    if (invoked != kTfLiteOk) return XIAOTAI_FAIL;
    started = xiaotai_time_us();
    const auto *out = state.interpreter->output(0);
    const int scored = wake_model_head(out->data.int8, out->params.scale, out->params.zero_point, probability);
    if (timing) timing->head_us = static_cast<uint32_t>(xiaotai_time_us() - started);
    return scored == 0 && std::isfinite(*probability) ? XIAOTAI_OK : XIAOTAI_FAIL;
}

const char *const phrases[] = {"\xe4\xbd\xa0\xe5\xa5\xbd\xe5\xb0\x8f\xe9\x92\x9b",
                               "\xe5\xb0\x8f\xe9\x92\x9b\xe5\xb0\x8f\xe9\x92\x9b",
                               "\xe5\xb0\x8f\xe9\x92\x9b\xe5\x90\x8c\xe5\xad\xa6", nullptr};
const xiaotai_wake_model_t builtin = {
    "nihaoxiaotai", phrases, WAKE_MEL_RATE, WAKE_MEL_SAMPLES, WAKE_MEL_HOP, 2560U, 12288U,
    model_open, model_detect, model_close,
};

}  // namespace

xiaotai_err_t wake_builtin_model_register(void)
{
    return xiaotai_wake_model_register(&builtin);
}
