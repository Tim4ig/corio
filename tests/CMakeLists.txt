include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.7.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Catch2)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

set(CORIO_TEST_SOURCES
    test_task.cc
    test_generator.cc
    test_gather.cc
    test_timer.cc
    test_io_context.cc
    test_cancellation.cc
    test_context_var.cc
)

add_executable(corio_tests ${CORIO_TEST_SOURCES})

target_link_libraries(corio_tests
    PRIVATE
        corio::corio
        Catch2::Catch2WithMain
)

target_compile_options(corio_tests PRIVATE
    -Wall -Wextra
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-Werror>
)

catch_discover_tests(corio_tests)
