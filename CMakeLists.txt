cmake_minimum_required(VERSION 3.24)

project(NeoTape VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

option(NEOTAPE_NATIVE_ARCH "Optimize for the build machine CPU" OFF)
option(NEOTAPE_BUILD_BENCHMARKS "Build NeoTape benchmarks" OFF)

find_program(NEOTAPE_TAR_EXECUTABLE NAMES tar REQUIRED)
add_custom_target(bot_bundle
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/output"
    COMMAND "${NEOTAPE_TAR_EXECUTABLE}" -cf "${CMAKE_BINARY_DIR}/output/bot.tar"
            --format=pax
            --exclude-vcs
            --exclude=./build
            --exclude=./output
            --exclude=./compile_commands.json
            --exclude=*.o
            --exclude=*.a
            --exclude=*.d
            --exclude=*/target
            --exclude=.cache
            --exclude=.pi
            --exclude=.codex
            --exclude=.vscode
            --exclude=.worktrees
            .
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Creating ${CMAKE_BINARY_DIR}/output/bot.tar"
    VERBATIM
)

include(CTest)
find_package(LibArchive REQUIRED)
find_package(Threads REQUIRED)

add_library(neotape_build_options INTERFACE)
target_compile_options(neotape_build_options INTERFACE
    $<$<COMPILE_LANG_AND_ID:C,GNU,Clang>:-Wall;-Wextra;-Wpedantic;-pipe>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wall;-Wextra;-Wpedantic;-pipe>
)
if(NEOTAPE_NATIVE_ARCH)
    target_compile_options(neotape_build_options INTERFACE
        $<$<COMPILE_LANG_AND_ID:C,GNU,Clang>:-march=native>
        $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-march=native>
    )
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Blake3)
include(Isal)
include(Signify)

add_subdirectory(src)

if(BUILD_TESTING OR NEOTAPE_BUILD_BENCHMARKS)
    add_subdirectory(tests)
endif()
