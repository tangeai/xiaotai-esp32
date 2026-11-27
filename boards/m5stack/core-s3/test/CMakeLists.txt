# SPDX-License-Identifier: MIT
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(library "${XIAOTAI_ROOT}/sdk/parts")
xiaotai_host_test(board_cores3_parts SOURCES board_test.cc LINK xiaotai_board ${parts}
    INCLUDE "${generated}" "${library}/xiaotai_part_audio/host" "${library}/xiaotai_part_audio/src"
    "${library}/xiaotai_part_camera_dvp/host" "${library}/xiaotai_part_camera_dvp/src"
    "${library}/xiaotai_part_panel_spi/host" "${library}/xiaotai_part_panel_spi/src"
    "${library}/xiaotai_part_touch_ft5x06/src")
foreach(symbol xiaotai_audio_device_create xiaotai_es7210_create xiaotai_aw88298_create
               xiaotai_mem_has_external xiaotai_delay_ms xt_spi_bus_open xt_ft_io_open xt_ft_io_close)
    target_link_options(board_cores3_parts PRIVATE "LINKER:--wrap=${symbol}")
endforeach()

foreach(step RANGE 0 20)
    add_test(NAME board_cores3_init_failure_${step} COMMAND board_cores3_parts init ${step})
endforeach()
foreach(step RANGE 0 10)
    add_test(NAME board_cores3_display_failure_${step} COMMAND board_cores3_parts display ${step})
endforeach()
foreach(step RANGE 0 4)
    add_test(NAME board_cores3_camera_power_failure_${step} COMMAND board_cores3_parts camera-power ${step})
endforeach()
add_test(NAME board_cores3_display_latched COMMAND board_cores3_parts display-latched)
add_test(NAME board_cores3_display_touch_cleanup COMMAND board_cores3_parts display-touch-cleanup)
add_test(NAME board_cores3_display_owned COMMAND board_cores3_parts display-owned)
add_test(NAME board_cores3_audio_latched COMMAND board_cores3_parts audio-latched)
add_test(NAME board_cores3_audio_dma_failure COMMAND board_cores3_parts audio-dma-failure)
add_test(NAME board_cores3_audio_codec_failure COMMAND board_cores3_parts audio-codec-failure)
add_test(NAME board_cores3_partitions COMMAND "${Python3_EXECUTABLE}" -B -c
    "from pathlib import Path; import sys; assert Path(sys.argv[1]).read_bytes() == Path(sys.argv[2]).read_bytes()"
    "${CMAKE_CURRENT_SOURCE_DIR}/../partitions.csv"
    "${CMAKE_CURRENT_SOURCE_DIR}/legacy_partitions.csv")
