/* Single-owner inference backend. No I2S, filesystem, MultiNet or session API. */
#include "wake_backend.h"
#include "mel_extractor.h"
#include "head.h"
#include "kws_postprocess.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include <cmath>
#include <cstring>
#include <new>

extern const uint8_t model_start[] asm("_binary_nihaoxiaotai_tflite_start");
extern const uint8_t model_end[] asm("_binary_nihaoxiaotai_tflite_end");
static constexpr size_t ARENA_BYTES = 128 * 1024;
static uint8_t *arena;
static uint8_t *model_bytes;
static float (*mel)[MEL_N_MELS];
static tflite::MicroMutableOpResolver<8> resolver;
alignas(tflite::MicroInterpreter) static uint8_t storage[sizeof(tflite::MicroInterpreter)];
static tflite::MicroInterpreter *interpreter;
static bool ops_ready;

void wake_backend_deinit(void)
{
    if (interpreter) { interpreter->~MicroInterpreter(); interpreter = nullptr; }
    heap_caps_free(arena); arena = nullptr;
    heap_caps_free(mel); mel = nullptr;
    heap_caps_free(model_bytes); model_bytes = nullptr;
}

esp_err_t wake_backend_init(void)
{
    if (interpreter) return ESP_ERR_INVALID_STATE;
    size_t model_size = model_end - model_start;
    // EMBED_FILES does not promise SIMD alignment. Own an aligned PSRAM copy.
    model_bytes = static_cast<uint8_t *>(heap_caps_aligned_alloc(16, model_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!model_bytes) return ESP_ERR_NO_MEM;
    std::memcpy(model_bytes, model_start, model_size);
    flatbuffers::Verifier verifier(model_bytes, model_size);
    if (!tflite::VerifyModelBuffer(verifier)) { wake_backend_deinit(); return ESP_ERR_INVALID_ARG; }
    const auto *model = tflite::GetModel(model_bytes);
    if (model->version() != TFLITE_SCHEMA_VERSION) { wake_backend_deinit(); return ESP_ERR_NOT_SUPPORTED; }
    if (!ops_ready) {
        if (resolver.AddConv2D() != kTfLiteOk || resolver.AddDepthwiseConv2D() != kTfLiteOk ||
            resolver.AddPad() != kTfLiteOk || resolver.AddAdd() != kTfLiteOk ||
            resolver.AddReshape() != kTfLiteOk || resolver.AddStridedSlice() != kTfLiteOk ||
            resolver.AddMean() != kTfLiteOk || resolver.AddConcatenation() != kTfLiteOk) {
            wake_backend_deinit(); return ESP_FAIL;
        }
        ops_ready = true;
    }
    // Never consume the contiguous internal heap reserved for TiRTC/TLS.
    arena = static_cast<uint8_t *>(heap_caps_aligned_alloc(16, ARENA_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    mel = static_cast<float (*)[MEL_N_MELS]>(heap_caps_malloc(sizeof(float) * MEL_TIME * MEL_N_MELS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!arena || !mel) { wake_backend_deinit(); return ESP_ERR_NO_MEM; }
    interpreter = new (storage) tflite::MicroInterpreter(model, resolver, arena, ARENA_BYTES);
    if (interpreter->AllocateTensors() != kTfLiteOk) { wake_backend_deinit(); return ESP_FAIL; }
    auto *in = interpreter->input(0);
    auto *out = interpreter->output(0);
    if (!in || !out || in->type != kTfLiteInt8 || out->type != kTfLiteInt8 ||
        in->dims->size != 3 || in->dims->data[0] != 1 || in->dims->data[1] != MEL_TIME ||
        in->dims->data[2] != MEL_N_MELS || in->bytes != MEL_TIME * MEL_N_MELS ||
        out->dims->size != 2 || out->dims->data[0] != 1 || out->dims->data[1] != KWS_HEAD_D ||
        out->bytes != KWS_HEAD_D || !std::isfinite(in->params.scale) || in->params.scale <= 0 ||
        !std::isfinite(out->params.scale) || out->params.scale <= 0) {
        wake_backend_deinit(); return ESP_ERR_NOT_SUPPORTED;
    }
    mel_extractor_init();
    ESP_LOGI("starter_voice", "TFLite v9.3-20260915-nhwc INT8 98x32 -> 256; model=%u arena=PSRAM %u used=%u",
             unsigned(model_size), unsigned(ARENA_BYTES), unsigned(interpreter->arena_used_bytes()));
    return ESP_OK;
}

esp_err_t wake_backend_run(const int16_t *pcm, float *probability)
{
    if (!interpreter || !pcm || !probability) return ESP_ERR_INVALID_STATE;
    if (mel_extract(pcm, mel) != 0) return ESP_FAIL;
    auto *in = interpreter->input(0);
    manbo_kws_quantize_mel(&mel[0][0], in->data.int8, MEL_TIME * MEL_N_MELS,
                          in->params.scale, in->params.zero_point);
    if (interpreter->Invoke() != kTfLiteOk) return ESP_FAIL;
    auto *out = interpreter->output(0);
    *probability = kws_postprocess(out->data.int8, out->params.scale, out->params.zero_point);
    return std::isfinite(*probability) ? ESP_OK : ESP_FAIL;
}
