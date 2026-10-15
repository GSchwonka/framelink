cmake_minimum_required(VERSION 3.16)

project(framelink
    VERSION 0.1.0
    DESCRIPTION "Reliable framed messaging over unreliable byte streams"
    HOMEPAGE_URL "https://github.com/GSchwonka/framelink"
    LANGUAGES C)

set(FL_TOP_LEVEL OFF)
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(FL_TOP_LEVEL ON)
endif()

option(FL_BUILD_TESTS "Build the unit tests and corpus replay tests" ${FL_TOP_LEVEL})
option(FL_CRC_TABLE   "Use the 512-byte CRC16 lookup table" OFF)
option(FL_SANITIZE    "Build everything with AddressSanitizer and UBSan" OFF)
option(FL_FUZZ        "Build libFuzzer harnesses (requires clang)" OFF)
option(FL_BUILD_EXAMPLES "Build example programs such as the TCP peer" OFF)
set(FL_MAX_PAYLOAD "" CACHE STRING "Override the maximum payload length, 0..255")

if(FL_SANITIZE)
    if(MSVC)
        message(FATAL_ERROR "FL_SANITIZE is only supported with GCC and Clang")
    endif()
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

if(FL_FUZZ)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "FL_FUZZ requires clang")
    endif()
    # Instrument everything for coverage. Harnesses add -fsanitize=fuzzer.
    add_compile_options(-fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=all -g)
    add_link_options(-fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------

add_library(framelink framelink/framelink.c)
add_library(framelink::framelink ALIAS framelink)
target_include_directories(framelink PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/framelink)
set_target_properties(framelink PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

if(FL_CRC_TABLE)
    target_compile_definitions(framelink PUBLIC FL_CRC_TABLE)
endif()
if(NOT FL_MAX_PAYLOAD STREQUAL "")
    target_compile_definitions(framelink PUBLIC FL_MAX_PAYLOAD=${FL_MAX_PAYLOAD})
endif()

# The library itself is held to the strictest warning level.
if(MSVC)
    target_compile_options(framelink PRIVATE /W4 /WX)
else()
    target_compile_options(framelink PRIVATE
        -Wall -Wextra -Wpedantic -Werror
        -Wconversion -Wsign-conversion -Wshadow -Wcast-qual
        -Wstrict-prototypes -Wmissing-prototypes -Wundef)
endif()

# ---------------------------------------------------------------------------
# Tests and fuzzing
# ---------------------------------------------------------------------------

if(FL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(FL_BUILD_TESTS OR FL_FUZZ)
    add_subdirectory(fuzz)
endif()

if(FL_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
