cmake_minimum_required(VERSION 3.20)

project(ld-compress-ng
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

option(LDCOMPRESS_ENABLE_OPENCL "Enable OpenCL device enumeration support" ON)
option(LDCOMPRESS_ENABLE_REAL_FIXTURE_TESTS "Enable opt-in tests against external real LDS fixtures" OFF)
option(LDCOMPRESS_ENABLE_FLAC_TEST_FILE_TESTS "Enable opt-in tests against external FLAC decoder testbench files" OFF)
set(LDCOMPRESS_REAL_FIXTURE_DIR "" CACHE PATH "Directory containing external .lds fixtures for opt-in real-fixture tests")
set(LDCOMPRESS_FLAC_TEST_FILE_DIR "" CACHE PATH "Directory containing external FLAC decoder testbench files")

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(FLAC REQUIRED IMPORTED_TARGET flac)
pkg_check_modules(OGG REQUIRED IMPORTED_TARGET ogg)

set(LDCOMPRESS_HAVE_OPENCL 0)
if(LDCOMPRESS_ENABLE_OPENCL)
    find_package(OpenCL QUIET)
    if(OpenCL_FOUND)
        set(LDCOMPRESS_HAVE_OPENCL 1)
        message(STATUS "OpenCL device enumeration enabled")
    else()
        message(STATUS "OpenCL not found; devices command will report unavailable")
    endif()
endif()

add_library(ldcompress_core
    src/compressor.cpp
    src/flac_codec.cpp
    src/flac_native_writer.cpp
    src/flac_primitives.cpp
    src/hash.cpp
    src/lds_codec.cpp
    src/native_flac_encoder.cpp
    src/opencl_devices.cpp
)

target_include_directories(ldcompress_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_options(ldcompress_core
    PRIVATE
        $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>
)

target_compile_definitions(ldcompress_core
    PRIVATE
        LDCOMPRESS_HAVE_OPENCL=${LDCOMPRESS_HAVE_OPENCL}
)

target_link_libraries(ldcompress_core
    PUBLIC
        PkgConfig::FLAC
        PkgConfig::OGG
        Threads::Threads
)

if(LDCOMPRESS_HAVE_OPENCL)
    target_link_libraries(ldcompress_core
        PRIVATE
            OpenCL::OpenCL
    )
endif()

add_executable(ld-compress-ng
    src/main.cpp
)

target_link_libraries(ld-compress-ng
    PRIVATE
        ldcompress_core
)

target_compile_options(ld-compress-ng
    PRIVATE
        $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>
)

if(BUILD_TESTING)
    add_executable(test_lds_codec
        tests/test_lds_codec.cpp
    )

    target_link_libraries(test_lds_codec
        PRIVATE
            ldcompress_core
    )

    add_test(NAME lds_codec COMMAND test_lds_codec)

    add_executable(test_flac_roundtrip
        tests/test_flac_roundtrip.cpp
    )

    target_link_libraries(test_flac_roundtrip
        PRIVATE
            ldcompress_core
    )

    add_test(NAME flac_roundtrip COMMAND test_flac_roundtrip)

    add_executable(test_flac_primitives
        tests/test_flac_primitives.cpp
    )

    target_link_libraries(test_flac_primitives
        PRIVATE
            ldcompress_core
    )

    add_test(NAME flac_primitives COMMAND test_flac_primitives)

    add_executable(test_flac_native_writer
        tests/test_flac_native_writer.cpp
    )

    target_link_libraries(test_flac_native_writer
        PRIVATE
            ldcompress_core
    )

    add_test(NAME flac_native_writer COMMAND test_flac_native_writer)

    add_executable(test_reference_ogg
        tests/test_reference_ogg.cpp
    )

    target_link_libraries(test_reference_ogg
        PRIVATE
            ldcompress_core
    )

    add_test(NAME reference_ogg COMMAND test_reference_ogg)

    add_executable(test_hash
        tests/test_hash.cpp
    )

    target_link_libraries(test_hash
        PRIVATE
            ldcompress_core
    )

    add_test(NAME hash COMMAND test_hash)

    add_executable(test_cli
        tests/test_cli.cpp
    )

    target_link_libraries(test_cli
        PRIVATE
            ldcompress_core
    )

    add_test(NAME cli COMMAND test_cli $<TARGET_FILE:ld-compress-ng>)

    if(LDCOMPRESS_ENABLE_REAL_FIXTURE_TESTS)
        if(NOT LDCOMPRESS_REAL_FIXTURE_DIR)
            message(FATAL_ERROR "LDCOMPRESS_REAL_FIXTURE_DIR must be set when LDCOMPRESS_ENABLE_REAL_FIXTURE_TESTS=ON")
        endif()
        if(NOT IS_DIRECTORY "${LDCOMPRESS_REAL_FIXTURE_DIR}")
            message(FATAL_ERROR "LDCOMPRESS_REAL_FIXTURE_DIR is not a directory: ${LDCOMPRESS_REAL_FIXTURE_DIR}")
        endif()

        add_executable(test_real_fixtures
            tests/test_real_fixtures.cpp
        )

        target_link_libraries(test_real_fixtures
            PRIVATE
                ldcompress_core
        )

        add_test(NAME real_fixtures COMMAND test_real_fixtures "${LDCOMPRESS_REAL_FIXTURE_DIR}")
        set_tests_properties(real_fixtures PROPERTIES LABELS real-fixtures)
    endif()

    if(LDCOMPRESS_ENABLE_FLAC_TEST_FILE_TESTS)
        if(NOT LDCOMPRESS_FLAC_TEST_FILE_DIR)
            message(FATAL_ERROR "LDCOMPRESS_FLAC_TEST_FILE_DIR must be set when LDCOMPRESS_ENABLE_FLAC_TEST_FILE_TESTS=ON")
        endif()
        if(NOT IS_DIRECTORY "${LDCOMPRESS_FLAC_TEST_FILE_DIR}")
            message(FATAL_ERROR "LDCOMPRESS_FLAC_TEST_FILE_DIR is not a directory: ${LDCOMPRESS_FLAC_TEST_FILE_DIR}")
        endif()

        add_executable(test_flac_test_files
            tests/test_flac_test_files.cpp
        )

        target_link_libraries(test_flac_test_files
            PRIVATE
                ldcompress_core
        )

        add_test(NAME flac_test_files COMMAND test_flac_test_files "${LDCOMPRESS_FLAC_TEST_FILE_DIR}")
        set_tests_properties(flac_test_files PROPERTIES LABELS flac-test-files)
    endif()
endif()
