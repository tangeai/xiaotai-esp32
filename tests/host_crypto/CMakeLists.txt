cmake_minimum_required(VERSION 3.16)
project(xiaotai_platform_crypto C)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(TF_PSA_CRYPTO_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
add_subdirectory("${PSA_SOURCE}" tfpsa)
# Espressif's builtin MPI source includes its compatibility header even when
# hardware acceleration is disabled. Host tests use the unmodified header.
target_include_directories(builtin BEFORE PRIVATE
    "${PSA_SOURCE}/../../port/include" "${CMAKE_CURRENT_SOURCE_DIR}/stubs")
add_executable(platform_hmac "${FIXTURE}")
target_link_libraries(platform_hmac PRIVATE tfpsacrypto)
