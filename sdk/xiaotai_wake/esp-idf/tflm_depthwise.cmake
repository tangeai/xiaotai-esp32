# SPDX-License-Identifier: MIT
# Generate the optional variant without modifying the managed component.
function(xiaotai_wake_generate_depthwise source output)
    file(READ "${source}" original)
    string(REPLACE "\r\n" "\n" original "${original}")
    string(SHA256 original_sha "${original}")
    if(NOT original_sha STREQUAL "53a7a368b9544e1e3d00a609fabeb73a8415110e5b0a65e63f9211489f70883c")
        message(FATAL_ERROR "XiaoTai wake: locked TFLM depthwise source changed; review the fast path")
    endif()

    string(REPLACE "#include <esp_nn.h>" "#include <esp_nn.h>\n#include \"dilated_depthwise.h\"" patched "${original}")
    string(REPLACE "  int buffer_idx;" "  int buffer_idx;\n  size_t polyphase_bytes;" patched "${patched}")
    string(REPLACE "inline void EvalQuantizedPerChannel(" "inline TfLiteStatus EvalQuantizedPerChannel(" patched "${patched}")
    set(fast_eval [=[
  if (data.polyphase_bytes) {
    const auto in = tflite::micro::GetTensorShape(input);
    const auto out = tflite::micro::GetTensorShape(output);
    if (!xiaotai_wake::dilated_depthwise(
        tflite::micro::GetTensorData<int8_t>(input),
        tflite::micro::GetTensorData<int8_t>(filter),
        tflite::micro::GetOptionalTensorData<int32_t>(bias),
        data.op_data.per_channel_output_multiplier, data.op_data.per_channel_output_shift,
        tflite::micro::GetTensorData<int8_t>(output), in.Dims(1), out.Dims(1), in.Dims(3),
        dilation_height_factor, -data.op_data.input_zero_point, data.op_data.output_zero_point,
        data.op_data.output_activation_min, data.op_data.output_activation_max,
        context->GetScratchBuffer(context, data.buffer_idx), data.polyphase_bytes)) return kTfLiteError;
    return kTfLiteOk;
  }

]=])
    string(REPLACE "  if (dilation_width_factor == 1 && dilation_height_factor == 1) {"
        "${fast_eval}  if (dilation_width_factor == 1 && dilation_height_factor == 1) {" patched "${patched}")
    set(fast_prepare [=[
    data->polyphase_bytes = 0;
    if (filter->type == kTfLiteInt8 && input->dims->data[0] == 1 &&
        output->dims->data[0] == 1 && input_width == 1 && output_width == 1 &&
        filter_height == 3 && filter_width == 1 &&
        num_input_channels == num_filter_channels &&
        output->dims->data[3] == num_input_channels && params.depth_multiplier == 1 &&
        params.stride_width == 1 && params.stride_height == 1 &&
        data->op_data.padding.width == 0 && data->op_data.padding.height == 0 &&
        params.dilation_width_factor == 1) {
      data->polyphase_bytes = xiaotai_wake::dilated_workspace_bytes(
          input_height, output_height, num_input_channels, params.dilation_height_factor);
    }
    if (data->polyphase_bytes) {
      scratch_buf_size = static_cast<int>(data->polyphase_bytes);
    }
]=])
    string(REPLACE "    if (scratch_buf_size > 0) {" "${fast_prepare}\n    if (scratch_buf_size > 0) {" patched "${patched}")
    string(REPLACE "          EvalQuantizedPerChannel(context, node, params, data, input, filter, bias,\n                                  output);"
        "          TF_LITE_ENSURE_STATUS(EvalQuantizedPerChannel(context, node, params, data, input, filter, bias,\n                                  output));" patched "${patched}")
    string(REPLACE "\n}\n#endif\n\nTfLiteStatus Prepare" "\n  return kTfLiteOk;\n}\n#endif\n\nTfLiteStatus Prepare" patched "${patched}")
    file(WRITE "${output}.tmp" "${patched}")
    configure_file("${output}.tmp" "${output}" COPYONLY)
    file(REMOVE "${output}.tmp")
endfunction()

function(xiaotai_wake_enable_depthwise)
    idf_component_get_property(tflm_dir espressif__esp-tflite-micro COMPONENT_DIR)
    idf_component_get_property(tflm_lib espressif__esp-tflite-micro COMPONENT_LIB)
    set(original "${tflm_dir}/tensorflow/lite/micro/kernels/esp_nn/depthwise_conv.cc")
    set(directory "${CMAKE_BINARY_DIR}/xiaotai-wake")
    set(generated "${directory}/depthwise_conv.cc")

    get_target_property(sources ${tflm_lib} SOURCES)
    list(FIND sources "${original}" index)
    if(index EQUAL -1)
        message(FATAL_ERROR "XiaoTai wake: TFLM depthwise source registration changed")
    endif()
    file(MAKE_DIRECTORY "${directory}")
    xiaotai_wake_generate_depthwise("${original}" "${generated}")
    list(REMOVE_AT sources ${index})
    list(INSERT sources ${index} "${generated}")
    set_property(TARGET ${tflm_lib} PROPERTY SOURCES "${sources}")
    target_include_directories(${tflm_lib} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${original}")
endfunction()
