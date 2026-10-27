cmake_minimum_required(VERSION 3.16)
project(etrace C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(etrace
    main.c
    etrace.c
    syscalls.c
    render.c
    recorder.c
    replayer.c
    dump.c
)
target_compile_definitions(etrace PRIVATE _GNU_SOURCE)
target_compile_options(etrace PRIVATE -Wall -Wextra -Wpedantic)

find_package(Threads REQUIRED)
add_executable(test_crash test_crash.c)
target_link_libraries(test_crash PRIVATE Threads::Threads)

enable_testing()
add_test(NAME etrace-version COMMAND etrace version)
add_test(NAME etrace-suite
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.sh)
set_tests_properties(etrace-suite PROPERTIES ENVIRONMENT
    "ETRACE_BIN=${CMAKE_CURRENT_BINARY_DIR}/etrace;TEST_CRASH=${CMAKE_CURRENT_BINARY_DIR}/test_crash")

install(TARGETS etrace RUNTIME DESTINATION bin)
