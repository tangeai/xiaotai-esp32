#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <esp_nn.h>

namespace starter_wake {

inline size_t Align16(size_t value) { return (value + 15u) & ~size_t(15u); }

inline bool DilatedShape(int input_rows, int output_rows, int channels,
                         int dilation) {
  return output_rows > 0 && output_rows <= 128 && channels > 0 &&
         channels <= 64 && dilation >= 2 && dilation <= 32 &&
         input_rows == output_rows + 2 * dilation;
}

inline dw_conv_params_t UnitDepthwise(int input_offset, int output_offset,
                                    int activation_min, int activation_max) {
  return {input_offset, output_offset, 1, {1, 1}, {0, 0}, {1, 1},
          {activation_min, activation_max}};
}

inline size_t DilatedWorkspaceBytes(int input_rows, int output_rows,
                                    int channels, int dilation) {
  if (!DilatedShape(input_rows, output_rows, channels, dilation)) return 0;
  const int padded = (channels + 15) & ~15;
  const int rows = (output_rows + dilation - 1) / dilation;
  const data_dims_t in = {1, rows + 2, padded, 1};
  const data_dims_t out = {1, rows, padded, 1};
  const data_dims_t filter = {1, 3, padded, 0};
  const auto params = UnitDepthwise(0, 0, -128, 127);
  const int scratch = esp_nn_get_depthwise_conv_scratch_size(
      &in, &filter, &out, &params);
  if (scratch < 0) return 0;
  // Every partition is aligned, including when the arena base is not.
  return 15 + Align16((rows + 2) * padded) + Align16(rows * padded) +
         Align16(3 * padded) + 3 * Align16(padded * sizeof(int32_t)) +
         Align16(static_cast<size_t>(scratch));
}

inline bool DilatedDepthwise(const int8_t* input, const int8_t* weights,
                            const int32_t* bias, const int32_t* multiplier,
                            const int32_t* shift, int8_t* output,
                            int input_rows, int output_rows, int channels,
                            int dilation, int input_offset, int output_offset,
                            int activation_min, int activation_max,
                            void* workspace, size_t workspace_bytes) {
  const size_t needed = DilatedWorkspaceBytes(input_rows, output_rows,
                                             channels, dilation);
  if (!needed || !input || !weights || !multiplier || !shift || !output ||
      !workspace || workspace_bytes < needed) return false;
  const int padded = (channels + 15) & ~15;
  const int max_rows = (output_rows + dilation - 1) / dilation;
  auto* cursor = reinterpret_cast<uint8_t*>(
      (reinterpret_cast<uintptr_t>(workspace) + 15u) & ~uintptr_t(15u));
  auto* gathered = reinterpret_cast<int8_t*>(cursor);
  cursor += Align16((max_rows + 2) * padded);
  auto* result = reinterpret_cast<int8_t*>(cursor);
  cursor += Align16(max_rows * padded);
  auto* filter = reinterpret_cast<int8_t*>(cursor);
  cursor += Align16(3 * padded);
  auto* padded_bias = reinterpret_cast<int32_t*>(cursor);
  cursor += Align16(padded * sizeof(int32_t));
  auto* padded_mult = reinterpret_cast<int32_t*>(cursor);
  cursor += Align16(padded * sizeof(int32_t));
  auto* padded_shift = reinterpret_cast<int32_t*>(cursor);
  cursor += Align16(padded * sizeof(int32_t));

  std::memset(filter, 0, 3 * padded);
  for (int k = 0; k < 3; ++k)
    std::memcpy(filter + k * padded, weights + k * channels, channels);
  std::memset(padded_bias, 0, padded * sizeof(int32_t));
  std::memset(padded_mult, 0, padded * sizeof(int32_t));
  std::memset(padded_shift, 0, padded * sizeof(int32_t));
  if (bias) std::memcpy(padded_bias, bias, channels * sizeof(int32_t));
  std::memcpy(padded_mult, multiplier, channels * sizeof(int32_t));
  std::memcpy(padded_shift, shift, channels * sizeof(int32_t));

  const data_dims_t filter_dims = {1, 3, padded, 0};
  const auto params = UnitDepthwise(input_offset, output_offset,
                                    activation_min, activation_max);
  const quant_data_t quant = {padded_shift, padded_mult};
  esp_nn_set_depthwise_conv_scratch_buf(cursor);

  // Y[y,c] uses X[y + k*d,c]. Partition by y%d so each phase is an
  // ordinary 3x1 convolution; scatter restores the exact original row order.
  for (int phase = 0; phase < dilation && phase < output_rows; ++phase) {
    const int rows = (output_rows - 1 - phase) / dilation + 1;
    for (int row = 0; row < rows + 2; ++row) {
      std::memcpy(gathered + row * padded,
                  input + (phase + row * dilation) * channels, channels);
      if (padded != channels)
        std::memset(gathered + row * padded + channels, 0, padded - channels);
    }
    const data_dims_t in_dims = {1, rows + 2, padded, 1};
    const data_dims_t out_dims = {1, rows, padded, 1};
    esp_nn_depthwise_conv_s8(&in_dims, gathered, &filter_dims, filter,
                             padded_bias, &out_dims, result, &params, &quant);
    for (int row = 0; row < rows; ++row)
      std::memcpy(output + (phase + row * dilation) * channels,
                  result + row * padded, channels);
  }
  return true;
}

}  // namespace starter_wake
