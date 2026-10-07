cmake_minimum_required(VERSION 3.16)
project(capt-lbp810-service C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Determine if cross-compiling for Windows from Linux
if(CMAKE_SYSTEM_NAME STREQUAL "Windows" OR WIN32)
    set(TARGET_WINDOWS TRUE)
else()
    set(TARGET_WINDOWS FALSE)
endif()

# Common compile flags
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
    if(NOT TARGET_WINDOWS)
        add_compile_options(-fsanitize=address,undefined)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

# ============================================================
# Main service executable
# ============================================================
set(SERVICE_SOURCES
    src/main.c
    src/ipp_server.c
    src/pwg_raster.c
    src/scoa.c
    src/capt.c
    src/usb_comm.c
    src/status.c
    src/dither.c
    src/log.c
)

add_executable(capt-service ${SERVICE_SOURCES})

find_package(Threads REQUIRED)

if(TARGET_WINDOWS)
    target_link_libraries(capt-service PRIVATE winusb setupapi ws2_32 advapi32)
    target_compile_definitions(capt-service PRIVATE _WIN32_WINNT=0x0A00)
else()
    # On Linux, link math and pthreads for testing
    target_link_libraries(capt-service PRIVATE Threads::Threads m)
endif()

target_include_directories(capt-service PRIVATE src)

# ============================================================
# Tests (always buildable on Linux for development)
# ============================================================
enable_testing()

# SCoA compression round-trip tests
add_executable(test_scoa test/test_scoa.c src/scoa.c src/log.c)
target_include_directories(test_scoa PRIVATE src)
add_test(NAME test_scoa COMMAND test_scoa)

# PWG-Raster decoder + dithering tests
add_executable(test_pwg_dither test/test_pwg_dither.c src/pwg_raster.c src/dither.c src/log.c)
target_include_directories(test_pwg_dither PRIVATE src)
add_test(NAME test_pwg_dither COMMAND test_pwg_dither)

# CAPT protocol packet encoding + status parsing tests
add_executable(test_capt test/test_capt.c src/capt.c src/usb_comm.c src/status.c src/scoa.c src/log.c)
target_include_directories(test_capt PRIVATE src)
if(TARGET_WINDOWS)
    target_link_libraries(test_capt PRIVATE winusb setupapi)
    target_compile_definitions(test_capt PRIVATE _WIN32_WINNT=0x0A00)
endif()
add_test(NAME test_capt COMMAND test_capt)

# IPP server unit tests
add_executable(test_ipp test/test_ipp.c src/ipp_server.c src/log.c)
target_include_directories(test_ipp PRIVATE src)
if(TARGET_WINDOWS)
    target_link_libraries(test_ipp PRIVATE ws2_32)
    target_compile_definitions(test_ipp PRIVATE _WIN32_WINNT=0x0A00)
else()
    target_link_libraries(test_ipp PRIVATE Threads::Threads)
endif()
add_test(NAME test_ipp COMMAND test_ipp)

# ============================================================
# Cross-compilation toolchain hint
# ============================================================
# To cross-compile for Windows from Linux:
#   mkdir build-win && cd build-win
#   cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/mingw-w64-x86_64.cmake ..
#   make -j$(nproc)
