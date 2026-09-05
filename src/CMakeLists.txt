function(neotape_library target)
    add_library(${target} STATIC ${ARGN})
    target_include_directories(${target} PUBLIC "${PROJECT_SOURCE_DIR}/include")
    target_link_libraries(${target} PRIVATE neotape_build_options)
endfunction()

function(neotape_executable target output source)
    add_executable(${target} "${source}")
    set_target_properties(${target} PROPERTIES OUTPUT_NAME "${output}")
    target_link_libraries(${target} PRIVATE neotape_build_options ${ARGN})
endfunction()

neotape_library(neotape_common neotape_common.cpp neotape_progress.cpp)
target_link_libraries(neotape_common PUBLIC Threads::Threads)

neotape_library(neotape_plan_codec neotape_plan.cpp)

neotape_library(neotape_bounded_buffer neotape_bounded_buffer.cpp)

neotape_library(neotape_format neotape_format.cpp)
target_link_libraries(neotape_format PRIVATE NeoTape::Blake3 neotape_common)

neotape_library(neotape_socket neotape_socket_util.cpp)
target_link_libraries(neotape_socket PUBLIC neotape_protocol)

neotape_library(neotape_protocol neotape_tcp_protocol.cpp)

neotape_library(neotape_fec neotape_fec.cpp)
target_link_libraries(neotape_fec
    PUBLIC neotape_format
    PRIVATE NeoTape::Isal
)

neotape_library(neotape_signature neotape_signature.cpp)
target_link_libraries(neotape_signature
    PUBLIC neotape_format
    PRIVATE NeoTape::Signify neotape_common
)

neotape_library(neotape_frame_builder neotape_frame_builder.cpp)
target_link_libraries(neotape_frame_builder PUBLIC
    neotape_fec
    neotape_format
    neotape_signature
)

neotape_library(neotape_validation neotape_validate.cpp)
target_link_libraries(neotape_validation PUBLIC
    neotape_fec
    neotape_format
)

neotape_library(neotape_tape neotape_tape.cpp neotape_media.cpp)
target_link_libraries(neotape_tape PUBLIC
    neotape_common
    neotape_format
)

neotape_library(neotape_writer neotape_writer.cpp)
target_link_libraries(neotape_writer PUBLIC
    neotape_common
    neotape_tape
    neotape_validation
    neotape_signature
    neotape_socket
    neotape_protocol
)

neotape_library(neotape_pax neotape_pax_writer.cpp)
target_link_libraries(neotape_pax PUBLIC
    neotape_plan_codec
    neotape_bounded_buffer
    neotape_common
    NeoTape::Blake3
    LibArchive::LibArchive
    Threads::Threads
)

neotape_library(neotape_volume_server neotape_volume_server.cpp)
target_link_libraries(neotape_volume_server PUBLIC
    neotape_common
    neotape_fec
    neotape_format
    neotape_frame_builder
    neotape_protocol
    neotape_signature
    neotape_socket
    Threads::Threads
)

neotape_library(neotape_tcp_server neotape_tcp_server.cpp)
target_link_libraries(neotape_tcp_server PUBLIC
    neotape_common
    neotape_pax
    neotape_protocol
    neotape_signature
    neotape_socket
    neotape_volume_server
)

neotape_library(neotape_extractor_core neotape_extractor.cpp)
target_link_libraries(neotape_extractor_core PUBLIC
    neotape_common
    neotape_fec
    neotape_format
    neotape_protocol
    neotape_signature
    neotape_socket
    neotape_validation
)

neotape_executable(mt_pax mt-pax mt-pax.cpp
    neotape_pax
)
neotape_executable(neotape_plan neotape-plan neotape_plan_cmd.cpp
    neotape_plan_codec
    neotape_common
    neotape_format
)
neotape_executable(neotape_archiver neotape-archiver neotape_archiver_cmd.cpp
    neotape_tcp_server
)
neotape_executable(neotape_raw_store neotape-raw-store neotape_raw_store_cmd.cpp
    neotape_volume_server
)
neotape_executable(neotape_write neotape-write neotape_write_cmd.cpp
    neotape_writer
    neotape_common
    neotape_fec
    neotape_format
    neotape_protocol
    neotape_signature
    neotape_socket
    neotape_tape
    neotape_validation
    Threads::Threads
)
neotape_executable(neotape_read neotape-read neotape_read_cmd.cpp
    neotape_common
    neotape_format
    neotape_protocol
    neotape_socket
    neotape_tape
)
neotape_executable(neotape_extractor neotape-extractor neotape_extractor_cmd.cpp
    neotape_extractor_core
)
neotape_executable(neotape_inspect neotape-inspect neotape_inspect_cmd.cpp
    neotape_common
    neotape_fec
    neotape_format
    neotape_signature
    neotape_tape
    neotape_validation
)
neotape_executable(neotape_scan neotape-scan neotape_scan_cmd.cpp
    neotape_common
    neotape_format
    neotape_tape
)
neotape_executable(neotape_dump neotape-dump neotape_dump_cmd.cpp
    neotape_common
    neotape_format
    neotape_tape
)
