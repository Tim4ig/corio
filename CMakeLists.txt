cmake_minimum_required(VERSION 3.28)

project(corio VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT LINUX)
    message(FATAL_ERROR "corio currently supports Linux only.")
endif()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
set(CORIO_SOURCES
    src/linux/io_uring.cc
    src/io_context.cc
    # Headers listed so IDEs include them in the project index
    include/corio/cancellation.h
    include/corio/context_var.h
    include/corio/gather.h
    include/corio/generator.h
    include/corio/io_context.h
    include/corio/task.h
    include/corio/timer.h
    include/corio/detail/context.h
    include/corio/detail/platform.h
    include/corio/detail/poller.h
    include/corio/detail/sleep.h
    include/corio/detail/token.h
    include/corio/detail/types.h
    include/corio/detail/linux/io_uring.h
)

option(BUILD_SHARED_LIBS "Build shared library" ON)
add_library(corio ${CORIO_SOURCES})
add_library(corio::corio ALIAS corio)

set_target_properties(corio PROPERTIES
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    OUTPUT_NAME corio
)

target_include_directories(corio
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        src/
)

target_link_libraries(corio PRIVATE uring)

target_compile_options(corio PRIVATE
    -Wall -Wextra -Wpedantic
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-Werror>
)

# ---------------------------------------------------------------------------
# Sanitizers (activated via CORIO_SANITIZE=ON or the asan preset)
# ---------------------------------------------------------------------------
option(CORIO_SANITIZE "Enable -fsanitize=address,undefined" OFF)
if(CORIO_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS corio
    EXPORT  corioTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT corioTargets
    FILE      corioTargets.cmake
    NAMESPACE corio::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/corio
)

include(CMakePackageConfigHelpers)

configure_package_config_file(
    cmake/corioConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/corioConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/corio
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/corioConfigVersion.cmake
    VERSION     ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/corioConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/corioConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/corio
)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
option(CORIO_BUILD_TESTS "Build tests" ON)
if(CORIO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
