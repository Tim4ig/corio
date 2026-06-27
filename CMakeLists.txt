cmake_minimum_required(VERSION 4.2)

set(CMAKE_C_COMPILER gcc)
set(CMAKE_CXX_COMPILER g++)

project(corio VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(SRC_FILES
  src/linux/epoll.cc
  src/io_context.cc
  src/main.cc
)

add_executable(${PROJECT_NAME} ${SRC_FILES})

target_include_directories(${PROJECT_NAME}
  PUBLIC include/
  PRIVATE src/
)
