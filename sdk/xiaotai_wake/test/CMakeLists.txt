# SPDX-License-Identifier: MIT
# Host tests of the wake-word engine.
set(internal "${CMAKE_CURRENT_SOURCE_DIR}/../src")

# W1: the window and the verdict (src/wake_window.h), the capture hand-off
# (src/wake_queue.h); header-only, nothing linked but the port (threads).
xiaotai_host_test(wake_window SOURCES window_test.c INCLUDE "${internal}")
xiaotai_host_test(wake_queue SOURCES queue_test.c INCLUDE "${internal}" LINK xiaotai_port)

# W1: the engine core built from source against fakes of the media tap, the
# tuning table, xiaotai_run and a scripted model, on a virtual clock.
xiaotai_host_test(wake_loop SOURCES loop_test.c ../src/wake.c fake_media_tap.c scripted_model.c
                  INCLUDE_ONLY xiaotai_wake INCLUDE "${internal}" LINK xiaotai_port m)

xiaotai_host_test(wake_mel SOURCES mel_test.c INCLUDE "${internal}" LINK xiaotai_wake xiaotai_port m)
xiaotai_host_test(wake_head SOURCES head_test.c INCLUDE "${internal}" LINK xiaotai_wake m)

set(at_test "${CMAKE_CURRENT_SOURCE_DIR}/../../xiaotai_at/test")
xiaotai_host_test(wake_diagnostics SOURCES diagnostics_test.c diagnostics_board.cc scripted_model.c
                  "${at_test}/at_test_help.c" INCLUDE "${internal}" "${at_test}" LINK xiaotai_wake)
target_link_options(wake_diagnostics PRIVATE "LINKER:--wrap=xiaotai_delay_ms")
if(XIAOTAI_HOST_SANITIZE)
    foreach(scenario absent raw)
        add_test(NAME wake_diagnostics_${scenario} COMMAND wake_diagnostics ${scenario})
        set_tests_properties(wake_diagnostics_${scenario} PROPERTIES LABELS xiaotai_wake TIMEOUT 20)
    endforeach()
    add_test(NAME wake_commands COMMAND wake_diagnostics commands)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME wake_commands_help COMMAND "${Python3_EXECUTABLE}" -B "${at_test}/at_test_help.py"
             --manifest "${CMAKE_CURRENT_SOURCE_DIR}/../engine.json" -- $<TARGET_FILE:wake_diagnostics> help)
    set_tests_properties(wake_commands wake_commands_help PROPERTIES LABELS xiaotai_wake TIMEOUT 20)
endif()

# Dispatch and cleanup for both target choices; DSP assembly is not run here.
foreach(kind portable optimized)
    xiaotai_host_test(wake_platform_fft_${kind} SOURCES platform_fft_test.c ../esp-idf/fft.c
                      INCLUDE "${internal}" stubs ../esp-idf LINK xiaotai_wake xiaotai_port)
endforeach()
target_compile_definitions(wake_platform_fft_portable PRIVATE CONFIG_XIAOTAI_WAKE_FFT_PORTABLE=1)
target_compile_definitions(wake_platform_fft_optimized PRIVATE CONFIG_XIAOTAI_WAKE_FFT_PORTABLE=0)
