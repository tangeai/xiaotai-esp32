# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.16)
include("${CMAKE_CURRENT_LIST_DIR}/../esp-idf/raw_try_take_overlay.cmake")
xiaotai_dvp_generate_raw_try_take("${CAMERA_DIR}" result)
