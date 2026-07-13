cmake_minimum_required(VERSION 3.16)
project(lsmkv VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(LSMKV_BUILD_TESTS "Build unit tests" ON)
option(LSMKV_BUILD_EXAMPLES "Build examples" ON)
option(LSMKV_BUILD_SERVER "Build TCP server binary" ON)

# Pick up every translation unit under src/ (and the reldb/ subdir) automatically.
file(GLOB LSMKV_LIB_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/reldb/*.cpp"
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

if(LSMKV_BUILD_EXAMPLES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/reldb_example.cpp")
    add_executable(reldb_example examples/reldb_example.cpp)
    target_link_libraries(reldb_example PRIVATE lsmkv_lib)
endif()

if(LSMKV_BUILD_EXAMPLES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/reldb_sql_example.cpp")
    add_executable(reldb_sql_example examples/reldb_sql_example.cpp)
    target_link_libraries(reldb_sql_example PRIVATE lsmkv_lib)
endif()

if(LSMKV_BUILD_EXAMPLES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/reldb_sql_shell.cpp")
    add_executable(reldb_sql_shell examples/reldb_sql_shell.cpp)
    target_link_libraries(reldb_sql_shell PRIVATE lsmkv_lib)
    # GNU Readline for line editing and statement history (Up/Down).
    find_path(READLINE_INCLUDE_DIR readline/readline.h)
    find_library(READLINE_LIBRARY readline)
    if(NOT READLINE_INCLUDE_DIR OR NOT READLINE_LIBRARY)
        message(FATAL_ERROR
            "reldb_sql_shell requires GNU Readline (install libreadline-dev)")
    endif()
    target_include_directories(reldb_sql_shell PRIVATE ${READLINE_INCLUDE_DIR})
    target_link_libraries(reldb_sql_shell PRIVATE ${READLINE_LIBRARY})
endif()

# Server library is built when the binary and/or tests need it.
set(LSMKV_NEED_SERVER_LIB OFF)
if(LSMKV_BUILD_SERVER OR LSMKV_BUILD_TESTS)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/server/server.cpp")
        set(LSMKV_NEED_SERVER_LIB ON)
    endif()
endif()

if(LSMKV_NEED_SERVER_LIB)
    add_library(lsmkv_server_lib server/server.cpp)
    target_include_directories(lsmkv_server_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/server)
    target_link_libraries(lsmkv_server_lib PUBLIC lsmkv_lib)
    target_compile_options(lsmkv_server_lib PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    )
endif()

if(LSMKV_BUILD_SERVER AND TARGET lsmkv_server_lib)
    add_executable(lsmkv_server server/main.cpp)
    target_link_libraries(lsmkv_server PRIVATE lsmkv_server_lib)
    target_compile_options(lsmkv_server PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    )
    install(TARGETS lsmkv_server RUNTIME DESTINATION bin)
endif()

# SQL protocol codec + connection request handler (no TCP listen yet).
# Shared by future reldb_sql_server / CLI / tests.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sql_server/protocol.cpp")
    set(RELDB_SQL_SERVER_SOURCES sql_server/protocol.cpp)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sql_server/sql_server.cpp")
        list(APPEND RELDB_SQL_SERVER_SOURCES sql_server/sql_server.cpp)
    endif()
    add_library(reldb_sql_protocol_lib ${RELDB_SQL_SERVER_SOURCES})
    target_include_directories(reldb_sql_protocol_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/sql_server)
    target_link_libraries(reldb_sql_protocol_lib PUBLIC lsmkv_lib)
    target_compile_options(reldb_sql_protocol_lib PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    )
endif()

if(LSMKV_BUILD_TESTS)
    enable_testing()
    file(GLOB LSMKV_TEST_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_*.cpp"
    )
    add_executable(lsmkv_tests ${LSMKV_TEST_SOURCES})
    target_link_libraries(lsmkv_tests PRIVATE lsmkv_lib)
    if(TARGET lsmkv_server_lib)
        target_link_libraries(lsmkv_tests PRIVATE lsmkv_server_lib)
    endif()
    if(TARGET reldb_sql_protocol_lib)
        target_link_libraries(lsmkv_tests PRIVATE reldb_sql_protocol_lib)
    endif()
    target_include_directories(lsmkv_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_options(lsmkv_tests PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    )
    add_test(NAME lsmkv_tests COMMAND lsmkv_tests)
endif()

install(TARGETS lsmkv_lib ARCHIVE DESTINATION lib LIBRARY DESTINATION lib)
install(DIRECTORY include/lsmkv DESTINATION include)
