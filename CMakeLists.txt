cmake_minimum_required(VERSION 3.16)
project(lsmkv VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(LSMKV_BUILD_TESTS "Build unit tests" ON)
option(LSMKV_BUILD_EXAMPLES "Build examples" ON)
option(LSMKV_BUILD_SERVER "Build TCP server binary" ON)

# Pick up every translation unit under src/ automatically.
file(GLOB LSMKV_LIB_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
add_library(lsmkv_lib ${LSMKV_LIB_SOURCES})
target_include_directories(lsmkv_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(lsmkv_lib PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
)
find_package(Threads REQUIRED)
target_link_libraries(lsmkv_lib PUBLIC Threads::Threads)

# Debug-only assertions (LSMKV_DCHECK*). Tied to the Debug *config*, not NDEBUG.
target_compile_definitions(lsmkv_lib PUBLIC
    $<$<CONFIG:Debug>:LSMKV_ENABLE_DCHECKS=1>
)

if(LSMKV_BUILD_EXAMPLES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/lsmkv_example.cpp")
    add_executable(lsmkv_example examples/lsmkv_example.cpp)
    target_link_libraries(lsmkv_example PRIVATE lsmkv_lib)
endif()

if(LSMKV_BUILD_SERVER AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/server/lsmkv_server.cpp")
    add_executable(lsmkv_server server/lsmkv_server.cpp)
    target_link_libraries(lsmkv_server PRIVATE lsmkv_lib)
    target_compile_options(lsmkv_server PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    )
    install(TARGETS lsmkv_server RUNTIME DESTINATION bin)
endif()

if(LSMKV_BUILD_TESTS)
    enable_testing()
    file(GLOB LSMKV_TEST_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_*.cpp"
    )
    add_executable(lsmkv_tests ${LSMKV_TEST_SOURCES})
    target_link_libraries(lsmkv_tests PRIVATE lsmkv_lib)
    target_include_directories(lsmkv_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_options(lsmkv_tests PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    )
    add_test(NAME lsmkv_tests COMMAND lsmkv_tests)
endif()

install(TARGETS lsmkv_lib ARCHIVE DESTINATION lib LIBRARY DESTINATION lib)
install(DIRECTORY include/lsmkv DESTINATION include)
