# SPDX-License-Identifier: MIT
xiaotai_host_test(part_camera_dvp SOURCES camera_dvp_test.c LINK xiaotai_part_camera_dvp INCLUDE ../src ../host)
target_link_options(part_camera_dvp PRIVATE "LINKER:--wrap=xiaotai_mem_alloc" "LINKER:--wrap=xiaotai_delay_ms")
xiaotai_host_test(part_camera_dvp_raw_try_take SOURCES raw_try_take_test.c INCLUDE ../esp-idf)
xiaotai_host_test(part_camera_dvp_native_io SOURCES native_io_test.c ../esp-idf/camera_dvp_io.c
                 INCLUDE native_stubs ../src ../esp-idf INCLUDE_ONLY xiaotai_part_camera_dvp xiaotai_part_bus
                 xiaotai_hal xiaotai_port)
target_compile_definitions(part_camera_dvp_native_io PRIVATE XIAOTAI_DVP_USE_OVERLAY=1)
xiaotai_host_test(part_camera_dvp_stock_io SOURCES stock_io_test.c ../esp-idf/camera_dvp_io.c
                 INCLUDE native_stubs ../src INCLUDE_ONLY xiaotai_part_camera_dvp xiaotai_part_bus
                 xiaotai_hal LINK xiaotai_port)
target_link_options(part_camera_dvp_stock_io PRIVATE "LINKER:--wrap=xiaotai_task_create"
                    "LINKER:--wrap=xiaotai_task_exit" "LINKER:--wrap=xiaotai_time_us")
