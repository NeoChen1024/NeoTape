find_program(NEOTAPE_MAKE_EXECUTABLE NAMES gmake make REQUIRED)

set(_isal_source_dir "${PROJECT_SOURCE_DIR}/3rdparty/isa-l")
set(_isal_build_dir "${CMAKE_BINARY_DIR}/_deps/isa-l")
set(_isal_library "${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}/libisal.a")

file(GLOB_RECURSE _isal_inputs CONFIGURE_DEPENDS
    "${_isal_source_dir}/*.asm"
    "${_isal_source_dir}/*.c"
    "${_isal_source_dir}/*.h"
    "${_isal_source_dir}/*.inc"
    "${_isal_source_dir}/*.mk"
    "${_isal_source_dir}/Makefile.unx"
    "${_isal_source_dir}/make.inc"
)

add_custom_command(
    OUTPUT "${_isal_library}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_isal_build_dir}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}"
    COMMAND "${NEOTAPE_MAKE_EXECUTABLE}"
            -C "${_isal_source_dir}"
            -f Makefile.unx
            "O=${_isal_build_dir}"
            "lib_name=${_isal_library}"
            "CC=${CMAKE_C_COMPILER}"
            lib
    DEPENDS ${_isal_inputs}
    WORKING_DIRECTORY "${_isal_source_dir}"
    COMMENT "Building bundled ISA-L"
    VERBATIM
)

add_custom_target(neotape_isal_build DEPENDS "${_isal_library}")
add_library(neotape_isal INTERFACE)
add_library(NeoTape::Isal ALIAS neotape_isal)
add_dependencies(neotape_isal neotape_isal_build)
target_include_directories(neotape_isal INTERFACE "${_isal_source_dir}/include")
target_link_libraries(neotape_isal INTERFACE "${_isal_library}")

unset(_isal_source_dir)
unset(_isal_build_dir)
unset(_isal_library)
unset(_isal_inputs)
