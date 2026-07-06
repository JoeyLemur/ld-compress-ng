cmake_minimum_required(VERSION 3.20)

project(ld-compress-ng
    VERSION 1.0.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
include(GNUInstallDirs)

option(LDCOMPRESS_ENABLE_OPENCL "Enable OpenCL device enumeration support" ON)
option(LDCOMPRESS_ENABLE_VULKAN "Enable Vulkan compute device enumeration support" ON)
option(LDCOMPRESS_ENABLE_REAL_FIXTURE_TESTS "Enable opt-in tests against external real LDS fixtures" OFF)
option(LDCOMPRESS_ENABLE_FLAC_TEST_FILE_TESTS "Enable opt-in tests against external FLAC decoder testbench files" OFF)
set(LDCOMPRESS_REAL_FIXTURE_DIR "" CACHE PATH "Directory containing external .lds fixtures for opt-in real-fixture tests")
set(LDCOMPRESS_FLAC_TEST_FILE_DIR "" CACHE PATH "Directory containing external FLAC decoder testbench files")
set(LDCOMPRESS_OPENCL_TEST_DEVICE "" CACHE STRING "Flattened OpenCL device index for opt-in OpenCL compatibility tests")
set(LDCOMPRESS_VULKAN_TEST_DEVICE "" CACHE STRING "Vulkan device index for opt-in Vulkan smoke tests")
set(LDCOMPRESS_GLSLANG_VALIDATOR "" CACHE FILEPATH "glslangValidator executable for Vulkan shader builds")

find_package(Python3 QUIET COMPONENTS Interpreter)
find_program(FFMPEG_EXECUTABLE ffmpeg)
find_program(FFPROBE_EXECUTABLE ffprobe)
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

set(LDCOMPRESS_HAVE_VULKAN 0)
if(LDCOMPRESS_ENABLE_VULKAN)
    find_package(Vulkan QUIET)
    if(NOT LDCOMPRESS_GLSLANG_VALIDATOR)
        find_program(LDCOMPRESS_FOUND_GLSLANG_VALIDATOR glslangValidator)
        if(LDCOMPRESS_FOUND_GLSLANG_VALIDATOR)
            set(LDCOMPRESS_GLSLANG_VALIDATOR "${LDCOMPRESS_FOUND_GLSLANG_VALIDATOR}"
                CACHE FILEPATH "glslangValidator executable for Vulkan shader builds" FORCE)
        endif()
    endif()
    if(Vulkan_FOUND AND LDCOMPRESS_GLSLANG_VALIDATOR)
        set(LDCOMPRESS_HAVE_VULKAN 1)
        message(STATUS "Vulkan compute support enabled")
    elseif(NOT Vulkan_FOUND)
        message(STATUS "Vulkan not found; devices command will report unavailable")
    else()
        message(STATUS "glslangValidator not found; Vulkan support disabled")
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
    src/opencl_analysis.cpp
    src/opencl_backend.cpp
    src/opencl_devices.cpp
    src/vulkan_devices.cpp
    src/vulkan_smoke.cpp
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
        LDCOMPRESS_HAVE_VULKAN=${LDCOMPRESS_HAVE_VULKAN}
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

if(LDCOMPRESS_HAVE_VULKAN)
    target_link_libraries(ldcompress_core
        PRIVATE
            Vulkan::Vulkan
    )

    set(LDCOMPRESS_VULKAN_SMOKE_SHADER
        "${CMAKE_CURRENT_BINARY_DIR}/shaders/vulkan_smoke.comp.spv")
    add_custom_command(
        OUTPUT "${LDCOMPRESS_VULKAN_SMOKE_SHADER}"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${CMAKE_CURRENT_BINARY_DIR}/shaders"
        COMMAND "${LDCOMPRESS_GLSLANG_VALIDATOR}" -V
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan_smoke.comp"
            -o "${LDCOMPRESS_VULKAN_SMOKE_SHADER}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan_smoke.comp"
        VERBATIM
    )
    add_custom_target(vulkan_smoke_shader
        DEPENDS "${LDCOMPRESS_VULKAN_SMOKE_SHADER}"
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

target_compile_definitions(ld-compress-ng
    PRIVATE
        LDCOMPRESS_VERSION="${PROJECT_VERSION}"
)

install(TARGETS ld-compress-ng
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES
    README.md
    LICENSE
    THIRD_PARTY_NOTICES.md
    CHANGELOG.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
)

install(DIRECTORY docs/
    DESTINATION ${CMAKE_INSTALL_DOCDIR}/docs
    FILES_MATCHING PATTERN "*.md"
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

    add_executable(test_opencl_analysis
        tests/test_opencl_analysis.cpp
    )

    target_link_libraries(test_opencl_analysis
        PRIVATE
            ldcompress_core
    )

    add_test(NAME opencl_analysis COMMAND test_opencl_analysis)
    set_tests_properties(opencl_analysis PROPERTIES LABELS opencl)

    add_executable(test_opencl_devices
        tests/test_opencl_devices.cpp
    )

    target_link_libraries(test_opencl_devices
        PRIVATE
            ldcompress_core
    )

    add_test(NAME opencl_devices COMMAND test_opencl_devices)
    set_tests_properties(opencl_devices PROPERTIES LABELS opencl)

    add_executable(test_vulkan_devices
        tests/test_vulkan_devices.cpp
    )

    target_link_libraries(test_vulkan_devices
        PRIVATE
            ldcompress_core
    )

    add_test(NAME vulkan_devices COMMAND test_vulkan_devices)
    set_tests_properties(vulkan_devices PROPERTIES LABELS vulkan)

    add_executable(test_vulkan_smoke
        tests/test_vulkan_smoke.cpp
    )

    target_link_libraries(test_vulkan_smoke
        PRIVATE
            ldcompress_core
    )

    set(vulkan_smoke_command test_vulkan_smoke)
    if(LDCOMPRESS_HAVE_VULKAN)
        add_dependencies(test_vulkan_smoke vulkan_smoke_shader)
        list(APPEND vulkan_smoke_command "${LDCOMPRESS_VULKAN_SMOKE_SHADER}")
        if(NOT "${LDCOMPRESS_VULKAN_TEST_DEVICE}" STREQUAL "")
            list(APPEND vulkan_smoke_command
                --device "${LDCOMPRESS_VULKAN_TEST_DEVICE}"
            )
        endif()
    endif()

    add_test(NAME vulkan_smoke COMMAND ${vulkan_smoke_command})
    set_tests_properties(vulkan_smoke PROPERTIES LABELS vulkan)

    add_executable(compare_opencl_scalar_frames
        tests/compare_opencl_scalar_frames.cpp
    )

    target_link_libraries(compare_opencl_scalar_frames
        PRIVATE
            ldcompress_core
    )

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

    if(Python3_Interpreter_FOUND)
        add_test(
            NAME ffmpeg_native_flac_compat
            COMMAND
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_flac_external_decode.py
                --decoder ffmpeg
                --binary $<TARGET_FILE:ld-compress-ng>
                --ffmpeg ${FFMPEG_EXECUTABLE}
                --ffprobe ${FFPROBE_EXECUTABLE}
        )
        set_tests_properties(ffmpeg_native_flac_compat PROPERTIES
            LABELS external-decode-compat
            SKIP_RETURN_CODE 77
        )

        add_test(
            NAME ld_decode_pyav_compat
            COMMAND
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_flac_external_decode.py
                --decoder ld-decode-pyav
                --binary $<TARGET_FILE:ld-compress-ng>
                --ldf-reader ${CMAKE_CURRENT_SOURCE_DIR}/reference/ld-decode/lddecode/ldf_reader.py
        )
        set_tests_properties(ld_decode_pyav_compat PROPERTIES
            LABELS external-decode-compat
            SKIP_RETURN_CODE 77
        )

        add_test(
            NAME ld_decode_loader_compat
            COMMAND
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_flac_external_decode.py
                --decoder ld-decode-loader
                --binary $<TARGET_FILE:ld-compress-ng>
                --ld-decode-dir ${CMAKE_CURRENT_SOURCE_DIR}/reference/ld-decode
        )
        set_tests_properties(ld_decode_loader_compat PROPERTIES
            LABELS external-decode-compat
            SKIP_RETURN_CODE 77
        )
    endif()

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

        if(Python3_Interpreter_FOUND)
            add_test(
                NAME ld_decode_loader_real_fixture_compat
                COMMAND
                    ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_flac_external_decode.py
                    --decoder ld-decode-loader
                    --binary $<TARGET_FILE:ld-compress-ng>
                    --ld-decode-dir ${CMAKE_CURRENT_SOURCE_DIR}/reference/ld-decode
                    --fixture-dir ${LDCOMPRESS_REAL_FIXTURE_DIR}
                    --fixture-limit 1
            )
            set_tests_properties(ld_decode_loader_real_fixture_compat PROPERTIES
                LABELS "real-fixtures;external-decode-compat"
                SKIP_RETURN_CODE 77
            )

            set(LDCOMPRESS_OPENCL_REAL_FIXTURE_ARGS)
            if(NOT LDCOMPRESS_OPENCL_TEST_DEVICE STREQUAL "")
                list(APPEND LDCOMPRESS_OPENCL_REAL_FIXTURE_ARGS
                    --opencl-device ${LDCOMPRESS_OPENCL_TEST_DEVICE}
                )
            endif()

            add_test(
                NAME ld_decode_loader_opencl_real_fixture_compat
                COMMAND
                    ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_flac_external_decode.py
                    --decoder ld-decode-loader
                    --binary $<TARGET_FILE:ld-compress-ng>
                    --ld-decode-dir ${CMAKE_CURRENT_SOURCE_DIR}/reference/ld-decode
                    --fixture-dir ${LDCOMPRESS_REAL_FIXTURE_DIR}
                    --fixture-limit 1
                    --real-fixture-backend opencl
                    ${LDCOMPRESS_OPENCL_REAL_FIXTURE_ARGS}
            )
            set_tests_properties(ld_decode_loader_opencl_real_fixture_compat PROPERTIES
                LABELS "real-fixtures;external-decode-compat;opencl"
                SKIP_RETURN_CODE 77
            )
        endif()
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
