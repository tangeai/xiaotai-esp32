// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <esp_nn.h>

namespace xiaotai_wake {

inline size_t align16(size_t bytes) { return (bytes + 15U) & ~size_t{15U}; }

inline dw_conv_params_t unit_depthwise(int input_offset, int output_offset,
                                       int activation_min, int activation_max)
{
    return {input_offset, output_offset, 1, {1, 1}, {0, 0}, {1, 1}, {activation_min, activation_max}};
}

inline size_t dilated_workspace_bytes(int in_rows, int out_rows, int channels, int dilation)
{
    if (out_rows <= 0 || out_rows > 128 || channels <= 0 || channels > 64 ||
        dilation < 2 || dilation > 32 || in_rows != out_rows + 2 * dilation) return 0;
    const int padded = (channels + 15) & ~15;
    const int rows = (out_rows + dilation - 1) / dilation;
    const data_dims_t in = {1, rows + 2, padded, 1};
    const data_dims_t filter = {1, 3, padded, 0};
    const data_dims_t out = {1, rows, padded, 1};
    const auto params = unit_depthwise(0, 0, -128, 127);
    const int scratch = esp_nn_get_depthwise_conv_scratch_size(&in, &filter, &out, &params);
    if (scratch < 0) return 0;
    return 15U + align16((rows + 2) * padded) + align16(rows * padded) + align16(3 * padded) +
           3 * align16(padded * sizeof(int32_t)) + align16(static_cast<size_t>(scratch));
}

inline bool dilated_depthwise(const int8_t *input, const int8_t *weights, const int32_t *bias,
                              const int32_t *multiplier, const int32_t *shift, int8_t *output,
                              int in_rows, int out_rows, int channels, int dilation, int input_offset,
                              int output_offset, int activation_min, int activation_max,
                              void *workspace, size_t workspace_bytes)
{
    const size_t need = dilated_workspace_bytes(in_rows, out_rows, channels, dilation);
    if (!need || !input || !weights || !multiplier || !shift || !output ||
        !workspace || workspace_bytes < need) return false;
    const int padded = (channels + 15) & ~15;
    const int max_rows = (out_rows + dilation - 1) / dilation;
    auto *cursor = reinterpret_cast<uint8_t *>((reinterpret_cast<uintptr_t>(workspace) + 15U) & ~uintptr_t{15U});
    const auto take = [&cursor](size_t bytes) {
        auto *result = cursor;
        cursor += align16(bytes);
        return result;
    };
    auto *phase_input = reinterpret_cast<int8_t *>(take((max_rows + 2) * padded));
    auto *phase_output = reinterpret_cast<int8_t *>(take(max_rows * padded));
    auto *filter = reinterpret_cast<int8_t *>(take(3 * padded));
    auto *padded_bias = reinterpret_cast<int32_t *>(take(padded * sizeof(int32_t)));
    auto *padded_mult = reinterpret_cast<int32_t *>(take(padded * sizeof(int32_t)));
    auto *padded_shift = reinterpret_cast<int32_t *>(take(padded * sizeof(int32_t)));

    std::memset(filter, 0, 3 * padded);
    for (int k = 0; k < 3; ++k) std::memcpy(filter + k * padded, weights + k * channels, channels);
    std::memset(padded_bias, 0, padded * sizeof(int32_t));
    std::memset(padded_mult, 0, padded * sizeof(int32_t));
    std::memset(padded_shift, 0, padded * sizeof(int32_t));
    if (bias) std::memcpy(padded_bias, bias, channels * sizeof(int32_t));
    std::memcpy(padded_mult, multiplier, channels * sizeof(int32_t));
    std::memcpy(padded_shift, shift, channels * sizeof(int32_t));

    const data_dims_t filter_dims = {1, 3, padded, 0};
    const auto params = unit_depthwise(input_offset, output_offset, activation_min, activation_max);
    const quant_data_t quant = {padded_shift, padded_mult};
    // This scratch handle is global to esp-nn. The firmware has one inference owner.
    esp_nn_set_depthwise_conv_scratch_buf(cursor);
    // Group rows by remainder modulo dilation to run ordinary 3x1 convolution.
    for (int phase = 0; phase < dilation && phase < out_rows; ++phase) {
        const int rows = (out_rows - 1 - phase) / dilation + 1;
        for (int row = 0; row < rows + 2; ++row) {
            auto *dest = phase_input + row * padded;
            std::memcpy(dest, input + (phase + row * dilation) * channels, channels);
            if (channels < padded) std::memset(dest + channels, 0, padded - channels);
        }
        const data_dims_t in_dims = {1, rows + 2, padded, 1};
        const data_dims_t out_dims = {1, rows, padded, 1};
        esp_nn_depthwise_conv_s8(&in_dims, phase_input, &filter_dims, filter, padded_bias,
                                &out_dims, phase_output, &params, &quant);
        for (int row = 0; row < rows; ++row)
            std::memcpy(output + (phase + row * dilation) * channels, phase_output + row * padded, channels);
    }
    return true;
}

}  // namespace xiaotai_wake
