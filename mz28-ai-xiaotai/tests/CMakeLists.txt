cmake_minimum_required(VERSION 3.16)
project(voip_host_tests C)
enable_testing()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(VOIP "${CMAKE_CURRENT_LIST_DIR}/../components/wechat_voip")
set(JSON "$ENV{IDF_PATH}/components/json/cJSON")
if(NOT EXISTS "${JSON}/cJSON.c")
    message(FATAL_ERROR "Set IDF_PATH to ESP-IDF 5.5.5 for its cJSON source")
endif()
add_executable(test_state test_state.c "${VOIP}/voip_state.c")
add_executable(test_protocol test_protocol.c "${VOIP}/voip_protocol.c" "${JSON}/cJSON.c")
target_include_directories(test_protocol PRIVATE "${JSON}")
if(NOT MSVC)
    target_compile_options(test_state PRIVATE -Wall -Wextra -Werror)
    target_compile_options(test_protocol PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(test_protocol PRIVATE m)
endif()
add_test(NAME state COMMAND test_state)
add_test(NAME protocol COMMAND test_protocol)
add_executable(test_boot_button_logic test_boot_button_logic.c
    "${CMAKE_CURRENT_LIST_DIR}/../main/boot_button_logic.c")
target_include_directories(test_boot_button_logic PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../main")
if(NOT MSVC)
    target_compile_options(test_boot_button_logic PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME boot_button_logic COMMAND test_boot_button_logic)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    enable_language(ASM)
    add_executable(test_audio test_audio.c audio_fixture.S "${VOIP}/sample_audio.c" "${VOIP}/opus_packet.c")
    target_include_directories(test_audio PRIVATE stubs "${VOIP}"
        "${VOIP}/../rtc_adapter/include" "${VOIP}/../tirtc_sdk/include")
    target_compile_options(test_audio PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(test_audio PRIVATE m)
    add_test(NAME audio COMMAND test_audio)
    add_executable(test_rtc_inputs test_rtc_inputs.c
        "${VOIP}/../rtc_adapter/rtc_adapter.c")
    target_include_directories(test_rtc_inputs PRIVATE stubs
        "${VOIP}/../rtc_adapter/include" "${VOIP}/../tirtc_sdk/include")
    target_compile_options(test_rtc_inputs PRIVATE -Wall -Wextra -Werror
        -ffunction-sections -fdata-sections)
    target_link_options(test_rtc_inputs PRIVATE -Wl,--gc-sections)
    add_test(NAME rtc_inputs COMMAND test_rtc_inputs)
    add_executable(test_rtc_contract test_rtc_contract.c
        "${VOIP}/../rtc_adapter/rtc_adapter.c")
    target_include_directories(test_rtc_contract PRIVATE stubs
        "${VOIP}/../rtc_adapter/include" "${VOIP}/../tirtc_sdk/include")
    target_compile_options(test_rtc_contract PRIVATE -Wall -Wextra -Werror
        -ffunction-sections -fdata-sections)
    target_link_options(test_rtc_contract PRIVATE -Wl,--gc-sections)
    add_test(NAME rtc_contract COMMAND test_rtc_contract)
    add_executable(test_voip_reject test_voip_reject.c "${JSON}/cJSON.c")
    target_include_directories(test_voip_reject PRIVATE voip_stubs stubs "${JSON}"
        "${VOIP}/include" "${VOIP}/../demo_platform/include"
        "${VOIP}/../rtc_adapter/include" "${VOIP}/../tirtc_sdk/include")
    target_compile_options(test_voip_reject PRIVATE -Wall -Wextra -Werror
        -ffunction-sections -fdata-sections)
    target_link_options(test_voip_reject PRIVATE -Wl,--gc-sections)
    target_link_libraries(test_voip_reject PRIVATE m)
    add_test(NAME voip_reject COMMAND test_voip_reject)
    add_executable(test_voip_messages test_voip_messages.c)
    target_include_directories(test_voip_messages PRIVATE voip_stubs stubs "${JSON}"
        "${VOIP}/include" "${VOIP}/../demo_platform/include"
        "${VOIP}/../rtc_adapter/include" "${VOIP}/../tirtc_sdk/include")
    target_compile_options(test_voip_messages PRIVATE -Wall -Wextra -Werror
        -ffunction-sections -fdata-sections)
    target_link_options(test_voip_messages PRIVATE -Wl,--gc-sections)
    target_link_libraries(test_voip_messages PRIVATE pthread)
    add_test(NAME voip_messages COMMAND test_voip_messages)
    add_executable(test_voip_join_lifetime test_voip_join_lifetime.c
        "${VOIP}/voip_state.c" "${VOIP}/voip_protocol.c" "${JSON}/cJSON.c")
    target_include_directories(test_voip_join_lifetime PRIVATE voip_stubs stubs "${JSON}"
        "${VOIP}/include" "${VOIP}/../demo_platform/include"
        "${VOIP}/../rtc_adapter/include" "${VOIP}/../tirtc_sdk/include")
    target_compile_options(test_voip_join_lifetime PRIVATE -Wall -Wextra -Werror
        -ffunction-sections -fdata-sections)
    target_link_options(test_voip_join_lifetime PRIVATE -Wl,--gc-sections)
    target_link_libraries(test_voip_join_lifetime PRIVATE m)
    add_test(NAME voip_join_lifetime COMMAND test_voip_join_lifetime)
endif()
