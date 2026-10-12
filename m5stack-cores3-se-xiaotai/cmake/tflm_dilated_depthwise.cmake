function(starter_patch_tflm_depthwise source output)
    file(READ "${source}" original)
    string(REPLACE "\r\n" "\n" original "${original}")
    string(SHA256 source_hash "${original}")
    if(NOT source_hash STREQUAL "1d4078ea7249c3792508d3ee9983283061833417da0ceec0f24f1d4e57a89520")
        message(FATAL_ERROR "TFLM depthwise adapter changed; review the dilated fast path")
    endif()
    string(REPLACE "#include <esp_nn.h>" "#include <esp_nn.h>\n#include \"wake_dilated_depthwise.h\"" patched "${original}")
    string(REPLACE "  int buffer_idx;" "  int buffer_idx;\n  size_t polyphase_bytes;" patched "${patched}")
    string(REPLACE "inline void EvalQuantizedPerChannel(" "inline TfLiteStatus EvalQuantizedPerChannel(" patched "${patched}")
    set(fast_eval [=[
  if (data.polyphase_bytes) {
    const auto input_shape = tflite::micro::GetTensorShape(input);
    const auto output_shape = tflite::micro::GetTensorShape(output);
    const bool ok = starter_wake::DilatedDepthwise(
        tflite::micro::GetTensorData<int8_t>(input),
        tflite::micro::GetTensorData<int8_t>(filter),
        tflite::micro::GetOptionalTensorData<int32_t>(bias),
        data.op_data.per_channel_output_multiplier,
        data.op_data.per_channel_output_shift,
        tflite::micro::GetTensorData<int8_t>(output),
        input_shape.Dims(1), output_shape.Dims(1), input_shape.Dims(3),
        dilation_height_factor, -data.op_data.input_zero_point,
        data.op_data.output_zero_point, data.op_data.output_activation_min,
        data.op_data.output_activation_max,
        context->GetScratchBuffer(context, data.buffer_idx), data.polyphase_bytes);
    if (!ok) return kTfLiteError;
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
      data->polyphase_bytes = starter_wake::DilatedWorkspaceBytes(
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

function(starter_enable_tflm_dilated_depthwise)
    idf_component_get_property(tflm_dir espressif__esp-tflite-micro COMPONENT_DIR)
    idf_component_get_property(tflm_lib espressif__esp-tflite-micro COMPONENT_LIB)
    set(original "${tflm_dir}/tensorflow/lite/micro/kernels/esp_nn/depthwise_conv.cc")
    set(generated "${CMAKE_BINARY_DIR}/starter_tflm/depthwise_conv.cc")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/starter_tflm")
    starter_patch_tflm_depthwise("${original}" "${generated}")
    get_target_property(sources ${tflm_lib} SOURCES)
    list(FIND sources "${original}" source_index)
    if(source_index EQUAL -1)
        message(FATAL_ERROR "TFLM depthwise source registration changed")
    endif()
    list(REMOVE_ITEM sources "${original}")
    list(APPEND sources "${generated}")
    set_property(TARGET ${tflm_lib} PROPERTY SOURCES "${sources}")
    target_include_directories(${tflm_lib} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/components/starter_voice/src")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${original}")
endfunction()
