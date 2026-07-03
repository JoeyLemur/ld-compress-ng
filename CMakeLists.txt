cmake_minimum_required(VERSION 3.20)

project(ld-compress-ng
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

add_library(ldcompress_core
    src/lds_codec.cpp
)

target_include_directories(ldcompress_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_options(ldcompress_core
    PRIVATE
        $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall -Wextra -Wpedantic>
)

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
endif()
