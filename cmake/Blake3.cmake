include(CheckCCompilerFlag)

set(_blake3_dir "${PROJECT_SOURCE_DIR}/3rdparty/BLAKE3/c")
set(_blake3_sources
    "${_blake3_dir}/blake3.c"
    "${_blake3_dir}/blake3_dispatch.c"
    "${_blake3_dir}/blake3_portable.c"
)
set(_blake3_definitions)

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _blake3_processor)
if(_blake3_processor MATCHES "^(x86_64|amd64|i[3-6]86)$")
    set(_blake3_x86_sources
        blake3_sse2.c
        blake3_sse41.c
        blake3_avx2.c
        blake3_avx512.c
    )
    foreach(_source IN LISTS _blake3_x86_sources)
        list(APPEND _blake3_sources "${_blake3_dir}/${_source}")
    endforeach()

    check_c_compiler_flag(-msse2 NEOTAPE_HAS_MSSE2)
    check_c_compiler_flag(-msse4.1 NEOTAPE_HAS_MSSE41)
    check_c_compiler_flag(-mavx2 NEOTAPE_HAS_MAVX2)
    check_c_compiler_flag("-mavx512f -mavx512vl" NEOTAPE_HAS_MAVX512)
    if(NOT NEOTAPE_HAS_MSSE2 OR NOT NEOTAPE_HAS_MSSE41 OR
       NOT NEOTAPE_HAS_MAVX2 OR NOT NEOTAPE_HAS_MAVX512)
        message(FATAL_ERROR "The selected x86 BLAKE3 implementation requires SSE2, SSE4.1, AVX2, and AVX-512 compiler support")
    endif()
    set_source_files_properties("${_blake3_dir}/blake3_sse2.c" PROPERTIES COMPILE_OPTIONS -msse2)
    set_source_files_properties("${_blake3_dir}/blake3_sse41.c" PROPERTIES COMPILE_OPTIONS -msse4.1)
    set_source_files_properties("${_blake3_dir}/blake3_avx2.c" PROPERTIES COMPILE_OPTIONS -mavx2)
    set_source_files_properties("${_blake3_dir}/blake3_avx512.c" PROPERTIES COMPILE_OPTIONS "-mavx512f;-mavx512vl")
elseif(_blake3_processor MATCHES "^(aarch64|arm64)$")
    list(APPEND _blake3_sources "${_blake3_dir}/blake3_neon.c")
else()
    list(APPEND _blake3_definitions
        BLAKE3_NO_SSE2
        BLAKE3_NO_SSE41
        BLAKE3_NO_AVX2
        BLAKE3_NO_AVX512
        BLAKE3_USE_NEON=0
    )
endif()

add_library(neotape_blake3 STATIC ${_blake3_sources})
add_library(NeoTape::Blake3 ALIAS neotape_blake3)
target_include_directories(neotape_blake3 PUBLIC "${_blake3_dir}")
target_compile_definitions(neotape_blake3 PRIVATE ${_blake3_definitions})

unset(_blake3_dir)
unset(_blake3_sources)
unset(_blake3_definitions)
unset(_blake3_processor)
unset(_blake3_x86_sources)
