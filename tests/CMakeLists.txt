if(BUILD_TESTING)
    find_package(Catch2 3 REQUIRED)
    include(Catch)

    function(neotape_unit_test target source)
        add_executable(${target} "${source}")
        target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/include")
        target_link_libraries(${target} PRIVATE
            neotape_build_options
            Catch2::Catch2WithMain
            ${ARGN}
        )
        catch_discover_tests(${target}
            TEST_PREFIX "unit."
            DISCOVERY_MODE PRE_TEST
            PROPERTIES LABELS unit TIMEOUT 30
        )
    endfunction()

    neotape_unit_test(test_pax_pipeline test_pax_pipeline.cpp Threads::Threads)
    neotape_unit_test(test_tcp_protocol test_tcp_protocol.cpp neotape_protocol)
    neotape_unit_test(test_format test_format.cpp neotape_format)
    neotape_unit_test(test_validate test_validate.cpp neotape_validation)
    neotape_unit_test(test_signature test_signature.cpp
        neotape_frame_builder neotape_fec neotape_signature)
    target_compile_definitions(test_signature PRIVATE
        NEOTAPE_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    )
    neotape_unit_test(test_fec test_fec.cpp
        neotape_fec neotape_frame_builder neotape_signature)

    add_library(neotape_test_support STATIC
        support/process.cpp
        support/temp_directory.cpp
    )
    target_include_directories(neotape_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(neotape_test_support PRIVATE
        neotape_build_options
        Threads::Threads
    )

    neotape_unit_test(test_stream_io test_stream_io.cpp
        neotape_tape neotape_plan_codec neotape_test_support)

    function(neotape_integration_test target source)
        add_executable(${target} "${source}")
        target_link_libraries(${target} PRIVATE
            neotape_build_options
            neotape_test_support
            Catch2::Catch2WithMain
        )
        target_compile_definitions(${target} PRIVATE
            NEOTAPE_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
            NEOTAPE_MT_PAX="$<TARGET_FILE:mt_pax>"
            NEOTAPE_PLAN="$<TARGET_FILE:neotape_plan>"
            NEOTAPE_ARCHIVER="$<TARGET_FILE:neotape_archiver>"
            NEOTAPE_RAW_STORE="$<TARGET_FILE:neotape_raw_store>"
            NEOTAPE_WRITE="$<TARGET_FILE:neotape_write>"
            NEOTAPE_READ="$<TARGET_FILE:neotape_read>"
            NEOTAPE_EXTRACTOR="$<TARGET_FILE:neotape_extractor>"
            NEOTAPE_INSPECT="$<TARGET_FILE:neotape_inspect>"
            NEOTAPE_SCAN="$<TARGET_FILE:neotape_scan>"
            NEOTAPE_DUMP="$<TARGET_FILE:neotape_dump>"
        )
        add_dependencies(${target}
            mt_pax neotape_plan neotape_archiver neotape_raw_store
            neotape_write neotape_read neotape_extractor neotape_inspect
            neotape_scan neotape_dump
        )
        set(_label integration)
        if(ARGC GREATER 2)
            set(_label "${ARGV2}")
        endif()
        catch_discover_tests(${target}
            TEST_PREFIX "integration."
            DISCOVERY_MODE PRE_TEST
            PROPERTIES LABELS "${_label}" TIMEOUT 240
        )
    endfunction()

    find_program(NEOTAPE_BSDTAR_EXECUTABLE NAMES bsdtar REQUIRED)
    find_program(NEOTAPE_DD_EXECUTABLE NAMES dd REQUIRED)

    neotape_integration_test(test_cli_integration test_cli_integration.cpp)
    neotape_integration_test(test_archive_integration test_archive_integration.cpp)
    target_link_libraries(test_archive_integration PRIVATE
        neotape_socket neotape_protocol neotape_format)
    target_compile_definitions(test_archive_integration PRIVATE
        NEOTAPE_BSDTAR="${NEOTAPE_BSDTAR_EXECUTABLE}"
    )

    neotape_integration_test(test_mt_pax_integration test_mt_pax_integration.cpp)
    target_link_libraries(test_mt_pax_integration PRIVATE neotape_pax)
    target_compile_definitions(test_mt_pax_integration PRIVATE
        NEOTAPE_BSDTAR="${NEOTAPE_BSDTAR_EXECUTABLE}"
        NEOTAPE_DD="${NEOTAPE_DD_EXECUTABLE}"
    )
    neotape_integration_test(test_restore_integration test_restore_integration.cpp)
    target_compile_definitions(test_restore_integration PRIVATE
        NEOTAPE_BSDTAR="${NEOTAPE_BSDTAR_EXECUTABLE}"
    )
    neotape_integration_test(test_recovery_integration test_recovery_integration.cpp)
    neotape_integration_test(test_signed_integration test_signed_integration.cpp)
    find_program(NEOTAPE_CMP_EXECUTABLE NAMES cmp REQUIRED)
    neotape_integration_test(test_bounded_memory_integration
        test_bounded_memory_integration.cpp "integration,large")
    target_compile_definitions(test_bounded_memory_integration PRIVATE
        NEOTAPE_CMP="${NEOTAPE_CMP_EXECUTABLE}"
    )
endif()

if(NEOTAPE_BUILD_BENCHMARKS)
    add_executable(benchmark_fec benchmark_fec.cpp)
    target_include_directories(benchmark_fec PRIVATE "${PROJECT_SOURCE_DIR}/include")
    target_link_libraries(benchmark_fec PRIVATE
        neotape_build_options
        neotape_common
        neotape_fec
        neotape_format
    )
endif()
