cmake_minimum_required(VERSION 3.16)
project(lsmkv VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(LSMKV_BUILD_TESTS "Build unit tests" ON)
option(LSMKV_BUILD_EXAMPLES "Build examples" OFF)

add_library(lsmkv_lib
    src/status.cpp
    src/encoding.cpp
)
target_include_directories(lsmkv_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(lsmkv_lib PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
)
find_package(Threads REQUIRED)
target_link_libraries(lsmkv_lib PUBLIC Threads::Threads)

if(LSMKV_BUILD_TESTS)
    enable_testing()
    add_executable(lsmkv_tests
        tests/test_main.cpp
        tests/test_smoke.cpp
        tests/test_status.cpp
        tests/test_slice.cpp
        tests/test_encoding.cpp
    )
    target_link_libraries(lsmkv_tests PRIVATE lsmkv_lib)
    target_include_directories(lsmkv_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME lsmkv_tests COMMAND lsmkv_tests)
endif()
